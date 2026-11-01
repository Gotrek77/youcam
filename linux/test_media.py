"""Records synthetic screen + mic with a pause, checks the movie. Run: python3 test_media.py"""
import os
import tempfile

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstPbutils", "1.0")
from gi.repository import GLib, Gst, GstPbutils  # noqa: E402

import media  # noqa: E402

RECORD_S, PAUSE_S = 2, 2


def run():
    loop = GLib.MainLoop()
    outcome = {}
    recorder = media.Recorder(on_finished=lambda p: (outcome.update(path=p), loop.quit()),
                              on_error=lambda e: (outcome.update(error=e), loop.quit()),
                              on_level=lambda db: outcome.update(level=db))
    path = os.path.join(tempfile.mkdtemp(), "test.mp4")
    mic = Gst.ElementFactory.make("audiotestsrc")
    mic.set_property("is-live", True)
    recorder.start(path, "videotestsrc is-live=true ! video/x-raw,width=1280,height=720,framerate=30/1",
                   crop=(0.25, 0.25, 0.5, 0.5), microphone=mic, gain=0.5)
    GLib.timeout_add_seconds(RECORD_S, recorder.toggle_pause)
    GLib.timeout_add_seconds(RECORD_S + PAUSE_S, recorder.toggle_pause)
    GLib.timeout_add_seconds(2 * RECORD_S + PAUSE_S, recorder.stop)
    loop.run()
    assert "error" not in outcome, outcome["error"]
    assert outcome.get("level", -100) > -60, "no audio level reported"
    info = GstPbutils.Discoverer.new(5 * Gst.SECOND).discover_uri(Gst.filename_to_uri(outcome["path"]))
    duration = info.get_duration() / Gst.SECOND
    video = info.get_video_streams()[0]
    assert (video.get_width(), video.get_height()) == (640, 360), (video.get_width(), video.get_height())
    assert info.get_audio_streams(), "audio track missing"
    assert abs(duration - 2 * RECORD_S) < PAUSE_S / 2, f"duration {duration:.2f}s, pause not removed"
    print(f"media ok: {duration:.2f}s 640x360 + audio")


if __name__ == "__main__":
    run()
