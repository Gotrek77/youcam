# Background Removal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Segment the person in the camera feed with our own C inference code and let the user make the background Transparent, Blurred, an Image, or a Color, identically in the preview and in recordings.

**Architecture:** An offline Python tool converts MediaPipe Selfie Segmentation (`.tflite`) into a tiny binary format (`.ysm`), which is embedded in the binary like pico's cascade. `segment.c` is a small NHWC float32 inference engine (one function per operator). `background.c` runs it on a worker thread fed by the camera thread, smooths the mask over time, and builds the layers that `camera_layer.c` paints behind and in front of the person.

**Tech Stack:** C99, GLib/GTK 3/cairo/gdk-pixbuf (already used), GCC `target_clones`; developer-only Python 3.12 venv with `tflite`, `numpy`, `opencv`, `mediapipe` for conversion and golden fixtures.

**Spec:** `docs/superpowers/specs/2026-11-01-background-removal-design.md`

## Global Constraints

- Branch `background-removal`; all code in `linux/`; the Swift sources stay untouched.
- No new runtime dependencies: GTK 3, GStreamer, GLib, cairo, gdk-pixbuf only. YouCam never reads `.tflite` and never needs Python at runtime.
- CFLAGS stay `-std=c99 -D_DEFAULT_SOURCE -Wall -Wextra`; no warnings.
- Declarations go in `linux/youcam.h`, in a section per file, like every other module.
- `.ysm` content is untrusted: every shape, index and count is validated; a bad file yields a clean `GError`, never a crash.
- Modes: **Off** (default), **Transparent**, **Blur**, **Image**, **Color**. Prefs keys `backgroundMode` (off/transparent/blur/image/color, unknown → off), `backgroundImage` (path; missing file → draw Blur, one log line), `backgroundColor` (`#rrggbb`, invalid → dark gray).
- Zero cost when Off: no thread, no model loaded.
- Golden test: max absolute per-pixel difference < 0.02 and IoU at 0.5 > 0.98 against MediaPipe, both variants.
- Performance target: ≤ 5 ms per inference on this machine; measured by `make bench-segment` (a measurement, not a pass/fail test).
- License gate: model license must be compatible with the repository's MIT license, else stop and report to the user.
- Commits: `<type>: <description>`, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never push.

## Deviations from the spec (deliberate, smaller)

- The `.ysm` files are embedded with `xxd -i` (as pico's `facefinder`), so `segment_load` takes bytes, not a path: `segment_model *segment_load(const guint8 *data, size_t size, GError **err)`. No install paths, nothing to find at runtime.
- `segment_run` takes packed 8-bit RGB and applies the normalization stored in the `.ysm` header, so no caller needs to know it.
- The worker publishes the mask at model resolution; it is upsampled (bilinear) while compositing, on the main thread. Same result, one less buffer.
- All activations in both models have `act=0`, dilation 1, depth multiplier 1, every MUL broadcasts `[1,1,C]` and every ADD is same-shape (verified on both `.tflite` files). The converter asserts these, so the C engine has no fused activations, no dilation and no generic broadcasting.

## Paths used below

- `SCRATCH=/home/patane/.cache/claude-tmp/claude-1000/-home-patane-Works-YouCam-A-youcam/46c95e59-cadb-4b73-9cd8-9e427ef68d24/scratchpad/seg`
- Python venv with tflite, numpy, opencv (cv2 5.0), mediapipe 1.2.0: `$SCRATCH/ref/bin/python` (always run with `-I`).
- Downloaded models: `$SCRATCH/model/selfie_segmenter_landscape.tflite`, `$SCRATCH/model/selfie_segmenter.tflite`.
- Fixture sources (Public domain, NASA, Wikimedia Commons):
  `$SCRATCH/fixtures-src/Sally_Ride_(1984).jpg` from `https://upload.wikimedia.org/wikipedia/commons/0/0c/Sally_Ride_%281984%29.jpg`,
  `$SCRATCH/fixtures-src/Neil_Armstrong_pose.jpg` from `https://upload.wikimedia.org/wikipedia/commons/0/0d/Neil_Armstrong_pose.jpg`.

## File map

| File | Responsibility |
|---|---|
| `linux/vendor/selfie/` | Original `.tflite` files, converted `.ysm`, `LICENSE`, `PROVENANCE.md` |
| `linux/tools/convert_selfie.py` | `.tflite` → `.ysm`; numpy reference implementation; `--check` against a golden mask |
| `linux/tools/golden_selfie.py` | Crops a portrait to model size, runs MediaPipe, writes fixture + golden mask PNGs |
| `linux/testdata/selfie/` | Fixtures (`<name>-<variant>.png`, `<name>-<variant>-mask.png`) and their `README.md` |
| `linux/segment.c` | `.ysm` loader with validation, operators, `segment_run` |
| `linux/background.c` | Mode names and parsing, pure image helpers, embedded models, worker thread, `background_now` |
| `linux/test_segment.c` | Operator, loader, golden, edge-case tests and `--bench` |
| `linux/camera_layer.c` | Paints backdrop + person; Transparent skips the shape clip |
| `linux/overlay.c`, `linux/app.c`, `linux/camera.c`, `linux/prefs.c`, `linux/youcam.h`, `linux/Makefile`, `linux/tests.c`, `linux/README.md` | Wiring, menu, prefs, tests, docs |

---

### Task 1: License gate and vendored models

**Files:**
- Create: `linux/vendor/selfie/selfie_segmenter_landscape.tflite`, `linux/vendor/selfie/selfie_segmenter.tflite`, `linux/vendor/selfie/LICENSE`, `linux/vendor/selfie/PROVENANCE.md`

**Interfaces:**
- Produces: the two `.tflite` files at the paths above, used by Tasks 2–3.

- [ ] **Step 1: Read the license on the official model card**

Fetch the MediaPipe Selfie Segmentation model card (linked from `https://ai.google.dev/edge/mediapipe/solutions/vision/image_segmenter#selfie-model`; the card is `https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Selfie%20Segmentation.pdf`). Find the "License" line.
Expected: Apache License 2.0.
If it is anything else (or no license is stated): **stop**, report the exact text to the user, do not vendor anything.

- [ ] **Step 2: Copy the models and record provenance**

```bash
cd /home/patane/Works/YouCam_A/youcam/linux
mkdir -p vendor/selfie
cp "$SCRATCH/model/selfie_segmenter_landscape.tflite" "$SCRATCH/model/selfie_segmenter.tflite" vendor/selfie/
curl -fsSL https://www.apache.org/licenses/LICENSE-2.0.txt -o vendor/selfie/LICENSE
sha256sum vendor/selfie/*.tflite
```

Write `vendor/selfie/PROVENANCE.md` (fill the two hashes from the command above; date is 2026-11-01):

```markdown
# MediaPipe Selfie Segmentation

Google, Apache License 2.0 (see LICENSE and the model card:
https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Selfie%20Segmentation.pdf).

| File | Source | SHA-256 |
|---|---|---|
| selfie_segmenter_landscape.tflite | https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_segmenter_landscape/float16/latest/selfie_segmenter_landscape.tflite | 490e9ea734313e0de10fa0cd9e3c6133e36ea4db2b7a49bde9ef019f72796b8e |
| selfie_segmenter.tflite | https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_segmenter/float16/latest/selfie_segmenter.tflite | 191ac9529ae506ee0beefa6b2c945a172dab9d07d1e802a290a4e4038226658b |

Downloaded 2026-11-01. The `.ysm` files are converted from these by
`tools/convert_selfie.py` (weights dequantized to float32, unchanged otherwise):

    python3 tools/convert_selfie.py vendor/selfie/selfie_segmenter_landscape.tflite vendor/selfie/selfie_landscape.ysm --norm <unit|signed>
    python3 tools/convert_selfie.py vendor/selfie/selfie_segmenter.tflite vendor/selfie/selfie_square.ysm --norm <unit|signed>
```

(The hashes are those of the files downloaded on 2026-11-01; recheck them with the `sha256sum` above. `--norm` is filled in by Task 2 Step 5; the file must contain no `<...>` when Task 2 is committed.)

- [ ] **Step 3: Commit**

```bash
git add vendor/selfie
git commit -m "chore: vendor MediaPipe Selfie Segmentation models

Apache 2.0, with license and provenance.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Converter, numpy reference, golden fixtures, `.ysm` files

**Files:**
- Create: `linux/tools/convert_selfie.py`, `linux/tools/golden_selfie.py`
- Create: `linux/testdata/selfie/{ride,armstrong}-{landscape,square}.png` and `...-mask.png`, `linux/testdata/selfie/README.md`
- Create: `linux/vendor/selfie/selfie_landscape.ysm`, `linux/vendor/selfie/selfie_square.ysm`
- Modify: `linux/vendor/selfie/PROVENANCE.md` (the `--norm` value)

**Interfaces:**
- Produces the `.ysm` format, little endian, read by Task 3:
  - Header: `char magic[4] = "YSM1"`, `u32 in_h, in_w`, `f32 norm_scale, norm_offset` (model input = byte × scale + offset), `u32 invert` (1: mask = 1 − output), `u32 tensor_count, layer_count, input, output`.
  - `tensor_count` × `u32 h, w, c` (activations only, batch 1).
  - `layer_count` × { `u32 op, in0, in1, out, kh, kw, stride, pad_top, pad_left, weight_count, bias_count`, then `f32 weights[weight_count]`, `f32 bias[bias_count]` }. `in1 = 0xFFFFFFFF` when unused.
  - Ops: 1 CONV, 2 DWCONV, 3 RELU, 4 HSWISH, 5 SIGMOID, 6 MEAN (global average, out 1×1×C), 7 MUL (in1 is 1×1×C), 8 ADD, 9 RESIZE (bilinear, half-pixel centers, to the out tensor size), 10 TCONV (kernel = stride, no padding).
  - Weight layouts: CONV and TCONV `[kh][kw][in c][out c]`; DWCONV `[kh][kw][c]`.
- Fixtures: `testdata/selfie/<name>-<variant>.png` (RGB, exactly model input size: landscape 256×144, square 256×256) and `<name>-<variant>-mask.png` (8-bit gray, person confidence × 255, from MediaPipe).

- [ ] **Step 1: Write `tools/golden_selfie.py`**

```python
#!/usr/bin/env python3
"""Golden masks for test_segment.c. Crops a portrait to each model's input
size, saves it, and runs the original model on it with MediaPipe.

Developer tool (Python 3.12): pip install mediapipe opencv-python numpy
Usage: golden_selfie.py PORTRAIT.jpg NAME MODEL_DIR OUT_DIR"""
import sys

import cv2
import mediapipe as mp
import numpy as np
from mediapipe.tasks.python import BaseOptions, vision

CROP_CENTER_Y = 0.4   # Portraits: keep the head and shoulders in the crop.
VARIANTS = {'landscape': ('selfie_segmenter_landscape.tflite', 256, 144),
            'square': ('selfie_segmenter.tflite', 256, 256)}


def crop(img, w, h):
    H, W = img.shape[:2]
    cw, ch = (round(H * w / h), H) if W / H > w / h else (W, round(W * h / w))
    x0 = (W - cw) // 2
    y0 = min(max(round(H * CROP_CENTER_Y - ch / 2), 0), H - ch)
    return cv2.resize(img[y0:y0 + ch, x0:x0 + cw], (w, h), interpolation=cv2.INTER_AREA)


def main(src, name, model_dir, out_dir):
    img = cv2.imread(src)
    if img is None:
        sys.exit(f'cannot read {src}')
    for variant, (model, w, h) in VARIANTS.items():
        small = crop(img, w, h)
        base = f'{out_dir}/{name}-{variant}'
        cv2.imwrite(base + '.png', small)
        rgb = np.ascontiguousarray(cv2.cvtColor(small, cv2.COLOR_BGR2RGB))
        opts = vision.ImageSegmenterOptions(base_options=BaseOptions(model_asset_path=f'{model_dir}/{model}'),
                                            output_confidence_masks=True)
        with vision.ImageSegmenter.create_from_options(opts) as seg:
            res = seg.segment(mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb))
        mask = res.confidence_masks[0].numpy_view().reshape(h, w, -1)[:, :, 0]
        cv2.imwrite(base + '-mask.png', np.round(np.clip(mask, 0, 1) * 255).astype(np.uint8))
        print(base, 'person fraction', round(float((mask >= 0.5).mean()), 3))


if __name__ == '__main__':
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
```

- [ ] **Step 2: Generate the fixtures**

```bash
cd /home/patane/Works/YouCam_A/youcam/linux
mkdir -p testdata/selfie
$SCRATCH/ref/bin/python -I tools/golden_selfie.py "$SCRATCH/fixtures-src/Sally_Ride_(1984).jpg" ride vendor/selfie testdata/selfie
$SCRATCH/ref/bin/python -I tools/golden_selfie.py "$SCRATCH/fixtures-src/Neil_Armstrong_pose.jpg" armstrong vendor/selfie testdata/selfie
```

Expected: four lines, each person fraction between 0.2 and 0.8. Open one fixture and its mask (Read tool on the PNG) and check the mask is white on the person. If a fraction is outside that range, change `CROP_CENTER_Y` and rerun.

Write `testdata/selfie/README.md`:

```markdown
# Segmentation fixtures

