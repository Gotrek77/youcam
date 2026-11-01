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
