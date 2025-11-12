/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 8, 2025
 */

#ifndef HIT_PROCESSOR_H
#define HIT_PROCESSOR_H

#include "tpx3_packets.h"
#include <vector>
#include <cstdint>
#include <array>
#include <map>
#include <string>
#include <limits>
#include <mutex>

struct Statistics {
    uint64_t total_hits;
    uint64_t total_chunks;
    uint64_t total_tdc_events;
    uint64_t total_tdc1_events;  // TDC1 events only (RISE + FALL)
    uint64_t total_tdc2_events;  // TDC2 events only (RISE + FALL)
    uint64_t total_control_packets;
    uint64_t total_decode_errors;
    uint64_t total_fractional_errors;  // TDC fractional timestamp errors
    uint64_t total_unknown_packets;    // Packets with unknown type
    std::map<uint8_t, uint64_t> packet_type_counts;  // Count of packets by type
    double hit_rate_hz;  // Instant hit rate (rolling average over ~1s window)
    double tdc1_rate_hz;  // Instant TDC1 rate (rolling average over ~1s window)
    double tdc2_rate_hz;  // Instant TDC2 rate (rolling average over ~1s window)
    double cumulative_hit_rate_hz;  // Cumulative average: total_hits / elapsed_time
    double cumulative_tdc1_rate_hz;  // Cumulative average: total_tdc1_events / elapsed_time
    double cumulative_tdc2_rate_hz;  // Cumulative average: total_tdc2_events / elapsed_time
    std::array<double, 4> chip_hit_rates_hz;
    std::array<bool, 4> chip_hit_rate_valid;
    std::array<uint64_t, 4> chip_tdc1_counts;
    std::array<double, 4> chip_tdc1_rates_hz;
    std::array<double, 4> chip_tdc1_cumulative_rates_hz;
    std::array<bool, 4> chip_tdc1_present;
    std::map<std::string, uint64_t> packet_byte_totals; // Bytes accounted per packet category
    uint64_t total_bytes_accounted;  // Total bytes accounted across all categories
    uint64_t earliest_hit_time_ticks;
    uint64_t latest_hit_time_ticks;
    bool hit_time_initialized;
    uint64_t earliest_tdc1_time_ticks;
    uint64_t latest_tdc1_time_ticks;
    bool tdc1_time_initialized;
    uint64_t total_reordered_packets;  // Packets processed out of order
    uint64_t reorder_max_distance;     // Maximum reorder distance observed
    uint64_t reorder_buffer_overflows; // Number of times reorder buffer overflowed
    uint64_t reorder_packets_dropped_too_old; // Packets dropped because they were too old
    bool started_mid_stream;           // True if first data lacked chunk header
};

class HitProcessor {
public:
    HitProcessor();
    
    void addHit(const PixelHit& hit);
    void addTdcEvent(const TDCEvent& tdc, uint8_t chip_index);
    void incrementChunkCount();
    void incrementChunkCountBatch(uint64_t count);  // Batch increment to reduce mutex contention
    void processChunkMetadata(const ChunkMetadata& metadata);
    void incrementDecodeError();
    void incrementFractionalError();
    void incrementUnknownPacket();
    void incrementPacketType(uint8_t packet_type);
    void updateReorderStats(uint64_t packets_reordered,
                            uint64_t max_reorder_distance,
                            uint64_t buffer_overflows,
                            uint64_t packets_dropped_too_old);
    void addPacketBytes(const std::string& category, uint64_t bytes);
    
    void setRecentHitCapacity(size_t capacity);
    std::vector<PixelHit> getRecentHits() const;
    std::vector<PixelHit> getHits() const { return getRecentHits(); } // Legacy compatibility
    Statistics getStatistics() const;
    void markMidStreamStart();
    bool startedMidStream() const { return stats_.started_mid_stream; }
    void finalizeRates();
    
    void clearHits();
    void resetStatistics();
    
private:
    friend class DecodeDispatcher;
    size_t recent_hit_capacity_;
    std::vector<PixelHit> recent_hits_buffer_;
    size_t recent_hits_head_;
    size_t recent_hits_size_;
    Statistics stats_;
    uint64_t start_time_ns_;  // Time when statistics started (for cumulative rates)
    uint64_t tdc1_start_time_ns_;  // Time when first TDC1 event arrived (for TDC1 cumulative rate)
    uint64_t last_update_time_ns_;
    uint64_t hits_at_last_update_;
    uint64_t tdc1_events_at_last_update_;
    uint64_t tdc2_events_at_last_update_;
    std::array<uint64_t, 4> chip_hit_totals_;
    std::array<uint64_t, 4> chip_hits_at_last_update_;
    std::array<uint64_t, 4> chip_tdc1_at_last_update_;
    std::array<uint64_t, 4> chip_tdc1_min_ticks_;
    std::array<uint64_t, 4> chip_tdc1_max_ticks_;
    uint64_t calls_since_last_update_;
    uint64_t last_hit_time_ticks_;
    uint64_t last_tdc1_time_ticks_;
    mutable std::recursive_mutex mutex_;
    
    void updateHitRate();
};

#endif // HIT_PROCESSOR_H