Cropped and resized by `tools/golden_selfie.py`; `*-mask.png` is the person
confidence (× 255) from the original model run by MediaPipe 1.2.0.

| Name | Source | License |
|---|---|---|
| ride | https://commons.wikimedia.org/wiki/File:Sally_Ride_(1984).jpg | Public domain (NASA) |
| armstrong | https://commons.wikimedia.org/wiki/File:Neil_Armstrong_pose.jpg | Public domain (NASA) |
```

- [ ] **Step 3: Write `tools/convert_selfie.py`**

```python
#!/usr/bin/env python3
"""Converts MediaPipe Selfie Segmentation (.tflite) to YouCam's .ysm, and
checks a numpy reference implementation of the model against a golden mask.

Developer tool: pip install tflite numpy opencv-python
  convert_selfie.py MODEL.tflite OUT.ysm --norm unit|signed [--invert]
  convert_selfie.py MODEL.tflite --check IMAGE.png MASK.png
The .ysm format is described in segment.c."""
import struct
import sys

import numpy as np
import tflite
from tflite.BuiltinOperator import BuiltinOperator as B

OPS = {'CONV': 1, 'DWCONV': 2, 'RELU': 3, 'HSWISH': 4, 'SIGMOID': 5, 'MEAN': 6,
       'MUL': 7, 'ADD': 8, 'RESIZE': 9, 'TCONV': 10}
NONE = 0xFFFFFFFF
NORMS = {'unit': (1 / 255, 0.0), 'signed': (2 / 255, -1.0)}
SAME, VALID = 0, 1   # tflite Padding enum
DTYPES = {tflite.TensorType.FLOAT16: np.float16, tflite.TensorType.FLOAT32: np.float32,
          tflite.TensorType.INT32: np.int32}


class Model:
    """The single subgraph, its operators as (name, inputs, output, op), and constants."""

    def __init__(self, path):
        self.m = tflite.Model.GetRootAsModel(open(path, 'rb').read(), 0)
        self.g = self.m.Subgraphs(0)
        self.consts = {}
        self.ops = []
        for i in range(self.g.OperatorsLength()):
            op = self.g.Operators(i)
            code = self.m.OperatorCodes(op.OpcodeIndex())
            b = max(code.BuiltinCode(), code.DeprecatedBuiltinCode())
            ins = [op.Inputs(j) for j in range(op.InputsLength())]
            out = op.Outputs(0)
            if b == B.DEQUANTIZE:
                self.consts[out] = self.raw(ins[0]).astype(np.float32)
                continue
            name = 'CUSTOM' if b == B.CUSTOM else next(k for k, v in B.__dict__.items() if v == b)
            self.ops.append((name, ins, out, op))
        self.input, self.output = self.g.Inputs(0), self.g.Outputs(0)

    def raw(self, t):
        tensor = self.g.Tensors(t)
        data = self.m.Buffers(tensor.Buffer()).DataAsNumpy()
        assert not isinstance(data, int), f'tensor {t} is not constant'
        return np.frombuffer(data.tobytes(), DTYPES[tensor.Type()]).reshape(tensor.ShapeAsNumpy())

    def const(self, t):
        return self.consts[t] if t in self.consts else self.raw(t).astype(np.float32)

    def shape(self, t):
        s = list(self.g.Tensors(t).ShapeAsNumpy())
        assert len(s) == 4 and s[0] == 1, f'tensor {t} shape {s}'
        return s[1:]


def options(op, cls):
    o = cls()
    t = op.BuiltinOptions()
    o.Init(t.Bytes, t.Pos)
    return o


def conv_params(op, cls, in_hw, k_hw, out_hw):
    """Stride and explicit top/left padding, checking what the C engine supports."""
    o = options(op, cls)
    assert o.FusedActivationFunction() == 0, 'fused activation'
    assert o.StrideH() == o.StrideW(), 'non-square stride'
    if cls is tflite.Conv2DOptions:
        assert o.DilationHFactor() == 1 and o.DilationWFactor() == 1, 'dilation'
    else:
        assert o.DepthMultiplier() == 1 and o.DilationHFactor() == 1, 'depth multiplier / dilation'
    s = o.StrideH()
    pads = [0, 0]
    if o.Padding() == SAME:
        pads = [max((out - 1) * s + k - size, 0) // 2 for size, k, out in zip(in_hw, k_hw, out_hw)]
    return s, pads


def tconv_params(op):
    padding, stride_w, stride_h = struct.unpack('<3i', bytes(op.CustomOptionsAsNumpy()))
    assert padding == VALID and stride_w == stride_h, 'transposed convolution options'
    return stride_h


# ---------------------------------------------------------------- converter

def convert(model, out_path, norm, invert):
    ids = {model.input: 0}
    def tid(t):
        return ids.setdefault(t, len(ids))
    layers = []
    for name, ins, out, op in model.ops:
        x = ins[0]
        hw_in, hw_out = model.shape(x)[:2], model.shape(out)[:2]
        rec = dict(op=None, in0=tid(x), in1=NONE, kh=0, kw=0, stride=0, pad=(0, 0), w=np.zeros(0), b=np.zeros(0))
        if name == 'CONV_2D':
            w = model.const(ins[1])                      # [O, KH, KW, I]
            s, pad = conv_params(op, tflite.Conv2DOptions, hw_in, w.shape[1:3], hw_out)
            rec.update(op='CONV', kh=w.shape[1], kw=w.shape[2], stride=s, pad=pad,
                       w=w.transpose(1, 2, 3, 0), b=model.const(ins[2]))
        elif name == 'DEPTHWISE_CONV_2D':
            w = model.const(ins[1])                      # [1, KH, KW, C]
            s, pad = conv_params(op, tflite.DepthwiseConv2DOptions, hw_in, w.shape[1:3], hw_out)
            rec.update(op='DWCONV', kh=w.shape[1], kw=w.shape[2], stride=s, pad=pad, w=w[0], b=model.const(ins[2]))
        elif name == 'CUSTOM':
            w = model.const(ins[1])                      # [O, KH, KW, I]
            s = tconv_params(op)
            assert w.shape[1] == w.shape[2] == s, 'transposed convolution kernel must equal its stride'
            rec.update(op='TCONV', kh=s, kw=s, stride=s, w=w.transpose(1, 2, 3, 0), b=model.const(ins[2]))
        elif name in ('RELU', 'HARD_SWISH', 'LOGISTIC'):
            rec['op'] = {'RELU': 'RELU', 'HARD_SWISH': 'HSWISH', 'LOGISTIC': 'SIGMOID'}[name]
        elif name in ('MEAN', 'AVERAGE_POOL_2D'):
            if name == 'AVERAGE_POOL_2D':
                o = options(op, tflite.Pool2DOptions)
                assert [o.FilterHeight(), o.FilterWidth()] == hw_in and o.FusedActivationFunction() == 0, 'not global'
            else:
                assert sorted(model.raw(ins[1]).tolist()) == [1, 2], 'mean axes'
            rec['op'] = 'MEAN'
        elif name in ('MUL', 'ADD'):
            o = options(op, tflite.MulOptions if name == 'MUL' else tflite.AddOptions)
            assert o.FusedActivationFunction() == 0, 'fused activation'
            rec.update(op=name, in1=tid(ins[1]))
        elif name == 'RESIZE_BILINEAR':
            o = options(op, tflite.ResizeBilinearOptions)
            assert not o.AlignCorners() and o.HalfPixelCenters(), 'resize mode'
            rec['op'] = 'RESIZE'
        else:
            raise SystemExit(f'unsupported operator {name}')
        rec['out'] = tid(out)
        layers.append(rec)
    assert model.output in ids
    in_h, in_w, _ = model.shape(model.input)
    scale, offset = NORMS[norm]
    blob = bytearray(struct.pack('<4s2I2f5I', b'YSM1', in_h, in_w, scale, offset, int(invert),
                                 len(ids), len(layers), ids[model.input], ids[model.output]))
    for t, _ in sorted(ids.items(), key=lambda kv: kv[1]):
        blob += struct.pack('<3I', *model.shape(t))
    for r in layers:
        w = np.ascontiguousarray(r['w'], np.float32).ravel()
        b = np.ascontiguousarray(r['b'], np.float32).ravel()
        blob += struct.pack('<11I', OPS[r['op']], r['in0'], r['in1'], r['out'], r['kh'], r['kw'],
                            r['stride'], r['pad'][0], r['pad'][1], w.size, b.size)
        blob += w.astype('<f4').tobytes() + b.astype('<f4').tobytes()
    open(out_path, 'wb').write(blob)
    print(f'{out_path}: {len(ids)} tensors, {len(layers)} layers, {len(blob)} bytes')


# ------------------------------------------------- numpy reference (tflite semantics)

def run_reference(model, rgb, norm, invert):
    scale, offset = NORMS[norm]
    v = {model.input: (rgb.astype(np.float32) * np.float32(scale) + np.float32(offset))[None]}
    for name, ins, out, op in model.ops:
        x = v[ins[0]]
        oh, ow = model.shape(out)[:2]
        if name in ('CONV_2D', 'DEPTHWISE_CONV_2D'):
            w, b = model.const(ins[1]), model.const(ins[2])
            cls = tflite.Conv2DOptions if name == 'CONV_2D' else tflite.DepthwiseConv2DOptions
            s, (pt, pl) = conv_params(op, cls, x.shape[1:3], w.shape[1:3], (oh, ow))
            kh, kw = w.shape[1:3]
            xp = np.pad(x, ((0, 0), (pt, kh + s * oh), (pl, kw + s * ow), (0, 0)))
            y = np.zeros((1, oh, ow, b.size), np.float32) + b
            for ky in range(kh):
                for kx in range(kw):
                    patch = xp[:, ky:ky + s * oh:s, kx:kx + s * ow:s, :]
                    if name == 'CONV_2D':
                        y += np.einsum('nhwi,oi->nhwo', patch, w[:, ky, kx, :])
                    else:
                        y += patch * w[0, ky, kx, :]
        elif name == 'CUSTOM':
            w, b = model.const(ins[1]), model.const(ins[2])
            s = tconv_params(op)
            y = np.zeros((1, oh, ow, b.size), np.float32) + b
            for ky in range(s):
                for kx in range(s):
                    y[:, ky::s, kx::s, :] += np.einsum('nhwi,oi->nhwo', x, w[:, ky, kx, :])
        elif name == 'RELU':
            y = np.maximum(x, 0)
        elif name == 'HARD_SWISH':
            y = x * np.clip(x + 3, 0, 6) / 6
        elif name == 'LOGISTIC':
            y = 1 / (1 + np.exp(-x))
        elif name in ('MEAN', 'AVERAGE_POOL_2D'):
            y = x.mean(axis=(1, 2), keepdims=True)
        elif name == 'MUL':
            y = x * v[ins[1]]
        elif name == 'ADD':
            y = x + v[ins[1]]
        elif name == 'RESIZE_BILINEAR':
            def axis(n_out, n_in):
                f = np.maximum((np.arange(n_out) + 0.5) * (n_in / n_out) - 0.5, 0)
                i0 = np.minimum(f.astype(int), n_in - 1)
                return i0, np.minimum(i0 + 1, n_in - 1), (f - i0).astype(np.float32)
            y0, y1, dy = axis(oh, x.shape[1])
            x0, x1, dx = axis(ow, x.shape[2])
            dx = dx[None, None, :, None]
            top = x[:, y0][:, :, x0] * (1 - dx) + x[:, y0][:, :, x1] * dx
            bottom = x[:, y1][:, :, x0] * (1 - dx) + x[:, y1][:, :, x1] * dx
            y = top + (bottom - top) * dy[None, :, None, None]
        else:
            raise SystemExit(f'unsupported operator {name}')
        v[out] = y.astype(np.float32)
    mask = v[model.output][0, :, :, 0]
    return 1 - mask if invert else mask


def check(model, image_path, mask_path):
    import cv2
    rgb = cv2.cvtColor(cv2.imread(image_path), cv2.COLOR_BGR2RGB)
    golden = cv2.imread(mask_path, cv2.IMREAD_GRAYSCALE).astype(np.float32) / 255
    for norm in NORMS:
        for invert in (False, True):
            m = run_reference(model, rgb, norm, invert)
            a, g = m >= 0.5, golden >= 0.5
            iou = (a & g).sum() / max((a | g).sum(), 1)
            print(f'norm={norm:6} invert={int(invert)} max_diff={np.abs(m - golden).max():.4f} iou={iou:.4f}')


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) == 4 and a[1] == '--check':
        check(Model(a[0]), a[2], a[3])
    elif len(a) in (4, 5) and a[2] == '--norm' and a[3] in NORMS and a[4:] in ([], ['--invert']):
        convert(Model(a[0]), a[1], a[3], a[4:] == ['--invert'])
    else:
        sys.exit(__doc__)
