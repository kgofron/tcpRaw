/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 4, 2025
 */

#include "hit_processor.h"
#include <chrono>
#include <limits>

HitProcessor::HitProcessor() {
    resetStatistics();
}

void HitProcessor::resetStatistics() {
    stats_.total_hits = 0;
    stats_.total_chunks = 0;
    stats_.total_tdc_events = 0;
    stats_.total_tdc1_events = 0;
    stats_.total_tdc2_events = 0;
    stats_.total_control_packets = 0;
    stats_.total_decode_errors = 0;
    stats_.total_fractional_errors = 0;
    stats_.total_unknown_packets = 0;
    stats_.packet_type_counts.clear();
    stats_.hit_rate_hz = 0.0;
    stats_.tdc1_rate_hz = 0.0;
    stats_.tdc2_rate_hz = 0.0;
    stats_.cumulative_hit_rate_hz = 0.0;
    stats_.cumulative_tdc1_rate_hz = 0.0;
    stats_.cumulative_tdc2_rate_hz = 0.0;
    stats_.chip_hit_rates_hz.clear();
    stats_.chip_tdc1_counts.clear();
    stats_.chip_tdc1_rates_hz.clear();
    stats_.packet_byte_totals.clear();
    stats_.total_bytes_accounted = 0;
    stats_.earliest_hit_time_ticks = std::numeric_limits<uint64_t>::max();
    stats_.latest_hit_time_ticks = 0;
    stats_.hit_time_initialized = false;
    stats_.earliest_tdc1_time_ticks = std::numeric_limits<uint64_t>::max();
    stats_.latest_tdc1_time_ticks = 0;
    stats_.tdc1_time_initialized = false;
    stats_.total_reordered_packets = 0;
    stats_.reorder_max_distance = 0;
    stats_.reorder_buffer_overflows = 0;
    stats_.reorder_packets_dropped_too_old = 0;
    start_time_ns_ = 0;  // Will be initialized on first hit to exclude idle time
    tdc1_start_time_ns_ = 0;  // Will be initialized on first TDC1 event
    last_update_time_ns_ = 0;
    hits_at_last_update_ = 0;
    tdc1_events_at_last_update_ = 0;
    tdc2_events_at_last_update_ = 0;
    chip_hits_at_last_update_.clear();
    chip_tdc1_at_last_update_.clear();
    calls_since_last_update_ = 0;
}

void HitProcessor::addHit(const PixelHit& hit) {
    hits_.push_back(hit);
    stats_.total_hits++;
    
    // Initialize start_time_ns_ on first hit (exclude idle time before data starts)
    if (start_time_ns_ == 0) {
        auto now = std::chrono::steady_clock::now();
        start_time_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        last_update_time_ns_ = start_time_ns_;
        hits_at_last_update_ = 0;
        tdc1_events_at_last_update_ = 0;
        tdc2_events_at_last_update_ = 0;
    }
    
    // Only update hit rate every 1000 hits to reduce overhead
    calls_since_last_update_++;
    if (calls_since_last_update_ >= 1000) {
        updateHitRate();
        calls_since_last_update_ = 0;
    }
    if (!stats_.hit_time_initialized || hit.toa_ns < stats_.earliest_hit_time_ticks) {
        stats_.earliest_hit_time_ticks = hit.toa_ns;
        stats_.hit_time_initialized = true;
    }
    if (hit.toa_ns > stats_.latest_hit_time_ticks) {
        stats_.latest_hit_time_ticks = hit.toa_ns;
    }
}

