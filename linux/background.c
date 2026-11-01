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
#define IMAGE_MAX_SIDE 1280  /* A background image is kept at most this long: a camera sized window. */

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
 * Plain per-pixel float math costs ~2-4 ms at 720p; the x taps could be
 * computed once per frame instead of once per row if a profile ever asks. */
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

/* A picture as it should be drawn: phones store photos sideways plus an
 * EXIF orientation, and a 12 MP image costs tens of milliseconds to paint,
 * so it is turned upright and shrunk once here instead of on every frame. */
GdkPixbuf *background_image_fit(GdkPixbuf *image) {
    GdkPixbuf *upright = gdk_pixbuf_apply_embedded_orientation(image);
    if (!upright) upright = g_object_ref(image);   /* Rotating ran out of memory: keep it sideways. */
    int w = gdk_pixbuf_get_width(upright), h = gdk_pixbuf_get_height(upright);
    if (MAX(w, h) <= IMAGE_MAX_SIDE) return upright;
    double scale = (double)IMAGE_MAX_SIDE / MAX(w, h);
    int sw = MAX(1, (int)lround(w * scale)), sh = MAX(1, (int)lround(h * scale));
    GdkPixbuf *small = gdk_pixbuf_scale_simple(upright, sw, sh, GDK_INTERP_BILINEAR);
    if (!small) return upright;   /* Out of memory: slow to paint, but still right. */
    g_object_unref(upright);
    return small;
}

/* ------------------------------------------------------------- worker --- */

#define SQUARE_BELOW_ASPECT 1.5   /* Cameras narrower than this use the 256x256 model. */
#define SEG_MAX_INPUT (256 * 256 * 3)

typedef struct { guint8 *mask; int w, h; } published_mask;

static struct {
    GMutex lock;
    GCond wake;
    GThread *thread;
    int quit, pending;            /* pending: input holds a frame the worker is running. */
    seg_variant variant;
    guint8 input[SEG_MAX_INPUT];
    int available;                /* -1 not checked yet. */
    int model_w[SEG_VARIANTS], model_h[SEG_VARIANTS];
    /* Main thread only. */
    guint8 *mask;
    int mask_w, mask_h;
    background_layer layer;
    char *image_path;             /* The path B.layer.image was loaded from. */
} B = {.available = -1};

int background_available(void) {
    if (B.available >= 0) return B.available;
    B.available = 1;
    for (int v = 0; v < SEG_VARIANTS; v++) {
        size_t size;
        const guint8 *data = background_model_bytes(v, &size);
        GError *err = NULL;
        segment_model *m = segment_load(data, size, &err);
        if (!m) {
            g_printerr("youcam: background removal unavailable: %s\n", err->message);
            g_clear_error(&err);
            B.available = 0;
            return 0;
        }
        segment_input_size(m, &B.model_w[v], &B.model_h[v]);
        if ((size_t)B.model_w[v] * B.model_h[v] * 3 > SEG_MAX_INPUT) B.available = 0;
        segment_free(m);
    }
    return B.available;
}

static gboolean deliver_mask(gpointer data) {
    published_mask *p = data;
    if (Prefs.background_mode == BG_OFF) {
        g_free(p->mask);
    } else {
        g_free(B.mask);
        B.mask = p->mask;
        B.mask_w = p->w;
        B.mask_h = p->h;
    }
    g_free(p);
    return G_SOURCE_REMOVE;
}

/* Runs the model on the latest frame the camera thread left in B.input. */
static gpointer worker(gpointer unused) {
    (void)unused;
    segment_model *model = NULL;
    int loaded = -1, has_previous = 0, n = 0, load_failed = 0;
    float *raw = NULL;
    guint8 *smoothed = NULL;
    g_mutex_lock(&B.lock);
    for (;;) {
        while (!B.pending && !B.quit) g_cond_wait(&B.wake, &B.lock);
        if (B.quit) break;
        seg_variant v = B.variant;
        g_mutex_unlock(&B.lock);
        if ((int)v != loaded) {   /* First frame, or the camera changed shape. */
            size_t size;
            const guint8 *data = background_model_bytes(v, &size);
            segment_free(model);
            /* The bytes were checked by background_available(), but the
             * activations are a large allocation that may still fail. */
            model = segment_load(data, size, NULL);
            if (!model) {
                if (!load_failed) g_printerr("youcam: background removal model did not load, skipping frames\n");
                load_failed = 1;
                loaded = -1;   /* Retry on the next frame, never run a NULL model. */
                g_mutex_lock(&B.lock);
                B.pending = 0;
                continue;
            }
            loaded = v;
            has_previous = 0;
            n = B.model_w[v] * B.model_h[v];
            raw = g_renew(float, raw, n);
            smoothed = g_renew(guint8, smoothed, n);
        }
        segment_run(model, B.input, raw);
        mask_smooth(smoothed, raw, n, has_previous);
        has_previous = 1;
        published_mask *p = g_new(published_mask, 1);
        *p = (published_mask){g_memdup2(smoothed, n), B.model_w[v], B.model_h[v]};
        g_idle_add(deliver_mask, p);
        g_mutex_lock(&B.lock);
        B.pending = 0;
    }
    g_mutex_unlock(&B.lock);
    segment_free(model);
    g_free(raw);
    g_free(smoothed);
    return NULL;
}

