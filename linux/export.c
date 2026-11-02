/* Choose After Recording (Linux addition): projects and their export.
 *
 * A project is an MKV holding the screen (H.264, as any recording), the
 * camera layer at screen size (FFV1: lossless, keeps alpha) and the
 * microphone (AAC). Exporting lays the layer over the screen with opacity
 *
 *     alpha = 1 - t / T_A
 *
 * where T_A is the transparency the camera already has in the screen
 * track: 1 when the Capture Exclusion extension kept the camera window out
 * of it, the preview transparency otherwise. The camera then ends up
 * exactly t transparent: its opacity is alpha + (1 - alpha)(1 - T_A) = 1 - t.
 *
 * The settings travel in the MKV header (the WritingApp field), on disk
 * from the first second: GStreamer writes tags only when the file is
 * finished, so a killed recording would lose them. A copy goes into the
 * COMMENT tag for players and ffprobe. */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <glib/gstdio.h>
#include "youcam.h"

#define PROJECT_MAGIC "YouCam project;"
#define PROJECT_HEADER_BYTES 16384   /* WritingApp sits near the start (byte 288 in our files). */
#define KEY_T_A ";YOUCAM_T_A="
#define KEY_PREVIEW ";YOUCAM_PREVIEW_TRANSPARENCY="
#define KEY_CONTAINER ";YOUCAM_CONTAINER="
#define KEY_AUDIO ";YOUCAM_AUDIO="
#define EXT_CHARS "abcdefghijklmnopqrstuvwxyz0123456789"

double later_max(double t_a) {
    return isfinite(t_a) && t_a > 0 ? fmin(t_a, TRANSPARENCY_MAX) : 0;
}

/* T_A = 0 (opaque preview, no extension): the camera in the screen track
 * is already opaque and the slider only offers 0, so the layer is opaque. */
double later_alpha(double t, double t_a) {
    if (!isfinite(t_a) || t_a <= 0) return 1;
    if (!isfinite(t)) t = 0;
    return fmin(1, fmax(0, 1 - t / t_a));
}

int project_info_format(const project_info *p, char *buf, size_t len) {
    char t_a[G_ASCII_DTOSTR_BUF_SIZE], preview[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(t_a, sizeof(t_a), "%.3f", p->t_a);   /* Never a decimal comma, whatever the locale. */
    g_ascii_formatd(preview, sizeof(preview), "%.3f", p->preview);
    int n = snprintf(buf, len, PROJECT_MAGIC "YOUCAM_T_A=%s" KEY_PREVIEW "%s" KEY_CONTAINER "%s" KEY_AUDIO "%d;",
                     t_a, preview, container_for(p->container)->ext, p->has_audio ? 1 : 0);
    return n > 0 && (size_t)n < len;
}

/* The number after key, ended by ';', within [0, max]. */
static int parse_value(const char *text, const char *key, double max, double *out) {
    const char *at = strstr(text, key);
    if (!at) return 0;
    at += strlen(key);
    char *end;
    double v = g_ascii_strtod(at, &end);
    if (end == at || *end != ';' || !isfinite(v) || v < 0 || v > max) return 0;
    *out = v;
    return 1;
}

/* Any file in pending/ can be passed here, truncated or not ours: every
 * field is checked, and nothing is read past len. */
int project_info_parse(const guint8 *bytes, size_t len, project_info *out) {
    size_t magic = strlen(PROJECT_MAGIC);
    for (size_t i = 0; i + magic <= len; i++) {
        if (memcmp(bytes + i, PROJECT_MAGIC, magic) != 0) continue;
        char text[PROJECT_INFO_MAX];
        size_t n = MIN(len - i, sizeof(text) - 1);
        memcpy(text, bytes + i, n);
        text[n] = 0;   /* Binary MKV bytes after the field may hold a NUL earlier: strstr stops there. */
        project_info p = {0};
        const char *ext = strstr(text, KEY_CONTAINER), *audio = strstr(text, KEY_AUDIO);
        if (!parse_value(text, KEY_T_A, 1, &p.t_a) || !parse_value(text, KEY_PREVIEW, TRANSPARENCY_MAX, &p.preview) ||
            !ext || !audio)
            return 0;
        ext += strlen(KEY_CONTAINER);
        audio += strlen(KEY_AUDIO);
        size_t k = strspn(ext, EXT_CHARS);
        if (k == 0 || k >= sizeof(p.container) || ext[k] != ';' || (audio[0] != '0' && audio[0] != '1') ||
            audio[1] != ';')
            return 0;
        char name[sizeof(p.container)];
        memcpy(name, ext, k);
        name[k] = 0;
        g_strlcpy(p.container, container_for(name)->ext, sizeof(p.container));   /* Unknown: MOV. */
        p.has_audio = audio[0] == '1';
        *out = p;
        return 1;
    }
    return 0;
}

int project_read_info(const char *path, project_info *out) {
    FILE *f = g_fopen(path, "rb");
    if (!f) return 0;
    guint8 *head = g_malloc(PROJECT_HEADER_BYTES);
    size_t n = fread(head, 1, PROJECT_HEADER_BYTES, f);
    fclose(f);
    int ok = project_info_parse(head, n, out);
    g_free(head);
    return ok;
}

char *project_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "youcam", "pending", NULL);
}

/* <dir>/<the final movie's name>.youcam.mkv, never overwriting. */
char *project_new_path(const char *dir, time_t when) {
    char *base = recording_base_name(when);
    char *path = g_strdup_printf("%s/%s" PROJECT_SUFFIX, dir, base);
    for (int suffix = 2; g_file_test(path, G_FILE_TEST_EXISTS); suffix++) {
        g_free(path);
        path = g_strdup_printf("%s/%s-%d" PROJECT_SUFFIX, dir, base, suffix);
    }
    g_free(base);
    return path;
}

/* The menu opens the newest project; older ones wait, without a list. */
char *project_latest(const char *dir) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return NULL;
    char *best = NULL;
    time_t best_time = 0;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, PROJECT_SUFFIX)) continue;
        char *path = g_build_filename(dir, name, NULL);
        GStatBuf st;
        if (g_stat(path, &st) == 0 && (!best || st.st_mtime > best_time)) {
            g_free(best);
            best = path;
            best_time = st.st_mtime;
        } else {
            g_free(path);
        }
    }
    g_dir_close(d);
    return best;
}

/* A .part left in pending/ by a crash is never useful. The Desktop is the
 * user's own folder: nothing there is touched. */
void project_clean_parts(const char *dir) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".part")) continue;
        char *path = g_build_filename(dir, name, NULL);
        if (g_unlink(path) != 0) g_printerr("youcam: could not remove %s\n", path);
        g_free(path);
    }
    g_dir_close(d);
}

/* <dir>/<project name without .youcam.mkv>.<ext>; an existing movie is
 * never overwritten: " (2)", " (3)"... before the extension. Caller frees. */
char *export_output_path(const char *dir, const char *project, const char *ext) {
    char *name = g_path_get_basename(project);
    if (g_str_has_suffix(name, PROJECT_SUFFIX)) name[strlen(name) - strlen(PROJECT_SUFFIX)] = 0;
    ext = container_for(ext)->ext;
    char *path = g_strdup_printf("%s/%s.%s", dir, name, ext);
    for (int n = 2; g_file_test(path, G_FILE_TEST_EXISTS); n++) {
        g_free(path);
        path = g_strdup_printf("%s/%s (%d).%s", dir, name, n, ext);
    }
    g_free(name);
    return path;
}
