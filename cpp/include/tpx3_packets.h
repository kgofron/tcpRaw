/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 4, 2025
 */

#ifndef TPX3_PACKETS_H
#define TPX3_PACKETS_H

#include <cstdint>

// Chunk header constants
constexpr uint64_t TPX3_MAGIC = 0x33585054; // 'TPX3' in little-endian

// Packet type identifiers
enum PacketType : uint8_t {
    PIXEL_COUNT_FB = 0xa,      // Pixel data in count_fb mode
    PIXEL_STANDARD = 0xb,      // Pixel data in standard mode
    GLOBAL_TIME_LOW = 0x44,    // Global time low word
    GLOBAL_TIME_HIGH = 0x45,   // Global time high word
    EXTRA_TIMESTAMP = 0x51,    // Extra timestamp packet (TPX3)
    EXTRA_TIMESTAMP_MPX3 = 0x21, // Extra timestamp packet (MPX3)
    TDC_DATA = 0x6,            // TDC data
    SPIDR_PACKET_ID = 0x50,    // SPIDR packet ID
    SPIDR_CONTROL = 0x5,       // SPIDR control
    TPX3_CONTROL = 0x71        // TPX3 control
};

// SPIDR control commands
enum SpidrControlCmd : uint8_t {
    SPIDR_SHUTTER_OPEN = 0xf,
    SPIDR_SHUTTER_CLOSE = 0xa,
    SPIDR_HEARTBEAT = 0xc
};

// TPX3 control commands
enum Tpx3ControlCmd : uint8_t {
    TPX3_END_SEQUENTIAL = 0xa0,
    TPX3_END_DATA_DRIVEN = 0xb0
};

// TDC event types
enum TDCEventType : uint8_t {
    TDC1_RISE = 0xf,
    TDC1_FALL = 0xa,
    TDC2_RISE = 0xe,
    TDC2_FALL = 0xb
};

// Chunk header structure
struct TPX3ChunkHeader {
    uint64_t data;
    
    uint16_t chunkSize() const { return (data >> 48) & 0xFFFF; }
    uint8_t chipIndex() const { return (data >> 32) & 0xFF; }
    bool isValid() const { return (data & 0xFFFFFFFF) == TPX3_MAGIC; }
};

// Pixel hit data
struct PixelHit {
    uint16_t x;           // Pixel X coordinate
    uint16_t y;           // Pixel Y coordinate
    uint64_t toa_ns;      // Time of arrival in 1.5625ns units (extended)
    uint16_t tot_ns;      // Time over threshold in 25ns units
    uint8_t chip_index;   // Chip index
    bool is_count_fb;     // True if from count_fb mode packet
};

// TDC event data
struct TDCEvent {
    TDCEventType type;    // TDC event type
    uint16_t trigger_count;
    uint64_t timestamp_ns; // Timestamp in 1.5625ns units (extended)
    uint8_t fine_timestamp; // Fine timestamp (1-12)
};

// SPIDR control packet
struct SpidrControl {
    SpidrControlCmd command;
    uint64_t timestamp_ns; // In 25ns units
};

// Global time packet
struct GlobalTime {
    bool is_high_word;
    uint32_t time_value;  // Time value depending on type
    uint16_t spidr_time;  // SPIDR time in 0.4096ms units
};

// Extra timestamp packet
struct ExtraTimestamp {
    bool is_tpx3;         // True if TPX3, false if MPX3
    bool error_flag;      // Debug only
    bool overflow_flag;   // Debug only
    uint64_t timestamp_ns; // Timestamp in 1.5625ns units
};

// Chunk metadata from extra packets
struct ChunkMetadata {
    uint64_t packet_gen_time_ns; // Packet generation timestamp
    uint64_t min_timestamp_ns;   // Minimum event timestamp
    uint64_t max_timestamp_ns;   // Maximum event timestamp
    bool has_extra_packets;      // True if extra packets were decoded
};

// Cluster candidate for future 3D clustering
struct ClusterCandidate {
    uint16_t x;
    uint16_t y;
    uint64_t toa_ns;
    uint16_t tot_ns;
};

#endif // TPX3_PACKETS_H

