# YouCam for Linux

A C port of the macOS app: a floating camera without a window frame, with
selectable cameras, shapes and microphone input, plus a screen recorder.
Built on GTK 3 and GStreamer, which GNOME desktops already ship.

## Build and run

Requires GTK 3, GStreamer 1.20+ with the good, ugly (x264) and libav (AAC)
plugins, the PipeWire GStreamer plugin and `xdg-desktop-portal` (GNOME,
KDE or another desktop with a ScreenCast portal).

```sh
make
make install      # ~/.local: binary, icons, desktop entry
youcam
```

The desktop entry gives the portal an app id, which the global shortcuts
need. `make uninstall` removes everything `make install` added.

## Use

- **Right-click the camera** (or Control-click) for controls. Drag to move
  it; scroll to resize. Choose a circle, square, or rectangle.
- **Recording Area → Select Area…** selects a region. Drag its corner dots to
  adjust it before recording, or choose **Whole Screen**. With several
  displays, **Whole Screen — <display>** records a specific one. Your area
  choice is remembered.
- **Start Recording**, **Pause / Resume**, and **Stop Recording** control
  capture. The first time, the system asks which screen to share; the choice
  is remembered. Movies save automatically to
  `~/Desktop/YouCam-Recording-<date>-<time>.mov`.
- **Audio Input** selects a microphone and provides a gain slider and live
  level meter. System/app audio is not recorded. **Noise Reduction** (Linux
  only: Off, Low, Moderate, High, Very High; default Moderate) removes hum and
  room noise from the recorded microphone with WebRTC noise suppression.
  **Automatic Microphone Level** (Linux only, on by default) adjusts the
  system level of the microphone (via `wpctl`) only while YouCam uses it:
  lower on clipping or loud peaks, higher for quiet speech. The original level
  is put back when YouCam stops using the microphone, quits, or restarts
  after a crash. Other apps see the changed level meanwhile.
- **Recording Format** (Linux only) picks the container: MOV (default, like
  macOS), MP4, MKV or AVI, all H.264 + AAC. MKV stays playable even if
  YouCam is killed mid-recording.
- Video is encoded on the GPU when possible (Intel VA-API, then NVIDIA NVENC,
  then x264 in software). `YOUCAM_ENCODER=va|nvenc|x264` forces one.
- **Transparency** optionally makes the camera see-through so you can see
  what it covers. Adjust the slider from 0% to 80%, or choose **Reset
  Transparency to 0%** to turn it off. Check **Apply Transparency to Saved
  Video** to record the same transparency; leave it unchecked to keep the
  recorded camera opaque. Both choices are remembered, and changes apply
  immediately, including during recording.
- **Center Stage (Follow Face)** gently pans and zooms the camera to keep
  your face centred, in the preview and in recordings. It is on by default
  and remembered.
- **Show Recording Panel** opens optional controls. **Hide Camera** lets you
  record just the screen; launch YouCam again to bring the camera back.

Global shortcuts: **Control–Alt–Super–R** starts/stops; **Control–Alt–Super–P**
pauses/resumes (GNOME asks to confirm them once).

Run checks with `make test` and `make smoke-test`. `./youcam --record-test 6` makes a real 6-second
recording through the portal (after one recording has saved the monitor choice, no dialog appears) and
quits; inspect the movie on the Desktop with `ffprobe`.

Face detection uses [pico](https://github.com/nenadmarkus/pico) by Nenad
Markus (MIT license), vendored in `vendor/pico/` with its license.

## Differences from macOS

- Windows run through XWayland, the only way on GNOME Wayland to keep the
  camera above other windows and remember where it was.
- The menu bar icon is a StatusNotifierItem: GNOME shows it only with the
  AppIndicator extension.
- macOS excludes YouCam's panel and guides from recordings. On GNOME this
  needs the small **YouCam Capture Exclusion** Shell extension, which `make
  install` copies: log out and in once, then run `gnome-extensions enable
  youcam-capture-exclusion@youcam.app`. While it is active YouCam records
  through Mutter's screen cast instead of the portal (no screen-sharing
  dialog) and the panel and guides stay on screen but out of the movie.
  Without it, a whole-screen recording shows the panel if it is open.
  `YOUCAM_CAPTURE=portal` or `=mutter` forces either path.
- Center Stage finds faces with pico instead of Apple's Vision framework, and
  never uses a camera's own Center Stage (macOS's Continuity Camera path).
- Menu keyboard equivalents (Command-H, Command-1…) are not available: the
  camera never takes keyboard focus. The global shortcuts work everywhere.
