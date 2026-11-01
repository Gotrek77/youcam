# Background removal — design

Date: 2026-11-01. Branch: `background-removal` (from `linux-port`). Scope: Linux C port in `linux/`.

## Goal

Segment the person in the camera feed and let the user choose what happens to
the background: **Transparent**, **Blur**, **Image**, **Color**, or **Off**.
The effect must look identical in the preview and in recordings. No external
dependencies: segmentation runs on our own C inference code, on the CPU.

## Non-goals (for now)

- GPU inference (CUDA, OpenVINO). Planned only if the benchmark shows the CPU
  path cannot meet the budget below. CUDA would go through the NVIDIA driver
  API with kernels built by nvcc, as an optional backend; OpenVINO would be an
  external dependency and needs a separate decision.
- Click-through on transparent pixels (the window input shape would have to
  change every frame).
- Adjustable blur strength, edge refinement controls, multiple people
  handling beyond what the model does.

## Model

MediaPipe Selfie Segmentation (Google), float16 weights, about 250 KB each:

| Variant | Input | Output | Used when |
|---|---|---|---|
| `selfie_segmenter_landscape` | 144×256×3 | 144×256×1 | camera aspect ≥ 1.5 (16:9) |
| `selfie_segmenter` | 256×256×3 | 256×256×1 | otherwise (4:3) |

Operators (246 per model): CONV_2D 43, DEPTHWISE_CONV_2D 11, RELU 22,
HARD_SWISH 11, LOGISTIC 11, MEAN / AVERAGE_POOL_2D 10 (global pooling in
squeeze-excitation blocks), MUL 10, ADD 14, RESIZE_BILINEAR 3, one MediaPipe
custom `Convolution2DTransposeBias`, and 110 DEQUANTIZE (fp16 → fp32 weights,
resolved offline).

Sources:
`https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_segmenter/float16/latest/selfie_segmenter.tflite`
and the same path with `selfie_segmenter_landscape`.

**License gate:** before vendoring, confirm the license on the official model
card (expected Apache 2.0). Copy the license and provenance (URL, date,
SHA-256) into `linux/vendor/selfie/`. If the license is not compatible with the
repository's MIT license, stop and report.

## Architecture

```
camera thread ──frame──▶ segment thread ──mask──▶ main loop ──▶ camera_layer_draw
                         (downscale, run model,                 (preview window and
                          smooth, publish latest)                recording layer)
```

### Offline conversion — `linux/tools/convert_selfie.py` (developer only)

- Reads the `.tflite` (flatbuffer, via the `tflite` Python package),
  dequantizes fp16 weights to fp32, and writes `linux/vendor/selfie/*.ysm`:
  a minimal binary format, a header and a flat list of layers
  (op, tensor indices, shapes, stride, padding, activation, fp32 weights).
- Also contains a numpy reference implementation of every operator, used to
  dump per-layer outputs for debugging.
- Generates golden fixtures (see Testing) by running the original model with
  the `mediapipe` Python package.
- YouCam never reads `.tflite` and never needs Python at runtime.

### Inference engine — `linux/segment.c`

- `segment_model *segment_load(const char *path, GError **err)`: parses `.ysm`,
  validates every shape and index (file content is untrusted), allocates
  activation buffers once.
- `void segment_run(segment_model *m, const float *rgb, float *mask)`: runs
  the layers in order. One function per operator. Convolutions are plain C
  loops the compiler can vectorise; the hot ones use GCC/Clang
  `target_clones("avx2", "default")`, so one portable binary uses AVX2 when the
  CPU has it. No intrinsics. Work is split across rows with a small thread pool.
- Pure, synchronous, no GTK: unit-testable. Declarations go in `youcam.h`,
  like every other module.

### Background worker — `linux/background.c`

- Owns the segment thread. Enabled only when the mode is not Off: no thread,
  no model loaded, zero cost when Off.
- Takes the latest camera frame (never queues; skips frames when behind),
  downscales to the model input, converts to RGB in [0,1] as MediaPipe expects,
  runs the model, upsamples the mask to camera resolution, smooths it over time
  (exponential moving average to stop edge flicker) and publishes it with
  `g_idle_add`, like Center Stage.
