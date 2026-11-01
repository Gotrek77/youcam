/* Movie writer (RecordingWriter.swift): screen + optional microphone into a
 * QuickTime .mov with H.264 video and AAC audio.
 *
 * Pausing pauses the whole pipeline. GStreamer's running time does not
 * advance while paused, so both tracks continue seamlessly after a resume
 * without the hand-made timeline the macOS code needs. A static screen still
 * produces frames thanks to pipewiresrc's keepalive and videorate, so the
 * movie always lasts until the real stop time. */

#include <glib/gstdio.h>
#include "youcam.h"

#define VIDEO_BITRATE_KBPS 12000
#define AUDIO_BITRATE 96000
#define FINISH_TIMEOUT_S 8

struct writer {
    GstElement *pipeline;
    char *path;
    writer_callbacks cb;
    rect crop;
    int has_crop;
    guint timeout;
    gint64 started, paused_at, paused_total;
    int done;
};

/* The portal stream's pixel size is known only once caps arrive. */
static GstPadProbeReturn crop_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    writer *w = data;
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(ev) != GST_EVENT_CAPS) return GST_PAD_PROBE_OK;
    GstCaps *caps;
    gst_event_parse_caps(ev, &caps);
    int fw = 0, fh = 0, l, r, t, b;
    GstStructure *s = gst_caps_get_structure(caps, 0);
    gst_structure_get_int(s, "width", &fw);
    gst_structure_get_int(s, "height", &fh);
    if (fw > 0 && fh > 0) {
        crop_pixels(w->crop, fw, fh, &l, &r, &t, &b);
        GstElement *crop = gst_pad_get_parent_element(pad);
        g_object_set(crop, "left", l, "right", r, "top", t, "bottom", b, NULL);
        gst_object_unref(crop);
    }
    return GST_PAD_PROBE_OK;
}

static void stop_in_thread(GstElement *element, gpointer data) {
    (void)data;
    gst_element_set_state(element, GST_STATE_NULL);
}

/* Stopping from the main loop right after a source error can deadlock inside
 * the source (seen with pipewiresrc); stopping off-thread is always safe. */
void pipeline_stop_async(GstElement *pipeline) {
    gst_element_call_async(pipeline, stop_in_thread, NULL, NULL);
}

static void complete(writer *w, const char *error) {
    if (w->done) return;
    w->done = 1;
    if (w->timeout) g_source_remove(w->timeout);
    w->timeout = 0;
    GstBus *bus = gst_element_get_bus(w->pipeline);
    gst_bus_remove_signal_watch(bus);
    gst_object_unref(bus);
    /* After an error the sources may be wedged: stop off the main thread. */
    if (error) pipeline_stop_async(w->pipeline);
    else gst_element_set_state(w->pipeline, GST_STATE_NULL);
    gst_object_unref(w->pipeline);
    if (error) w->cb.failed(error, w->cb.ud);
    else w->cb.finished(w->path, w->cb.ud);
    g_free(w->path);
    g_free(w);
}

static void on_eos(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)msg;
    complete(data, NULL);
}

static void on_error(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus;
    GError *err = NULL;
    gst_message_parse_error(msg, &err, NULL);
    char *text = g_strdup(err ? err->message : "The movie could not be written.");
    g_clear_error(&err);
    complete(data, text);
    g_free(text);
}

static gboolean finish_timeout(gpointer data) {
    writer *w = data;
    w->timeout = 0;
    complete(w, "Finishing the movie timed out.");
    return G_SOURCE_REMOVE;
}

