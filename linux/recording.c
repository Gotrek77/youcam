/* Recording controller (RecordingController.swift): phases, status text,
 * starting through the portal, pausing, stopping and failure handling.
 *
 * The portal lets the user pick which monitor to share and then remembers it.
 * YouCam knows which monitor the user means (the one under the camera, a
 * chosen display, or the one holding the selected area): when the remembered
 * stream sits elsewhere, the restore token is dropped and the portal asks
 * once more, so menu choices like "Whole Screen — HDMI-1" are respected. */

#include <string.h>
#include <unistd.h>
#include "youcam.h"

#define TICK_MS 250

static struct {
    recording_phase phase;
    writer *movie;
    char *session;
    char *monitor;            /* Name of the monitor being recorded. */
    rect monitor_geometry;
    rect crop;
    int has_crop;
    int cancel_start;
    int retried;
    char *last_path;
    char *last_error;
    char *pending_error;      /* Reported once the movie is safely finished. */
    guint timer;
    char status[128];
    void (*on_change)(void);
    void (*on_error)(const char *);
} R;

static void changed(void) { if (R.on_change) R.on_change(); }

recording_phase recording_phase_now(void) { return R.phase; }
const char *recording_last_path(void) { return R.last_path; }
const char *recording_target_monitor(void) { return R.monitor; }

const char *recording_status(void) {
    char t[16];
    recording_time_label(R.movie ? writer_elapsed(R.movie) : 0, t, sizeof(t));
    switch (R.phase) {
    case PHASE_IDLE:
        return R.last_error ? "Recording stopped" : R.last_path ? "Saved to Desktop" : "Ready to record";
    case PHASE_STARTING: return "Starting recording…";
    case PHASE_RECORDING: snprintf(R.status, sizeof(R.status), "Recording · %s", t); return R.status;
    case PHASE_PAUSED: snprintf(R.status, sizeof(R.status), "Paused · %s", t); return R.status;
    case PHASE_FINISHING: return "Saving recording…";
    }
    return "";
}

static gboolean tick(gpointer data) {
    (void)data;
    changed();
    return G_SOURCE_CONTINUE;
}

static void complete_stop(void) {
    if (R.timer) g_source_remove(R.timer);
    R.timer = 0;
    portal_close_session(R.session);
    g_free(R.session);
    R.session = NULL;
    R.movie = NULL;
    R.phase = PHASE_IDLE;
    audio_monitor("recording", 0);
    capture_exclusion_update();
    changed();
}

static void report(const char *message) {
    prefs_set_string(&R.last_error, message);
    complete_stop();
    if (R.on_error) R.on_error(message);
}

static void movie_finished(const char *path, void *ud) {
    (void)ud;
    prefs_set_string(&R.last_path, path);
    g_print("youcam: saved %s\n", path);
    if (R.pending_error) {
        char *message = R.pending_error;
        R.pending_error = NULL;
        report(message);
        g_free(message);
        return;
    }
    complete_stop();
}

static void movie_failed(const char *message, void *ud) {
    (void)ud;
    g_free(R.pending_error);
    R.pending_error = NULL;
    report(message);
}

static const char *recordings_dir(void) {
    const char *desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    return desktop && g_file_test(desktop, G_FILE_TEST_IS_DIR) ? desktop : g_get_home_dir();
}

static void begin(void);

static void screen_failed(const char *message, void *ud) {
    (void)ud;
    if (R.cancel_start) {
        R.cancel_start = 0;
        complete_stop();
        return;
    }
    report(message);
}

static void screen_ready(int fd, guint32 node, const char *token, const char *session,
                         int has_pos, int x, int y, void *ud) {
    (void)ud;
    R.session = g_strdup(session);
    if (R.cancel_start) {
        close(fd);
        R.cancel_start = 0;
        complete_stop();
        return;
    }
    GdkDisplay *display = gdk_display_get_default();
    int wrong_monitor = has_pos && gdk_display_get_n_monitors(display) > 1 &&
                        (x != (int)R.monitor_geometry.x || y != (int)R.monitor_geometry.y);
    if (wrong_monitor && !R.retried) {
        /* The remembered monitor is not the one asked for: ask again. */
        close(fd);
        portal_close_session(R.session);
        g_free(R.session);
        R.session = NULL;
        R.retried = 1;
        prefs_set_string(&Prefs.restore_token, NULL);
        begin();
        return;
    }
    prefs_set_string(&Prefs.restore_token, token);
    prefs_save();

    GstElement *mic = NULL;
    if (audio_has_audio()) {
        mic = audio_create_source();
        if (!mic) {
            close(fd);
            report("The selected microphone is not available. Select another microphone or No Audio.");
            return;
        }
    }
    char *path = recording_next_path(recordings_dir(), time(NULL));
    char *video = g_strdup_printf(
        "pipewiresrc fd=%d path=%u do-timestamp=true always-copy=true keepalive-time=1000", fd, node);
    GError *err = NULL;
    audio_monitor("recording", 1);
    R.movie = writer_start(path, video, mic, R.has_crop ? &R.crop : NULL,
                           (writer_callbacks){movie_finished, movie_failed, NULL}, &err);
    g_free(video);
    g_free(path);
    if (!R.movie) {
        if (mic) gst_object_unref(mic);
        close(fd);
        char *msg = g_strdup(err ? err->message : "The recording could not start.");
        g_clear_error(&err);
        report(msg);
        g_free(msg);
        return;
    }
    R.phase = PHASE_RECORDING;
    R.timer = g_timeout_add(TICK_MS, tick, NULL);
    capture_exclusion_update();
    changed();
}

static void begin(void) {
    portal_screencast_start(Prefs.restore_token, screen_ready, screen_failed, NULL);
}

void recording_start(GdkMonitor *monitor, const rect *local) {
    if (R.phase != PHASE_IDLE || !monitor) return;
    R.phase = PHASE_STARTING;
    R.cancel_start = 0;
    R.retried = 0;
    prefs_set_string(&R.last_error, NULL);
    prefs_set_string(&R.monitor, monitor_name(monitor));
    R.monitor_geometry = monitor_rect(monitor);
    R.has_crop = local != NULL;
    if (local) {
        rect m = R.monitor_geometry;
        R.crop = (rect){local->x / m.w, local->y / m.h, local->w / m.w, local->h / m.h};
    }
    changed();
    begin();
}

void recording_toggle_pause(void) {
    if (R.phase == PHASE_RECORDING) {
        writer_pause(R.movie);
        R.phase = PHASE_PAUSED;
    } else if (R.phase == PHASE_PAUSED) {
        writer_resume(R.movie);
        R.phase = PHASE_RECORDING;
    } else {
        return;
    }
    changed();
}

void recording_stop(void) {
    if (R.phase == PHASE_STARTING) {
        R.cancel_start = 1;   /* The portal callback finishes the cancellation. */
        return;
    }
    if (R.phase != PHASE_RECORDING && R.phase != PHASE_PAUSED) return;
    R.phase = PHASE_FINISHING;
    if (R.timer) g_source_remove(R.timer);
    R.timer = 0;
    changed();
    writer_finish(R.movie);
}

/* Something outside the pipeline went wrong (microphone unplugged, display
 * removed): keep what was recorded, then tell the user. */
void recording_fail(const char *message) {
    if (R.phase == PHASE_RECORDING || R.phase == PHASE_PAUSED) {
        prefs_set_string(&R.pending_error, message);
        recording_stop();
    }
}

void recording_init(void (*on_change)(void), void (*on_error)(const char *)) {
    R.on_change = on_change;
    R.on_error = on_error;
}
