/* YouCam for Linux: a frameless camera overlay and screen recorder.
 *
 * Port of the macOS app in Sources/. Every .c file maps to the Swift
 * file with the same role. Geometry is in GDK coordinates: origin at the top
 * left, y grows downwards (AppKit uses a bottom-left origin, so the math
 * below is mirrored vertically compared to the Swift code). */

#ifndef YOUCAM_H
#define YOUCAM_H

#include <gtk/gtk.h>
#include <gst/gst.h>

#define APP_ID "app.youcam.YouCam"

/* ---------------------------------------------------------- geometry.c --- */

typedef struct { double x, y, w, h; } rect;

/* A decoded camera frame: BGRx, 4 bytes per pixel. */
typedef struct {
    int width, height, stride;
    guint8 *data;
} camera_frame;

typedef enum { SHAPE_CIRCLE, SHAPE_SQUARE, SHAPE_RECTANGLE, SHAPE_COUNT } camera_shape;

typedef enum {
    CORNER_TOP_LEFT, CORNER_TOP_RIGHT, CORNER_BOTTOM_LEFT, CORNER_BOTTOM_RIGHT, CORNER_COUNT
} area_corner;

#define OVERLAY_MIN_WIDTH 120
#define OVERLAY_MAX_WIDTH 960
#define OVERLAY_DEFAULT_WIDTH 280
#define OVERLAY_EDGE_MARGIN 32
#define AREA_MIN_SIZE 40

const char *shape_name(camera_shape s);
const char *shape_title(camera_shape s);
camera_shape shape_from_name(const char *name);
double shape_aspect(camera_shape s);

void overlay_size(double width, camera_shape s, rect screen, double *w, double *h);
rect overlay_constrain(rect frame, rect screen);
rect overlay_resized(rect frame, double width, camera_shape s, rect screen);
rect overlay_initial(double width, camera_shape s, rect screen);
double rect_intersection_area(rect a, rect b);
int rect_contains(rect outer, rect inner);
int rect_equal(rect a, rect b);

const char *corner_title(area_corner c);
void corner_point(area_corner c, rect r, double *x, double *y);
rect corner_resized(area_corner c, rect r, double px, double py, rect screen, double scale);

/* ----------------------------------------------------------- support.c --- */

typedef enum {
    PHASE_IDLE, PHASE_STARTING, PHASE_RECORDING, PHASE_PAUSED, PHASE_FINISHING
} recording_phase;

#define PHASE_BUSY(p) ((p) != PHASE_IDLE)

typedef struct { float peak; int clipped; float source_peak; } audio_level;   /* source_peak: before the gain. */

#define GAIN_DB_MIN (-36.0)
#define GAIN_DB_MAX 12.0

audio_level audio_gain_apply(float *samples, size_t count, float gain);
float audio_level_db(audio_level level);
double gain_db_clamp(double db);
#define NOISE_REDUCTION_LEVELS 5     /* Off, then webrtcdsp levels low...very high. */
#define NOISE_REDUCTION_DEFAULT 2    /* Moderate: removes room hum, keeps the voice natural. */
int noise_reduction_clamp(int level);
float gain_from_db(double db);
char *recording_next_path(const char *dir, time_t when, const char *ext);

/* Linux addition: the recording container. Every one holds H.264 + AAC. */
typedef struct { const char *ext, *muxer, *label; } container;
extern const container Containers[];
#define CONTAINER_COUNT 4
const container *container_for(const char *ext);   /* Unknown: the default, MOV. */
void recording_time_label(double seconds, char *buf, size_t len);
void crop_pixels(rect fraction, int frame_w, int frame_h, int *left, int *right, int *top, int *bottom);

/* Which panel transport controls are usable in a phase (RecordingPanel). */
typedef struct { int play, pause, stop, area; } panel_buttons;
panel_buttons panel_buttons_for(recording_phase phase, int selecting_area);

/* ------------------------------------------------------------- prefs.c --- */

typedef enum { AREA_WHOLE_SCREEN, AREA_DISPLAY, AREA_REGION } area_mode;

/* A recording area. 'monitor' is the GDK monitor model (connector name), which
 * is stable across sessions; 'local' is monitor-local, in logical pixels. */
typedef struct {
    area_mode mode;
    char monitor[64];
    rect local;
} recording_area;

typedef struct {
    camera_shape shape;
    int mirrored;
    double width;
    int has_origin;
    double origin_x, origin_y;
    char *camera;          /* Display name, NULL = default. */
    char *audio;           /* Display name, "none" = No Audio, NULL = default. */
    double gain_db;
    recording_area area;
    char *restore_token;   /* ScreenCast portal: remembers the shared monitor. */
    double transparency;   /* Camera preview transparency, 0...0.8. */
    int record_transparency; /* Apply it to the saved video too. */
    int center_stage;      /* Follow the face (default on). */
    int noise_reduction;   /* Linux only: 0 off, 1...4 = WebRTC suppression low...very high. */
    char *container;       /* Linux only: file extension of Containers[], NULL = mov. */
    int auto_mic_level;    /* Linux only: adjust the system microphone level while in use. */
    char *mic_restore;     /* Linux only: "node:volume" to put back (survives a crash). */
} prefs;

