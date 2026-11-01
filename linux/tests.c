/* Logic checks ported from the Swift tests in Tests/ (the parts that are not AppKit UI).
 * Coordinates are GDK's (top-left origin): the macOS cases are mirrored
 * vertically inside a 1440x900 display. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include "youcam.h"

static int Checks;

#define EXPECT(cond, msg) do { \
    Checks++; \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); exit(1); } \
} while (0)

static int near(double a, double b) { return fabs(a - b) < 0.001; }

static void test_overlay_geometry(void) {
    rect screen = {0, 0, 1920, 1080};
    double w, h;
    overlay_size(10, SHAPE_CIRCLE, screen, &w, &h);
    EXPECT(w == 120 && h == 120, "The camera has a usable minimum size");
    overlay_size(5000, SHAPE_RECTANGLE, screen, &w, &h);
    EXPECT(w == 960 && near(h, 540), "The camera has a maximum size and keeps 16:9");
    overlay_size(NAN, SHAPE_SQUARE, screen, &w, &h);
    EXPECT(w == 280, "A corrupt saved width falls back to the default");
    rect small = {0, 0, 300, 200};
    overlay_size(400, SHAPE_SQUARE, small, &w, &h);
    EXPECT(w == 200, "The camera never exceeds the screen");
    rect r = overlay_initial(280, SHAPE_CIRCLE, screen);
    EXPECT(rect_equal(r, (rect){1920 - 280 - 32, 1080 - 280 - 32, 280, 280}),
           "The camera starts in the bottom right corner");
    r = overlay_resized((rect){100, 100, 280, 280}, 400, SHAPE_RECTANGLE, screen);
    EXPECT(r.x == 100 && r.y == 100 && r.w == 400 && near(r.h, 225), "Resizing keeps the top-left corner still");
    r = overlay_constrain((rect){1900, -50, 200, 200}, screen);
    EXPECT(rect_equal(r, (rect){1720, 0, 200, 200}), "The camera is kept on screen");
    rect offset = {-1920, 0, 1920, 1080};
    r = overlay_resized((rect){-100, 900, 280, 280}, 280, SHAPE_CIRCLE, offset);
    EXPECT(r.x == -280 && r.y == 800, "Constraining works on displays left of the primary one");
    EXPECT(rect_intersection_area((rect){0, 0, 10, 10}, (rect){5, 5, 10, 10}) == 25, "Intersection area");
    EXPECT(rect_intersection_area((rect){0, 0, 10, 10}, (rect){20, 0, 10, 10}) == 0, "Disjoint frames do not intersect");
}

/* RecordingAreaTests.swift, first block, mirrored to a top-left origin. */
static void test_area_corners(void) {
    struct { area_corner corner; double px, py; rect expected; area_corner opposite; } cases[] = {
        {CORNER_TOP_LEFT, 80, 550, {80, 550, 340, 230}, CORNER_BOTTOM_RIGHT},
        {CORNER_TOP_RIGHT, 500, 550, {100, 550, 400, 230}, CORNER_BOTTOM_LEFT},
        {CORNER_BOTTOM_LEFT, 80, 810, {80, 600, 340, 210}, CORNER_TOP_RIGHT},
        {CORNER_BOTTOM_RIGHT, 500, 810, {100, 600, 400, 210}, CORNER_TOP_LEFT},
    };
    double origins[][2] = {{0, 0}, {-1920, -400}, {200, 1080}};
    for (int o = 0; o < 3; o++) {
        double ox = origins[o][0], oy = origins[o][1];
        rect display = {ox, oy, 1440, 900};
        rect r = {ox + 100, oy + 600, 320, 180};
        for (int i = 0; i < 4; i++) {
            area_corner c = cases[i].corner, opp = cases[i].opposite;
            double px = ox + cases[i].px, py = oy + cases[i].py;
            rect e = cases[i].expected;
            e.x += ox; e.y += oy;
            EXPECT(rect_equal(corner_resized(c, r, px, py, display, 2), e),
                   "Each corner adjusts width and height independently on offset displays");

            double ax, ay, cx, cy;
            corner_point(opp, r, &ax, &ay);
            corner_point(c, r, &cx, &cy);
            double outx = cx + (cx < ax ? -10000 : 10000), outy = cy + (cy < ay ? -10000 : 10000);
            rect expanded = corner_resized(c, r, outx, outy, display, 1);
            double ex, ey;
            corner_point(opp, expanded, &ex, &ey);
            EXPECT(rect_contains(display, expanded), "Dragging beyond the display keeps the region on that display");
            EXPECT(ex == ax && ey == ay, "Clamping keeps the opposite corner fixed");

            double crossx = ax + (cx < ax ? 100 : -100), crossy = ay + (cy < ay ? 100 : -100);
            rect minimum = corner_resized(c, r, crossx, crossy, display, 2);
            corner_point(opp, minimum, &ex, &ey);
            EXPECT(minimum.w == AREA_MIN_SIZE && minimum.h == AREA_MIN_SIZE,
                   "Dragging across the opposite corner retains a usable minimum");
            EXPECT(ex == ax && ey == ay, "Shrinking keeps the opposite corner fixed");

            rect aligned = corner_resized(c, r, px + 0.26, py + 0.26, display, 2);
            corner_point(c, aligned, &ex, &ey);
            EXPECT(ex == px + 0.5 && ey == py + 0.5, "HiDPI dragging aligns to physical pixels");
        }
    }
}