```

Run `$SCRATCH/ref/bin/python -I -m pyflakes tools/*.py` if pyflakes is installed; otherwise `python -I -m py_compile tools/*.py`.

- [ ] **Step 4: Find the input normalization and output polarity with the numpy reference**

```bash
for v in landscape:selfie_segmenter_landscape square:selfie_segmenter; do
  for n in ride armstrong; do
    $SCRATCH/ref/bin/python -I tools/convert_selfie.py vendor/selfie/${v#*:}.tflite \
      --check testdata/selfie/$n-${v%%:*}.png testdata/selfie/$n-${v%%:*}-mask.png
  done
done
```

Expected: for every fixture, exactly one of the four rows has `max_diff < 0.02` and `iou > 0.98`, and it is the same row everywhere. That row gives `--norm` and `--invert`.
If no row passes: the reference is wrong, not the model. Debug one operator at a time (compare against `tflite` intermediate tensors via `mediapipe`'s bundled interpreter, or reason about tflite reference semantics); do not loosen the thresholds.

- [ ] **Step 5: Convert both models**

With the winning row (shown here as `unit`, no invert):

```bash
$SCRATCH/ref/bin/python -I tools/convert_selfie.py vendor/selfie/selfie_segmenter_landscape.tflite vendor/selfie/selfie_landscape.ysm --norm unit
$SCRATCH/ref/bin/python -I tools/convert_selfie.py vendor/selfie/selfie_segmenter.tflite vendor/selfie/selfie_square.ysm --norm unit
```

Expected: each prints about 125 tensors, about 135 layers and roughly 0.5 MB. Put the real `--norm` (and `--invert` if used) into `PROVENANCE.md`.

- [ ] **Step 6: Commit**

```bash
git add tools testdata vendor/selfie
git commit -m "feat: convert the selfie segmentation models to .ysm

A numpy reference of every operator matches MediaPipe on public-domain
portraits; it also fixes the input normalization.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: `segment.c` operators

**Files:**
- Create: `linux/segment.c` (operators only in this task), `linux/test_segment.c`
- Modify: `linux/youcam.h` (new section), `linux/Makefile`

**Interfaces:**
- Produces (in `youcam.h`):

```c
/* ----------------------------------------------------------- segment.c --- */

typedef struct { int h, w, c; } seg_dims;   /* An NHWC tensor, batch 1. */

typedef struct {
    const float *in; seg_dims in_dims;
    const float *weights, *bias;   /* Weights [kh][kw][in c][out c]; depthwise [kh][kw][c]. */
    int kh, kw, stride, pad_top, pad_left;
    float *out; seg_dims out_dims;
} seg_conv_args;

void seg_conv(const seg_conv_args *a);
void seg_dwconv(const seg_conv_args *a);
void seg_tconv(const seg_conv_args *a);      /* Kernel == stride, no padding. */
void seg_resize(const float *in, seg_dims di, float *out, seg_dims dout);
void seg_mean(const float *in, seg_dims di, float *out);
void seg_relu(const float *in, float *out, size_t n);
void seg_hswish(const float *in, float *out, size_t n);
void seg_sigmoid(const float *in, float *out, size_t n);
void seg_mul_channels(const float *in, const float *scale, seg_dims d, float *out);
void seg_add(const float *a, const float *b, float *out, size_t n);
```

- [ ] **Step 1: Write the failing operator tests**

`linux/test_segment.c`:

```c
/* Person segmentation checks: operators, the .ysm loader, the full models
 * against MediaPipe's masks, and a benchmark (--bench). */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "youcam.h"

static int Checks;

#define EXPECT(cond, msg) do { \
    Checks++; \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); exit(1); } \
} while (0)

static int close_to(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_conv(void) {
    float in[16], ones[9], out[4], bias = 0;
    for (int i = 0; i < 16; i++) in[i] = i;   /* in[y][x] = y * 4 + x */
    for (int i = 0; i < 9; i++) ones[i] = 1;
    seg_conv(&(seg_conv_args){in, {4, 4, 1}, ones, &bias, 3, 3, 2, 0, 0, out, {2, 2, 1}});
    EXPECT(close_to(out[0], 45) && close_to(out[3], 50), "Strided SAME convolution pads only bottom/right");

    float img[9], full[9];
    for (int i = 0; i < 9; i++) img[i] = i + 1;
    seg_conv(&(seg_conv_args){img, {3, 3, 1}, ones, &bias, 3, 3, 1, 1, 1, full, {3, 3, 1}});
    EXPECT(close_to(full[4], 45) && close_to(full[0], 12), "Padding reads zeros outside the image");

    float px[2] = {1, 2}, w[4] = {1, 10, 100, 1000}, b2[2] = {0.5f, -0.5f}, o2[2];
    seg_conv(&(seg_conv_args){px, {1, 1, 2}, w, b2, 1, 1, 1, 0, 0, o2, {1, 1, 2}});
    EXPECT(close_to(o2[0], 201.5f) && close_to(o2[1], 2009.5f), "Weights are [kh][kw][in][out]");
}

static void test_dwconv(void) {
    float in[18], w[18], b[2] = {0, 0}, out[18];
    for (int i = 0; i < 18; i++) in[i] = 1;
    for (int i = 0; i < 9; i++) { w[2 * i] = 1; w[2 * i + 1] = 2; }
    seg_dwconv(&(seg_conv_args){in, {3, 3, 2}, w, b, 3, 3, 1, 1, 1, out, {3, 3, 2}});
    EXPECT(close_to(out[8], 9) && close_to(out[9], 18) && close_to(out[0], 4),
           "Depthwise convolution keeps channels apart");
}

static void test_tconv(void) {
    float in[2] = {1, 2}, w[8] = {1, 2, 3, 4, 5, 6, 7, 8}, b = 0.5f, out[4];
    seg_tconv(&(seg_conv_args){in, {1, 1, 2}, w, &b, 2, 2, 2, 0, 0, out, {2, 2, 1}});
    EXPECT(close_to(out[0], 5.5f) && close_to(out[1], 11.5f) && close_to(out[2], 17.5f) &&
           close_to(out[3], 23.5f), "Transposed convolution places each kernel tap");
}

static void test_small_ops(void) {
    float in[2] = {0, 1}, out[4];
    seg_resize(in, (seg_dims){1, 2, 1}, out, (seg_dims){1, 4, 1});
    EXPECT(close_to(out[0], 0) && close_to(out[1], 0.25f) && close_to(out[2], 0.75f) && close_to(out[3], 1),
           "Bilinear resize uses half-pixel centers, not aligned corners");
    float m[4] = {1, 2, 3, 6}, avg;
    seg_mean(m, (seg_dims){2, 2, 1}, &avg);
    EXPECT(close_to(avg, 3), "Mean averages over height and width");
    float x[5] = {-1, 1, -4, 4, 0}, y[5];
    seg_relu(x, y, 1);
    EXPECT(y[0] == 0, "ReLU clamps negatives");
    seg_hswish(x + 1, y, 3);
    EXPECT(close_to(y[0], 4.0f / 6) && y[1] == 0 && close_to(y[2], 4), "Hard swish is x * relu6(x + 3) / 6");
    seg_sigmoid(x + 4, y, 1);
    EXPECT(close_to(y[0], 0.5f), "Sigmoid of zero is one half");
    float v[2] = {2, 3}, s[2] = {10, 100}, p[2];
    seg_mul_channels(v, s, (seg_dims){1, 1, 2}, p);
    EXPECT(close_to(p[0], 20) && close_to(p[1], 300), "Multiply scales each channel");
    seg_add(v, s, p, 2);
    EXPECT(close_to(p[0], 12) && close_to(p[1], 103), "Add is element-wise");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    test_conv();
    test_dwconv();
    test_tconv();
    test_small_ops();
    printf("Passed %d segmentation checks.\n", Checks);
    return 0;
}
```

Makefile: add `segment.o` to `CORE`, a target, `test_segment.o` to the header dependency line, `-O3` for the engine, and the test to `test`/`clean`:

```make
CORE := geometry.o support.o prefs.o camera_layer.o center_stage.o picornt.o mutter_cast.o mic_level.o segment.o

test-segment: test_segment.o $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(APP) tests.o test_media.o test_segment.o make_icon.o: youcam.h

segment.o: CFLAGS += -O3

test: tests test-media test-segment
	./tests
	./test-segment
	./test-media
```

and add `test-segment` to the `rm -rf` line of `clean` (it is a file target, not `.PHONY`).

- [ ] **Step 2: Run, verify it fails**

Run: `cd linux && make test-segment`
Expected: link/compile errors, `seg_conv` undefined (no `segment.c` yet).

- [ ] **Step 3: Implement the operators**

`linux/segment.c`:

```c
/* Person segmentation: a tiny inference engine for MediaPipe Selfie
 * Segmentation, converted offline to .ysm by tools/convert_selfie.py.
 * Tensors are NHWC float32 with batch 1; one function per operator.
 * Convolutions keep the output channels innermost so the compiler can
 * vectorise them; HOT builds an AVX2/FMA clone picked at load time. */

#include <limits.h>
#include <math.h>
#include <string.h>
#include "youcam.h"

#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__)
#define HOT __attribute__((target_clones("arch=haswell", "default")))
#else
#define HOT
#endif

/* ponytail: one thread. Split rows across a GThreadPool here if the
 * benchmark misses its budget (Task 7). */
static void run_rows(void (*fn)(const seg_conv_args *, int, int), const seg_conv_args *a) {
    fn(a, 0, a->out_dims.h);
}

HOT static void conv_rows(const seg_conv_args *a, int y0, int y1) {
    int ic = a->in_dims.c, oc = a->out_dims.c;
    for (int oy = y0; oy < y1; oy++)
        for (int ox = 0; ox < a->out_dims.w; ox++) {
            float *restrict o = a->out + ((size_t)oy * a->out_dims.w + ox) * oc;
            memcpy(o, a->bias, oc * sizeof(float));
            for (int ky = 0; ky < a->kh; ky++) {
                int iy = oy * a->stride - a->pad_top + ky;
                if (iy < 0 || iy >= a->in_dims.h) continue;
                for (int kx = 0; kx < a->kw; kx++) {
                    int ix = ox * a->stride - a->pad_left + kx;
                    if (ix < 0 || ix >= a->in_dims.w) continue;
                    const float *px = a->in + ((size_t)iy * a->in_dims.w + ix) * ic;
                    const float *wk = a->weights + (size_t)(ky * a->kw + kx) * ic * oc;
                    for (int c = 0; c < ic; c++) {
                        float v = px[c];
                        const float *restrict wr = wk + (size_t)c * oc;
                        for (int k = 0; k < oc; k++) o[k] += v * wr[k];
                    }
                }
            }
        }
}

HOT static void dwconv_rows(const seg_conv_args *a, int y0, int y1) {
    int ch = a->in_dims.c;
    for (int oy = y0; oy < y1; oy++)
        for (int ox = 0; ox < a->out_dims.w; ox++) {
            float *restrict o = a->out + ((size_t)oy * a->out_dims.w + ox) * ch;
            memcpy(o, a->bias, ch * sizeof(float));
            for (int ky = 0; ky < a->kh; ky++) {
                int iy = oy * a->stride - a->pad_top + ky;
                if (iy < 0 || iy >= a->in_dims.h) continue;
                for (int kx = 0; kx < a->kw; kx++) {
                    int ix = ox * a->stride - a->pad_left + kx;
                    if (ix < 0 || ix >= a->in_dims.w) continue;
                    const float *restrict px = a->in + ((size_t)iy * a->in_dims.w + ix) * ch;
                    const float *restrict wk = a->weights + (size_t)(ky * a->kw + kx) * ch;
                    for (int c = 0; c < ch; c++) o[c] += px[c] * wk[c];
                }
            }
        }
}

HOT static void tconv_rows(const seg_conv_args *a, int y0, int y1) {
    int ic = a->in_dims.c, oc = a->out_dims.c, k = a->stride;
    for (int oy = y0; oy < y1; oy++)
        for (int ox = 0; ox < a->out_dims.w; ox++) {
            float *restrict o = a->out + ((size_t)oy * a->out_dims.w + ox) * oc;
            memcpy(o, a->bias, oc * sizeof(float));
            const float *px = a->in + ((size_t)(oy / k) * a->in_dims.w + ox / k) * ic;
            const float *wk = a->weights + (size_t)((oy % k) * k + ox % k) * ic * oc;
            for (int c = 0; c < ic; c++) {
                float v = px[c];
                const float *restrict wr = wk + (size_t)c * oc;
                for (int j = 0; j < oc; j++) o[j] += v * wr[j];
            }
        }
}

void seg_conv(const seg_conv_args *a) { run_rows(conv_rows, a); }
void seg_dwconv(const seg_conv_args *a) { run_rows(dwconv_rows, a); }
void seg_tconv(const seg_conv_args *a) { run_rows(tconv_rows, a); }

/* TFLite RESIZE_BILINEAR with half_pixel_centers and no align_corners. */
void seg_resize(const float *in, seg_dims di, float *out, seg_dims dout) {
    double sy = (double)di.h / dout.h, sx = (double)di.w / dout.w;
    for (int oy = 0; oy < dout.h; oy++) {
        double fy = fmax(0, (oy + 0.5) * sy - 0.5);
        int y0 = MIN((int)fy, di.h - 1), y1 = MIN(y0 + 1, di.h - 1);
        float dy = (float)(fy - y0);
        for (int ox = 0; ox < dout.w; ox++) {
            double fx = fmax(0, (ox + 0.5) * sx - 0.5);
            int x0 = MIN((int)fx, di.w - 1), x1 = MIN(x0 + 1, di.w - 1);
            float dx = (float)(fx - x0);
            const float *a = in + ((size_t)y0 * di.w + x0) * di.c, *b = in + ((size_t)y0 * di.w + x1) * di.c;
            const float *c = in + ((size_t)y1 * di.w + x0) * di.c, *d = in + ((size_t)y1 * di.w + x1) * di.c;
            float *o = out + ((size_t)oy * dout.w + ox) * dout.c;
            for (int k = 0; k < di.c; k++) {
                float top = a[k] + (b[k] - a[k]) * dx, bottom = c[k] + (d[k] - c[k]) * dx;
                o[k] = top + (bottom - top) * dy;
            }
        }
    }
}

void seg_mean(const float *in, seg_dims di, float *out) {
    size_t n = (size_t)di.h * di.w;
    for (int c = 0; c < di.c; c++) out[c] = 0;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < di.c; c++) out[c] += in[i * di.c + c];
    for (int c = 0; c < di.c; c++) out[c] /= n;
}

