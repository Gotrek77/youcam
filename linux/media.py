"""GStreamer plumbing: device lists, camera preview frames, audio meter, screen recorder."""
import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402

from logic import crop_pixels  # noqa: E402

Gst.init(None)

PREVIEW_WIDTH = 640
LEVEL_INTERVAL_NS = 100_000_000
VIDEO_BITRATE_KBPS = 12_000
AUDIO_BITRATE = 96_000
STOP_TIMEOUT_S = 8


class Devices:
    """Cameras and microphones, keyed by a display name that stays stable across sessions."""

    def __init__(self, on_change):
        self.monitor = Gst.DeviceMonitor()
        self.monitor.add_filter("Audio/Source", None)
        # Cameras straight from V4L2: PipeWire camera nodes fail with "target not found"
        # outside the camera portal, and V4L2 names tell the RGB and IR cameras apart.
        self.v4l2 = Gst.DeviceProviderFactory.get_by_name("v4l2deviceprovider")
        if self.v4l2 is None:
            self.monitor.add_filter("Video/Source", None)
        for source in (self.monitor, self.v4l2):
            if source:
                source.get_bus().add_watch(GLib.PRIORITY_DEFAULT, lambda *_: (on_change(), True)[1])
                source.start()

    def _list(self, kind):
        devices, seen = [], {}
        found = self.v4l2.get_devices() if self.v4l2 and kind == "Video/Source" else self.monitor.get_devices()
        for device in found:
            if device.get_device_class() != kind or _is_monitor(device):
                continue
            name = device.get_display_name()
            seen[name] = seen.get(name, 0) + 1
            devices.append((name if seen[name] == 1 else f"{name} ({seen[name]})", device))
        return devices

    def cameras(self):
        return self._list("Video/Source")

    def microphones(self):
        return self._list("Audio/Source")


def _is_monitor(device):
    props = device.get_properties()
    return bool(props and props.has_field("device.class") and props.get_value("device.class") == "monitor")


def _pipeline_with_source(description, device, link_to):
    pipeline = Gst.parse_launch(description)
    # Tests pass a ready element instead of a Gst.Device.
    source = device if isinstance(device, Gst.Element) else device.create_element(None)
    pipeline.add(source)
    if not source.link(pipeline.get_by_name(link_to)):
        raise RuntimeError("The device could not be connected.")
    return pipeline


class Camera:
    """Decodes a camera into BGRx frames; on_frame() fires on the main loop."""

    def __init__(self, on_frame, on_state):
        self.frame = None  # (width, height, stride, bytes)
        self.pipeline = None
        self._on_frame, self._on_state = on_frame, on_state
        self._draw_pending = False

    def start(self, device):
        self.stop()
        if device is None:
            return self._on_state("No camera found")
        description = (f"decodebin name=decode ! videoconvert ! videoscale ! "
                       f"video/x-raw,format=BGRx,width={PREVIEW_WIDTH},pixel-aspect-ratio=1/1 ! "
                       "appsink name=sink emit-signals=true max-buffers=1 drop=true sync=false")
        try:
            self.pipeline = _pipeline_with_source(description, device, "decode")
        except (GLib.Error, RuntimeError) as error:
            return self._on_state(f"Camera error: {error}")
        self.pipeline.get_by_name("sink").connect("new-sample", self._sample)
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message::error", lambda _b, m: self._failed(m.parse_error()[0].message))
        self._on_state("Camera starting…")
        self.pipeline.set_state(Gst.State.PLAYING)

    def stop(self):
        if self.pipeline:
            self.pipeline.get_bus().remove_signal_watch()
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None
        self.frame = None

    def _failed(self, message):
        print(f"youcam: camera error: {message}")
        if self.pipeline:
            self.pipeline.get_bus().remove_signal_watch()
            _stop_async(self.pipeline)
            self.pipeline = self.frame = None
        self._on_state(f"Camera error: {message}")

    def _sample(self, sink):
        sample = sink.emit("pull-sample")
        caps = sample.get_caps().get_structure(0)
        width, height = caps.get_value("width"), caps.get_value("height")
        buffer = sample.get_buffer()
        ok, info = buffer.map(Gst.MapFlags.READ)
        if ok:
            # ponytail: one copy per frame at 640 px wide, fine for a preview.
            self.frame = (width, height, width * 4, bytearray(info.data))
            buffer.unmap(info)
            if not self._draw_pending:
                self._draw_pending = True
                GLib.idle_add(self._deliver)
        return Gst.FlowReturn.OK

    def _deliver(self):
        self._draw_pending = False
        if self.frame and self.pipeline:
            self._on_state("Camera on")
            self._on_frame()
        return False


def _watch_level(pipeline, on_level):
    def on_element(_bus, message):
        structure = message.get_structure()
        if structure and structure.get_name() == "level":
            peaks = structure.get_value("peak")
            on_level(max(peaks) if peaks else -60.0)
    pipeline.get_bus().connect("message::element", on_element)


class AudioMeter:
    """Live microphone level while the menu is open and nothing is recording."""

    def __init__(self, on_level):
        self.pipeline = None
        self._on_level = on_level

    def start(self, device, gain):
        self.stop()
        if device is None:
            return
        description = (f"audioconvert name=convert ! volume name=volume volume={gain} ! "
                       f"level interval={LEVEL_INTERVAL_NS} ! fakesink sync=false")
        try:
            self.pipeline = _pipeline_with_source(description, device, "convert")
        except (GLib.Error, RuntimeError) as error:
            print(f"youcam: audio meter unavailable: {error}")
            return
        self.pipeline.get_bus().add_signal_watch()
        _watch_level(self.pipeline, self._on_level)
        self.pipeline.set_state(Gst.State.PLAYING)

    def set_gain(self, gain):
        if self.pipeline:
            self.pipeline.get_by_name("volume").set_property("volume", gain)

    def stop(self):
        if self.pipeline:
            self.pipeline.get_bus().remove_signal_watch()
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None


