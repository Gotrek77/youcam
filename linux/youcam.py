#!/usr/bin/env python3
"""YouCam for Linux: a frameless floating camera and screen recorder."""
import json
import math
import os
import sys

# XWayland: GNOME Wayland does not let apps keep a window on top or place it.
# Screen capture still goes through the portal, so it records Wayland apps too.
os.environ.setdefault("GDK_BACKEND", "x11")

import gi  # noqa: E402

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
import cairo  # noqa: E402
from gi.repository import Gdk, Gio, GLib, Gtk  # noqa: E402

import logic  # noqa: E402
import media  # noqa: E402
import portal  # noqa: E402
from region import RegionPicker  # noqa: E402

CONFIG_PATH = os.path.join(GLib.get_user_config_dir(), "youcam", "settings.json")
DEFAULTS = {"shape": "circle", "width": logic.DEFAULT_WIDTH, "x": None, "y": None, "mirror": True,
            "camera": None, "microphone": None, "gain_db": 0.0, "region": None, "region_name": None,
            "restore_token": None}
SCROLL_STEP = 1.15
CORNER_RADIUS = 0.08
SHORTCUTS = [("toggle-recording", "Start or stop recording", "CTRL+ALT+LOGO+r"),
             ("toggle-pause", "Pause or resume recording", "CTRL+ALT+LOGO+p")]
HELP = ("Drag the camera to move it. Scroll to resize. Right-click the camera for controls.\n\n"
        "Recording Area → Select Area… lets you drag a rectangle; Whole Screen records the full monitor. "
        "The first recording asks which monitor to share; the choice is remembered.\n\n"
        "Movies are saved to your Desktop. Ctrl+Alt+Super+R starts/stops, Ctrl+Alt+Super+P pauses/resumes.\n\n"
        "Audio Input selects the microphone and offers a gain slider and live meter. "
        "Hide Camera lets you record just the screen.")


def load_settings():
    settings = dict(DEFAULTS)
    try:
        with open(CONFIG_PATH) as file:
            saved = json.load(file)
        settings.update({k: v for k, v in saved.items() if k in DEFAULTS})
    except FileNotFoundError:
        pass
    except (OSError, ValueError) as error:
        print(f"youcam: ignoring unreadable settings: {error}")
    if settings["shape"] not in logic.SHAPES:
        settings["shape"] = "circle"
    if not logic.valid_fractions(settings["region"]):
        settings["region"] = None
    return settings


def save_settings(settings):
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    tmp = CONFIG_PATH + ".tmp"
    with open(tmp, "w") as file:
        json.dump(settings, file, indent=2)
    os.replace(tmp, CONFIG_PATH)


def recordings_dir():
    desktop = GLib.get_user_special_dir(GLib.UserDirectory.DIRECTORY_DESKTOP)
    return desktop if desktop and os.path.isdir(desktop) else GLib.get_home_dir()


class YouCam(Gtk.Application):
    def __init__(self):
        super().__init__(application_id=portal.APP_ID)
        self.window = None

    def do_activate(self):
        if self.window:
            return self.window.reveal()
        portal.register_app()
        self.window = CameraWindow(self)
        if "--smoke-test" in sys.argv:
            GLib.timeout_add_seconds(2, self.quit)