/* RecordingTests.swift: gain, clipping, file naming. */
static void test_audio_gain(void) {
    float quiet[] = {0, 0.1f, -0.25f, 0.5f};
    audio_level l = audio_gain_apply(quiet, 4, 0.5f);
    EXPECT(quiet[0] == 0 && quiet[1] == 0.05f && quiet[2] == -0.125f && quiet[3] == 0.25f,
           "The input slider changes actual recorded samples");
    EXPECT(!l.clipped && near(l.peak, 0.25), "The meter reports the gain-adjusted signal");
    float loud[] = {-0.8f, 0.8f};
    l = audio_gain_apply(loud, 2, 2);
    EXPECT(loud[0] == -1 && loud[1] == 1 && l.clipped, "Gain cannot wrap or overflow audio samples");
    float hot[] = {1};
    EXPECT(audio_gain_apply(hot, 1, 0.1f).clipped, "The meter warns about source clipping even after reducing gain");
    float invalid[] = {NAN, INFINITY};
    audio_gain_apply(invalid, 2, 1);
    EXPECT(invalid[0] == 0 && invalid[1] == 0, "Invalid input samples do not corrupt the audio encoder");
    EXPECT(audio_level_db((audio_level){0, 0}) == -60, "Silence reads -60 dB");
    EXPECT(near(audio_level_db((audio_level){1, 0}), 0), "Full scale reads 0 dB");
    EXPECT(gain_db_clamp(100) == GAIN_DB_MAX && gain_db_clamp(-100) == GAIN_DB_MIN && gain_db_clamp(NAN) == 0,
           "Gain stays within -36...+12 dB");
    EXPECT(near(gain_from_db(0), 1), "0 dB is unity gain");
}

static void test_files_and_labels(void) {
    char *dir = g_dir_make_tmp("youcam-tests-XXXXXX", NULL);
    time_t when = 1790066001;
    char *first = recording_next_path(dir, when);
    g_file_set_contents(first, "existing movie", -1, NULL);
    char *second = recording_next_path(dir, when);
    EXPECT(strcmp(first, second) != 0 && g_str_has_suffix(second, "-2.mov"),
           "Automatic file naming never overwrites an existing recording");
    EXPECT(!strchr(strrchr(second, '/'), ':'), "Recording filenames are safe in file managers");
    EXPECT(strstr(first, "/YouCam-Recording-") != NULL, "Movies use the macOS naming scheme");
    g_unlink(first);
    g_rmdir(dir);
    g_free(first); g_free(second); g_free(dir);

    char label[16];
    recording_time_label(3725.9, label, sizeof(label));
    EXPECT(strcmp(label, "01:02:05") == 0, "The timer shows hours, minutes and seconds");
    recording_time_label(-3, label, sizeof(label));
    EXPECT(strcmp(label, "00:00:00") == 0, "The timer never goes negative");
}

static void test_crop(void) {
    int l, r, t, b;
    crop_pixels((rect){0.25, 0.25, 0.5, 0.5}, 3840, 2160, &l, &r, &t, &b);
    EXPECT(l == 960 && r == 960 && t == 540 && b == 540, "HiDPI streams crop the same logical area");
    crop_pixels((rect){0.1, 0.1, 0.333, 0.333}, 1001, 999, &l, &r, &t, &b);
    EXPECT((1001 - l - r) % 2 == 0 && (999 - t - b) % 2 == 0, "Large displays produce encoder-compatible dimensions");
    crop_pixels((rect){0.9, 0.9, 0.1, 0.1}, 100, 100, &l, &r, &t, &b);
    EXPECT(l == 90 && r == 0 && t == 90 && b == 0, "Crops at the edge stay inside the frame");
}