void seg_relu(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = in[i] > 0 ? in[i] : 0;
}

void seg_hswish(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = in[i] * fminf(fmaxf(in[i] + 3, 0), 6) / 6;
}

void seg_sigmoid(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = 1 / (1 + expf(-in[i]));
}

void seg_mul_channels(const float *in, const float *scale, seg_dims d, float *out) {
    size_t n = (size_t)d.h * d.w;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < d.c; c++) out[i * d.c + c] = in[i * d.c + c] * scale[c];
}

void seg_add(const float *a, const float *b, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = a[i] + b[i];
}
```

- [ ] **Step 4: Run, verify it passes**

Run: `cd linux && make test-segment && ./test-segment && make tests && ./tests`
Expected: `Passed 12 segmentation checks.` and the existing `Passed 244 logic checks.`; no compiler warnings.

- [ ] **Step 5: Commit**

```bash
git add segment.c test_segment.c youcam.h Makefile
git commit -m "feat: segmentation operators in plain C

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: `.ysm` loader with validation, `segment_run`, embedded models

**Files:**
- Modify: `linux/segment.c`, `linux/youcam.h`, `linux/Makefile`, `linux/test_segment.c`
- Create: `linux/background.c` (only the embedded-model accessor in this task)

**Interfaces:**
- Consumes: operators from Task 3; `.ysm` format from Task 2.
- Produces (in `youcam.h`):

```c
typedef struct segment_model segment_model;
segment_model *segment_load(const guint8 *data, size_t size, GError **err);
void segment_free(segment_model *m);
void segment_input_size(const segment_model *m, int *w, int *h);
/* rgb: packed 8-bit RGB at the input size; mask: person confidence 0...1, input size. */
void segment_run(segment_model *m, const guint8 *rgb, float *mask);

/* ---------------------------------------------------------- background.c --- */

typedef enum { SEG_LANDSCAPE, SEG_SQUARE, SEG_VARIANTS } seg_variant;
const guint8 *background_model_bytes(seg_variant v, size_t *size);
```

- [ ] **Step 1: Write the failing loader tests**

Add to `test_segment.c`, and call `test_loader();` from `main` after `test_small_ops();`:

```c
#define HEADER_TENSOR_COUNT 24   /* Offset of tensor_count in the .ysm header. */
#define HEADER_SIZE 40

static guint8 *model_copy(seg_variant v, size_t *size) {
    const guint8 *data = background_model_bytes(v, size);
    return g_memdup2(data, *size);
}

static int loads(const guint8 *data, size_t size) {
    GError *err = NULL;
    segment_model *m = segment_load(data, size, &err);
    int ok = m != NULL;
    EXPECT(ok || (err && strstr(err->message, "Invalid segmentation model")), "A rejected model explains why");
    segment_free(m);
    g_clear_error(&err);
    return ok;
}

static void test_loader(void) {
    for (int v = 0; v < SEG_VARIANTS; v++) {
        size_t size;
        guint8 *d = model_copy(v, &size);
        EXPECT(loads(d, size), "The embedded models load");
        EXPECT(!loads(d, size / 2), "A truncated model is rejected");
        EXPECT(!loads(d, 3), "A model shorter than its magic is rejected");
        d[0] = 'X';
        EXPECT(!loads(d, size), "A file that is not .ysm is rejected");
        d[0] = 'Y';
        guint32 tensors;
        memcpy(&tensors, d + HEADER_TENSOR_COUNT, 4);
        size_t first_layer = HEADER_SIZE + 12 * (size_t)tensors;
        guint32 saved, bad = 999999;
        memcpy(&saved, d + first_layer + 4, 4);              /* in0 */
        memcpy(d + first_layer + 4, &bad, 4);
        EXPECT(!loads(d, size), "A tensor index out of range is rejected");
        memcpy(d + first_layer + 4, &saved, 4);
        memcpy(&saved, d + first_layer + 36, 4);             /* weight_count */
        guint32 more = saved + 1;
        memcpy(d + first_layer + 36, &more, 4);
        EXPECT(!loads(d, size), "Weights that do not match the shapes are rejected");
        memcpy(d + first_layer + 36, &saved, 4);
        EXPECT(loads(d, size), "Restoring the bytes makes the model valid again");
        g_free(d);
    }
}
```

Makefile: add `background.o` to `CORE` and the embedding rule:

```make
CORE := geometry.o support.o prefs.o camera_layer.o center_stage.o picornt.o mutter_cast.o mic_level.o segment.o background.o

# Person segmentation: MediaPipe Selfie Segmentation (vendor/selfie, Apache 2.0), converted to .ysm.
selfie_%.h: vendor/selfie/selfie_%.ysm
	cd vendor/selfie && xxd -i selfie_$*.ysm > ../../$@

background.o: selfie_landscape.h selfie_square.h
```

and add `selfie_*.h` to the `rm -rf` line of `clean`.

- [ ] **Step 2: Run, verify it fails**

Run: `cd linux && make test-segment`
Expected: undefined `segment_load`, `background_model_bytes`.

- [ ] **Step 3: Implement the loader and `segment_run`**

Append to `segment.c`:

```c
/* ---------------------------------------------------------------- .ysm
 *
 * Little endian. Header: "YSM1", u32 in_h, in_w, f32 norm_scale,
 * norm_offset (input = byte * scale + offset), u32 invert, tensor_count,
 * layer_count, input, output. Then u32 h, w, c per tensor, then per layer
 * u32 op, in0, in1, out, kh, kw, stride, pad_top, pad_left, weight_count,
 * bias_count and the floats. The file is untrusted: every count, shape and
 * index is checked before use. */

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error ".ysm files are little endian"
#endif

#define YSM_MAX_TENSORS 1024
#define YSM_MAX_LAYERS 1024
#define YSM_MAX_DIM 4096
#define YSM_MAX_ELEMENTS (1 << 24)
#define YSM_MAX_KERNEL 15
#define YSM_LAYER_FIELDS 11

enum { OP_CONV = 1, OP_DWCONV, OP_RELU, OP_HSWISH, OP_SIGMOID, OP_MEAN, OP_MUL, OP_ADD, OP_RESIZE, OP_TCONV };

typedef struct {
    int op, in0, in1, out, kh, kw, stride, pad_top, pad_left;
    float *weights, *bias;   /* One allocation; bias points inside it. */
} seg_layer;

struct segment_model {
    int in_w, in_h, invert;
    float norm_scale, norm_offset;
    int tensor_count, layer_count, input, output;
    seg_dims *dims;
    float **act;
    seg_layer *layers;
};

typedef struct { const guint8 *p; size_t left; int ok; } reader;

static guint32 read_u32(reader *r) {
    guint32 v = 0;
    if (r->left < 4) { r->ok = 0; return 0; }
    memcpy(&v, r->p, 4);
    r->p += 4;
    r->left -= 4;
    return v;
}

static float read_f32(reader *r) {
    guint32 u = read_u32(r);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int field(guint32 v) { return v > INT_MAX ? -1 : (int)v; }   /* 0xFFFFFFFF (unused) -> -1 */

static int same_dims(seg_dims a, seg_dims b) { return a.h == b.h && a.w == b.w && a.c == b.c; }

static const char *check_layer(const segment_model *m, const seg_layer *l, guint32 wc, guint32 bc) {
    int binary = l->op == OP_MUL || l->op == OP_ADD, kernel = l->op == OP_CONV || l->op == OP_DWCONV || l->op == OP_TCONV;
    if (l->op < OP_CONV || l->op > OP_TCONV) return "unknown operator";
    if (l->in0 < 0 || l->in0 >= m->tensor_count || l->out < 0 || l->out >= m->tensor_count ||
        (binary && (l->in1 < 0 || l->in1 >= m->tensor_count)))
        return "tensor index out of range";
    if (l->out == l->in0 || (binary && l->out == l->in1) || l->out == m->input) return "a layer overwrites its input";
    if (kernel && (l->kh < 1 || l->kw < 1 || l->kh > YSM_MAX_KERNEL || l->kw > YSM_MAX_KERNEL ||
                   l->stride < 1 || l->stride > YSM_MAX_KERNEL || l->pad_top < 0 || l->pad_left < 0 ||
                   l->pad_top >= l->kh || l->pad_left >= l->kw))
        return "bad kernel";
    if (!kernel && (wc || bc)) return "unexpected weights";
    seg_dims i = m->dims[l->in0], o = m->dims[l->out];
    guint64 taps = (guint64)l->kh * l->kw;
    switch (l->op) {
    case OP_CONV:
        return wc == taps * i.c * o.c && bc == (guint32)o.c ? NULL : "convolution weights do not match";
    case OP_DWCONV:
        return i.c == o.c && wc == taps * i.c && bc == (guint32)o.c ? NULL : "depthwise weights do not match";
    case OP_TCONV:
        return l->kh == l->kw && l->kh == l->stride && !l->pad_top && !l->pad_left &&
               o.h == i.h * l->stride && o.w == i.w * l->stride && wc == taps * i.c * o.c &&
               bc == (guint32)o.c ? NULL : "transposed convolution does not match";
    case OP_MEAN:
        return o.h == 1 && o.w == 1 && o.c == i.c ? NULL : "mean shape";
    case OP_MUL: {
        seg_dims s = m->dims[l->in1];
        return s.h == 1 && s.w == 1 && s.c == i.c && same_dims(i, o) ? NULL : "multiply shape";
    }
    case OP_ADD:
        return same_dims(i, m->dims[l->in1]) && same_dims(i, o) ? NULL : "add shape";
    case OP_RESIZE:
        return i.c == o.c ? NULL : "resize channels";
    default:   /* RELU, HSWISH, SIGMOID */
        return same_dims(i, o) ? NULL : "activation shape";
    }
}

static const char *read_tensors(segment_model *m, reader *r, guint32 count) {
    m->dims = g_new0(seg_dims, count);
    m->act = g_new0(float *, count);
    m->tensor_count = count;
    for (guint32 t = 0; t < count; t++) {
        guint32 h = read_u32(r), w = read_u32(r), c = read_u32(r);
        if (!r->ok) return "truncated tensors";
        if (!h || !w || !c || h > YSM_MAX_DIM || w > YSM_MAX_DIM || c > YSM_MAX_DIM ||
            (guint64)h * w * c > YSM_MAX_ELEMENTS)
            return "bad tensor shape";
        m->dims[t] = (seg_dims){(int)h, (int)w, (int)c};
        m->act[t] = g_new0(float, (size_t)h * w * c);
    }
    return NULL;
}

static const char *read_layers(segment_model *m, reader *r, guint32 count) {
    m->layers = g_new0(seg_layer, count);
    for (guint32 i = 0; i < count; i++) {
        guint32 f[YSM_LAYER_FIELDS];
        for (int k = 0; k < YSM_LAYER_FIELDS; k++) f[k] = read_u32(r);
        if (!r->ok) return "truncated layer";
        seg_layer *l = &m->layers[i];
        m->layer_count = i + 1;
        *l = (seg_layer){field(f[0]), field(f[1]), field(f[2]), field(f[3]), field(f[4]), field(f[5]),
                         field(f[6]), field(f[7]), field(f[8]), NULL, NULL};
        const char *bad = check_layer(m, l, f[9], f[10]);
        if (bad) return bad;
        guint64 n = (guint64)f[9] + f[10];
        if (n * 4 > r->left) return "truncated weights";
        l->weights = g_new(float, n + 1);
        memcpy(l->weights, r->p, n * 4);
        r->p += n * 4;
        r->left -= n * 4;
        l->bias = l->weights + f[9];
    }
    return NULL;
}

static const char *read_model(segment_model *m, reader *r) {
    if (r->left < 4 || memcmp(r->p, "YSM1", 4) != 0) return "not a .ysm file";
    r->p += 4;
    r->left -= 4;
    guint32 in_h = read_u32(r), in_w = read_u32(r);
    m->norm_scale = read_f32(r);
    m->norm_offset = read_f32(r);
    guint32 invert = read_u32(r), tensors = read_u32(r), layers = read_u32(r);
    guint32 input = read_u32(r), output = read_u32(r);
    if (!r->ok) return "truncated header";
    if (tensors < 2 || tensors > YSM_MAX_TENSORS || layers < 1 || layers > YSM_MAX_LAYERS ||
        input >= tensors || output >= tensors || invert > 1 ||
        !isfinite(m->norm_scale) || !isfinite(m->norm_offset))
        return "bad header";
    m->in_h = field(in_h);
    m->in_w = field(in_w);
    m->invert = invert;
    m->input = input;
    m->output = output;
    const char *bad = read_tensors(m, r, tensors);
    if (bad) return bad;
    seg_dims in = m->dims[input], out = m->dims[output];
    if (in.h != m->in_h || in.w != m->in_w || in.c != 3 || out.h != m->in_h || out.w != m->in_w || out.c != 1)
        return "bad input or output shape";
    if ((bad = read_layers(m, r, layers))) return bad;
    return r->left ? "trailing bytes" : NULL;
}

segment_model *segment_load(const guint8 *data, size_t size, GError **err) {
    reader r = {data, size, 1};
    segment_model *m = g_new0(segment_model, 1);
    const char *problem = read_model(m, &r);
    if (!problem) return m;
    g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Invalid segmentation model: %s", problem);
    segment_free(m);
    return NULL;
}

void segment_free(segment_model *m) {
    if (!m) return;
    for (int i = 0; i < m->tensor_count; i++) g_free(m->act[i]);
    for (int i = 0; i < m->layer_count; i++) g_free(m->layers[i].weights);
    g_free(m->act);
    g_free(m->dims);
    g_free(m->layers);
    g_free(m);
}

void segment_input_size(const segment_model *m, int *w, int *h) {
    *w = m->in_w;
    *h = m->in_h;
}

static void run_layer(segment_model *m, const seg_layer *l) {
    seg_dims di = m->dims[l->in0], dout = m->dims[l->out];
    const float *in = m->act[l->in0];
    float *out = m->act[l->out];
    size_t n = (size_t)dout.h * dout.w * dout.c;
    seg_conv_args a = {in, di, l->weights, l->bias, l->kh, l->kw, l->stride, l->pad_top, l->pad_left, out, dout};
    switch (l->op) {
    case OP_CONV: seg_conv(&a); break;
    case OP_DWCONV: seg_dwconv(&a); break;
    case OP_TCONV: seg_tconv(&a); break;
    case OP_RELU: seg_relu(in, out, n); break;
    case OP_HSWISH: seg_hswish(in, out, n); break;
    case OP_SIGMOID: seg_sigmoid(in, out, n); break;
    case OP_MEAN: seg_mean(in, di, out); break;
    case OP_MUL: seg_mul_channels(in, m->act[l->in1], di, out); break;
    case OP_ADD: seg_add(in, m->act[l->in1], out, n); break;
    case OP_RESIZE: seg_resize(in, di, out, dout); break;
    }
}

void segment_run(segment_model *m, const guint8 *rgb, float *mask) {
    float *in = m->act[m->input];
    size_t n = (size_t)m->in_w * m->in_h;
    for (size_t i = 0; i < n * 3; i++) in[i] = rgb[i] * m->norm_scale + m->norm_offset;
    for (int i = 0; i < m->layer_count; i++) run_layer(m, &m->layers[i]);
    const float *out = m->act[m->output];
    for (size_t i = 0; i < n; i++) mask[i] = m->invert ? 1 - out[i] : out[i];
}
```