extern prefs Prefs;
void prefs_load(void);
void prefs_save(void);
void prefs_set_string(char **field, const char *value);
recording_area prefs_area_validate(recording_area area, GdkDisplay *display);

/* ------------------------------------------------------------ monitor helpers */

GdkMonitor *monitor_by_name(const char *name);
const char *monitor_name(GdkMonitor *m);
rect monitor_rect(GdkMonitor *m);
rect monitor_workarea(GdkMonitor *m);

/* ------------------------------------------------------------ camera.c --- */

typedef enum {
    CAMERA_IDLE, CAMERA_STARTING, CAMERA_RUNNING, CAMERA_NONE, CAMERA_FAILED
} camera_state_kind;

void camera_init(void (*on_change)(void), void (*on_frame)(void));
void camera_show(void);
void camera_hide(void);
void camera_retry(void);
void camera_select(const char *name);
void camera_shutdown(void);
camera_state_kind camera_state(void);
const char *camera_menu_title(void);
int camera_message(const char **title, const char **detail);
const camera_frame *camera_current_frame(void);
int camera_device_count(void);
const char *camera_device_name(int i);
const char *camera_selected_name(void);

/* ------------------------------------------------------------- audio.c --- */

void audio_init(void (*on_change)(void), void (*on_device_lost)(void));
int audio_has_audio(void);
const char *audio_name(void);
const char *audio_status(void);
audio_level audio_current_level(void);
double audio_gain_db(void);
void audio_set_gain(double db);
void audio_select(const char *name);
int audio_device_count(void);
const char *audio_device_name(int i);
const char *audio_selected_name(void);
GstElement *audio_create_source(void);
void audio_monitor(const char *reason, int enabled);
void audio_post_level(audio_level level);
void audio_shutdown(void);
void audio_set_auto_mic_level(int on);
void audio_install_gain_probe(GstElement *element);

/* ------------------------------------------------------ camera_layer.c --- */

#define TRANSPARENCY_MAX 0.8

/* Center Stage pan and zoom of the camera picture inside its mask. The
 * shift is a fraction of the mask size, so the camera window and the
 * recording layer, drawn at different pixel sizes, show the same picture. */
typedef struct { double zoom, x, y; } stage_transform;

#define STAGE_IDENTITY ((stage_transform){1, 0, 0})

typedef struct {
    rect frame;          /* Output pixels, top-left origin. */
    double radius;
    camera_shape shape;
} camera_layer;

double transparency_normalize(double value);
int camera_layer_needed(double transparency, int apply_to_video, int visible, int running);
camera_layer camera_layer_map(rect window, rect monitor, rect region, int out_w, int out_h,
                              camera_shape shape);
int camera_layer_clip(camera_layer l, int out_w, int out_h, int *x, int *y, int *w, int *h);
void camera_layer_draw(cairo_t *cr, camera_layer l, int cx, int cy, const camera_frame *f,
                       int mirrored, stage_transform t);
void camera_paint_frame(cairo_t *cr, const camera_frame *f, double w, double h, int mirrored,
                        stage_transform t);
void stage_canvas(double w, double h, double *cw, double *ch);

/* ------------------------------------------------------ center_stage.c --- */

/* A face in normalized camera coordinates, top-left origin: centre and size. */
typedef struct { double cx, cy, w, h; } stage_face;

double stage_soft_clamp(double value, double max);
double stage_zoom(const stage_face *f, camera_shape shape);
int stage_smooth(stage_face *smoothed, int has_smoothed, stage_face f);
stage_transform stage_transform_for(const stage_face *f, double w, double h, camera_shape shape, int mirrored);
int stage_detect(const guint8 *gray, int w, int h, stage_face *out);
void center_stage_feed(const guint8 *bgrx, int w, int h, int stride);
void center_stage_update(const stage_face *f);
void center_stage_reset(void);
int center_stage_has_face(void);
int center_stage_animating(void);
stage_transform center_stage_now(double w, double h, camera_shape shape, int mirrored);
void center_stage_init(void (*on_change)(void));

/* ------------------------------------------------------------ writer.c --- */

typedef struct writer writer;

typedef struct {
    void (*finished)(const char *path, void *ud);
    void (*failed)(const char *message, void *ud);
    void *ud;
} writer_callbacks;