class CameraWindow(Gtk.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="YouCam")
        self.settings = load_settings()
        self.camera_state = "Camera starting…"
        self.starting = False
        self.last_path = None
        self.level_db = -60.0
        self.level_bar = None
        self.shortcuts_available = None
        self.picker = None
        self.screencast_session = None
        self.quitting = False

        self.set_decorated(False)
        self.set_keep_above(True)
        self.set_skip_taskbar_hint(True)
        self.set_skip_pager_hint(True)
        self.stick()
        self.set_app_paintable(True)
        visual = self.get_screen().get_rgba_visual()
        if visual:
            self.set_visual(visual)
        self.add_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.SCROLL_MASK)
        self.connect("draw", self._draw)
        self.connect("button-press-event", self._press)
        self.connect("scroll-event", self._scroll)
        self.connect("configure-event", self._moved)
        self.connect("delete-event", lambda *_: (self.get_application().quit(), True)[1])

        self.camera = media.Camera(on_frame=self.queue_draw, on_state=self._camera_state)
        self.meter = media.AudioMeter(on_level=self._level)
        self.recorder = media.Recorder(on_finished=self._recording_finished,
                                       on_error=self._recording_failed, on_level=self._level)
        self.devices = media.Devices(on_change=self._devices_changed)
        self.shortcuts = portal.GlobalShortcuts(SHORTCUTS, self._shortcut, self._shortcuts_status)

        self._place(self.settings["width"], keep_position=self.settings["x"] is not None)
        self.show_all()
        self._start_camera()

    # Geometry -----------------------------------------------------------------

    def _workarea(self, x=None, y=None):
        display = Gdk.Display.get_default()
        if x is None:
            monitor = display.get_monitor_at_window(self.get_window()) if self.get_window() \
                else display.get_primary_monitor() or display.get_monitor(0)
        else:
            monitor = display.get_monitor_at_point(x, y)
        area = monitor.get_workarea()
        return area.x, area.y, area.width, area.height

    def _place(self, width, keep_position=True, reset=False):
        shape = self.settings["shape"]
        if reset or not keep_position:
            frame = logic.initial(width, shape, self._workarea())
        else:
            x, y = (self.settings["x"], self.settings["y"]) if self.get_window() is None else self.get_position()
            w, h = self.get_size()
            frame = logic.resized((x, y, w, h), width, shape, self._workarea(x + w // 2, y + h // 2))
        x, y, w, h = frame
        self.set_size_request(w, h)
        self.resize(w, h)
        self.move(x, y)
        self.settings.update(x=x, y=y, width=w)
        self._save()

    def _moved(self, _widget, event):
        if (event.x, event.y) != (self.settings["x"], self.settings["y"]):
            self.settings.update(x=event.x, y=event.y)
            self._save()
        return False

    def _save(self):
        try:
            save_settings(self.settings)
        except OSError as error:
            print(f"youcam: could not save settings: {error}")

    # Drawing and input --------------------------------------------------------

    def _shape_path(self, cr, w, h):
        if self.settings["shape"] == "circle":
            cr.arc(w / 2, h / 2, min(w, h) / 2, 0, 2 * math.pi)
            return
        r = min(w, h) * CORNER_RADIUS
        cr.new_sub_path()
        cr.arc(w - r, r, r, -math.pi / 2, 0)
        cr.arc(w - r, h - r, r, 0, math.pi / 2)
        cr.arc(r, h - r, r, math.pi / 2, math.pi)
        cr.arc(r, r, r, math.pi, 3 * math.pi / 2)
        cr.close_path()

    def _draw(self, _widget, cr):
        w, h = self.get_allocated_width(), self.get_allocated_height()
        cr.set_operator(cairo.OPERATOR_SOURCE)
        cr.set_source_rgba(0, 0, 0, 0)
        cr.paint()
        cr.set_operator(cairo.OPERATOR_OVER)
        self._shape_path(cr, w, h)
        cr.clip()
        frame = self.camera.frame
        if not frame:
            cr.set_source_rgb(0.12, 0.12, 0.12)
            cr.paint()
            cr.set_source_rgb(0.8, 0.8, 0.8)
            cr.set_font_size(13)
            extents = cr.text_extents(self.camera_state)
            cr.move_to((w - extents.width) / 2, h / 2)
            cr.show_text(self.camera_state)
            return
        fw, fh, stride, data = frame
        image = cairo.ImageSurface.create_for_data(data, cairo.FORMAT_RGB24, fw, fh, stride)
        scale = max(w / fw, h / fh)  # cover: fill the shape, crop the overflow
        if self.settings["mirror"]:
            cr.translate(w, 0)
            cr.scale(-1, 1)
        cr.translate((w - fw * scale) / 2, (h - fh * scale) / 2)
        cr.scale(scale, scale)
        cr.set_source_surface(image, 0, 0)
        cr.paint()

    def _press(self, _widget, event):
        if event.type != Gdk.EventType.BUTTON_PRESS:
            return False
        if event.button == Gdk.BUTTON_PRIMARY:
            self.begin_move_drag(event.button, int(event.x_root), int(event.y_root), event.time)
        elif event.button == Gdk.BUTTON_SECONDARY:
            self.build_menu().popup_at_pointer(event)
        return True

    def _scroll(self, _widget, event):
        if event.direction == Gdk.ScrollDirection.SMOOTH:
            dy = event.get_scroll_deltas()[2]
            factor = SCROLL_STEP ** (-dy) if dy else 1
        else:
            factor = {Gdk.ScrollDirection.UP: SCROLL_STEP, Gdk.ScrollDirection.DOWN: 1 / SCROLL_STEP}.get(event.direction, 1)
        if factor != 1:
            self._place(self.get_size()[0] * factor)
        return True

    # Devices ------------------------------------------------------------------

    def _find(self, devices, name):
        return next((device for label, device in devices if label == name), None)

    def _start_camera(self):
        cameras = self.devices.cameras()
        device = self._find(cameras, self.settings["camera"])
        if device is None and cameras:
            self.settings["camera"], device = cameras[0]
        self.camera.start(device)

    def _camera_state(self, state):
        self.camera_state = state
        self.queue_draw()

    def _devices_changed(self):
        if self.camera.pipeline is None:
            self._start_camera()

    def _microphone(self):
        return self._find(self.devices.microphones(), self.settings["microphone"])

    def _gain(self):
        return logic.db_to_gain(self.settings["gain_db"])

    def _level(self, db):
        self.level_db = db
        if self.level_bar:
            self.level_bar.set_value(max(0.0, (db + 60) / 60))

    # Recording ----------------------------------------------------------------

    def toggle_recording(self, *_):
        if self.recorder.busy:
            return self.recorder.stop()
        if self.starting or self.picker:
            return
        self.starting = True
        self.meter.stop()
        portal.ScreenCast().start(self.settings["restore_token"], self._screen_ready, self._recording_failed)

    def _screen_ready(self, fd, node_id, token, session):
        self.starting = False
        self.screencast_session = session
        self.settings["restore_token"] = token
        self._save()
        path = logic.next_recording_path(recordings_dir())
        try:
            self.recorder.start(path, media.video_source(fd, node_id), self.settings["region"],
                                self._microphone(), self._gain())
        except RuntimeError as error:
            self._recording_failed(str(error))
            return

    def toggle_pause(self, *_):
        self.recorder.toggle_pause()

    def _end_session(self):
        portal.close_session(self.screencast_session)
        self.screencast_session = None

    def _recording_finished(self, path):
        self._end_session()
        self.last_path = path
        print(f"youcam: saved {path}")
        if self.quitting:
            self.quit()

    def _recording_failed(self, message):
        self.starting = False
        self._end_session()
        dialog = Gtk.MessageDialog(transient_for=self, modal=True, message_type=Gtk.MessageType.ERROR,
                                   buttons=Gtk.ButtonsType.OK, text="Recording failed")
        dialog.format_secondary_text(message)
        dialog.run()
        dialog.destroy()

    def _status(self):
        if self.starting:
            return "Waiting for screen sharing…"
        if self.recorder.busy:
            minutes, seconds = divmod(int(self.recorder.elapsed()), 60)
            return f"{'Paused' if self.recorder.paused else 'Recording'} {minutes}:{seconds:02d}"
        return "Not recording"

    def _area_name(self):
        return self.settings["region_name"] if self.settings["region"] else "Whole Screen"

    def select_area(self, *_):
        display = Gdk.Display.get_default()
        monitor = display.get_monitor_at_window(self.get_window())
        index = next(i for i in range(display.get_n_monitors()) if display.get_monitor(i) == monitor)
        self.picker = RegionPicker(index, self._area_picked)

    def _area_picked(self, rect, monitor):
        self.picker = None
        if rect is None:
            return
        geometry = monitor.get_geometry()
        fractions = logic.region_fractions(rect, (0, 0, geometry.width, geometry.height))
        if fractions is None:
            return
        self.settings.update(region=fractions,
                             region_name=f"{int(rect[2])} × {int(rect[3])} on {monitor.get_model() or 'screen'}")
        self._save()

    def whole_screen(self, *_):
        self.settings.update(region=None, region_name=None)
        self._save()

    def forget_monitor(self, *_):
        self.settings["restore_token"] = None
        self._save()

    def reveal_recording(self, *_):
        uri = Gio.File.new_for_path(self.last_path).get_uri()
        try:
            Gio.bus_get_sync(Gio.BusType.SESSION).call_sync(
                "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1",
                "ShowItems", GLib.Variant("(ass)", ([uri], "")), None, Gio.DBusCallFlags.NONE, -1, None)
        except GLib.Error:
            Gio.AppInfo.launch_default_for_uri(Gio.File.new_for_path(recordings_dir()).get_uri(), None)

    # Shortcuts ----------------------------------------------------------------

    def _shortcut(self, shortcut_id):
        {"toggle-recording": self.toggle_recording, "toggle-pause": self.toggle_pause}.get(shortcut_id, print)()

    def _shortcuts_status(self, available):
        self.shortcuts_available = available

    # Menu ---------------------------------------------------------------------

    def build_menu(self):
        menu = Gtk.Menu()
        busy = self.recorder.busy or self.starting

        def add(parent, label, action=None, enabled=True, check=None, radio=None):
            item = Gtk.CheckMenuItem(label=label, draw_as_radio=bool(radio)) if check is not None \
                else Gtk.MenuItem(label=label)
            if check is not None:
                item.set_active(check)
            if action:
                item.connect("activate", lambda *_: action())
            item.set_sensitive(enabled and (action is not None or isinstance(item, Gtk.CheckMenuItem)))
            parent.append(item)
            return item

        def submenu(parent, label, enabled=True):
            item = Gtk.MenuItem(label=label)
            item.set_sensitive(enabled)
            child = Gtk.Menu()
            item.set_submenu(child)
            parent.append(item)
            return child

        def separator(parent):
            parent.append(Gtk.SeparatorMenuItem())

        for text in (self.camera_state, self._status(), f"Area: {self._area_name()}"):
            add(menu, text, enabled=False)
        separator(menu)
        add(menu, "Stop Recording" if self.recorder.busy else "Start Recording", self.toggle_recording,
            enabled=not self.starting and self.picker is None)
        if self.recorder.busy:
            add(menu, "Resume Recording" if self.recorder.paused else "Pause Recording", self.toggle_pause)
        area = submenu(menu, "Recording Area", enabled=not busy)
        add(area, "Whole Screen", self.whole_screen, check=self.settings["region"] is None, radio=True)
        add(area, "Select Area…", self.select_area)
        separator(area)
        add(area, "Choose Another Monitor Next Time", self.forget_monitor, enabled=bool(self.settings["restore_token"]))
        if self.shortcuts_available is False:
            add(menu, "Global shortcuts unavailable", enabled=False)
        if self.last_path:
            add(menu, "Show Last Recording", self.reveal_recording)
        separator(menu)

        add(menu, "Hide Camera", self.hide_camera)
        cameras = submenu(menu, "Camera")
        devices = self.devices.cameras()
        if not devices:
            add(cameras, "No cameras available", enabled=False)
        for label, _device in devices:
            add(cameras, label, lambda l=label: self.select_camera(l), check=label == self.settings["camera"], radio=True)
        self._audio_menu(submenu(menu, "Audio Input"), busy)

        shapes = submenu(menu, "Shape")
        for key, (label, _aspect) in logic.SHAPES.items():
            add(shapes, label, lambda k=key: self.select_shape(k), check=key == self.settings["shape"], radio=True)
        sizes = submenu(menu, "Size")
        width = self.get_size()[0]
        for label, size in logic.SIZES:
            add(sizes, label, lambda s=size: self._place(s), check=abs(width - size) < 1, radio=True)
        separator(sizes)
        add(sizes, "Increase Size", lambda: self._place(width * SCROLL_STEP))
        add(sizes, "Decrease Size", lambda: self._place(width / SCROLL_STEP))
        add(menu, "Mirror Camera", self.toggle_mirror, check=self.settings["mirror"])
        add(menu, "Move to Bottom Right", lambda: self._place(width, reset=True))
        separator(menu)
        add(menu, "Retry Camera", self._start_camera)
        add(menu, "How to Use YouCam", self.show_help)
        separator(menu)
        add(menu, "Quit YouCam", self.quit)
        menu.attach_to_widget(self)
        menu.connect("deactivate", lambda m: (self.meter.stop(), GLib.idle_add(m.destroy)))
        menu.show_all()
        return menu

    def _audio_menu(self, audio, busy):
        current = self.settings["microphone"]
        mics = self.devices.microphones()
        known = any(label == current for label, _ in mics)

        def choose(label):
            self.settings["microphone"] = label
            self._save()

        def add_choice(label, value):
            item = Gtk.CheckMenuItem(label=label, draw_as_radio=True)
            item.set_active(value == current or (value is None and not known))
            item.set_sensitive(not busy)
            item.connect("activate", lambda *_: choose(value))
            audio.append(item)

        add_choice("No Audio", None)
        for label, _device in mics:
            add_choice(label, label)
        audio.append(Gtk.SeparatorMenuItem())

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, *logic.GAIN_DB_RANGE, 1)
        scale.set_value(self.settings["gain_db"])
        scale.set_size_request(220, -1)
        scale.connect("format-value", lambda _s, v: f"Gain {v:+.0f} dB")
        scale.connect("value-changed", self._gain_changed)
        self.level_bar = Gtk.LevelBar()
        self.level_bar.set_value(max(0.0, (self.level_db + 60) / 60))
        box.pack_start(scale, False, False, 0)
        box.pack_start(self.level_bar, False, False, 0)
        holder = Gtk.MenuItem()
        holder.add(box)
        holder.connect("button-press-event", lambda *_: True)  # keep the menu open while dragging
        holder.connect("button-release-event", lambda *_: True)
        audio.append(holder)
        audio.append(Gtk.SeparatorMenuItem())
        settings = Gtk.MenuItem(label="Sound Settings…")
        settings.connect("activate", lambda *_: GLib.spawn_command_line_async("gnome-control-center sound"))
        audio.append(settings)

        # Mic is opened only while the Audio Input item is hovered; the parent menu closing stops it.
        audio.get_attach_widget().connect(
            "select", lambda *_: None if self.recorder.busy or self.meter.pipeline
            else self.meter.start(self._microphone(), self._gain()))

    def _gain_changed(self, scale):
        self.settings["gain_db"] = scale.get_value()
        self._save()
        self.meter.set_gain(self._gain())
        self.recorder.set_gain(self._gain())

    # Simple actions -----------------------------------------------------------

    def select_camera(self, label):
        self.settings["camera"] = label
        self._save()
        self._start_camera()

    def select_shape(self, key):
        self.settings["shape"] = key
        self._place(self.get_size()[0])
        self.queue_draw()

    def toggle_mirror(self):
        self.settings["mirror"] = not self.settings["mirror"]
        self._save()
        self.queue_draw()

    def hide_camera(self):
        # ponytail: hidden camera is brought back by launching YouCam again (single instance).
        self.camera.stop()
        self.hide()
        self.get_application().hold()
        self._show_notice("YouCam is hidden. Launch YouCam again to show the camera.")

    def _show_notice(self, text):
        note = Gio.Notification.new("YouCam")
        note.set_body(text)
        self.get_application().send_notification("hidden", note)

    def reveal(self):
        if not self.get_visible():
            self.get_application().release()
            self.show()
            self._start_camera()
        self.present()

    def show_help(self):
        dialog = Gtk.MessageDialog(transient_for=self, modal=True, message_type=Gtk.MessageType.INFO,
                                   buttons=Gtk.ButtonsType.OK, text="Your camera, without the window.")
        dialog.format_secondary_text(HELP)
        dialog.run()
        dialog.destroy()

    def quit(self):
        if self.recorder.busy:
            self.quitting = True
            return self.recorder.stop()  # _recording_finished quits once the movie is complete
        self.camera.stop()
        self.meter.stop()
        self.get_application().quit()


if __name__ == "__main__":
    sys.exit(YouCam().run([sys.argv[0]]))