writer *writer_start(const char *path, const char *video_source, GstElement *mic,
                     const rect *crop_fraction, writer_callbacks cb, GError **err) {
    char *audio = mic ? g_strdup_printf(
        "audioconvert name=aconvert ! audioresample ! "
        "audio/x-raw,format=F32LE,channels=1,rate=48000 ! identity name=gain ! "
        "audioconvert ! avenc_aac bitrate=%d ! queue ! mux. ", AUDIO_BITRATE) : g_strdup("");
    char *desc = g_strdup_printf(
        "%s ! videoconvert ! videocrop name=crop ! videorate ! video/x-raw,framerate=30/1 ! "
        "videoconvert ! video/x-raw,format=I420 ! "
        "x264enc tune=zerolatency speed-preset=veryfast bitrate=%d key-int-max=60 ! "
        "h264parse ! queue ! mux. %sqtmux name=mux ! filesink name=file",
        video_source, VIDEO_BITRATE_KBPS, audio);
    GstElement *p = gst_parse_launch(desc, err);
    g_free(desc);
    g_free(audio);
    if (!p) return NULL;

    if (mic) {
        GstElement *convert = gst_bin_get_by_name(GST_BIN(p), "aconvert");
        int ok = gst_bin_add(GST_BIN(p), mic) && gst_element_link(mic, convert);
        gst_object_unref(convert);
        if (!ok) {
            g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
                        "The microphone could not be connected.");
            gst_object_unref(p);
            return NULL;
        }
        GstElement *gain = gst_bin_get_by_name(GST_BIN(p), "gain");
        audio_install_gain_probe(gain);
        gst_object_unref(gain);
    }

    writer *w = g_new0(writer, 1);
    w->pipeline = p;
    w->path = g_strdup(path);
    w->cb = cb;
    GstElement *file = gst_bin_get_by_name(GST_BIN(p), "file");
    g_object_set(file, "location", path, NULL);
    gst_object_unref(file);
    if (crop_fraction) {
        w->crop = *crop_fraction;
        GstElement *crop = gst_bin_get_by_name(GST_BIN(p), "crop");
        GstPad *pad = gst_element_get_static_pad(crop, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, crop_probe, w, NULL);
        gst_object_unref(pad);
        gst_object_unref(crop);
    }
    GstBus *bus = gst_element_get_bus(p);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::eos", G_CALLBACK(on_eos), w);
    g_signal_connect(bus, "message::error", G_CALLBACK(on_error), w);
    gst_object_unref(bus);

    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_STATE_CHANGE, "The recording could not start.");
        bus = gst_element_get_bus(p);
        gst_bus_remove_signal_watch(bus);
        gst_object_unref(bus);
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
        g_unlink(path);
        g_free(w->path);
        g_free(w);
        return NULL;
    }
    w->started = g_get_monotonic_time();
    return w;
}

void writer_pause(writer *w) {
    if (w->paused_at) return;
    w->paused_at = g_get_monotonic_time();
    gst_element_set_state(w->pipeline, GST_STATE_PAUSED);
}

void writer_resume(writer *w) {
    if (!w->paused_at) return;
    w->paused_total += g_get_monotonic_time() - w->paused_at;
    w->paused_at = 0;
    gst_element_set_state(w->pipeline, GST_STATE_PLAYING);
}

double writer_elapsed(writer *w) {
    gint64 end = w->paused_at ? w->paused_at : g_get_monotonic_time();
    return (end - w->started - w->paused_total) / 1e6;
}

/* EOS lets qtmux write the movie index; 'finished' or 'failed' follows. */
void writer_finish(writer *w) {
    if (w->timeout) return;
    writer_resume(w);
    gst_element_send_event(w->pipeline, gst_event_new_eos());
    w->timeout = g_timeout_add_seconds(FINISH_TIMEOUT_S, finish_timeout, w);
}

/* A cancelled start is not a recording: drop the file, no callbacks. */
void writer_cancel(writer *w) {
    char *path = g_strdup(w->path);
    w->cb.failed = NULL;
    w->done = 1;
    if (w->timeout) g_source_remove(w->timeout);
    GstBus *bus = gst_element_get_bus(w->pipeline);
    gst_bus_remove_signal_watch(bus);
    gst_object_unref(bus);
    gst_element_set_state(w->pipeline, GST_STATE_NULL);
    gst_object_unref(w->pipeline);
    g_unlink(path);
    g_free(path);
    g_free(w->path);
    g_free(w);
}
