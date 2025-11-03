#include "hit_processor.h"
#include <chrono>

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
    stats_.chip_hit_rates_hz.clear();
    last_update_time_ns_ = 0;
    hits_at_last_update_ = 0;
    tdc1_events_at_last_update_ = 0;
    tdc2_events_at_last_update_ = 0;
    chip_hits_at_last_update_.clear();
    calls_since_last_update_ = 0;
}

void HitProcessor::addHit(const PixelHit& hit) {
    hits_.push_back(hit);
    stats_.total_hits++;
    
    // Only update hit rate every 1000 hits to reduce overhead
    calls_since_last_update_++;
    if (calls_since_last_update_ >= 1000) {
        updateHitRate();
        calls_since_last_update_ = 0;
    }
}

void HitProcessor::addTdcEvent(const TDCEvent& tdc) {
    stats_.total_tdc_events++;
    // Track TDC1 events separately (RISE and FALL)
    if (tdc.type == TDC1_RISE || tdc.type == TDC1_FALL) {
        stats_.total_tdc1_events++;
    }
    // Track TDC2 events separately (RISE and FALL)
    if (tdc.type == TDC2_RISE || tdc.type == TDC2_FALL) {
        stats_.total_tdc2_events++;
    }
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
    
    if (last_update_time_ns_ == 0) {
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
        
        // Update total hit rate using stats_.total_hits (more reliable than hits_.size())
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
            stats_.chip_hit_rates_hz[chip] = new_chip_hits / elapsed_seconds;
        }
        
        // Update last state
        last_update_time_ns_ = current_time_ns;
        hits_at_last_update_ = stats_.total_hits;  // Use stats_.total_hits instead of hits_.size()
        tdc1_events_at_last_update_ = stats_.total_tdc1_events;
        tdc2_events_at_last_update_ = stats_.total_tdc2_events;
        chip_hits_at_last_update_ = current_chip_hits;
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

