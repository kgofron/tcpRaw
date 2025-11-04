/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 4, 2025
 */

#ifndef TPX3_DECODER_H
#define TPX3_DECODER_H

#include "tpx3_packets.h"
#include <tuple>

// Bit manipulation utilities
inline uint64_t get_bits(uint64_t data, int high, int low) {
    int num = (high - low) + 1;
    uint64_t mask = (1ULL << num) - 1;  // N consecutive ones
    return (data >> low) & mask;
}

inline bool matches_nibble(uint64_t data, uint8_t nibble) {
    return (data >> 60) == nibble;
}

// Convert PixAddr to (x, y) coordinates according to Table 6.6
inline std::tuple<uint16_t, uint16_t> pixaddr_to_xy(uint64_t pixaddr) {
    // Extract double column (bits 15-9), 128 double columns
    uint16_t dcol = (pixaddr >> 9) & 0x7F;
    
    // Extract super pixel (bits 8-3), 64 super pixels per double column
    uint16_t spix = (pixaddr >> 3) & 0x3F;
    
    // Extract pixel index (bits 2-0), 8 pixels per super pixel
    uint8_t pix = pixaddr & 0x7;
    
    // Calculate X: dcol * 2 + left/right column (pix 0-3 = left, 4-7 = right)
    uint16_t x = (dcol * 2) + (pix >= 4 ? 1 : 0);
    
    // Calculate Y: spix * 4 + pixel row within super pixel
    // Pix layout: 0-3 = left column, 4-7 = right column
    // Within each column, pixels are stacked vertically
    uint16_t y = (spix * 4) + (pix & 0x3);
    
    return std::make_tuple(x, y);
}

// Clock conversion constants
constexpr double CLOCK_640MHZ = 640.0e6;  // 1.5625 ns per count
constexpr double CLOCK_40MHZ = 40.0e6;    // 25 ns per count
constexpr double CLOCK_320MHZ = 320.0e6;  // 3.125 ns per count (TDC coarse)
constexpr double CLOCK_SPIDR = 1.0 / 0.4096e-3; // SPIDR clock in Hz

// Convert clock counts to nanoseconds
inline double clock_to_ns(uint64_t count, double clock_hz) {
    return (static_cast<double>(count) / clock_hz) * 1e9;
}

// Decode TPX3 chunk header
inline TPX3ChunkHeader decode_chunk_header(uint64_t data) {
    TPX3ChunkHeader header;
    header.data = data;
    return header;
}

// Decode pixel data packet (0xa or 0xb)
PixelHit decode_pixel_data(uint64_t data, uint8_t chip_index);
PixelHit decode_pixel_data_count_fb(uint64_t data, uint8_t chip_index);
PixelHit decode_pixel_data_standard(uint64_t data, uint8_t chip_index);

// Decode TDC data packet (0x6)
TDCEvent decode_tdc_data(uint64_t data);

// Decode global time packet (0x44 or 0x45)
GlobalTime decode_global_time(uint64_t data);

// Decode SPIDR control packet (0x50 or 0x5)
bool decode_spidr_packet_id(uint64_t data, uint64_t& packet_count);
bool decode_spidr_control(uint64_t data, SpidrControl& ctrl);

// Decode TPX3 control packet (0x71)
bool decode_tpx3_control(uint64_t data, Tpx3ControlCmd& cmd);

// Decode extra timestamp packet
ExtraTimestamp decode_extra_timestamp(uint64_t data);

#endif // TPX3_DECODER_H

