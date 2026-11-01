# Choose Camera Transparency After Recording — Design

Date: 2026-11-01. Branch `transparency-later` (from `linux-port`). Linux port only (`linux/`); the Swift sources stay untouched.

## Goal

Let the user record first and pick the camera transparency of the saved video afterwards, with a live preview, instead of fixing it before recording. With the YouCam Capture Exclusion extension active any transparency from 0 to 80% is possible; without it, any transparency from 0 to the one used in the preview while recording.

## User-facing behaviour

### Option

The Transparency submenu's check item "Apply Transparency to Saved Video" becomes a radio group **Saved Video**:

- **Same as Preview** — today's checked state.
- **Opaque** — today's unchecked state.
- **Choose After Recording** — new. Disabled, with an explanation, when the lossless layer codec is unavailable.

Preference `recordCameraTransparency` (boolean) is replaced by `savedTransparency` = `preview` | `opaque` | `later`. Migration on load: `true` → `preview`, `false` → `opaque`, missing → `opaque` (today's default). Unknown values → `opaque`.

### Stop dialog

When a `later` recording stops, a normal GTK window opens:

- **Preview:** one frame from the middle of the recording, scaled to about 640 px wide. Two stills are extracted once when the window opens: A = screen track alone, B = camera layer over the screen. The slider blends them with cairo (B over A with opacity α, see below), so moving it is instant.
- **Slider "Camera Transparency"** from 0 to the project's maximum, starting at the preview transparency used while recording (clamped to the maximum). Without the extension a line below reads: "Up to N%: enable the YouCam Capture Exclusion extension to choose up to 80%."
- **Save** exports and closes. **Later** closes and keeps the project. **Discard** asks for confirmation, then deletes the project.
- While saving, the controls are disabled and a progress bar shows position / duration. The UI stays responsive. On success the usual "Saved …" notification appears and the project is deleted.

The same window opens from the menu item **Adjust Last Recording Transparency…**, enabled when at least one project exists; it opens the most recent one. Starting a new recording while the window is open is allowed and does not touch the open project.

### Files

- Projects live in `~/.local/share/youcam/pending/` as `YouCam-Recording-<date>-<time>.youcam.mkv` (the name the final movie will have, plus `.youcam.mkv`).
- After a successful Save the project is deleted: only the final movie remains.
- Later or closing the window keeps the project. A project from a crashed or killed recording stays readable (MKV) and can be exported.
- Several projects may accumulate; the menu always opens the most recent. No list UI.

## Recording

### Camera layer

`app.c`'s `layer_tick` already draws the camera layer (shape, mirror, Center Stage, background removal) with `camera_layer_draw` into an ARGB frame pushed to the writer's `appsrc`. In `later` mode:

- It always draws at transparency 0 (opaque camera) and runs for the whole recording, not only when `camera_layer_needed()` says so today.
- `writer.c` does not composite the layer: it encodes it as a separate track with a lossless codec that keeps alpha (FFV1 or QTRLE; the plan chooses by measuring size and CPU on a real recording).

### Screen

- **Extension active** (`mutter_cast_available()`): during a `later` recording the camera window's title starts with `youcam-hidden`, which the extension already honours through `notify::title`; the camera stays visible on screen but is absent from the screen track. The title is restored at stop. Recorded `T_A = 1`.
- **Extension inactive** (portal): nothing changes on screen; the screen track contains the translucent preview at the current preview transparency p. Recorded `T_A = p`.

`T_A` is the transparency at which the camera appears in the screen track.

### Project file

One MKV per recording:

| Track | Codec | Content |
|---|---|---|
| 0 | H.264 (VA → NVENC → x264, as today) | screen, cropped to the recording area as today |
| 1 | lossless with alpha | camera layer at screen size, transparent outside the camera |
| 2 | AAC | microphone, with noise reduction and automatic level as today (absent with No Audio) |

MKV tags: `YOUCAM_T_A` (T_A), `YOUCAM_PREVIEW_TRANSPARENCY` (p at start), `YOUCAM_CONTAINER` (the chosen Recording Format extension).

## Export

### Formula

For a chosen final transparency t, the layer is composited over the screen track with opacity

    α = 1 − t / T_A

which yields camera transparency exactly t (camera opacity = α + (1 − α)(1 − T_A)). The slider range is `0 … min(T_A, 0.8)`. With `T_A = 0` (preview was opaque, no extension) the range is just 0 and α = 1.

### Pipeline (`export.c`)

    filesrc ! matroskademux
      screen: h264parse ! decoder ! compositor.sink_0
      layer:  lossless decoder ! compositor.sink_1 (alpha = α)
      compositor ! encoder ! muxer of YOUCAM_CONTAINER
      audio:  aacparse ! same muxer (copied, not re-encoded)

- Encoder choice, bitrate and the MOV/MP4/MKV/AVI muxers are shared with `writer.c` by extracting them into common functions, not duplicated.
- Runs as fast as decode + encode allow (not real time), on the GLib main loop; progress = position / duration polled every 200 ms.
- Output goes to the Desktop as `<name>.<ext>.part`, renamed to `<name>.<ext>` on success; if that name exists, " (2)", " (3)" … is appended before the extension.

## Errors

- Lossless codec missing at start-up: **Choose After Recording** disabled with an explanation.
- A `later` recording that cannot start its layer branch: the user is told and the recording proceeds as **Opaque** — a recording is never lost because of this feature.
- Export failure (GStreamer error, disk full): the `.part` is deleted, the project kept, a message gives the cause, the user can retry.
- Unreadable project: the window says so and offers only Discard.
- Extension toggled during a recording: `T_A` reflects the state at Start; logged, accepted.
- Start-up cleans stale `.part` files only inside `pending/`, never on the Desktop.

## Testing

- **Logic (`tests.c`):** α(t, T_A) and slider limits with and without the extension; `savedTransparency` migration and unknown values; output naming with " (2)".
- **Media (`test_media.c`, synthetic screen and camera):** a `later` recording produces an MKV with the three tracks and the tags; exporting at t = 0 makes the camera-centre pixel match the opaque camera, at t = T_A match the screen, at a middle value match the expected mix within H.264 tolerance; audio present with the same duration; export to each container.
- **Real check:** one recording with the extension active and one without; the final movies checked by eye on a crop of the camera rectangle only, files deleted afterwards.

## Out of scope

Changing shape, position, size or background after recording; a list of several projects; a full video player in the dialog; the macOS app.