/* Camera thread, every frame. Never queues: while the worker is busy the
 * frame is skipped, so a slow model lowers the mask rate, not the preview's. */
void background_feed(const guint8 *bgrx, int w, int h, int stride) {
    if (Prefs.background_mode == BG_OFF) return;
    g_mutex_lock(&B.lock);
    if (B.thread && !B.pending) {
        seg_variant v = (double)w / h < SQUARE_BELOW_ASPECT ? SEG_SQUARE : SEG_LANDSCAPE;
        frame_to_model(bgrx, w, h, stride, B.input, B.model_w[v], B.model_h[v]);
        B.variant = v;
        B.pending = 1;
        g_cond_signal(&B.wake);
    }
    g_mutex_unlock(&B.lock);
}

/* B.thread changes under the lock because background_feed reads it on the
 * camera thread. Stopping joins the worker, which frees the model. */
void background_apply_prefs(void) {
    int wanted = Prefs.background_mode != BG_OFF && background_available();
    g_mutex_lock(&B.lock);
    GThread *stopping = NULL;
    if (wanted && !B.thread) {
        B.quit = 0;
        B.pending = 0;
        B.thread = g_thread_new("youcam-segment", worker, NULL);
    } else if (!wanted && B.thread) {
        B.quit = 1;
        g_cond_signal(&B.wake);
        stopping = B.thread;
        B.thread = NULL;
    }
    g_mutex_unlock(&B.lock);
    if (stopping) {
        g_thread_join(stopping);
        g_clear_pointer(&B.mask, g_free);
    }
}

/* The chosen image, loaded once per path; NULL (and one log line) if it cannot be read. */
static cairo_surface_t *background_image(void) {
    const char *path = Prefs.background_image;
    if (g_strcmp0(path, B.image_path) == 0) return B.layer.image;
    g_clear_pointer(&B.layer.image, cairo_surface_destroy);
    prefs_set_string(&B.image_path, path);
    GError *err = NULL;
    GdkPixbuf *loaded = path ? gdk_pixbuf_new_from_file(path, &err) : NULL;
    if (!loaded) {
        g_printerr("youcam: background image unavailable, using Blur: %s\n", err ? err->message : "none chosen");
        g_clear_error(&err);
        return NULL;
    }
    /* Converted to a cairo surface here, not by every paint. Drawn by hand
     * because gdk_cairo_surface_create_from_pixbuf wants a display. */
    GdkPixbuf *fitted = background_image_fit(loaded);
    B.layer.image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gdk_pixbuf_get_width(fitted),
                                               gdk_pixbuf_get_height(fitted));
    cairo_t *cr = cairo_create(B.layer.image);
    gdk_cairo_set_source_pixbuf(cr, fitted, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    g_object_unref(fitted);
    g_object_unref(loaded);
    return B.layer.image;
}

/* Recomposed on every call (preview and recording layer), about 2-4 ms each
 * at 720p; worth caching per camera frame only if it ever shows up. */
const background_layer *background_now(const camera_frame *f) {
    background_mode mode = Prefs.background_mode;
    if (mode == BG_OFF || !B.mask || !f) return NULL;
    background_layer *l = &B.layer;
    mask_compose(f, B.mask, B.mask_w, B.mask_h, &l->person);
    l->mode = mode;
    if (mode == BG_IMAGE && !background_image()) l->mode = BG_BLUR;
    if (l->mode == BG_BLUR) blur_small(f, &l->blurred);
    if (l->mode == BG_COLOR && !background_color_parse(Prefs.background_color, l->rgb))
        background_color_parse(BACKGROUND_DEFAULT_COLOR, l->rgb);
    return l;
}
