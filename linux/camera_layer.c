/* Opaque camera layer for recordings (RecordingCameraOverlay.swift).
 *
 * On Linux the portal captures the camera window like any other window. When
 * the preview is translucent but the user wants an opaque camera in the
 * movie, the writer composites an opaque copy of the camera exactly over the
 * captured translucent one. These functions map the camera window into
 * output pixels and draw that copy; they are pure and covered by tests.c. */

#include <math.h>
#include "youcam.h"

#define CAMERA_CORNER_RADIUS 14
#define HALO_PX 1   /* Grow the copy slightly so no translucent edge shows. */

double transparency_normalize(double value) {
    return isfinite(value) ? fmin(TRANSPARENCY_MAX, fmax(0, value)) : 0;
}

/* The copy is needed only to hide a translucent camera the user does not
 * want in the movie; a saved transparency is already in the capture. */
int camera_layer_needed(double transparency, int apply_to_video, int visible, int running) {
    return visible && running && !apply_to_video && transparency_normalize(transparency) > 0;
}

/* window: camera window, global logical coordinates. monitor: recorded
 * monitor, global. region: monitor-local recorded area (the whole monitor
 * when not cropping). out_w x out_h: movie pixels. */
camera_layer camera_layer_map(rect window, rect monitor, rect region, int out_w, int out_h,
                              camera_shape shape) {
    camera_layer l;
    double sx = out_w / region.w, sy = out_h / region.h;
    l.frame = (rect){(window.x - monitor.x - region.x) * sx, (window.y - monitor.y - region.y) * sy,
                     window.w * sx, window.h * sy};
    l.radius = (shape == SHAPE_CIRCLE ? window.w / 2 : CAMERA_CORNER_RADIUS) * fmin(sx, sy);
    l.shape = shape;
    return l;
}

/* Part of the layer inside the movie, in whole pixels (with the halo). */
int camera_layer_clip(camera_layer l, int out_w, int out_h, int *x, int *y, int *w, int *h) {
    double x0 = fmax(0, floor(l.frame.x - HALO_PX)), y0 = fmax(0, floor(l.frame.y - HALO_PX));
    double x1 = fmin(out_w, ceil(l.frame.x + l.frame.w + HALO_PX));
    double y1 = fmin(out_h, ceil(l.frame.y + l.frame.h + HALO_PX));
    if (x1 - x0 < 2 || y1 - y0 < 2) return 0;
    *x = (int)x0; *y = (int)y0; *w = (int)(x1 - x0); *h = (int)(y1 - y0);
    return 1;
}

/* Draws the camera (aspect fill, mirrored if asked, shape-masked) into a
 * BGRA canvas whose top-left is at (cx, cy) in output pixels. 'view' is the
 * Center Stage crop of the camera frame in normalized coordinates. */
void camera_layer_draw(cairo_t *cr, camera_layer l, int cx, int cy, const camera_frame *f,
                       int mirrored, rect view) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, -cx, -cy);
    rect r = {l.frame.x - HALO_PX, l.frame.y - HALO_PX, l.frame.w + 2 * HALO_PX, l.frame.h + 2 * HALO_PX};
    double rad = fmin(l.radius + HALO_PX, fmin(r.w, r.h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, r.x + r.w - rad, r.y + rad, rad, -M_PI / 2, 0);
    cairo_arc(cr, r.x + r.w - rad, r.y + r.h - rad, rad, 0, M_PI / 2);
    cairo_arc(cr, r.x + rad, r.y + r.h - rad, rad, M_PI / 2, M_PI);
    cairo_arc(cr, r.x + rad, r.y + rad, rad, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_clip(cr);
    cairo_translate(cr, r.x, r.y);
    camera_paint_frame(cr, f, r.w, r.h, mirrored, view);
    cairo_restore(cr);
}

/* Aspect-fill a camera frame into w x h, using the 'view' crop (normalized
 * camera coordinates; the whole frame is {0,0,1,1}). Shared with the
 * preview so both always show the same picture. */
void camera_paint_frame(cairo_t *cr, const camera_frame *f, double w, double h, int mirrored, rect view) {
    cairo_save(cr);
    double vw = view.w * f->width, vh = view.h * f->height;
    double scale = fmax(w / vw, h / vh);
    if (mirrored) {
        cairo_translate(cr, w, 0);
        cairo_scale(cr, -1, 1);
    }
    cairo_translate(cr, (w - vw * scale) / 2, (h - vh * scale) / 2);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -view.x * f->width, -view.y * f->height);
    cairo_surface_t *image = cairo_image_surface_create_for_data(
        f->data, CAIRO_FORMAT_RGB24, f->width, f->height, f->stride);
    cairo_set_source_surface(cr, image, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_surface_destroy(image);
    cairo_restore(cr);
}
