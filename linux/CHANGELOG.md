# Changelog — YouCam for Linux

## Unreleased

First Linux version: a C port (GTK 3 + GStreamer) of the macOS app, kept in
`linux/` next to the untouched Swift sources.

### Ported from macOS

- Floating camera without a window frame: circle, square or rectangle;
  drag to move, scroll to resize, position and size remembered.
- Camera, microphone and shape selection from the right-click menu and the
  tray icon (StatusNotifierItem).
- Screen recorder: whole screen, a chosen display, or a selected area with
  draggable corner dots; Start, Pause / Resume, Stop; recording panel.
- Microphone gain slider and live level meter with clipping warning.
- Camera transparency (upstream PR #2): 0–80% slider, optionally applied to
  the saved video.
- Center Stage face tracking (upstream PR #1): pans and zooms to keep the
  face centred, in the preview and in recordings, using vendored pico.
- Global shortcuts Control–Alt–Super–R (start/stop) and
  Control–Alt–Super–P (pause/resume) through the GlobalShortcuts portal.
- Movies saved to the Desktop as `YouCam-Recording-<date>-<time>`.

### Linux additions

- **Recording Format**: MOV (default), MP4, MKV or AVI, all H.264 + AAC.
- **GPU encoding**: Intel VA-API, then NVIDIA NVENC, then x264;
  `YOUCAM_ENCODER=va|nvenc|x264` forces one.
- **Noise Reduction**: Off, Low, Moderate (default), High, Very High, with
  WebRTC noise suppression on the recorded microphone.
- **Automatic Microphone Level** (on by default): adjusts the system level
  of the microphone while YouCam uses it (6 dB lower at once on clipping,
  lower for peaks above −6 dBFS, higher for quiet speech) and puts the
  original level back on stop, quit, or the next start after a crash.
- **Capture exclusion on GNOME**: the YouCam Capture Exclusion Shell
  extension keeps the panel and area guides on screen but out of the
  recording, as macOS does. With it active YouCam records through Mutter's
  screen cast; without it, through the ScreenCast portal.
  `YOUCAM_CAPTURE=portal|mutter` forces either path.
- **Background** (Off, Transparent, Blur, Image, Color): CPU segmentation
  with MediaPipe Selfie Segmentation converted to plain C, no new runtime
  dependency; applies to the preview and to recordings.
- `make install` / `make uninstall` (to `~/.local`), `make test`,
  `make smoke-test`, and `--record-test N` for a real N-second recording.

### Fixes since the first build

- Recordings came out 16x16 with no visible picture: the movie size is now
  pinned to the recorded screen area.
- Menu sliders (transparency, gain) could not be dragged.
- Black frames at the start of a recording.
- The panel showed in the first frame of a recording with capture exclusion.
- Microphone clipping with a loud voice (automatic level).

### Known differences from macOS

- Windows run through XWayland on GNOME Wayland (to stay on top and keep
  their position).
- The tray icon needs the AppIndicator extension on GNOME.
- Capture exclusion needs the Shell extension, enabled once after a
  re-login.
- No menu keyboard equivalents (Command-H, Command-1…): the camera never
  takes keyboard focus.