Note: `HEADER_SIZE` in the test is 4 + 2·4 + 2·4 + 5·4 = 40 and `tensor_count` sits at offset 4 + 8 + 8 + 4 = 24, matching `struct.pack('<4s2I2f5I', ...)` in the converter.

`linux/background.c` (first part):

```c
/* Background removal: the mode names, image helpers, the segmentation
 * worker thread and the layers camera_layer.c paints. */

#include <math.h>
#include <string.h>
#include "youcam.h"
#include "selfie_landscape.h"
#include "selfie_square.h"

const guint8 *background_model_bytes(seg_variant v, size_t *size) {
    *size = v == SEG_SQUARE ? selfie_square_ysm_len : selfie_landscape_ysm_len;
    return v == SEG_SQUARE ? selfie_square_ysm : selfie_landscape_ysm;
}
```

- [ ] **Step 4: Run, verify it passes**

Run: `cd linux && make test-segment && ./test-segment && make tests && ./tests && make youcam`
Expected: `Passed 29 segmentation checks.` (12 + 2 variants × 8 loader checks + the per-load "explains why" checks; exact count may differ, any count with no FAIL is fine), logic checks still pass, no warnings.

- [ ] **Step 5: Commit**

```bash
git add segment.c background.c test_segment.c youcam.h Makefile
git commit -m "feat: load and run .ysm segmentation models

The models are compiled in; every shape, index and count in the file is
validated before use.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Golden model test and edge cases

**Files:**
- Modify: `linux/test_segment.c`

**Interfaces:**
- Consumes: `segment_load`, `segment_run`, `segment_input_size`, `background_model_bytes`, fixtures from Task 2.

- [ ] **Step 1: Write the golden and edge-case tests**

Add to `test_segment.c` (with `#include <gdk-pixbuf/gdk-pixbuf.h>` — already reachable through `youcam.h`'s GTK include), and call `test_golden(); test_edge_cases();` from `main` after `test_loader();`:

```c
#define GOLDEN_MAX_DIFF 0.02
#define GOLDEN_MIN_IOU 0.98
#define MASK_QUANTUM (0.5 / 255)   /* The golden masks are 8-bit PNGs. */

static const char *const Fixtures[] = {"ride", "armstrong"};
static const char *const VariantNames[SEG_VARIANTS] = {"landscape", "square"};

/* Packed RGB of a PNG, or NULL. */
static guint8 *load_rgb(const char *path, int *w, int *h) {
    GdkPixbuf *p = gdk_pixbuf_new_from_file(path, NULL);
    if (!p) return NULL;
    *w = gdk_pixbuf_get_width(p);
    *h = gdk_pixbuf_get_height(p);
    int n = gdk_pixbuf_get_n_channels(p), stride = gdk_pixbuf_get_rowstride(p);
    const guint8 *px = gdk_pixbuf_read_pixels(p);
    guint8 *rgb = g_malloc((size_t)*w * *h * 3);
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++)
            memcpy(rgb + ((size_t)y * *w + x) * 3, px + (size_t)y * stride + x * n, 3);
    g_object_unref(p);
    return rgb;
}

static segment_model *load_variant(seg_variant v) {
    size_t size;
    const guint8 *data = background_model_bytes(v, &size);
    return segment_load(data, size, NULL);
}

static void test_golden(void) {
    for (int v = 0; v < SEG_VARIANTS; v++) {
        segment_model *m = load_variant(v);
        int mw, mh;
        segment_input_size(m, &mw, &mh);
        for (size_t f = 0; f < G_N_ELEMENTS(Fixtures); f++) {
            char *image = g_strdup_printf("testdata/selfie/%s-%s.png", Fixtures[f], VariantNames[v]);
            char *golden = g_strdup_printf("testdata/selfie/%s-%s-mask.png", Fixtures[f], VariantNames[v]);
            int w, h, gw, gh;
            guint8 *rgb = load_rgb(image, &w, &h), *expected = load_rgb(golden, &gw, &gh);
            EXPECT(rgb && expected && w == mw && h == mh && gw == mw && gh == mh, "Fixtures match the model size");
            float *mask = g_new(float, (size_t)w * h);
            segment_run(m, rgb, mask);
            double worst = 0;
            int both = 0, either = 0;
            for (int i = 0; i < w * h; i++) {
                double g = expected[i * 3] / 255.0;
                worst = fmax(worst, fabs(mask[i] - g));
                both += mask[i] >= 0.5 && g >= 0.5;
                either += mask[i] >= 0.5 || g >= 0.5;
            }
            double iou = either ? (double)both / either : 1;
            printf("  %s %s: max diff %.4f, IoU %.4f\n", Fixtures[f], VariantNames[v], worst, iou);
            EXPECT(worst < GOLDEN_MAX_DIFF + MASK_QUANTUM && iou > GOLDEN_MIN_IOU,
                   "The C model matches MediaPipe's mask");
            g_free(mask); g_free(rgb); g_free(expected); g_free(image); g_free(golden);
        }
        segment_free(m);
    }
}

static void test_edge_cases(void) {
    segment_model *m = load_variant(SEG_LANDSCAPE);
    int w, h;
    segment_input_size(m, &w, &h);
    guint8 *black = g_new0(guint8, (size_t)w * h * 3);
    float *mask = g_new(float, (size_t)w * h);
    segment_run(m, black, mask);
    int sane = 1, people = 0;
    for (int i = 0; i < w * h; i++) {
        sane &= isfinite(mask[i]) && mask[i] >= 0 && mask[i] <= 1;
        people += mask[i] >= 0.5;
    }
    EXPECT(sane, "A black frame gives a valid mask");
    EXPECT(people < w * h / 20, "A black frame has (almost) no person");
    g_free(black); g_free(mask);
    segment_free(m);
}
```

- [ ] **Step 2: Run**

Run: `cd linux && make test-segment && ./test-segment`
Expected: four `max diff ... IoU ...` lines and `Passed N segmentation checks.`
If a golden check fails while the numpy reference passed in Task 2: dump the first differing layer. Temporarily add a `printf` of `m->act[l->out][0..3]` per layer in `segment_run` and the same from `run_reference` (print `v[out].ravel()[:4]`), find the first layer that differs, fix that operator, remove the prints. Do not loosen the thresholds.

- [ ] **Step 3: Commit**

```bash
git add test_segment.c
git commit -m "test: the C segmentation matches MediaPipe on both models

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Benchmark, and threads only if needed

**Files:**
- Modify: `linux/test_segment.c`, `linux/Makefile`, maybe `linux/segment.c`

**Interfaces:**
- Produces: `make bench-segment` printing `<variant>: X.XX ms per inference`.

- [ ] **Step 1: Add the benchmark**

In `test_segment.c`:

```c
#define BENCH_RUNS 200

static void bench(void) {
    for (int v = 0; v < SEG_VARIANTS; v++) {
        segment_model *m = load_variant(v);
        int w, h;
        segment_input_size(m, &w, &h);
        guint8 *rgb = g_malloc((size_t)w * h * 3);
        for (int i = 0; i < w * h * 3; i++) rgb[i] = (guint8)(i * 31);
        float *mask = g_new(float, (size_t)w * h);
        segment_run(m, rgb, mask);   /* Warm up. */
        gint64 start = g_get_monotonic_time();
        for (int i = 0; i < BENCH_RUNS; i++) segment_run(m, rgb, mask);
        printf("%s: %.2f ms per inference\n", VariantNames[v],
               (g_get_monotonic_time() - start) / 1000.0 / BENCH_RUNS);
        g_free(rgb); g_free(mask);
        segment_free(m);
    }
}
```

and replace `main`'s first line with:

```c
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) { bench(); return 0; }
```

(drop the `(void)argc; (void)argv;` line). Makefile:

```make
bench-segment: test-segment
	./test-segment --bench
```

and add `bench-segment` to `.PHONY`.

- [ ] **Step 2: Measure**

Run: `cd linux && make bench-segment`
Record both numbers in the commit message.
- **If landscape ≤ 5 ms:** skip Step 3.
- **If above:** do Step 3.

- [ ] **Step 3 (only if over budget): split rows across threads**

Replace `run_rows` in `segment.c`:

```c
#define SEG_MIN_ROWS_PER_JOB 4   /* Smaller jobs cost more to hand out than they save. */
#define SEG_MAX_THREADS 8

typedef struct {
    void (*fn)(const seg_conv_args *, int, int);
    const seg_conv_args *a;
    int y0, y1;
} seg_job;

static GThreadPool *Pool;
static GMutex PoolLock;
static GCond PoolDone;
static int PoolPending;

static void pool_work(gpointer data, gpointer unused) {
    (void)unused;
    seg_job *j = data;
    j->fn(j->a, j->y0, j->y1);
    g_mutex_lock(&PoolLock);
    if (--PoolPending == 0) g_cond_signal(&PoolDone);
    g_mutex_unlock(&PoolLock);
}