/* RecordingPanelTests.swift: transport buttons offer only valid actions. */
static void test_panel_buttons(void) {
    struct { recording_phase phase; int play, pause, stop; } cases[] = {
        {PHASE_IDLE, 1, 0, 0}, {PHASE_STARTING, 0, 0, 1}, {PHASE_RECORDING, 0, 1, 1},
        {PHASE_PAUSED, 1, 0, 1}, {PHASE_FINISHING, 0, 0, 0},
    };
    for (int i = 0; i < 5; i++) {
        panel_buttons b = panel_buttons_for(cases[i].phase, 0);
        EXPECT(b.play == cases[i].play && b.pause == cases[i].pause && b.stop == cases[i].stop,
               "Transport buttons offer only valid actions");
        EXPECT(b.area == !PHASE_BUSY(cases[i].phase), "The area stays locked while busy");
    }
    panel_buttons b = panel_buttons_for(PHASE_IDLE, 1);
    EXPECT(!b.play && !b.area, "Selecting a region prevents recording before the selection is complete");
}

/* RecordingAreaPreferences: corrupt or impossible areas fall back to the
 * whole screen. Needs a display, so it is skipped when there is none. */
static void test_prefs(void) {
    char *dir = g_dir_make_tmp("youcam-prefs-XXXXXX", NULL);
    char *path = g_build_filename(dir, "prefs.ini", NULL);
    g_setenv("YOUCAM_CONFIG", path, TRUE);
    prefs_load();
    EXPECT(Prefs.shape == SHAPE_CIRCLE && Prefs.mirrored && Prefs.width == OVERLAY_DEFAULT_WIDTH,
           "Defaults: circle, mirrored, 280 points");
    EXPECT(Prefs.center_stage, "Center Stage is enabled by default");
    Prefs.shape = SHAPE_RECTANGLE;
    Prefs.gain_db = -6;
    Prefs.has_origin = 1; Prefs.origin_x = 12; Prefs.origin_y = 34;
    Prefs.area = (recording_area){AREA_REGION, "HDMI-1", {10, 20, 300, 200}};
    prefs_set_string(&Prefs.restore_token, "token");
    Prefs.transparency = 0.55;
    Prefs.record_transparency = 1;
    Prefs.center_stage = 0;
    prefs_save();
    memset(&Prefs, 0, sizeof(Prefs));
    prefs_load();
    EXPECT(Prefs.shape == SHAPE_RECTANGLE && Prefs.gain_db == -6 && Prefs.origin_y == 34,
           "Preferences survive a reload");
    EXPECT(Prefs.area.mode == AREA_REGION && strcmp(Prefs.area.monitor, "HDMI-1") == 0 &&
           rect_equal(Prefs.area.local, (rect){10, 20, 300, 200}), "The recording area survives a reload");
    EXPECT(strcmp(Prefs.restore_token, "token") == 0, "The shared monitor is remembered");
    EXPECT(Prefs.transparency == 0.55 && Prefs.record_transparency, "Both transparency choices are remembered");
    EXPECT(!Prefs.center_stage, "Turning Center Stage off is remembered");
    recording_area missing = prefs_area_validate(Prefs.area, NULL);
    EXPECT(missing.mode == AREA_WHOLE_SCREEN, "An area on a disconnected display falls back to the whole screen");
    g_file_set_contents(path, "[youcam]\nwidth=banana\ncameraPreviewTransparency=5\n", -1, NULL);
    prefs_load();
    EXPECT(Prefs.width == OVERLAY_DEFAULT_WIDTH, "Corrupt values fall back to defaults");
    EXPECT(Prefs.transparency == TRANSPARENCY_MAX && !Prefs.record_transparency,
           "Saved transparency is normalized; the saved-video option is off by default");
    g_unlink(path);
    g_rmdir(dir);
    g_free(path); g_free(dir);
}