void HitProcessor::addTdcEvent(const TDCEvent& tdc, uint8_t chip_index) {
    stats_.total_tdc_events++;
    
    // Initialize start_time_ns_ on first event (hit or TDC) to exclude idle time
    // This ensures cumulative rates are calculated from first data, not just first hit
    if (start_time_ns_ == 0) {
        auto now = std::chrono::steady_clock::now();
        start_time_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        last_update_time_ns_ = start_time_ns_;
        hits_at_last_update_ = 0;
        tdc1_events_at_last_update_ = 0;
        tdc2_events_at_last_update_ = 0;
    }
    
    // Track TDC1 events separately (RISE and FALL)
    if (tdc.type == TDC1_RISE || tdc.type == TDC1_FALL) {
        if (!stats_.tdc1_time_initialized || tdc.timestamp_ns < stats_.earliest_tdc1_time_ticks) {
            stats_.earliest_tdc1_time_ticks = tdc.timestamp_ns;
            stats_.tdc1_time_initialized = true;
        }
        if (tdc.timestamp_ns > stats_.latest_tdc1_time_ticks) {
            stats_.latest_tdc1_time_ticks = tdc.timestamp_ns;
        }
        stats_.total_tdc1_events++;
        stats_.chip_tdc1_counts[chip_index]++;  // Track per-chip TDC1 counts
    }
    // Track TDC2 events separately (RISE and FALL)
    if (tdc.type == TDC2_RISE || tdc.type == TDC2_FALL) {
        stats_.total_tdc2_events++;
    }
    
    // Update cumulative rates immediately for TDC events
    // (Rate updates are throttled for hits, but TDC events are infrequent)
    updateHitRate();
}

void HitProcessor::updateReorderStats(uint64_t packets_reordered,
                                      uint64_t max_reorder_distance,
                                      uint64_t buffer_overflows,
                                      uint64_t packets_dropped_too_old) {
    stats_.total_reordered_packets = packets_reordered;
    stats_.reorder_max_distance = max_reorder_distance;
    stats_.reorder_buffer_overflows = buffer_overflows;
    stats_.reorder_packets_dropped_too_old = packets_dropped_too_old;
}

void HitProcessor::addPacketBytes(const std::string& category, uint64_t bytes) {
    stats_.packet_byte_totals[category] += bytes;
    stats_.total_bytes_accounted += bytes;
}

void HitProcessor::incrementChunkCount() {
    stats_.total_chunks++;
}

void HitProcessor::processChunkMetadata(const ChunkMetadata&) {
    // For now, just track that we received metadata
    // Future: Could use this for time alignment
}

void HitProcessor::clearHits() {
    hits_.clear();
}

