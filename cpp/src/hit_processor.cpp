#include "hit_processor.h"
#include <chrono>

HitProcessor::HitProcessor() {
    resetStatistics();
}

void HitProcessor::resetStatistics() {
    stats_.total_hits = 0;
    stats_.total_chunks = 0;
    stats_.total_tdc_events = 0;
    stats_.total_control_packets = 0;
    stats_.hit_rate_hz = 0.0;
    last_update_time_ns_ = 0;
}

void HitProcessor::addHit(const PixelHit& hit) {
    hits_.push_back(hit);
    stats_.total_hits++;
    updateHitRate();
}

void HitProcessor::addTdcEvent(const TDCEvent&) {
    stats_.total_tdc_events++;
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
        return;
    }
    
    uint64_t elapsed_ns = current_time_ns - last_update_time_ns_;
    if (elapsed_ns > 1'000'000'000) { // Update every second
        double elapsed_seconds = elapsed_ns / 1e9;
        stats_.hit_rate_hz = hits_.size() / elapsed_seconds;
        last_update_time_ns_ = current_time_ns;
    }
}

