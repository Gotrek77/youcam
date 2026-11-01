/* Person segmentation: a tiny inference engine for MediaPipe Selfie
 * Segmentation, converted offline to .ysm by tools/convert_selfie.py.
 * Tensors are NHWC float32 with batch 1; one function per operator.
 * Convolutions keep the output channels innermost so the compiler can
 * vectorise them; HOT builds an AVX2/FMA clone picked at load time. */

#include <limits.h>
#include <math.h>
#include <string.h>
#include "youcam.h"

#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__)
#define HOT __attribute__((target_clones("arch=haswell", "default")))
#else
#define HOT
#endif

/* ponytail: one thread. Split rows across a GThreadPool here if the
 * benchmark misses its budget (Task 7). */
static void run_rows(void (*fn)(const seg_conv_args *, int, int), const seg_conv_args *a) {
    fn(a, 0, a->out_dims.h);
}

HOT static void conv_rows(const seg_conv_args *a, int y0, int y1) {
    int ic = a->in_dims.c, oc = a->out_dims.c;
    for (int oy = y0; oy < y1; oy++)
        for (int ox = 0; ox < a->out_dims.w; ox++) {
            float *restrict o = a->out + ((size_t)oy * a->out_dims.w + ox) * oc;
            memcpy(o, a->bias, oc * sizeof(float));
            for (int ky = 0; ky < a->kh; ky++) {
                int iy = oy * a->stride - a->pad_top + ky;
                if (iy < 0 || iy >= a->in_dims.h) continue;
                for (int kx = 0; kx < a->kw; kx++) {
                    int ix = ox * a->stride - a->pad_left + kx;
                    if (ix < 0 || ix >= a->in_dims.w) continue;
                    const float *px = a->in + ((size_t)iy * a->in_dims.w + ix) * ic;
                    const float *wk = a->weights + (size_t)(ky * a->kw + kx) * ic * oc;
                    for (int c = 0; c < ic; c++) {
                        float v = px[c];
                        const float *restrict wr = wk + (size_t)c * oc;
                        for (int k = 0; k < oc; k++) o[k] += v * wr[k];
                    }
                }
            }
        }
}

HOT static void dwconv_rows(const seg_conv_args *a, int y0, int y1) {
    int ch = a->in_dims.c;
    for (int oy = y0; oy < y1; oy++)
        for (int ox = 0; ox < a->out_dims.w; ox++) {
            float *restrict o = a->out + ((size_t)oy * a->out_dims.w + ox) * ch;
            memcpy(o, a->bias, ch * sizeof(float));
            for (int ky = 0; ky < a->kh; ky++) {
                int iy = oy * a->stride - a->pad_top + ky;
                if (iy < 0 || iy >= a->in_dims.h) continue;
                for (int kx = 0; kx < a->kw; kx++) {
                    int ix = ox * a->stride - a->pad_left + kx;
                    if (ix < 0 || ix >= a->in_dims.w) continue;
                    const float *restrict px = a->in + ((size_t)iy * a->in_dims.w + ix) * ch;
                    const float *restrict wk = a->weights + (size_t)(ky * a->kw + kx) * ch;
                    for (int c = 0; c < ch; c++) o[c] += px[c] * wk[c];
                }
            }
        }
}

HOT static void tconv_rows(const seg_conv_args *a, int y0, int y1) {
    int ic = a->in_dims.c, oc = a->out_dims.c, k = a->stride;
    for (int oy = y0; oy < y1; oy++)
        for (int ox = 0; ox < a->out_dims.w; ox++) {
            float *restrict o = a->out + ((size_t)oy * a->out_dims.w + ox) * oc;
            memcpy(o, a->bias, oc * sizeof(float));
            const float *px = a->in + ((size_t)(oy / k) * a->in_dims.w + ox / k) * ic;
            const float *wk = a->weights + (size_t)((oy % k) * k + ox % k) * ic * oc;
            for (int c = 0; c < ic; c++) {
                float v = px[c];
                const float *restrict wr = wk + (size_t)c * oc;
                for (int j = 0; j < oc; j++) o[j] += v * wr[j];
            }
        }
}

void seg_conv(const seg_conv_args *a) { run_rows(conv_rows, a); }
void seg_dwconv(const seg_conv_args *a) { run_rows(dwconv_rows, a); }
void seg_tconv(const seg_conv_args *a) { run_rows(tconv_rows, a); }

/* TFLite RESIZE_BILINEAR with half_pixel_centers and no align_corners. */
void seg_resize(const float *in, seg_dims di, float *out, seg_dims dout) {
    double sy = (double)di.h / dout.h, sx = (double)di.w / dout.w;
    for (int oy = 0; oy < dout.h; oy++) {
        double fy = fmax(0, (oy + 0.5) * sy - 0.5);
        int y0 = MIN((int)fy, di.h - 1), y1 = MIN(y0 + 1, di.h - 1);
        float dy = (float)(fy - y0);
        for (int ox = 0; ox < dout.w; ox++) {
            double fx = fmax(0, (ox + 0.5) * sx - 0.5);
            int x0 = MIN((int)fx, di.w - 1), x1 = MIN(x0 + 1, di.w - 1);
            float dx = (float)(fx - x0);
            const float *a = in + ((size_t)y0 * di.w + x0) * di.c, *b = in + ((size_t)y0 * di.w + x1) * di.c;
            const float *c = in + ((size_t)y1 * di.w + x0) * di.c, *d = in + ((size_t)y1 * di.w + x1) * di.c;
            float *o = out + ((size_t)oy * dout.w + ox) * dout.c;
            for (int k = 0; k < di.c; k++) {
                float top = a[k] + (b[k] - a[k]) * dx, bottom = c[k] + (d[k] - c[k]) * dx;
                o[k] = top + (bottom - top) * dy;
            }
        }
    }
}

void seg_mean(const float *in, seg_dims di, float *out) {
    size_t n = (size_t)di.h * di.w;
    for (int c = 0; c < di.c; c++) out[c] = 0;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < di.c; c++) out[c] += in[i * di.c + c];
    for (int c = 0; c < di.c; c++) out[c] /= n;
}

void seg_relu(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = in[i] > 0 ? in[i] : 0;
}

void seg_hswish(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = in[i] * fminf(fmaxf(in[i] + 3, 0), 6) / 6;
}

void seg_sigmoid(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = 1 / (1 + expf(-in[i]));
}

void seg_mul_channels(const float *in, const float *scale, seg_dims d, float *out) {
    size_t n = (size_t)d.h * d.w;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < d.c; c++) out[i * d.c + c] = in[i * d.c + c] * scale[c];
}

void seg_add(const float *a, const float *b, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = a[i] + b[i];
}