- Picks the model variant from the camera aspect ratio; reloads when the camera
  changes.
- Load failure: logs the reason, marks background removal unavailable.

### Compositing — `linux/camera_layer.c`

The existing shared drawing path (preview window and recorded camera layer)
gains a background step, applied with the same Center Stage transform and
mirroring as the camera image, so mask and image stay aligned:

| Mode | Drawing |
|---|---|
| Transparent | mask as alpha; the shape (circle/square/rectangle) no longer clips |
| Blur | background = two-pass box blur of a downscaled frame; person sharp on top |
| Image | user image, scaled to fill (cover), behind the person |
| Color | solid color behind the person |

The transparency slider multiplies on top in every mode.

## User interface

New **Background** submenu, below Transparency:

- Radio items: **Off** (default), **Transparent**, **Blur**, **Image**, **Color**.
- **Choose Image…**: GTK file chooser (PNG, JPEG). Choosing Image without a
  saved image opens the chooser first.
- **Choose Color…**: GTK color chooser.
- When unavailable: a disabled item "Background removal unavailable".

Preferences (keyfile, existing `prefs.c`): `backgroundMode`
(off/transparent/blur/image/color, unknown → off), `backgroundImage` (path;
missing file → fall back to Blur with a log line), `backgroundColor`
(`#rrggbb`, invalid → dark gray).

Help text and README describe the feature; README credits the model and its
license.

## Interaction with existing features

- **Shapes:** Transparent ignores the shape mask; other modes keep it.
- **Transparency:** stacks.
- **Center Stage, mirror:** the mask is computed on the full camera frame and
  follows the same transform.
- **Dragging and menu:** the whole camera window stays clickable, including
  transparent pixels.
- **Recording:** the portal captures the window as seen, so every mode reaches
  the movie without extra work. When the camera layer is composited into the
  movie (transparency not applied to the saved video), it uses the same
  drawing, so the result is identical.

## Performance budget

- Target: ≤ 5 ms per inference on this machine (24 cores, AVX2), and under one
  core on average at the mask update rate (15–30 Hz).
- If inference is slower, the mask updates less often; the preview never
  stalls.
- `make bench-segment` prints ms per inference for both variants. It is a
  measurement, not a pass/fail test; it decides whether a GPU backend is needed.

## Testing (`make test`)

1. **Operator tests:** each operator on small cases computed by hand or numpy,
   including padding SAME with stride 2, depthwise multipliers, bilinear resize
   semantics (half-pixel vs align-corners as the model specifies), and the
   custom transposed convolution.
2. **Golden model test:** C mask vs the MediaPipe mask on committed fixtures.
   Pass if the max absolute per-pixel difference is < 0.02 and IoU at 0.5 is
   > 0.98, for both variants.
3. **Edge cases:** black and empty frames (no person, no crash); truncated or
   corrupt `.ysm` (clean error, feature unavailable).
4. **Compositing:** offscreen render of each mode; check alpha and pixels in
   person and background regions.
5. **Media:** `--record-test` with Transparent mode; the movie shows the screen
   around the silhouette.

Fixtures: public-domain or CC0 portraits (e.g. Wikimedia Commons), stored at
model input size with their source and license noted. Webcam frames of the
user are used only locally, never committed.

Manual verification by the user: edges, hair, hands, low light, moving.

## Risks

- **Operator semantics** (padding, resize, transposed conv) silently wrong:
  mitigated by per-layer comparison against the numpy reference and the golden
  test.
- **Mask quality** at the edges with fast motion: temporal smoothing; tune with
  the user's real camera.
- **CPU cost** higher than estimated: frame skipping keeps the preview fluid;
  GPU backend as a follow-up.
- **License** not compatible: stop before vendoring.

## Files

New: `linux/segment.c`, `linux/background.c`,
`linux/tools/convert_selfie.py`, `linux/vendor/selfie/` (models, LICENSE,
PROVENANCE), `linux/test_segment.c`, test fixtures.
Changed: `linux/camera_layer.c`, `linux/app.c` (menu), `linux/prefs.c`,
`linux/youcam.h`, `linux/Makefile`, `linux/README.md`.
