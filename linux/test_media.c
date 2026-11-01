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

static GstDiscovererInfo *record(const char *path, int with_audio, int with_pause) {
    GstElement *mic = NULL;
    if (with_audio) {
        mic = gst_element_factory_make("audiotestsrc", NULL);
        g_object_set(mic, "is-live", TRUE, NULL);
    }
    rect crop = {0.25, 0.25, 0.5, 0.5};
    GError *err = NULL;
    Result = Error = NULL;
    W = writer_start(path, "videotestsrc is-live=true ! video/x-raw,width=1280,height=720,framerate=30/1",
                     mic, &crop, (writer_callbacks){finished, failed, NULL}, &err);
    EXPECT(W, "writer did not start: %s", err ? err->message : "?");
    int stop_at = RECORD_S;
    if (with_pause) {
        g_timeout_add_seconds(RECORD_S, do_pause, NULL);
        g_timeout_add_seconds(RECORD_S + PAUSE_S, do_resume, NULL);
        stop_at = 2 * RECORD_S + PAUSE_S;
    }
    g_timeout_add_seconds(stop_at, do_finish, NULL);
    g_main_loop_run(Loop);
    EXPECT(!Error, "recording failed: %s", Error);
    char *uri = gst_filename_to_uri(Result, NULL);
    GstDiscoverer *d = gst_discoverer_new(5 * GST_SECOND, NULL);
    GstDiscovererInfo *info = gst_discoverer_discover_uri(d, uri, &err);
    EXPECT(info, "movie unreadable: %s", err ? err->message : "?");
    g_free(uri);
    g_object_unref(d);
    return info;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    Loop = g_main_loop_new(NULL, FALSE);
    char *dir = g_dir_make_tmp("youcam-media-XXXXXX", NULL);
    char *path = g_build_filename(dir, "pause-resume.mov", NULL);

    GstDiscovererInfo *info = record(path, 1, 1);
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

    info = record(path, 0, 0);
    audios = gst_discoverer_info_get_audio_streams(info);
    EXPECT(!audios, "No Audio creates a movie without an audio track");
    gst_discoverer_info_unref(info);
    g_unlink(path);
    g_rmdir(dir);
    printf("media ok: %.2fs 640x360 .mov with audio, pause removed; silent movie has no audio track\n", duration);
    return 0;
}