/* Splits the output rows across a few threads and waits for them. */
static void run_rows(void (*fn)(const seg_conv_args *, int, int), const seg_conv_args *a) {
    int rows = a->out_dims.h;
    int jobs = MIN(MIN(SEG_MAX_THREADS, (int)g_get_num_processors()), rows / SEG_MIN_ROWS_PER_JOB);
    if (jobs < 2) { fn(a, 0, rows); return; }
    if (!Pool) Pool = g_thread_pool_new(pool_work, NULL, jobs - 1, TRUE, NULL);
    seg_job job[SEG_MAX_THREADS];
    g_mutex_lock(&PoolLock);
    PoolPending = jobs - 1;
    g_mutex_unlock(&PoolLock);
    for (int i = 0; i < jobs; i++) job[i] = (seg_job){fn, a, rows * i / jobs, rows * (i + 1) / jobs};
    for (int i = 1; i < jobs; i++) g_thread_pool_push(Pool, &job[i], NULL);
    fn(a, job[0].y0, job[0].y1);   /* This thread does the first share. */
    g_mutex_lock(&PoolLock);
    while (PoolPending) g_cond_wait(&PoolDone, &PoolLock);
    g_mutex_unlock(&PoolLock);
}
```

(`ponytail:` note: one pool shared by all callers; segment_run is only ever called from one thread at a time — the background worker and the tests.) Remove the old `ponytail: one thread` comment. Rerun `make test-segment && ./test-segment && make bench-segment`; tests must still pass. If still above 5 ms, record it: the spec allows a lower mask rate (the preview never stalls), and a GPU backend becomes a follow-up to discuss with the user — do not start one.

- [ ] **Step 4: Commit**

```bash
git add test_segment.c Makefile segment.c
git commit -m "perf: segmentation benchmark (landscape X.XX ms, square Y.YY ms)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Background modes, prefs and pure image helpers

**Files:**
- Modify: `linux/background.c`, `linux/youcam.h`, `linux/prefs.c`, `linux/tests.c`

**Interfaces:**
- Produces (in `youcam.h`, `background.c` section; `camera_frame` is the existing BGRx struct, reused for ARGB32 and blurred frames):

```c
typedef enum { BG_OFF, BG_TRANSPARENT, BG_BLUR, BG_IMAGE, BG_COLOR, BG_COUNT } background_mode;
extern const char *const BackgroundNames[BG_COUNT];    /* Prefs values: "off", "transparent", ... */
extern const char *const BackgroundTitles[BG_COUNT];   /* Menu labels: "Off", "Transparent", ... */
#define BACKGROUND_DEFAULT_COLOR "#333333"

background_mode background_mode_parse(const char *name);              /* Unknown or NULL -> BG_OFF. */
int background_color_parse(const char *text, double rgb[3]);          /* "#rrggbb"; 0 if invalid. */
void frame_to_model(const guint8 *bgrx, int w, int h, int stride, guint8 *rgb, int mw, int mh);
void mask_smooth(guint8 *mask, const float *raw, int n, int has_previous);
void mask_compose(const camera_frame *f, const guint8 *mask, int mw, int mh, camera_frame *out);
void blur_small(const camera_frame *f, camera_frame *out);
```

- Prefs fields (in `prefs`): `background_mode background_mode;`, `char *background_image;`, `char *background_color;`. Because `prefs` is defined before the `background.c` section in `youcam.h`, put the `background_mode` enum just above `typedef struct { ... } prefs;` in the prefs section, and the rest in a `background.c` section.

- [ ] **Step 1: Write the failing tests**

Add to `tests.c`, and call `test_background();` from `main` after `test_center_stage();`:

```c
static void test_background(void) {
    EXPECT(background_mode_parse("blur") == BG_BLUR && background_mode_parse("transparent") == BG_TRANSPARENT,
           "Background modes are read by name");
    EXPECT(background_mode_parse("banana") == BG_OFF && background_mode_parse(NULL) == BG_OFF,
           "An unknown background mode is Off");
    double rgb[3];
    EXPECT(background_color_parse("#ff8000", rgb) && rgb[0] == 1 && near(rgb[1], 128 / 255.0) && rgb[2] == 0,
           "Colors are #rrggbb");
    EXPECT(!background_color_parse("red", rgb) && !background_color_parse("#12345", rgb) &&
           !background_color_parse(NULL, rgb), "Invalid colors are rejected");

    guint8 bgrx[4 * 2 * 4];   /* 4x2: left half red, right half blue. */
    for (int i = 0; i < 8; i++) {
        guint8 *p = bgrx + i * 4;
        int left = i % 4 < 2;
        p[0] = left ? 0 : 255; p[1] = 0; p[2] = left ? 255 : 0; p[3] = 255;
    }
    guint8 rgb2[6];
    frame_to_model(bgrx, 4, 2, 16, rgb2, 2, 1);
    EXPECT(rgb2[0] == 255 && rgb2[2] == 0 && rgb2[3] == 0 && rgb2[5] == 255,
           "Frames shrink to packed RGB by averaging");

    float raw[2] = {1, 0};
    guint8 mask[2];
    mask_smooth(mask, raw, 2, 0);
    EXPECT(mask[0] == 255 && mask[1] == 0, "The first mask is taken as is");
    float flipped[2] = {0, 1};
    mask_smooth(mask, flipped, 2, 1);
    EXPECT(mask[0] > 0 && mask[0] < 255 && mask[1] > 0 && mask[1] < 255, "Later masks are smoothed over time");

    guint8 white[4 * 4];
    memset(white, 255, sizeof(white));
    camera_frame f = {4, 1, 16, white}, person = {0, 0, 0, NULL};
    guint8 half[2] = {255, 0};
    mask_compose(&f, half, 2, 1, &person);
    EXPECT(person.width == 4 && person.data[3] == 255 && person.data[15] == 0, "The mask becomes alpha");
    EXPECT(person.data[7] == 191 && person.data[11] == 64 && person.data[4] == 191,
           "Alpha is interpolated and colors premultiplied");
    g_free(person.data);

    guint8 gray[64 * 32 * 4];
    memset(gray, 128, sizeof(gray));
    camera_frame g = {64, 32, 256, gray}, blurred = {0, 0, 0, NULL};
    blur_small(&g, &blurred);
    EXPECT(blurred.width == 4 && blurred.height == 2 && blurred.data[0] == 128 && blurred.data[5] == 128,
           "A uniform frame stays uniform when blurred");
    g_free(blurred.data);
}
```

Extend `test_prefs` (before `prefs_save();`): `Prefs.background_mode = BG_BLUR; prefs_set_string(&Prefs.background_color, "#102030");` and after the reload:

```c
    EXPECT(Prefs.background_mode == BG_BLUR && strcmp(Prefs.background_color, "#102030") == 0,
           "The background choice is remembered");
```

and in the defaults check right after the first `prefs_load();`:

```c
    EXPECT(Prefs.background_mode == BG_OFF, "Background removal is off by default");
```

- [ ] **Step 2: Run, verify it fails**

Run: `cd linux && make tests`
Expected: undefined `background_mode_parse` etc.

- [ ] **Step 3: Implement**

Append to `background.c`:

```c
const char *const BackgroundNames[BG_COUNT] = {"off", "transparent", "blur", "image", "color"};
const char *const BackgroundTitles[BG_COUNT] = {"Off", "Transparent", "Blur", "Image", "Color"};

#define MASK_SMOOTHING 0.6   /* Weight of the newest mask: lower is steadier, higher follows faster. */
#define BLUR_FACTOR 16       /* The blurred background is computed this many times smaller... */
#define BLUR_RADIUS 2        /* ...with a box blur of this radius, then scaled up smoothly. */

background_mode background_mode_parse(const char *name) {
    for (int i = 0; name && i < BG_COUNT; i++)
        if (strcmp(name, BackgroundNames[i]) == 0) return i;
    return BG_OFF;
}

int background_color_parse(const char *text, double rgb[3]) {
    unsigned v;
    char end;
    if (!text || strlen(text) != 7 || sscanf(text, "#%6x%c", &v, &end) != 1) return 0;
    for (int i = 0; i < 3; i++) rgb[i] = ((v >> (16 - 8 * i)) & 0xff) / 255.0;
    return 1;
}

static void frame_resize(camera_frame *f, int w, int h) {
    if (f->width == w && f->height == h && f->data) return;
    f->data = g_realloc(f->data, (size_t)w * h * 4);
    f->width = w;
    f->height = h;
    f->stride = w * 4;
}

/* Box-average shrink of a BGRx frame, to packed RGB (rgb = 1) or BGRx. */
static void shrink(const guint8 *bgrx, int w, int h, int stride, guint8 *out, int ow, int oh, int rgb) {
    int bpp = rgb ? 3 : 4;
    for (int oy = 0; oy < oh; oy++) {
        int y0 = oy * h / oh, y1 = MAX(y0 + 1, (oy + 1) * h / oh);
        for (int ox = 0; ox < ow; ox++) {
            int x0 = ox * w / ow, x1 = MAX(x0 + 1, (ox + 1) * w / ow);
            unsigned sum[3] = {0, 0, 0}, n = (unsigned)((y1 - y0) * (x1 - x0));
            for (int y = y0; y < y1; y++) {
                const guint8 *p = bgrx + (size_t)y * stride + (size_t)x0 * 4;
                for (int x = x0; x < x1; x++, p += 4) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; }
            }
            guint8 *o = out + ((size_t)oy * ow + ox) * bpp;
            for (int c = 0; c < 3; c++) o[rgb ? 2 - c : c] = (guint8)((sum[c] + n / 2) / n);
            if (!rgb) o[3] = 255;
        }
    }
}

void frame_to_model(const guint8 *bgrx, int w, int h, int stride, guint8 *rgb, int mw, int mh) {
    shrink(bgrx, w, h, stride, rgb, mw, mh, 1);
}

void mask_smooth(guint8 *mask, const float *raw, int n, int has_previous) {
    for (int i = 0; i < n; i++) {
        double v = fmin(fmax(raw[i], 0), 1) * 255;
        if (has_previous) v = MASK_SMOOTHING * v + (1 - MASK_SMOOTHING) * mask[i];
        mask[i] = (guint8)lround(v);
    }
}

/* The camera frame with the mask (bilinear, half-pixel centers) as alpha:
 * premultiplied ARGB32, the person surface painted over the backdrop.
 * ponytail: per-pixel float math, ~2-4 ms at 720p; precompute the x taps
 * per row if it ever shows in a profile. */
void mask_compose(const camera_frame *f, const guint8 *mask, int mw, int mh, camera_frame *out) {
    frame_resize(out, f->width, f->height);
    for (int y = 0; y < f->height; y++) {
        double my = fmax(0, (y + 0.5) * mh / f->height - 0.5);
        int y0 = MIN((int)my, mh - 1), y1 = MIN(y0 + 1, mh - 1);
        double fy = my - y0;
        const guint8 *p = f->data + (size_t)y * f->stride, *r0 = mask + (size_t)y0 * mw, *r1 = mask + (size_t)y1 * mw;
        guint8 *o = out->data + (size_t)y * out->stride;
        for (int x = 0; x < f->width; x++, p += 4, o += 4) {
            double mx = fmax(0, (x + 0.5) * mw / f->width - 0.5);
            int x0 = MIN((int)mx, mw - 1), x1 = MIN(x0 + 1, mw - 1);
            double fx = mx - x0;
            double top = r0[x0] + (r0[x1] - r0[x0]) * fx, bottom = r1[x0] + (r1[x1] - r1[x0]) * fx;
            unsigned a = (unsigned)lround(top + (bottom - top) * fy);
            for (int c = 0; c < 3; c++) o[c] = (guint8)((p[c] * a + 127) / 255);
            o[3] = (guint8)a;
        }
    }
}

static void box_pass(const guint8 *in, guint8 *out, int w, int h, int dx, int dy) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                unsigned sum = 0;
                for (int k = -BLUR_RADIUS; k <= BLUR_RADIUS; k++) {
                    int sx = CLAMP(x + k * dx, 0, w - 1), sy = CLAMP(y + k * dy, 0, h - 1);
                    sum += in[((size_t)sy * w + sx) * 4 + c];
                }
                out[((size_t)y * w + x) * 4 + c] = (guint8)(sum / (2 * BLUR_RADIUS + 1));
            }
}

/* A small, blurred copy of the frame (BGRx), painted scaled up behind the person. */
void blur_small(const camera_frame *f, camera_frame *out) {
    int w = MAX(1, f->width / BLUR_FACTOR), h = MAX(1, f->height / BLUR_FACTOR);
    frame_resize(out, w, h);
    guint8 *tmp = g_malloc((size_t)w * h * 4);
    shrink(f->data, f->width, f->height, f->stride, out->data, w, h, 0);
    memcpy(tmp, out->data, (size_t)w * h * 4);   /* Keeps the 255 alpha bytes. */
    box_pass(out->data, tmp, w, h, 1, 0);
    box_pass(tmp, out->data, w, h, 0, 1);
    g_free(tmp);
}
```

`prefs.c`, in `prefs_load` after the `noise_reduction` line:

```c
    char *background = g_key_file_get_string(kf, "youcam", "backgroundMode", NULL);
    Prefs.background_mode = background_mode_parse(background);
    g_free(background);
    Prefs.background_image = g_key_file_get_string(kf, "youcam", "backgroundImage", NULL);
    Prefs.background_color = g_key_file_get_string(kf, "youcam", "backgroundColor", NULL);
```

and in `prefs_save` after the `centerStage` line:

```c
    g_key_file_set_string(kf, "youcam", "backgroundMode", BackgroundNames[Prefs.background_mode]);
    set_string(kf, "youcam", "backgroundImage", Prefs.background_image);
    set_string(kf, "youcam", "backgroundColor", Prefs.background_color);
```

