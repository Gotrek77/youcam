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
    Prefs.shape = SHAPE_RECTANGLE;
    Prefs.gain_db = -6;
    Prefs.has_origin = 1; Prefs.origin_x = 12; Prefs.origin_y = 34;
    Prefs.area = (recording_area){AREA_REGION, "HDMI-1", {10, 20, 300, 200}};
    prefs_set_string(&Prefs.restore_token, "token");
    prefs_save();
    memset(&Prefs, 0, sizeof(Prefs));
    prefs_load();
    EXPECT(Prefs.shape == SHAPE_RECTANGLE && Prefs.gain_db == -6 && Prefs.origin_y == 34,
           "Preferences survive a reload");
    EXPECT(Prefs.area.mode == AREA_REGION && strcmp(Prefs.area.monitor, "HDMI-1") == 0 &&
           rect_equal(Prefs.area.local, (rect){10, 20, 300, 200}), "The recording area survives a reload");
    EXPECT(strcmp(Prefs.restore_token, "token") == 0, "The shared monitor is remembered");
    recording_area missing = prefs_area_validate(Prefs.area, NULL);
    EXPECT(missing.mode == AREA_WHOLE_SCREEN, "An area on a disconnected display falls back to the whole screen");
    g_file_set_contents(path, "[youcam]\nwidth=banana\n", -1, NULL);
    prefs_load();
    EXPECT(Prefs.width == OVERLAY_DEFAULT_WIDTH, "Corrupt values fall back to defaults");
    g_unlink(path);
    g_rmdir(dir);
    g_free(path); g_free(dir);
}

int main(void) {
    test_overlay_geometry();
    test_area_corners();
    test_audio_gain();
    test_files_and_labels();
    test_crop();
    test_panel_buttons();
    test_prefs();
    printf("Passed %d logic checks.\n", Checks);
    return 0;
}