/* CameraTransparencyTests.swift: preferences and the opaque camera copy. */
static void test_transparency(void) {
    double inputs[][2] = {{-1, 0}, {2, 0.8}, {NAN, 0}, {INFINITY, 0}, {0.55, 0.55}};
    for (int i = 0; i < 5; i++)
        EXPECT(transparency_normalize(inputs[i][0]) == inputs[i][1], "Invalid values cannot make the preview invisible");
    EXPECT(!camera_layer_needed(0, 0, 1, 1), "An opaque preview is recorded as captured");
    EXPECT(camera_layer_needed(0.55, 0, 1, 1), "Saved video stays opaque by default even with a translucent preview");
    EXPECT(!camera_layer_needed(0.55, 1, 1, 1), "Applying transparency to the video records the capture as is");
    EXPECT(!camera_layer_needed(0.55, 0, 0, 1) && !camera_layer_needed(0.55, 0, 1, 0),
           "A hidden or stopped camera adds nothing to the movie");

    /* Mapping: negative display origin, region offset, 2x pixels. */
    rect monitor = {-1920, -300, 1920, 1080};
    camera_layer l = camera_layer_map((rect){-1800, 0, 200, 200}, monitor, (rect){100, 200, 640, 360},
                                      1280, 720, SHAPE_CIRCLE);
    EXPECT(rect_equal(l.frame, (rect){40, 200, 400, 400}) && l.radius == 200,
           "Camera mapping handles negative display coordinates, region offsets and HiDPI scale");
    int x, y, w, h;
    camera_layer edge = camera_layer_map((rect){-60, -60, 120, 120}, (rect){0, 0, 320, 180},
                                         (rect){0, 0, 320, 180}, 320, 180, SHAPE_CIRCLE);
    EXPECT(camera_layer_clip(edge, 320, 180, &x, &y, &w, &h) && x == 0 && y == 0 && w == 61 && h == 61,
           "A camera crossing the recording boundary is clipped to the output");
    camera_layer away = camera_layer_map((rect){500, 500, 120, 120}, (rect){0, 0, 320, 180},
                                         (rect){0, 0, 320, 180}, 320, 180, SHAPE_CIRCLE);
    EXPECT(!camera_layer_clip(away, 320, 180, &x, &y, &w, &h), "A camera outside the area is not drawn");

    /* Render: left half red, right half blue camera. */
    camera_frame f = {320, 180, 320 * 4, g_malloc0(320 * 180 * 4)};
    for (int py = 0; py < 180; py++)
        for (int px = 0; px < 320; px++) f.data[py * f.stride + px * 4 + (px < 160 ? 2 : 0)] = 255;
    rect screen = {0, 0, 320, 180};
    for (int s = 0; s < SHAPE_COUNT; s++) {
        double height = 120 / shape_aspect(s);
        for (int mirrored = 0; mirrored < 2; mirrored++) {
            camera_layer cl = camera_layer_map((rect){100, 30, 120, height}, screen, screen, 320, 180, s);
            EXPECT(camera_layer_clip(cl, 320, 180, &x, &y, &w, &h), "The layer is inside the movie");
            cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            cairo_t *cr = cairo_create(img);
            camera_layer_draw(cr, cl, x, y, &f, mirrored, STAGE_IDENTITY);
            cairo_destroy(cr);
            cairo_surface_flush(img);
            guint8 *p = cairo_image_surface_get_data(img);
            int stride = cairo_image_surface_get_stride(img), mid = (int)(30 + height / 2) - y;
            guint8 *left = p + mid * stride + (140 - x) * 4, *right = p + mid * stride + (180 - x) * 4;
            EXPECT(left[mirrored ? 0 : 2] > 250 && left[1] < 5 && left[3] == 255,
                   "Camera pixels remain fully opaque with correct mirroring");
            EXPECT(right[mirrored ? 2 : 0] > 250 && right[1] < 5, "The camera is cropped and mirrored like the preview");
            EXPECT(p[(30 - y) * stride + (100 - x) * 4 + 3] == 0, "Clipped corners reveal the recorded desktop");
            cairo_surface_destroy(img);
        }
    }
    g_free(f.data);
}

/* Center Stage (PR antirez/youcam#1): soft clamp, zoom, smoothing, the
 * transform, no black bars at the extremes, face loss, and pico. */
