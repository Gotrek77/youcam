# YouCam for Linux

Port of the macOS app: a frameless floating camera and screen recorder for GNOME (Wayland or X11).
Python + GTK 3 + GStreamer, no pip packages.

## Requirements

`python-gobject`, `gtk3`, GStreamer 1.20+ with `gst-plugins-good`, `gst-plugins-ugly` (x264), `gst-libav` (AAC),
`gst-plugin-pipewire`, and `xdg-desktop-portal-gnome`.

## Run

```sh
make install   # ~/.local: launcher, app files, desktop entry (needed for global shortcuts)
youcam         # or: make run
```

## Differences from macOS

- The camera window runs through XWayland so it can stay on top and remember its position.
- Screen capture goes through the desktop portal: the first recording asks which monitor to share,
  later ones reuse it (**Recording Area → Choose Another Monitor Next Time** resets that).
- A selected area is stored as a fraction of the monitor and cropped from the captured stream.
- Audio Input starts on **No Audio**; pick a microphone once and it is remembered.
- Movies are `.mp4` (H.264 + AAC) on the Desktop.
- Global shortcuts Ctrl+Alt+Super+R / P use the GlobalShortcuts portal; GNOME asks to confirm them once.
- Not ported: menu-bar icon (GNOME has no tray), recording panel, area corner handles, hiding guides from recordings.
  Hide Camera hides the window; launch YouCam again to bring it back.

`make uninstall` removes what `make install` added. `make test` runs the logic self-check and a synthetic recording with pause/resume.