def video_source(fd, node_id):
    return f"pipewiresrc fd={fd} path={node_id} do-timestamp=true always-copy=true keepalive-time=1000"


class Recorder:
    """Screen (+ optional microphone) to an H.264/AAC .mp4.

    Pausing pauses the pipeline: GStreamer's running time skips the pause,
    so screen and audio stay in sync without a hand-made timeline."""

    def __init__(self, on_finished, on_error, on_level):
        self.pipeline = None
        self.path = None
        self.paused = False
        self._on_finished, self._on_error, self._on_level = on_finished, on_error, on_level
        self._timeout = None
        self._started_at = 0
        self._paused_total = 0
        self._paused_at = None

    @property
    def busy(self):
        return self.pipeline is not None

    def start(self, path, video, crop, microphone=None, gain=1.0):
        """video: gst-launch source string; crop: fractions or None; microphone: Gst.Device."""
        audio = ""
        if microphone is not None:
            audio = (f"audioconvert name=aconvert ! audioresample ! audio/x-raw,rate=48000,channels=1 ! "
                     f"volume name=volume volume={gain} ! level interval={LEVEL_INTERVAL_NS} ! "
                     f"audioconvert ! avenc_aac bitrate={AUDIO_BITRATE} ! queue ! mux. ")
        description = (
            f"{video} ! videoconvert ! videocrop name=crop ! videorate ! video/x-raw,framerate=30/1 ! "
            "videoconvert ! video/x-raw,format=I420 ! "
            f"x264enc tune=zerolatency speed-preset=veryfast bitrate={VIDEO_BITRATE_KBPS} key-int-max=60 ! "
            "h264parse ! queue ! mux. "
            f"{audio}mp4mux name=mux ! filesink name=file")
        try:
            pipeline = (_pipeline_with_source(description, microphone, "aconvert") if microphone
                        else Gst.parse_launch(description))
        except (GLib.Error, RuntimeError) as error:
            raise RuntimeError(f"The recording could not be set up: {error}") from error
        pipeline.get_by_name("file").set_property("location", path)
        if crop:
            _apply_crop_on_caps(pipeline.get_by_name("crop"), crop)
        bus = pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message::eos", lambda *_: self._finish(None))
        bus.connect("message::error", lambda _b, m: self._finish(m.parse_error()[0].message))
        _watch_level(pipeline, self._on_level)
        if pipeline.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
            pipeline.set_state(Gst.State.NULL)
            raise RuntimeError("The recording could not start.")
        self.pipeline, self.path, self.paused = pipeline, path, False
        self._started_at, self._paused_total, self._paused_at = GLib.get_monotonic_time(), 0, None

    def set_gain(self, gain):
        volume = self.pipeline and self.pipeline.get_by_name("volume")
        if volume:
            volume.set_property("volume", gain)

    def toggle_pause(self):
        if not self.pipeline:
            return
        now = GLib.get_monotonic_time()
        if self.paused:
            self._paused_total += now - self._paused_at
            self._paused_at = None
            self.pipeline.set_state(Gst.State.PLAYING)
        else:
            self._paused_at = now
            self.pipeline.set_state(Gst.State.PAUSED)
        self.paused = not self.paused

    def elapsed(self):
        if not self.pipeline:
            return 0
        end = self._paused_at or GLib.get_monotonic_time()
        return (end - self._started_at - self._paused_total) / 1_000_000

    def stop(self):
        """Finish the file; on_finished(path) or on_error(message) follows."""
        if not self.pipeline or self._timeout:
            return
        if self.paused:
            self.toggle_pause()
        self.pipeline.send_event(Gst.Event.new_eos())
        self._timeout = GLib.timeout_add_seconds(STOP_TIMEOUT_S, lambda: self._finish("Finishing the movie timed out."))

    def _finish(self, error):
        if not self.pipeline:
            return False
        if self._timeout:
            GLib.source_remove(self._timeout)
            self._timeout = None
        self.pipeline.get_bus().remove_signal_watch()
        if error:
            _stop_async(self.pipeline)
        else:
            self.pipeline.set_state(Gst.State.NULL)
        self.pipeline, path = None, self.path
        if error:
            self._on_error(error)
        else:
            self._on_finished(path)
        return False


def _stop_async(pipeline):
    """Stopping from the main loop right after a PipeWire error can deadlock; stop off-thread."""
    pipeline.call_async(lambda element: element.set_state(Gst.State.NULL))


def _apply_crop_on_caps(crop, fractions):
    """The portal stream's pixel size is known only once caps arrive."""
    def probe(_pad, info):
        event = info.get_event()
        if event.type == Gst.EventType.CAPS:
            structure = event.parse_caps().get_structure(0)
            sides = crop_pixels(fractions, structure.get_value("width"), structure.get_value("height"))
            for name, value in zip(("left", "right", "top", "bottom"), sides):
                crop.set_property(name, value)
        return Gst.PadProbeReturn.OK

    crop.get_static_pad("sink").add_probe(Gst.PadProbeType.EVENT_DOWNSTREAM, probe)
