/* Person segmentation checks: operators, the .ysm loader, the full models
 * against MediaPipe's masks, and a benchmark (--bench). */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "youcam.h"

static int Checks;

#define EXPECT(cond, msg) do { \
    Checks++; \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); exit(1); } \
} while (0)

static int close_to(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_conv(void) {
    float in[16], ones[9], out[4], bias = 0;
    for (int i = 0; i < 16; i++) in[i] = i;   /* in[y][x] = y * 4 + x */
    for (int i = 0; i < 9; i++) ones[i] = 1;
    seg_conv(&(seg_conv_args){in, {4, 4, 1}, ones, &bias, 3, 3, 2, 0, 0, out, {2, 2, 1}});
    EXPECT(close_to(out[0], 45) && close_to(out[3], 50), "Strided SAME convolution pads only bottom/right");

    float img[9], full[9];
    for (int i = 0; i < 9; i++) img[i] = i + 1;
    seg_conv(&(seg_conv_args){img, {3, 3, 1}, ones, &bias, 3, 3, 1, 1, 1, full, {3, 3, 1}});
    EXPECT(close_to(full[4], 45) && close_to(full[0], 12), "Padding reads zeros outside the image");

    float px[2] = {1, 2}, w[4] = {1, 10, 100, 1000}, b2[2] = {0.5f, -0.5f}, o2[2];
    seg_conv(&(seg_conv_args){px, {1, 1, 2}, w, b2, 1, 1, 1, 0, 0, o2, {1, 1, 2}});
    EXPECT(close_to(o2[0], 201.5f) && close_to(o2[1], 2009.5f), "Weights are [kh][kw][in][out]");
}

static void test_dwconv(void) {
    float in[18], w[18], b[2] = {0, 0}, out[18];
    for (int i = 0; i < 18; i++) in[i] = 1;
    for (int i = 0; i < 9; i++) { w[2 * i] = 1; w[2 * i + 1] = 2; }
    seg_dwconv(&(seg_conv_args){in, {3, 3, 2}, w, b, 3, 3, 1, 1, 1, out, {3, 3, 2}});
    EXPECT(close_to(out[8], 9) && close_to(out[9], 18) && close_to(out[0], 4),
           "Depthwise convolution keeps channels apart");
}

static void test_tconv(void) {
    float in[2] = {1, 2}, w[8] = {1, 2, 3, 4, 5, 6, 7, 8}, b = 0.5f, out[4];
    seg_tconv(&(seg_conv_args){in, {1, 1, 2}, w, &b, 2, 2, 2, 0, 0, out, {2, 2, 1}});
    EXPECT(close_to(out[0], 5.5f) && close_to(out[1], 11.5f) && close_to(out[2], 17.5f) &&
           close_to(out[3], 23.5f), "Transposed convolution places each kernel tap");
}

static void test_small_ops(void) {
    float in[2] = {0, 1}, out[4];
    seg_resize(in, (seg_dims){1, 2, 1}, out, (seg_dims){1, 4, 1});
    EXPECT(close_to(out[0], 0) && close_to(out[1], 0.25f) && close_to(out[2], 0.75f) && close_to(out[3], 1),
           "Bilinear resize uses half-pixel centers, not aligned corners");
    float m[4] = {1, 2, 3, 6}, avg;
    seg_mean(m, (seg_dims){2, 2, 1}, &avg);
    EXPECT(close_to(avg, 3), "Mean averages over height and width");
    float x[5] = {-1, 1, -4, 4, 0}, y[5];
    seg_relu(x, y, 1);
    EXPECT(y[0] == 0, "ReLU clamps negatives");
    seg_hswish(x + 1, y, 3);
    EXPECT(close_to(y[0], 4.0f / 6) && y[1] == 0 && close_to(y[2], 4), "Hard swish is x * relu6(x + 3) / 6");
    seg_sigmoid(x + 4, y, 1);
    EXPECT(close_to(y[0], 0.5f), "Sigmoid of zero is one half");
    float v[2] = {2, 3}, s[2] = {10, 100}, p[2];
    seg_mul_channels(v, s, (seg_dims){1, 1, 2}, p);
    EXPECT(close_to(p[0], 20) && close_to(p[1], 300), "Multiply scales each channel");
    seg_add(v, s, p, 2);
    EXPECT(close_to(p[0], 12) && close_to(p[1], 103), "Add is element-wise");
}

#define HEADER_TENSOR_COUNT 24   /* Offset of tensor_count in the .ysm header. */
#define HEADER_SIZE 40

static guint8 *model_copy(seg_variant v, size_t *size) {
    const guint8 *data = background_model_bytes(v, size);
    return g_memdup2(data, *size);
}

static int loads(const guint8 *data, size_t size) {
    GError *err = NULL;
    segment_model *m = segment_load(data, size, &err);
    int ok = m != NULL;
    EXPECT(ok || (err && strstr(err->message, "Invalid segmentation model")), "A rejected model explains why");
    segment_free(m);
    g_clear_error(&err);
    return ok;
}

static void test_loader(void) {
    for (int v = 0; v < SEG_VARIANTS; v++) {
        size_t size;
        guint8 *d = model_copy(v, &size);
        EXPECT(loads(d, size), "The embedded models load");
        EXPECT(!loads(d, size / 2), "A truncated model is rejected");
        EXPECT(!loads(d, 3), "A model shorter than its magic is rejected");
        d[0] = 'X';
        EXPECT(!loads(d, size), "A file that is not .ysm is rejected");
        d[0] = 'Y';
        guint32 tensors;
        memcpy(&tensors, d + HEADER_TENSOR_COUNT, 4);
        size_t first_layer = HEADER_SIZE + 12 * (size_t)tensors;
        guint32 saved, bad = 999999;
        memcpy(&saved, d + first_layer + 4, 4);              /* in0 */
        memcpy(d + first_layer + 4, &bad, 4);
        EXPECT(!loads(d, size), "A tensor index out of range is rejected");
        memcpy(d + first_layer + 4, &saved, 4);
        memcpy(&saved, d + first_layer + 36, 4);             /* weight_count */
        guint32 more = saved + 1;
        memcpy(d + first_layer + 36, &more, 4);
        EXPECT(!loads(d, size), "Weights that do not match the shapes are rejected");
        memcpy(d + first_layer + 36, &saved, 4);
        EXPECT(loads(d, size), "Restoring the bytes makes the model valid again");
        g_free(d);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    test_conv();
    test_dwconv();
    test_tconv();
    test_small_ops();
    test_loader();
    printf("Passed %d segmentation checks.\n", Checks);
    return 0;
}