void HitProcessor::updateHitRate() {
    auto now = std::chrono::steady_clock::now();
    uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    
    constexpr double TOA_UNIT_SECONDS = 1.5625e-9;
    
    // Always update cumulative rates based on data time when available; fall back to wall time
    double data_elapsed_seconds_hits = 0.0;
    if (stats_.hit_time_initialized && stats_.latest_hit_time_ticks > stats_.earliest_hit_time_ticks) {
        data_elapsed_seconds_hits = (stats_.latest_hit_time_ticks - stats_.earliest_hit_time_ticks) * TOA_UNIT_SECONDS;
    }
    double data_elapsed_seconds_tdc1 = 0.0;
    if (stats_.tdc1_time_initialized && stats_.latest_tdc1_time_ticks > stats_.earliest_tdc1_time_ticks) {
        data_elapsed_seconds_tdc1 = (stats_.latest_tdc1_time_ticks - stats_.earliest_tdc1_time_ticks) * TOA_UNIT_SECONDS;
    }
    
    if (data_elapsed_seconds_hits > 0.0) {
        stats_.cumulative_hit_rate_hz = stats_.total_hits / data_elapsed_seconds_hits;
    } else if (start_time_ns_ > 0) {
        uint64_t total_elapsed_ns = current_time_ns - start_time_ns_;
        if (total_elapsed_ns > 0) {
            stats_.cumulative_hit_rate_hz = stats_.total_hits / (total_elapsed_ns / 1e9);
        }
    }
    if (data_elapsed_seconds_tdc1 > 0.0) {
        stats_.cumulative_tdc1_rate_hz = stats_.total_tdc1_events / data_elapsed_seconds_tdc1;
    } else if (start_time_ns_ > 0) {
        uint64_t total_elapsed_ns = current_time_ns - start_time_ns_;
        if (total_elapsed_ns > 0) {
            stats_.cumulative_tdc1_rate_hz = stats_.total_tdc1_events / (total_elapsed_ns / 1e9);
        }
    }
    if (start_time_ns_ > 0) {
        uint64_t total_elapsed_ns = current_time_ns - start_time_ns_;
        if (total_elapsed_ns > 0) {
            stats_.cumulative_tdc2_rate_hz = stats_.total_tdc2_events / (total_elapsed_ns / 1e9);
        }
    }
    
    if (last_update_time_ns_ == 0 || last_update_time_ns_ == start_time_ns_) {
        last_update_time_ns_ = current_time_ns;
        hits_at_last_update_ = stats_.total_hits;  // Use stats_.total_hits instead of hits_.size()
        tdc1_events_at_last_update_ = stats_.total_tdc1_events;
        tdc2_events_at_last_update_ = stats_.total_tdc2_events;
        // Initialize per-chip counters for all chips we've seen
        for (const auto& hit : hits_) {
            chip_hits_at_last_update_[hit.chip_index] = 0;
        }
        return;
    }
    
    uint64_t elapsed_ns = current_time_ns - last_update_time_ns_;
    if (elapsed_ns > 1'000'000'000) { // Update every second
        double elapsed_seconds = elapsed_ns / 1e9;
        
        // Update instant rates (rolling average over ~1s window)
        uint64_t new_hits = stats_.total_hits - hits_at_last_update_;
        stats_.hit_rate_hz = new_hits / elapsed_seconds;
        
        // Update TDC1 rate
        uint64_t new_tdc1_events = stats_.total_tdc1_events - tdc1_events_at_last_update_;
        stats_.tdc1_rate_hz = new_tdc1_events / elapsed_seconds;
        
        // Update TDC2 rate
        uint64_t new_tdc2_events = stats_.total_tdc2_events - tdc2_events_at_last_update_;
        stats_.tdc2_rate_hz = new_tdc2_events / elapsed_seconds;
        
        // Update per-chip hit rates
        stats_.chip_hit_rates_hz.clear();
        std::map<uint8_t, uint64_t> current_chip_hits;
        for (const auto& hit : hits_) {
            current_chip_hits[hit.chip_index]++;
        }
        
        for (const auto& pair : current_chip_hits) {
            uint8_t chip = pair.first;
            uint64_t current_count = pair.second;
            uint64_t last_count = chip_hits_at_last_update_.count(chip) 
                ? chip_hits_at_last_update_[chip] : 0;
            uint64_t new_chip_hits = current_count - last_count;
            if (elapsed_seconds > 0.0) {
                stats_.chip_hit_rates_hz[chip] = new_chip_hits / elapsed_seconds;
            }
        }
        
        // Update per-chip TDC1 rates
        stats_.chip_tdc1_rates_hz.clear();
        for (const auto& pair : stats_.chip_tdc1_counts) {
            uint8_t chip = pair.first;
            uint64_t current_count = pair.second;
            uint64_t last_count = chip_tdc1_at_last_update_.count(chip) 
                ? chip_tdc1_at_last_update_[chip] : 0;
            uint64_t new_tdc1_events = current_count - last_count;
            if (elapsed_seconds > 0.0) {
                stats_.chip_tdc1_rates_hz[chip] = new_tdc1_events / elapsed_seconds;
            }
        }
        
        // Update last state
        last_update_time_ns_ = current_time_ns;
        hits_at_last_update_ = stats_.total_hits;  // Use stats_.total_hits instead of hits_.size()
        tdc1_events_at_last_update_ = stats_.total_tdc1_events;
        tdc2_events_at_last_update_ = stats_.total_tdc2_events;
        chip_hits_at_last_update_ = current_chip_hits;
        chip_tdc1_at_last_update_ = stats_.chip_tdc1_counts;
    }
}

void HitProcessor::incrementDecodeError() {
    stats_.total_decode_errors++;
}

void HitProcessor::incrementFractionalError() {
    stats_.total_fractional_errors++;
}

void HitProcessor::incrementUnknownPacket() {
    stats_.total_unknown_packets++;
}

void HitProcessor::incrementPacketType(uint8_t packet_type) {
    stats_.packet_type_counts[packet_type]++;
}