(`prefs_load` already starts from zeroed or previous fields; `g_key_file_get_string` returns a fresh copy, and the existing code does not free the previous `camera`/`audio` strings either — follow the file's convention.)

- [ ] **Step 4: Run, verify it passes**

Run: `cd linux && make tests && ./tests && make youcam`
Expected: `Passed 2xx logic checks.` with no FAIL; no warnings.

- [ ] **Step 5: Commit**

```bash
git add background.c youcam.h prefs.c tests.c
git commit -m "feat: background modes, preferences and mask compositing helpers

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Painting the backdrop and the person

**Files:**
- Modify: `linux/camera_layer.c`, `linux/youcam.h`, `linux/overlay.c`, `linux/app.c` (the recording-layer call), `linux/tests.c`

**Interfaces:**
- Produces (in `youcam.h`, `background.c` section, *above* the `camera_layer.c` section so the type is known there — move the `background.c` section up if needed):

```c
/* What camera_layer.c paints instead of the plain frame. */
typedef struct {
    background_mode mode;     /* Never BG_OFF. */
    camera_frame person;      /* Premultiplied ARGB32, alpha = person. */
    camera_frame blurred;     /* BG_BLUR: small BGRx frame. */
    GdkPixbuf *image;         /* BG_IMAGE. */
    double rgb[3];            /* BG_COLOR. */
} background_layer;
```

- Changed signatures (every caller updated in this task; `NULL` = plain picture):

```c
void camera_paint_frame(cairo_t *cr, const camera_frame *f, double w, double h, int mirrored,
                        stage_transform t, const background_layer *bg);
void camera_layer_draw(cairo_t *cr, camera_layer l, int cx, int cy, const camera_frame *f,
                       int mirrored, stage_transform t, const background_layer *bg);
```

- [ ] **Step 1: Write the failing render tests**

Add to `tests.c`, called from `main` after `test_background();`:

```c
/* Renders a 64x36 frame (16:9, so the canvas is the frame) whose left half is
 * the person, and returns the ARGB32 pixel at x of row 18. */
static guint32 render_pixel(background_mode mode, int x) {
    static guint8 white[64 * 36 * 4];
    memset(white, 255, sizeof(white));
    camera_frame f = {64, 36, 256, white};
    guint8 mask[2] = {255, 0};
    background_layer bg = {mode, {0, 0, 0, NULL}, {0, 0, 0, NULL}, NULL, {1, 0, 0}};
    mask_compose(&f, mask, 2, 1, &bg.person);
    guint8 blue[4 * 2 * 4];
    for (int i = 0; i < 8; i++) { blue[i * 4] = 255; blue[i * 4 + 1] = 0; blue[i * 4 + 2] = 0; blue[i * 4 + 3] = 255; }
    bg.blurred = (camera_frame){4, 2, 16, g_memdup2(blue, sizeof(blue))};
    bg.image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 8, 8);
    gdk_pixbuf_fill(bg.image, 0x00ff00ff);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 36);
    cairo_t *cr = cairo_create(s);
    camera_paint_frame(cr, &f, 64, 36, 0, STAGE_IDENTITY, &bg);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    guint32 px = *(guint32 *)(cairo_image_surface_get_data(s) + 18 * cairo_image_surface_get_stride(s) + x * 4);
    cairo_surface_destroy(s);
    g_free(bg.person.data);
    g_free(bg.blurred.data);
    g_object_unref(bg.image);
    return px;
}

static void test_background_paint(void) {
    EXPECT(render_pixel(BG_COLOR, 2) == 0xffffffff, "The person stays in front");
    EXPECT(render_pixel(BG_COLOR, 61) == 0xffff0000, "Color fills the background");
    EXPECT(render_pixel(BG_IMAGE, 61) == 0xff00ff00, "The image fills the background");
    EXPECT(render_pixel(BG_BLUR, 61) == 0xff0000ff, "The blurred frame fills the background");
    EXPECT(render_pixel(BG_TRANSPARENT, 61) >> 24 == 0, "Transparent leaves the background empty");
}
```

Update the existing `camera_layer_draw(cr, cl, x, y, &f, mirrored, STAGE_IDENTITY);` call in `tests.c` to pass `NULL` as the last argument.

- [ ] **Step 2: Run, verify it fails**

Run: `cd linux && make tests`
Expected: compile errors: too many arguments to `camera_paint_frame`, `background_layer` unknown.

- [ ] **Step 3: Implement in `camera_layer.c`**

Replace `camera_paint_frame` with:

```c
/* Paints w x h pixels of any cairo format with the camera transform:
 * aspect fill into the 16:9 canvas centred on the mask, then the Center
 * Stage shift and zoom around the mask centre. A smaller copy of the
 * frame (the blurred background) lands on exactly the same place. */
static void paint_pixels(cairo_t *cr, const camera_frame *f, cairo_format_t format, double w, double h,
                         int mirrored, stage_transform t) {
    double cw, ch;
    stage_canvas(w, h, &cw, &ch);
    cairo_save(cr);
    cairo_translate(cr, w / 2 + t.x * w, h / 2 + t.y * h);
    cairo_scale(cr, t.zoom, t.zoom);
    cairo_translate(cr, -cw / 2, -ch / 2);
    if (mirrored) {
        cairo_translate(cr, cw, 0);
        cairo_scale(cr, -1, 1);
    }
    double scale = fmax(cw / f->width, ch / f->height);
    cairo_translate(cr, (cw - f->width * scale) / 2, (ch - f->height * scale) / 2);
    cairo_scale(cr, scale, scale);
    cairo_surface_t *image = cairo_image_surface_create_for_data(f->data, format, f->width, f->height, f->stride);
    cairo_set_source_surface(cr, image, 0, 0);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);   /* No dark seams when scaled up. */
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_rectangle(cr, 0, 0, f->width, f->height);
    cairo_fill(cr);
    cairo_surface_destroy(image);
    cairo_restore(cr);
}

/* A background image fills the mask (cover), still and unmirrored. */
static void paint_cover(cairo_t *cr, GdkPixbuf *image, double w, double h) {
    double iw = gdk_pixbuf_get_width(image), ih = gdk_pixbuf_get_height(image);
    double scale = fmax(w / iw, h / ih);
    cairo_save(cr);
    cairo_translate(cr, (w - iw * scale) / 2, (h - ih * scale) / 2);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, image, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

/* Paints a camera frame into a w x h mask, shared with the preview so both
 * show the same picture. With a background layer, paints the backdrop
 * first and the person, cut out by the mask, on top. */
void camera_paint_frame(cairo_t *cr, const camera_frame *f, double w, double h, int mirrored,
                        stage_transform t, const background_layer *bg) {
    if (!bg) {
        paint_pixels(cr, f, CAIRO_FORMAT_RGB24, w, h, mirrored, t);
        return;
    }
    if (bg->mode == BG_COLOR) {
        cairo_set_source_rgb(cr, bg->rgb[0], bg->rgb[1], bg->rgb[2]);
        cairo_paint(cr);
    } else if (bg->mode == BG_IMAGE) {
        paint_cover(cr, bg->image, w, h);
    } else if (bg->mode == BG_BLUR) {
        paint_pixels(cr, &bg->blurred, CAIRO_FORMAT_RGB24, w, h, mirrored, t);
    }
    paint_pixels(cr, &bg->person, CAIRO_FORMAT_ARGB32, w, h, mirrored, t);
}
```

Note: the old version used `cairo_paint` with the default `EXTEND_NONE`; painting a filled rectangle of the image with `EXTEND_PAD` gives the same result inside the frame and avoids a half-transparent edge on the upscaled blur. Run the existing tests to confirm nothing else changed.

In `camera_layer_draw`, add the `bg` parameter; Transparent drops the rounded shape:

```c
void camera_layer_draw(cairo_t *cr, camera_layer l, int cx, int cy, const camera_frame *f,
                       int mirrored, stage_transform t, const background_layer *bg) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, -cx, -cy);
    rect r = {l.frame.x - HALO_PX, l.frame.y - HALO_PX, l.frame.w + 2 * HALO_PX, l.frame.h + 2 * HALO_PX};
    if (bg && bg->mode == BG_TRANSPARENT) {
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);   /* The person is the shape. */
    } else {
        double rad = fmin(l.radius + HALO_PX, fmin(r.w, r.h) / 2);
        cairo_new_sub_path(cr);
        cairo_arc(cr, r.x + r.w - rad, r.y + rad, rad, -M_PI / 2, 0);
        cairo_arc(cr, r.x + r.w - rad, r.y + r.h - rad, rad, 0, M_PI / 2);
        cairo_arc(cr, r.x + rad, r.y + r.h - rad, rad, M_PI / 2, M_PI);
        cairo_arc(cr, r.x + rad, r.y + rad, rad, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);
    }
    cairo_clip(cr);
    cairo_translate(cr, r.x, r.y);
    camera_paint_frame(cr, f, r.w, r.h, mirrored, t, bg);
    cairo_restore(cr);
}
```

- [ ] **Step 4: Update the callers to pass `NULL` for now**

- `overlay.c` `on_draw`: `camera_paint_frame(cr, f, w, h, Prefs.mirrored, center_stage_now(...), NULL);`
- `app.c` (recording layer, around line 503): `camera_layer_draw(cr, l, x, y, f, Prefs.mirrored, center_stage_now(...), NULL);`

(Task 9 replaces both `NULL`s with `background_now(f)`.)

- [ ] **Step 5: Run, verify it passes**

Run: `cd linux && make tests && ./tests && make youcam test-media && ./test-media`
Expected: all logic checks pass (the five new ones included), media test ok, no warnings.

- [ ] **Step 6: Commit**

```bash
git add camera_layer.c youcam.h overlay.c app.c tests.c
git commit -m "feat: paint a backdrop behind the segmented person

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Worker thread and live wiring

**Files:**
- Modify: `linux/background.c`, `linux/youcam.h`, `linux/camera.c`, `linux/overlay.c`, `linux/app.c`

**Interfaces:**
- Consumes: everything above.
- Produces (in `youcam.h`, `background.c` section):

```c
int background_available(void);                      /* Both models load (checked once). */
void background_apply_prefs(void);                   /* Starts or stops the worker for Prefs.background_mode. */
void background_feed(const guint8 *bgrx, int w, int h, int stride);   /* Camera thread. */
const background_layer *background_now(const camera_frame *f);      /* Main thread; NULL = plain picture. */
```

- [ ] **Step 1: Implement the worker and `background_now`**

Append to `background.c`:

