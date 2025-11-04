/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 4, 2025
 */

#include "timestamp_extension.h"
#include "tpx3_decoder.h"

void extend_pixel_hit_timestamp(PixelHit& hit, uint64_t minimum_timestamp, uint64_t n_bits) {
    hit.toa_ns = extend_timestamp(hit.toa_ns, minimum_timestamp, n_bits);
}