static void test_center_stage(void) {
    EXPECT(stage_soft_clamp(10, 0.4) == 0, "No pan room means no pan");
    EXPECT(near(stage_soft_clamp(0, 50), 0) && fabs(stage_soft_clamp(1000, 50)) < 50 &&
           stage_soft_clamp(-1000, 50) > -50, "The soft clamp never passes the limit");
    EXPECT(near(stage_soft_clamp(10, 50), 50 * tanh(10 / 50.0 * 0.95)), "The soft clamp follows tanh");

    stage_face far = {0.5, 0.5, 0.2, 0.3}, close = {0.5, 0.5, 0.6, 0.9};
    EXPECT(near(stage_zoom(&far, SHAPE_RECTANGLE), 1.28) && near(stage_zoom(&far, SHAPE_CIRCLE), 1.10),
           "Base zoom: 1.28 in a rectangle, 1.10 otherwise");
    EXPECT(near(stage_zoom(&close, SHAPE_CIRCLE), 1.02), "Zoom backs off to 1.02 when close");

    stage_face s, f = {0.5, 0.5, 0.2, 0.3};
    EXPECT(stage_smooth(&s, 0, f) && near(s.cx, 0.5), "The first face is taken as is");
    EXPECT(!stage_smooth(&s, 1, (stage_face){0.505, 0.505, 0.21, 0.3}), "Small movements are ignored");
    EXPECT(stage_smooth(&s, 1, (stage_face){0.6, 0.5, 0.2, 0.3}) && near(s.cx, 0.5 + 0.1 * 0.22),
           "Larger movements are followed smoothly");

    stage_transform t = stage_transform_for(NULL, 280, 280, SHAPE_CIRCLE, 1);
    EXPECT(t.zoom == 1 && t.x == 0 && t.y == 0, "No face: identity");
    stage_face left = {0.3, 0.5, 0.2, 0.3};
    t = stage_transform_for(&left, 280, 280, SHAPE_SQUARE, 0);
    EXPECT(t.x > 0 && t.y > 0, "A face left of centre moves the picture right; head room moves it down");
    stage_transform m = stage_transform_for(&left, 280, 280, SHAPE_SQUARE, 1);
    EXPECT(near(m.x, -t.x) && near(m.y, t.y), "Mirroring flips the horizontal pan");

    double masks[SHAPE_COUNT][2] = {{280, 280}, {280, 280}, {400, 225}};
    stage_face extremes[] = {{0.01, 0.01, 0.25, 0.35}, {0.99, 0.99, 0.25, 0.35},
                             {0.01, 0.99, 0.25, 0.35}, {0.99, 0.01, 0.25, 0.35}};
    for (int shape = 0; shape < SHAPE_COUNT; shape++) {
        double w = masks[shape][0], h = masks[shape][1], cw, ch;
        stage_canvas(w, h, &cw, &ch);
        for (int mirrored = 0; mirrored < 2; mirrored++) {
            for (size_t i = 0; i < G_N_ELEMENTS(extremes); i++) {
                t = stage_transform_for(&extremes[i], w, h, shape, mirrored);
                double left_edge = w / 2 + t.x * w - cw * t.zoom / 2, top_edge = h / 2 + t.y * h - ch * t.zoom / 2;
                EXPECT(left_edge <= 0.5 && left_edge + cw * t.zoom >= w - 0.5,
                       "Horizontal pan at the extremes never exposes black bars");
                EXPECT(top_edge <= 0.5 && top_edge + ch * t.zoom >= h - 0.5,
                       "Vertical pan at the extremes never exposes black bars");
            }
        }
    }

    center_stage_update(&left);
    EXPECT(center_stage_has_face(), "A detected face is followed");
    for (int i = 0; i < 4; i++) center_stage_update(NULL);
    EXPECT(center_stage_has_face(), "A few missed frames keep the face");
    center_stage_update(NULL);
    EXPECT(!center_stage_has_face(), "Losing the face goes back to identity");
    g_usleep(300000);
    t = center_stage_now(280, 280, SHAPE_CIRCLE, 1);
    EXPECT(near(t.zoom, 1) && near(t.x, 0) && near(t.y, 0), "After the animation the picture is identity");
    center_stage_update(&left);
    center_stage_reset();
    EXPECT(!center_stage_has_face(), "Turning Center Stage off forgets the face");

    int w = 640, h = 360;
    guint8 *gray = g_malloc(w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) gray[y * w + x] = (guint8)((x / 40 + y / 40) % 2 ? 200 : 40);
    stage_face none;
    EXPECT(!stage_detect(gray, w, h, &none), "pico finds no face in a checkerboard");
    memset(gray, 128, w * h);
    EXPECT(!stage_detect(gray, w, h, &none), "pico finds no face in a flat image");
    g_free(gray);
}

int main(void) {
    test_overlay_geometry();
    test_area_corners();
    test_audio_gain();
    test_files_and_labels();
    test_crop();
    test_panel_buttons();
    test_prefs();
    test_transparency();
    test_center_stage();
    printf("Passed %d logic checks.\n", Checks);
    return 0;
}
