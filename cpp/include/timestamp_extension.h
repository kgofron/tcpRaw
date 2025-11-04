/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 4, 2025
 */

#ifndef TIMESTAMP_EXTENSION_H
#define TIMESTAMP_EXTENSION_H

#include <cstdint>

// Forward declaration
struct PixelHit;

// Extend timestamp using minimum timestamp
// Based on the Rust implementation from the manual
inline uint64_t extend_timestamp(uint64_t timestamp, uint64_t minimum_timestamp, uint64_t n_bits) {
    // Get the maximum value that the timestamp can have
    uint64_t bit_mask = (1ULL << n_bits) - 1;
    
    // Calculate the least significant bits while ensuring
    // the value remains greater than minimum_timestamp
    // If the result is negative, a wraparound has occurred
    uint64_t delta_t = ((timestamp - minimum_timestamp) & bit_mask);
    
    return minimum_timestamp + delta_t;
}

// Apply timestamp extension to pixel hit
// Implementation in .cpp to avoid circular dependency
void extend_pixel_hit_timestamp(PixelHit& hit, uint64_t minimum_timestamp, uint64_t n_bits);

#endif // TIMESTAMP_EXTENSION_H

