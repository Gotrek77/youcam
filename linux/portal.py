"""xdg-desktop-portal helpers: ScreenCast (screen capture via PipeWire) and GlobalShortcuts."""
import itertools

from gi.repository import Gio, GLib

BUS_NAME = "org.freedesktop.portal.Desktop"
OBJECT_PATH = "/org/freedesktop/portal/desktop"
APP_ID = "app.youcam.YouCam"
SOURCE_MONITOR = 1
CURSOR_EMBEDDED = 2
PERSIST_UNTIL_REVOKED = 2
_tokens = itertools.count()


class PortalError(Exception):
    pass


def _bus():
    return Gio.bus_get_sync(Gio.BusType.SESSION, None)


def register_app():
    """Give the portal an app id for a non-sandboxed app. Must precede other portal calls."""
    try:
        _bus().call_sync(BUS_NAME, OBJECT_PATH, "org.freedesktop.host.portal.Registry", "Register",
                         GLib.Variant("(sa{sv})", (APP_ID, {})), None, Gio.DBusCallFlags.NONE, -1, None)
    except GLib.Error as error:
        if "already associated" not in error.message:  # GtkApplication may have registered already
            print(f"youcam: portal registry unavailable: {error.message}")


def _request(interface, method, args, signature, options, on_done):
    """Call a portal method that answers through a Request object; on_done(results | None)."""
    bus = _bus()
    token = f"youcam{next(_tokens)}"
    sender = bus.get_unique_name()[1:].replace(".", "_")
    path = f"/org/freedesktop/portal/desktop/request/{sender}/{token}"
    options = dict(options, handle_token=GLib.Variant("s", token))

    def on_response(_bus, _sender, _path, _iface, _signal, params):
        bus.signal_unsubscribe(subscription)
        code, results = params.unpack()
        on_done(results if code == 0 else None)

    subscription = bus.signal_subscribe(BUS_NAME, "org.freedesktop.portal.Request", "Response", path,
                                        None, Gio.DBusSignalFlags.NO_MATCH_RULE, on_response)
    try:
        bus.call_sync(BUS_NAME, OBJECT_PATH, interface, method,
                      GLib.Variant(f"({signature}a{{sv}})", (*args, options)),
                      None, Gio.DBusCallFlags.NONE, -1, None)
    except GLib.Error as error:
        bus.signal_unsubscribe(subscription)
        raise PortalError(error.message) from error


def _session_options():
    return {"session_handle_token": GLib.Variant("s", f"youcam{next(_tokens)}")}


def close_session(handle):
    if handle:
        _bus().call(BUS_NAME, handle, "org.freedesktop.portal.Session", "Close",
                    None, None, Gio.DBusCallFlags.NONE, -1, None, None, None)


class ScreenCast:
    """Asks the user for a monitor once, then remembers it with a restore token.

    start(restore_token, on_ready, on_error): on_ready(fd, node_id, new_token, session)."""
    IFACE = "org.freedesktop.portal.ScreenCast"

    def start(self, restore_token, on_ready, on_error):
        def fail(step):
            on_error(f"Screen capture was cancelled or denied ({step}).")

        def created(results):
            if results is None:
                return fail("session")
            session = results["session_handle"]
            options = {"types": GLib.Variant("u", SOURCE_MONITOR),
                       "multiple": GLib.Variant("b", False),
                       "cursor_mode": GLib.Variant("u", CURSOR_EMBEDDED),
                       "persist_mode": GLib.Variant("u", PERSIST_UNTIL_REVOKED)}
            if restore_token:
                options["restore_token"] = GLib.Variant("s", restore_token)
            self._call("SelectSources", (session,), "o", options,
                       lambda r: selected(session, r), on_error)

        def selected(session, results):
            if results is None:
                close_session(session)
                return fail("source")
            self._call("Start", (session, ""), "os", {}, lambda r: started(session, r), on_error)

        def started(session, results):
            if results is None or not results.get("streams"):
                close_session(session)
                return fail("start")
            node_id = results["streams"][0][0]
            try:
                fd = _open_pipewire_remote(session)
            except GLib.Error as error:
                close_session(session)
                return on_error(f"PipeWire is not available: {error.message}")
            on_ready(fd, node_id, results.get("restore_token", restore_token), session)

        self._call("CreateSession", (), "", _session_options(), created, on_error)

    def _call(self, method, args, signature, options, on_done, on_error):
        try:
            _request(self.IFACE, method, args, signature, options, on_done)
        except PortalError as error:
            on_error(f"Screen capture portal failed: {error}")


def _open_pipewire_remote(session):
    result, fds = _bus().call_with_unix_fd_list_sync(
        BUS_NAME, OBJECT_PATH, ScreenCast.IFACE, "OpenPipeWireRemote",
        GLib.Variant("(oa{sv})", (session, {})), GLib.VariantType("(h)"),
        Gio.DBusCallFlags.NONE, -1, None, None)
    return fds.get(result.unpack()[0])


class GlobalShortcuts:
    """Binds shortcuts through the portal; on_activated(shortcut_id) runs on the main loop."""
    IFACE = "org.freedesktop.portal.GlobalShortcuts"

    def __init__(self, shortcuts, on_activated, on_status):
        """shortcuts: [(id, description, trigger)]; on_status(bool available)."""
        self.session = None
        self._shortcuts = shortcuts
        self._on_activated = on_activated
        self._on_status = on_status
        try:
            _request(self.IFACE, "CreateSession", (), "", _session_options(), self._created)
        except PortalError as error:
            print(f"youcam: global shortcuts unavailable: {error}")
            on_status(False)

    def _created(self, results):
        if results is None:
            return self._on_status(False)
        self.session = results["session_handle"]
        _bus().signal_subscribe(BUS_NAME, self.IFACE, "Activated", OBJECT_PATH, None,
                                Gio.DBusSignalFlags.NONE, self._activated)
        bindings = [(sid, {"description": GLib.Variant("s", text), "preferred_trigger": GLib.Variant("s", trigger)})
                    for sid, text, trigger in self._shortcuts]
        try:
            _request(self.IFACE, "BindShortcuts", (self.session, bindings, ""), "oa(sa{sv})s", {},
                     lambda r: self._on_status(r is not None))
        except PortalError as error:
            print(f"youcam: binding shortcuts failed: {error}")
            self._on_status(False)

    def _activated(self, _bus, _sender, _path, _iface, _signal, params):
        session, shortcut_id = params.unpack()[:2]
        if session == self.session:
            self._on_activated(shortcut_id)
