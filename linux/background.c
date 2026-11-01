/* Background removal: the mode names, image helpers, the segmentation
 * worker thread and the layers camera_layer.c paints. */

#include <math.h>
#include <string.h>
#include "youcam.h"
#include "selfie_landscape.h"
#include "selfie_square.h"

const guint8 *background_model_bytes(seg_variant v, size_t *size) {
    *size = v == SEG_SQUARE ? selfie_square_ysm_len : selfie_landscape_ysm_len;
    return v == SEG_SQUARE ? selfie_square_ysm : selfie_landscape_ysm;
}

const char *const BackgroundNames[BG_COUNT] = {"off", "transparent", "blur", "image", "color"};
const char *const BackgroundTitles[BG_COUNT] = {"Off", "Transparent", "Blur", "Image", "Color"};

#define MASK_SMOOTHING 0.6   /* Weight of the newest mask: lower is steadier, higher follows faster. */
#define BLUR_FACTOR 16       /* The blurred background is computed this many times smaller... */
#define BLUR_RADIUS 2        /* ...with a box blur of this radius, then scaled up smoothly. */

background_mode background_mode_parse(const char *name) {
    for (int i = 0; name && i < BG_COUNT; i++)
        if (strcmp(name, BackgroundNames[i]) == 0) return i;
    return BG_OFF;
}

int background_color_parse(const char *text, double rgb[3]) {
    unsigned v;
    char end;
    if (!text || strlen(text) != 7 || sscanf(text, "#%6x%c", &v, &end) != 1) return 0;
    for (int i = 0; i < 3; i++) rgb[i] = ((v >> (16 - 8 * i)) & 0xff) / 255.0;
    return 1;
}

static void frame_resize(camera_frame *f, int w, int h) {
    if (f->width == w && f->height == h && f->data) return;
    f->data = g_realloc(f->data, (size_t)w * h * 4);
    f->width = w;
    f->height = h;
    f->stride = w * 4;
}

/* Box-average shrink of a BGRx frame, to packed RGB (rgb = 1) or BGRx. */
static void shrink(const guint8 *bgrx, int w, int h, int stride, guint8 *out, int ow, int oh, int rgb) {
    int bpp = rgb ? 3 : 4;
    for (int oy = 0; oy < oh; oy++) {
        int y0 = oy * h / oh, y1 = MAX(y0 + 1, (oy + 1) * h / oh);
        for (int ox = 0; ox < ow; ox++) {
            int x0 = ox * w / ow, x1 = MAX(x0 + 1, (ox + 1) * w / ow);
            unsigned sum[3] = {0, 0, 0}, n = (unsigned)((y1 - y0) * (x1 - x0));
            for (int y = y0; y < y1; y++) {
                const guint8 *p = bgrx + (size_t)y * stride + (size_t)x0 * 4;
                for (int x = x0; x < x1; x++, p += 4) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; }
            }
            guint8 *o = out + ((size_t)oy * ow + ox) * bpp;
            for (int c = 0; c < 3; c++) o[rgb ? 2 - c : c] = (guint8)((sum[c] + n / 2) / n);
            if (!rgb) o[3] = 255;
        }
    }
}

void frame_to_model(const guint8 *bgrx, int w, int h, int stride, guint8 *rgb, int mw, int mh) {
    shrink(bgrx, w, h, stride, rgb, mw, mh, 1);
}

void mask_smooth(guint8 *mask, const float *raw, int n, int has_previous) {
    for (int i = 0; i < n; i++) {
        double v = fmin(fmax(raw[i], 0), 1) * 255;
        if (has_previous) v = MASK_SMOOTHING * v + (1 - MASK_SMOOTHING) * mask[i];
        mask[i] = (guint8)lround(v);
    }
}

/* The camera frame with the mask (bilinear, half-pixel centers) as alpha:
 * premultiplied ARGB32, the person surface painted over the backdrop.
 * ponytail: per-pixel float math, ~2-4 ms at 720p; precompute the x taps
 * per row if it ever shows in a profile. */
void mask_compose(const camera_frame *f, const guint8 *mask, int mw, int mh, camera_frame *out) {
    frame_resize(out, f->width, f->height);
    for (int y = 0; y < f->height; y++) {
        double my = fmax(0, (y + 0.5) * mh / f->height - 0.5);
        int y0 = MIN((int)my, mh - 1), y1 = MIN(y0 + 1, mh - 1);
        double fy = my - y0;
        const guint8 *p = f->data + (size_t)y * f->stride, *r0 = mask + (size_t)y0 * mw, *r1 = mask + (size_t)y1 * mw;
        guint8 *o = out->data + (size_t)y * out->stride;
        for (int x = 0; x < f->width; x++, p += 4, o += 4) {
            double mx = fmax(0, (x + 0.5) * mw / f->width - 0.5);
            int x0 = MIN((int)mx, mw - 1), x1 = MIN(x0 + 1, mw - 1);
            double fx = mx - x0;
            double top = r0[x0] + (r0[x1] - r0[x0]) * fx, bottom = r1[x0] + (r1[x1] - r1[x0]) * fx;
            unsigned a = (unsigned)lround(top + (bottom - top) * fy);
            for (int c = 0; c < 3; c++) o[c] = (guint8)((p[c] * a + 127) / 255);
            o[3] = (guint8)a;
        }
    }
}

static void box_pass(const guint8 *in, guint8 *out, int w, int h, int dx, int dy) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                unsigned sum = 0;
                for (int k = -BLUR_RADIUS; k <= BLUR_RADIUS; k++) {
                    int sx = CLAMP(x + k * dx, 0, w - 1), sy = CLAMP(y + k * dy, 0, h - 1);
                    sum += in[((size_t)sy * w + sx) * 4 + c];
                }
                out[((size_t)y * w + x) * 4 + c] = (guint8)(sum / (2 * BLUR_RADIUS + 1));
            }
}

/* A small, blurred copy of the frame (BGRx), painted scaled up behind the person. */
void blur_small(const camera_frame *f, camera_frame *out) {
    int w = MAX(1, f->width / BLUR_FACTOR), h = MAX(1, f->height / BLUR_FACTOR);
    frame_resize(out, w, h);
    guint8 *tmp = g_malloc((size_t)w * h * 4);
    shrink(f->data, f->width, f->height, f->stride, out->data, w, h, 0);
    memcpy(tmp, out->data, (size_t)w * h * 4);   /* Keeps the 255 alpha bytes. */
    box_pass(out->data, tmp, w, h, 1, 0);
    box_pass(tmp, out->data, w, h, 0, 1);
    g_free(tmp);
}
