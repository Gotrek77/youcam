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
  level meter. System/app audio is not recorded.
- **Transparency** optionally makes the camera see-through so you can see
  what it covers. Adjust the slider from 0% to 80%, or choose **Reset
  Transparency to 0%** to turn it off. Check **Apply Transparency to Saved
  Video** to record the same transparency; leave it unchecked to keep the
  recorded camera opaque. Both choices are remembered, and changes apply
  immediately, including during recording.
- **Show Recording Panel** opens optional controls. **Hide Camera** lets you
  record just the screen; launch YouCam again to bring the camera back.

Global shortcuts: **Control–Alt–Super–R** starts/stops; **Control–Alt–Super–P**
pauses/resumes (GNOME asks to confirm them once).

Run checks with `make test` and `make smoke-test`.

## Differences from macOS

- Windows run through XWayland, the only way on GNOME Wayland to keep the
  camera above other windows and remember where it was.
- The menu bar icon is a StatusNotifierItem: GNOME shows it only with the
  AppIndicator extension.
- macOS excludes YouCam's panel and guides from recordings. The portal cannot
  exclude windows: with a selected area they stay outside the crop, but a
  whole-screen recording shows the panel if it is open.
- Menu keyboard equivalents (Command-H, Command-1…) are not available: the
  camera never takes keyboard focus. The global shortcuts work everywhere.
