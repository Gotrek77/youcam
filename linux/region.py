"""Full-screen picker: drag a rectangle on one monitor, Escape cancels."""
import cairo
from gi.repository import Gdk, Gtk

DIM = (0, 0, 0, 0.4)
EDGE = (1, 1, 1, 0.9)


class RegionPicker(Gtk.Window):
    def __init__(self, monitor_index, on_done):
        """on_done((x, y, w, h) monitor-local rect, monitor) or on_done(None, None)."""
        super().__init__(type=Gtk.WindowType.TOPLEVEL)
        display = Gdk.Display.get_default()
        self.monitor = display.get_monitor(monitor_index)
        self.on_done = on_done
        self.start = self.end = None
        self.set_decorated(False)
        self.set_keep_above(True)
        self.set_app_paintable(True)
        visual = self.get_screen().get_rgba_visual()
        if visual:
            self.set_visual(visual)
        self.add_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.BUTTON_RELEASE_MASK
                        | Gdk.EventMask.POINTER_MOTION_MASK)
        self.connect("draw", self._draw)
        self.connect("button-press-event", self._press)
        self.connect("motion-notify-event", self._motion)
        self.connect("button-release-event", self._release)
        self.connect("key-press-event", self._key)
        self.connect("realize", lambda w: w.get_window().set_cursor(
            Gdk.Cursor.new_from_name(display, "crosshair")))
        self.fullscreen_on_monitor(self.get_screen(), monitor_index)
        self.show_all()
        self.present()

    def _rect(self):
        (x0, y0), (x1, y1) = self.start, self.end
        return min(x0, x1), min(y0, y1), abs(x1 - x0), abs(y1 - y0)

    def _draw(self, _widget, cr):
        cr.set_operator(cairo.OPERATOR_SOURCE)
        cr.set_source_rgba(*DIM)
        cr.paint()
        if self.start and self.end:
            x, y, w, h = self._rect()
            cr.set_source_rgba(0, 0, 0, 0)
            cr.rectangle(x, y, w, h)
            cr.fill()
            cr.set_operator(cairo.OPERATOR_OVER)
            cr.set_source_rgba(*EDGE)
            cr.set_line_width(1)
            cr.rectangle(x + 0.5, y + 0.5, w - 1, h - 1)
            cr.stroke()
            cr.move_to(x + 6, y + h + 18 if y + h + 24 < self.get_allocated_height() else y - 8)
            cr.set_font_size(13)
            cr.show_text(f"{int(w)} × {int(h)}")

    def _press(self, _widget, event):
        if event.button == Gdk.BUTTON_PRIMARY:
            self.start = self.end = (event.x, event.y)
        elif event.button == Gdk.BUTTON_SECONDARY:
            self._finish(None)

    def _motion(self, _widget, event):
        if self.start:
            self.end = (event.x, event.y)
            self.queue_draw()

    def _release(self, _widget, event):
        if self.start and event.button == Gdk.BUTTON_PRIMARY:
            self.end = (event.x, event.y)
            self._finish(self._rect())

    def _key(self, _widget, event):
        if event.keyval == Gdk.KEY_Escape:
            self._finish(None)

    def _finish(self, rect):
        self.destroy()
        self.on_done(rect, self.monitor if rect else None)
