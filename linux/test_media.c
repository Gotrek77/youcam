/* Records synthetic screen + microphone through the real writer pipeline,
 * with a pause in the middle, then inspects the movie (RecordingTests.swift,
 * movie part). A second movie without audio must have no audio track. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <gst/pbutils/pbutils.h>
#include "youcam.h"

#define RECORD_S 2
#define PAUSE_S 2

static GMainLoop *Loop;
static char *Result;
static char *Error;
static writer *W;

#define EXPECT(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } \
} while (0)

static void finished(const char *path, void *ud) { (void)ud; Result = g_strdup(path); g_main_loop_quit(Loop); }
static void failed(const char *msg, void *ud) { (void)ud; Error = g_strdup(msg); g_main_loop_quit(Loop); }
static gboolean do_pause(gpointer d) { (void)d; writer_pause(W); return G_SOURCE_REMOVE; }
static gboolean do_resume(gpointer d) { (void)d; writer_resume(W); return G_SOURCE_REMOVE; }
static gboolean do_finish(gpointer d) { (void)d; writer_finish(W); return G_SOURCE_REMOVE; }

/* Camera layer: an opaque red square pushed at ~30 fps, moving. */
static int LayerFrames;

static gboolean push_layer(gpointer d) {
    (void)d;
    if (!W) return G_SOURCE_REMOVE;
    static guint8 red[64 * 64 * 4];
    for (int i = 0; i < 64 * 64; i++) { red[i * 4 + 2] = 255; red[i * 4 + 3] = 255; }
    int x = (LayerFrames * 4) % 500;
    writer_push_camera(W, red, 64, 64, 64 * 4, x, 100, 1);
    LayerFrames++;
    return G_SOURCE_CONTINUE;
}

static GstDiscovererInfo *record(const char *path, int with_audio, int with_pause, int with_layer) {
    GstElement *mic = NULL;
    if (with_audio) {
        mic = gst_element_factory_make("audiotestsrc", NULL);
        g_object_set(mic, "is-live", TRUE, NULL);
    }
    rect crop = {0.25, 0.25, 0.5, 0.5};
    GError *err = NULL;
    Result = Error = NULL;
    W = writer_start(path, "videotestsrc is-live=true ! video/x-raw,width=1280,height=720,framerate=30/1",
                     mic, &crop, with_layer, (writer_callbacks){finished, failed, NULL}, &err);
    EXPECT(W, "writer did not start: %s", err ? err->message : "?");
    guint layer = with_layer ? g_timeout_add(33, push_layer, NULL) : 0;
    int stop_at = RECORD_S;
    if (with_pause) {
        g_timeout_add_seconds(RECORD_S, do_pause, NULL);
        g_timeout_add_seconds(RECORD_S + PAUSE_S, do_resume, NULL);
        stop_at = 2 * RECORD_S + PAUSE_S;
    }
    g_timeout_add_seconds(stop_at, do_finish, NULL);
    g_main_loop_run(Loop);
    W = NULL;
    if (layer) g_source_remove(layer);
    EXPECT(!Error, "recording failed: %s", Error);
    char *uri = gst_filename_to_uri(Result, NULL);
    GstDiscoverer *d = gst_discoverer_new(5 * GST_SECOND, NULL);
    GstDiscovererInfo *info = gst_discoverer_discover_uri(d, uri, &err);
    EXPECT(info, "movie unreadable: %s", err ? err->message : "?");
    g_free(uri);
    g_object_unref(d);
    return info;
}

/* Decodes frame number 'index' and counts strongly red pixels on 'row'. */
static int red_pixels_on_row(const char *path, int index, int row) {
    char *desc = g_strdup_printf("filesrc location=\"%s\" ! decodebin ! videoconvert ! "
                                 "video/x-raw,format=RGB ! appsink name=sink sync=false", path);
    GstElement *p = gst_parse_launch(desc, NULL);
    g_free(desc);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(p), "sink");
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstSample *sample = NULL;
    for (int i = 0; i <= index; i++) {
        if (sample) gst_sample_unref(sample);
        g_signal_emit_by_name(sink, "pull-sample", &sample);
        if (!sample) break;
    }
    int count = 0;
    if (sample) {
        int w = 0;
        gst_structure_get_int(gst_caps_get_structure(gst_sample_get_caps(sample), 0), "width", &w);
        GstMapInfo map;
        GstBuffer *buf = gst_sample_get_buffer(sample);
        gst_buffer_map(buf, &map, GST_MAP_READ);
        int stride = (w * 3 + 3) & ~3;
        for (int x = 0; x < w; x++) {
            const guint8 *px = map.data + row * stride + x * 3;
            if (px[0] > 200 && px[1] < 60 && px[2] < 60) count++;
        }
        gst_buffer_unmap(buf, &map);
        gst_sample_unref(sample);
    }
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(p);
    return count;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    Loop = g_main_loop_new(NULL, FALSE);
    char *dir = g_dir_make_tmp("youcam-media-XXXXXX", NULL);
    char *path = g_build_filename(dir, "pause-resume.mov", NULL);

    GstDiscovererInfo *info = record(path, 1, 1, 0);
    double duration = (double)gst_discoverer_info_get_duration(info) / GST_SECOND;
    GList *videos = gst_discoverer_info_get_video_streams(info);
    GList *audios = gst_discoverer_info_get_audio_streams(info);
    EXPECT(videos && audios, "MOV has video and microphone tracks");
    GstDiscovererVideoInfo *v = videos->data;
    EXPECT(gst_discoverer_video_info_get_width(v) == 640 && gst_discoverer_video_info_get_height(v) == 360,
           "the selected area is cropped (got %ux%u)", gst_discoverer_video_info_get_width(v),
           gst_discoverer_video_info_get_height(v));
    EXPECT(fabs(duration - 2 * RECORD_S) < PAUSE_S / 2.0,
           "the pause is removed from the movie (duration %.2fs)", duration);
    gst_discoverer_stream_info_list_free(videos);
    gst_discoverer_stream_info_list_free(audios);
    gst_discoverer_info_unref(info);
    g_unlink(path);

    info = record(path, 0, 0, 0);
    audios = gst_discoverer_info_get_audio_streams(info);
    EXPECT(!audios, "No Audio creates a movie without an audio track");
    gst_discoverer_info_unref(info);
    g_unlink(path);

    /* Camera layer through the compositor, with a pause. */
    info = record(path, 1, 1, 1);
    double layered = (double)gst_discoverer_info_get_duration(info) / GST_SECOND;
    videos = gst_discoverer_info_get_video_streams(info);
    EXPECT(videos && gst_discoverer_video_info_get_width(videos->data) == 640,
           "the camera layer keeps the movie at the cropped size");
    EXPECT(fabs(layered - 2 * RECORD_S) < PAUSE_S / 2.0, "camera layer movie duration %.2fs", layered);
    EXPECT(LayerFrames > 30, "camera frames were pushed (%d)", LayerFrames);
    int red = red_pixels_on_row(Result, 30, 132);
    EXPECT(red >= 60, "the camera layer is visible in the movie (%d red pixels)", red);
    gst_discoverer_stream_info_list_free(videos);
    gst_discoverer_info_unref(info);
    g_unlink(path);
    g_rmdir(dir);
    printf("media ok: %.2fs 640x360 .mov with audio, pause removed; silent movie has no audio track; "
           "camera layer movie %.2fs with %d composited frames\n", duration, layered, LayerFrames);
    return 0;
}