```c
#define SQUARE_BELOW_ASPECT 1.5   /* Cameras narrower than this use the 256x256 model. */
#define SEG_MAX_INPUT (256 * 256 * 3)

typedef struct { guint8 *mask; int w, h; } published_mask;

static struct {
    GMutex lock;
    GCond wake;
    GThread *thread;
    int quit, pending;            /* pending: input holds a frame the worker is running. */
    seg_variant variant;
    guint8 input[SEG_MAX_INPUT];
    int available;                /* -1 not checked yet. */
    int model_w[SEG_VARIANTS], model_h[SEG_VARIANTS];
    /* Main thread only. */
    guint8 *mask;
    int mask_w, mask_h;
    background_layer layer;
    char *image_path;             /* The path B.layer.image was loaded from. */
} B = {.available = -1};

int background_available(void) {
    if (B.available >= 0) return B.available;
    B.available = 1;
    for (int v = 0; v < SEG_VARIANTS; v++) {
        size_t size;
        const guint8 *data = background_model_bytes(v, &size);
        GError *err = NULL;
        segment_model *m = segment_load(data, size, &err);
        if (!m) {
            g_printerr("youcam: background removal unavailable: %s\n", err->message);
            g_clear_error(&err);
            B.available = 0;
            return 0;
        }
        segment_input_size(m, &B.model_w[v], &B.model_h[v]);
        if ((size_t)B.model_w[v] * B.model_h[v] * 3 > SEG_MAX_INPUT) B.available = 0;
        segment_free(m);
    }
    return B.available;
}

static gboolean deliver_mask(gpointer data) {
    published_mask *p = data;
    if (Prefs.background_mode == BG_OFF) {
        g_free(p->mask);
    } else {
        g_free(B.mask);
        B.mask = p->mask;
        B.mask_w = p->w;
        B.mask_h = p->h;
    }
    g_free(p);
    return G_SOURCE_REMOVE;
}

/* Runs the model on the latest frame the camera thread left in B.input. */
static gpointer worker(gpointer unused) {
    (void)unused;
    segment_model *model = NULL;
    int loaded = -1, has_previous = 0, n = 0;
    float *raw = NULL;
    guint8 *smoothed = NULL;
    g_mutex_lock(&B.lock);
    for (;;) {
        while (!B.pending && !B.quit) g_cond_wait(&B.wake, &B.lock);
        if (B.quit) break;
        seg_variant v = B.variant;
        g_mutex_unlock(&B.lock);
        if ((int)v != loaded) {   /* First frame, or the camera changed shape. */
            size_t size;
            const guint8 *data = background_model_bytes(v, &size);
            segment_free(model);
            model = segment_load(data, size, NULL);   /* Checked by background_available(). */
            loaded = v;
            has_previous = 0;
            n = B.model_w[v] * B.model_h[v];
            raw = g_renew(float, raw, n);
            smoothed = g_renew(guint8, smoothed, n);
        }
        segment_run(model, B.input, raw);
        mask_smooth(smoothed, raw, n, has_previous);
        has_previous = 1;
        published_mask *p = g_new(published_mask, 1);
        *p = (published_mask){g_memdup2(smoothed, n), B.model_w[v], B.model_h[v]};
        g_idle_add(deliver_mask, p);
        g_mutex_lock(&B.lock);
        B.pending = 0;
    }
    g_mutex_unlock(&B.lock);
    segment_free(model);
    g_free(raw);
    g_free(smoothed);
    return NULL;
}

/* Camera thread, every frame. Never queues: while the worker is busy the
 * frame is skipped, so a slow model lowers the mask rate, not the preview's. */
void background_feed(const guint8 *bgrx, int w, int h, int stride) {
    if (Prefs.background_mode == BG_OFF) return;
    g_mutex_lock(&B.lock);
    if (B.thread && !B.pending) {
        seg_variant v = (double)w / h < SQUARE_BELOW_ASPECT ? SEG_SQUARE : SEG_LANDSCAPE;
        frame_to_model(bgrx, w, h, stride, B.input, B.model_w[v], B.model_h[v]);
        B.variant = v;
        B.pending = 1;
        g_cond_signal(&B.wake);
    }
    g_mutex_unlock(&B.lock);
}

void background_apply_prefs(void) {
    int wanted = Prefs.background_mode != BG_OFF && background_available();
    if (wanted && !B.thread) {
        B.quit = 0;
        B.pending = 0;
        B.thread = g_thread_new("youcam-segment", worker, NULL);
    } else if (!wanted && B.thread) {
        g_mutex_lock(&B.lock);
        B.quit = 1;
        g_cond_signal(&B.wake);
        g_mutex_unlock(&B.lock);
        g_thread_join(B.thread);
        B.thread = NULL;
        g_clear_pointer(&B.mask, g_free);
    }
}

/* The chosen image, loaded once per path; NULL (and one log line) if it cannot be read. */
static GdkPixbuf *background_image(void) {
    const char *path = Prefs.background_image;
    if (g_strcmp0(path, B.image_path) == 0) return B.layer.image;
    g_clear_object(&B.layer.image);
    prefs_set_string(&B.image_path, path);
    GError *err = NULL;
    if (path) B.layer.image = gdk_pixbuf_new_from_file(path, &err);
    if (!B.layer.image) {
        g_printerr("youcam: background image unavailable, using Blur: %s\n", err ? err->message : "none chosen");
        g_clear_error(&err);
    }
    return B.layer.image;
}

/* ponytail: recomposed on every call (preview and recording layer), ~2-4 ms
 * each at 720p; cache per camera frame if it ever matters. */
const background_layer *background_now(const camera_frame *f) {
    background_mode mode = Prefs.background_mode;
    if (mode == BG_OFF || !B.mask || !f) return NULL;
    background_layer *l = &B.layer;
    mask_compose(f, B.mask, B.mask_w, B.mask_h, &l->person);
    l->mode = mode;
    if (mode == BG_IMAGE && !background_image()) l->mode = BG_BLUR;
    if (l->mode == BG_BLUR) blur_small(f, &l->blurred);
    if (l->mode == BG_COLOR && !background_color_parse(Prefs.background_color, l->rgb))
        background_color_parse(BACKGROUND_DEFAULT_COLOR, l->rgb);
    return l;
}
```

Notes for the implementer:
- `B.input` is written by the camera thread only while `pending == 0` and read by the worker only while `pending == 1`, so it needs no copy.
- `Prefs.background_mode` is read on the camera thread without a lock, exactly as `center_stage_feed` reads `Prefs.center_stage`.
- `background_image()` logs once per path change, not per frame, because `B.image_path` is updated even when loading fails.

- [ ] **Step 2: Wire it in**

`camera.c` `new_sample`, right after the `center_stage_feed` line:

```c
        if (map.size >= size) background_feed(map.data, w, h, w * 4);
```

`overlay.c` `on_draw` — Transparent drops the shape and the dark fill:

```c
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    (void)data;
    double w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const char *title, *detail;
    int message = camera_message(&title, &detail);
    const camera_frame *f = message ? NULL : camera_current_frame();
    const background_layer *bg = f ? background_now(f) : NULL;
    if (!bg || bg->mode != BG_TRANSPARENT) {   /* Transparent: the person is the shape. */
        shape_path(cr, w, h);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, 0.09, 0.09, 0.09);
        cairo_paint(cr);
    }
    if (message) {
        draw_placeholder(cr, w, h, title, detail);
        return TRUE;
    }
    if (!f) return TRUE;
    camera_paint_frame(cr, f, w, h, Prefs.mirrored, center_stage_now(w, h, Prefs.shape, Prefs.mirrored), bg);
    return TRUE;
}
```

`app.c` recording layer: replace the `NULL` from Task 8 with `background_now(f)`.

`app.c` startup: right after `center_stage_init(overlay_animate);` add `background_apply_prefs();`.

- [ ] **Step 3: Build and run all checks**

Run: `cd linux && make && make test && make smoke-test`
Expected: everything passes, no warnings.

- [ ] **Step 4: Live check with the real camera**

```bash
cd linux
cp ~/.config/youcam/prefs.ini "$SCRATCH/prefs-backup.ini"
sed -i '/^backgroundMode=/d' ~/.config/youcam/prefs.ini
sed -i 's/^\[youcam\]$/[youcam]\nbackgroundMode=blur/' ~/.config/youcam/prefs.ini
timeout 15 ./youcam & sleep 8; top -b -n 1 -p $(pgrep -x youcam) | tail -2; wait
cp "$SCRATCH/prefs-backup.ini" ~/.config/youcam/prefs.ini
```

Expected: no crash or warnings on stderr; CPU of `youcam` reported (record it). The visual result is checked by the user in Task 11.

- [ ] **Step 5: Commit**

```bash
git add background.c youcam.h camera.c overlay.c app.c
git commit -m "feat: segment the camera on a worker thread

The camera thread hands over the latest frame (never queues), the worker
smooths the mask over time, and the preview and the recording layer
composite with the same mask.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Background menu, help and README

**Files:**
- Modify: `linux/app.c`, `linux/README.md`

**Interfaces:**
- Consumes: `BackgroundNames`, `BackgroundTitles`, `background_mode_parse`, `background_available`, `background_apply_prefs`, existing `add_choice`, `add_item`, `add_separator`, `add_submenu`, `prefs_save`, `prefs_set_string`, `overlay_queue_draw`, `app_show_message`.

- [ ] **Step 1: Add the menu**

In `app.c`, after `build_transparency_menu`:

```c
/* ------------------------------------------------------------- background */

static void set_background(background_mode mode) {
    Prefs.background_mode = mode;
    prefs_save();
    background_apply_prefs();
    overlay_queue_draw();
}

static void choose_background_image(void) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Choose Background Image", NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images (PNG, JPEG)");
    gtk_file_filter_add_mime_type(filter, "image/png");
    gtk_file_filter_add_mime_type(filter, "image/jpeg");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        prefs_set_string(&Prefs.background_image, path);
        g_free(path);
        set_background(BG_IMAGE);
    }
    gtk_widget_destroy(dialog);
}

static void choose_background_color(void) {
    GtkWidget *dialog = gtk_color_chooser_dialog_new("Choose Background Color", NULL);
    double rgb[3];
    if (background_color_parse(Prefs.background_color, rgb))
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog), &(GdkRGBA){rgb[0], rgb[1], rgb[2], 1});
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog), &c);
        char text[8];
        snprintf(text, sizeof(text), "#%02x%02x%02x", (int)lround(c.red * 255), (int)lround(c.green * 255),
                 (int)lround(c.blue * 255));
        prefs_set_string(&Prefs.background_color, text);
        set_background(BG_COLOR);
    }
    gtk_widget_destroy(dialog);
}

static void select_background(const char *name) {
    background_mode mode = background_mode_parse(name);
    if (mode == BG_IMAGE && !Prefs.background_image) choose_background_image();   /* Needs an image first. */
    else set_background(mode);
}

static void build_background_menu(GtkWidget *menu) {
    if (!background_available()) {
        add_item(menu, "Background removal unavailable", NULL, 0);
        return;
    }
    for (int m = 0; m < BG_COUNT; m++)
        add_choice(menu, BackgroundTitles[m], BackgroundNames[m], Prefs.background_mode == (background_mode)m, 1,
                   select_background);
    add_separator(menu);
    add_item(menu, "Choose Image…", choose_background_image, 1);
    add_item(menu, "Choose Color…", choose_background_color, 1);
}
```

In `app_build_menu`, right after `build_transparency_menu(add_submenu(menu, "Transparency", 1));`:

```c
    build_background_menu(add_submenu(menu, "Background", 1));
```

(`app_build_menu` ends with `gtk_widget_show_all(menu)`, which shows submenu items too, as for Transparency.)

Help text: in `HelpText`, after the Center Stage sentence, add:

```c
    "Background (Linux only) removes what is behind you: Transparent shows only you, Blur softens the "
    "room, Image and Color replace it. It runs on your computer and applies to recordings too. "
```

(adjust the surrounding string concatenation so sentences stay separated by `\n\n` as the existing paragraphs are.)

- [ ] **Step 2: README**

In `README.md` under **Use**, after the Center Stage bullet:

```markdown
- **Background** (Linux only) separates you from the room: **Transparent**
  shows only you (the shape no longer applies), **Blur** softens the
  background, **Image** puts a picture of your choice behind you, **Color** a
  solid color. **Choose Image…** and **Choose Color…** pick them. It runs on
  the CPU with no extra software, in the preview and in recordings, and is
  off by default. `make bench-segment` shows how fast your CPU runs it.
```

After the pico credit paragraph:

```markdown
Background removal uses MediaPipe Selfie Segmentation by Google (Apache
License 2.0), converted by `tools/convert_selfie.py` and vendored in
`vendor/selfie/` with its license and provenance. Test portraits in
`testdata/selfie/` are public domain (NASA).
```

- [ ] **Step 3: Build and test**

Run: `cd linux && make && make test && make smoke-test`
Expected: all pass, no warnings.

- [ ] **Step 4: Commit**

```bash
git add app.c README.md
git commit -m "feat: Background menu with Transparent, Blur, Image and Color

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: Real recording check and hand-off to the user

**Files:**
- None changed unless a bug is found (then: failing test first, fix, commit `fix: ...`).

- [ ] **Step 1: Record with Transparent and with Blur**

```bash
cd linux
cp ~/.config/youcam/prefs.ini "$SCRATCH/prefs-backup.ini"
for mode in transparent blur; do
  sed -i '/^backgroundMode=/d' ~/.config/youcam/prefs.ini
  sed -i "s/^\[youcam\]\$/[youcam]\nbackgroundMode=$mode/" ~/.config/youcam/prefs.ini
  ./youcam --record-test 6
  f=$(ls -t ~/Scrivania/YouCam-Recording-* | head -1)
  ffprobe -v error -show_entries stream=codec_type,width,height -of csv=p=0 "$f"
  ffmpeg -v error -y -ss 4 -i "$f" -frames:v 1 "$SCRATCH/bg-$mode.png"
  mv "$f" "$SCRATCH/"
done
cp "$SCRATCH/prefs-backup.ini" ~/.config/youcam/prefs.ini
```

Expected: each movie has a video stream at screen size and an audio stream; the extracted frames (open them with the Read tool) show the camera window with the chosen mode applied. Movies are moved to the scratchpad so the Desktop stays clean.

- [ ] **Step 2: Repeat Step 1 with "Apply Transparency to Saved Video" off and transparency 40%** (so the recording composites the camera layer itself — set `cameraPreviewTransparency=0.4` and `recordCameraTransparency=false` the same way). Expected: the recorded camera shows the same mode, opaque.

- [ ] **Step 3: Final checks**

Run: `cd linux && make clean && make && make test && make smoke-test && make bench-segment`
Expected: clean build, all checks pass, benchmark numbers printed.

- [ ] **Step 4: Ask the user to verify by eye** (the spec's manual checks): edges, hair, hands, low light, moving; each mode; Choose Image / Choose Color; Transparent with each shape; Center Stage and Mirror on. Report the benchmark numbers and the CPU usage measured in Task 9.

---

## Self-review

- **Spec coverage:** model and license gate (T1); converter, numpy reference, golden fixtures (T2); engine, validation, operators (T3–T4); golden and edge-case tests (T5); `target_clones`, thread pool if needed, `bench-segment` (T3, T6); worker, frame skipping, EMA, variant by aspect, unavailable state (T9); compositing modes, Transparent ignores shape, transparency stacks (window opacity and layer alpha are applied after this drawing, unchanged), mirror and Center Stage share the transform (T8); menu, file and color choosers, prefs with fallbacks (T7, T10); README credit and help (T10); record-test and manual checks (T11). Corrupt `.ysm` → feature unavailable: `background_available()` + disabled menu item (T9, T10).
- **Deviations** are listed at the top (embedding, mask resolution, normalization in the header, restricted op set).
- **Types:** `seg_dims`, `seg_conv_args`, `seg_variant`, `background_mode`, `background_layer`, `camera_frame` are used with the same fields in every task; `camera_paint_frame`/`camera_layer_draw` gain `const background_layer *bg` in T8 and every caller is updated there.