writer *writer_start(const char *path, const char *video_source, GstElement *mic,
                     const rect *crop_fraction, int camera_layer, writer_callbacks cb, GError **err);
int writer_output_size(writer *w, int *width, int *height);
const char *video_encoder_name(void);
void writer_push_camera(writer *w, const guint8 *bgra, int width, int height, int stride,
                        int x, int y, int visible);
void writer_pause(writer *w);
void writer_resume(writer *w);
double writer_elapsed(writer *w);
void writer_finish(writer *w);
void writer_cancel(writer *w);
void pipeline_stop_async(GstElement *pipeline);

/* ------------------------------------------------------------ portal.c --- */

void portal_register_app(void);
typedef void (*screencast_ready)(int fd, guint32 node, const char *token,
                                 const char *session, int has_pos, int x, int y, void *ud);
typedef void (*portal_error)(const char *message, void *ud);
void portal_screencast_start(const char *restore_token, screencast_ready ready,
                             portal_error error, void *ud);
void portal_close_session(const char *handle);
GDBusConnection *portal_bus(void);
typedef void (*request_done)(GVariant *results, void *ud);   /* results NULL = cancelled */
int portal_call(const char *iface, const char *method, GVariant **prefix_args, int n_prefix,
                GVariantBuilder *options, request_done done, void *ud, GError **err);

/* ----------------------------------------------------------- hotkeys.c --- */

void hotkeys_init(void (*on_record)(void), void (*on_pause)(void));
int hotkeys_available(void);
void hotkeys_shutdown(void);

/* --------------------------------------------------------- recording.c --- */

void recording_init(void (*on_change)(void), void (*on_error)(const char *));
recording_phase recording_phase_now(void);
const char *recording_status(void);
const char *recording_last_path(void);
const char *recording_target_monitor(void);
void recording_start(GdkMonitor *monitor, const rect *local);
void recording_toggle_pause(void);
void recording_stop(void);
void recording_fail(const char *message);
writer *recording_layer_target(rect *monitor, rect *region);

/* ----------------------------------------------------------- overlay.c --- */

GtkWidget *overlay_create(void);
void overlay_apply_shape(void);
void overlay_queue_draw(void);
void overlay_animate(void);

/* ------------------------------------------------------------- panel.c --- */

GtkWidget *panel_create(void);
void panel_update(void);
void panel_update_audio(void);
GtkWidget *audio_controls_new(void);

/* ------------------------------------------------------------ region.c --- */

void region_select(void (*done)(GdkMonitor *m, const rect *local));
int region_is_selecting(void);
void region_cancel(void);

/* ------------------------------------------------------ area_overlay.c --- */

void area_overlay_show(GdkMonitor *m, rect local);
void area_overlay_hide(void);
void area_overlay_set_editable(int editable);
void area_overlay_set_on_change(void (*cb)(GdkMonitor *m, rect local));

/* -------------------------------------------------------- statusicon.c --- */

void status_icon_init(void (*on_activate)(int x, int y));
void status_icon_update(const char *icon_name, const char *tooltip);

/* ----------------------------------------------------------- mic_level.c --- */

#define MIC_AGC_CLIP_STEP_DB (-6.0)
#define MIC_AGC_STEP_DB 1.5
#define MIC_VOLUME_MIN 0.05
#define MIC_VOLUME_MAX 1.0

typedef struct { gint64 window_start, hold_until; float window_peak; } mic_agc;

double mic_agc_step(mic_agc *a, float source_peak, gint64 now_us);
double mic_volume_after(double volume, double db);
double mic_volume_parse(const char *wpctl_output);
void mic_level_use(const char *node);
void mic_level_feed(float source_peak);
void mic_level_recover(void);

/* ---------------------------------------------------------- mutter_cast.c --- */

/* Windows with this role stay out of recordings (see mutter_cast.c). */
#define CAPTURE_HIDDEN_ROLE "youcam-hidden"

int mutter_cast_available(void);
int mutter_monitor_rect(GVariant *state, const char *connector, int out[4]);
void mutter_cast_start(const char *connector, screencast_ready ready, portal_error error, void *ud);
void mutter_cast_stop(const char *session);

/* --------------------------------------------------------------- app.c --- */

void app_startup(GtkApplication *gapp, int smoke_test);
void app_reopen(void);
void app_snapshot(const char *dir);
GtkWidget *app_build_menu(void);
GtkWidget *app_build_area_menu(void);
void app_popup_menu(GtkWidget *menu, GdkEvent *event);
void app_resize(double width);
void app_save_geometry(void);
void app_toggle_recording(void);
void app_toggle_pause(void);
void app_toggle_panel(void);
void app_panel_play(void);
void app_panel_stop(void);
const char *app_area_name(void);
int app_panel_visible(void);
void app_quit(void);
void app_show_message(const char *title, const char *text, GtkMessageType type);

#endif
