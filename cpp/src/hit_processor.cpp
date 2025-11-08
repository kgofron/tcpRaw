/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 8, 2025
 */

#include "hit_processor.h"
#include <chrono>
#include <limits>

HitProcessor::HitProcessor()
    : recent_hit_capacity_(10),
      recent_hits_buffer_(recent_hit_capacity_),
      recent_hits_head_(0),
      recent_hits_size_(0) {
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
    stats_.chip_tdc1_cumulative_rates_hz.clear();
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
    stats_.started_mid_stream = false;
    start_time_ns_ = 0;
    tdc1_start_time_ns_ = 0;
    last_update_time_ns_ = 0;
    hits_at_last_update_ = 0;
    tdc1_events_at_last_update_ = 0;
    tdc2_events_at_last_update_ = 0;
    chip_hit_totals_.clear();
    chip_hits_at_last_update_.clear();
    chip_tdc1_at_last_update_.clear();
    chip_tdc1_min_ticks_.clear();
    chip_tdc1_max_ticks_.clear();
    calls_since_last_update_ = 0;
    last_hit_time_ticks_ = 0;
    last_tdc1_time_ticks_ = 0;
    recent_hits_head_ = 0;
    recent_hits_size_ = 0;
}

void HitProcessor::setRecentHitCapacity(size_t capacity) {
    recent_hit_capacity_ = capacity;
    recent_hits_buffer_.assign(recent_hit_capacity_, PixelHit{});
    recent_hits_head_ = 0;
    recent_hits_size_ = 0;
}

std::vector<PixelHit> HitProcessor::getRecentHits() const {
    std::vector<PixelHit> result;
    if (recent_hit_capacity_ == 0 || recent_hits_size_ == 0 || recent_hits_buffer_.empty()) {
        return result;
    }

    result.reserve(recent_hits_size_);
    size_t capacity = recent_hits_buffer_.size();
    size_t start_index = (recent_hits_head_ + capacity - recent_hits_size_) % capacity;

    for (size_t i = 0; i < recent_hits_size_; ++i) {
        size_t index = (start_index + i) % capacity;
        result.push_back(recent_hits_buffer_[index]);
    }
    return result;
}

void HitProcessor::clearHits() {
    recent_hits_head_ = 0;
    recent_hits_size_ = 0;
}

void HitProcessor::markMidStreamStart() {
    stats_.started_mid_stream = true;
}

void HitProcessor::addHit(const PixelHit& hit) {
    if (recent_hit_capacity_ > 0) {
        if (recent_hits_buffer_.size() != recent_hit_capacity_) {
            recent_hits_buffer_.assign(recent_hit_capacity_, PixelHit{});
        }
        recent_hits_buffer_[recent_hits_head_] = hit;
        recent_hits_head_ = (recent_hits_head_ + 1) % recent_hit_capacity_;
        if (recent_hits_size_ < recent_hit_capacity_) {
            recent_hits_size_++;
        }
    }

    stats_.total_hits++;
    chip_hit_totals_[hit.chip_index]++;

    if (start_time_ns_ == 0) {
        auto now = std::chrono::steady_clock::now();
        start_time_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        last_update_time_ns_ = start_time_ns_;
        hits_at_last_update_ = 0;
        tdc1_events_at_last_update_ = 0;
        tdc2_events_at_last_update_ = 0;
    }

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

    if (start_time_ns_ == 0) {
        auto now = std::chrono::steady_clock::now();
        start_time_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        last_update_time_ns_ = start_time_ns_;
        hits_at_last_update_ = 0;
        tdc1_events_at_last_update_ = 0;
        tdc2_events_at_last_update_ = 0;
    }

    if (tdc.type == TDC1_RISE || tdc.type == TDC1_FALL) {
        if (!stats_.tdc1_time_initialized || tdc.timestamp_ns < stats_.earliest_tdc1_time_ticks) {
            stats_.earliest_tdc1_time_ticks = tdc.timestamp_ns;
            stats_.tdc1_time_initialized = true;
        }
        if (tdc.timestamp_ns > stats_.latest_tdc1_time_ticks) {
            stats_.latest_tdc1_time_ticks = tdc.timestamp_ns;
        }
        stats_.total_tdc1_events++;
        stats_.chip_tdc1_counts[chip_index]++;
        auto min_it = chip_tdc1_min_ticks_.find(chip_index);
        if (min_it == chip_tdc1_min_ticks_.end()) {
            chip_tdc1_min_ticks_[chip_index] = tdc.timestamp_ns;
            chip_tdc1_max_ticks_[chip_index] = tdc.timestamp_ns;
        } else {
            if (tdc.timestamp_ns < min_it->second) {
                min_it->second = tdc.timestamp_ns;
            }
            auto& max_entry = chip_tdc1_max_ticks_[chip_index];
            if (tdc.timestamp_ns > max_entry) {
                max_entry = tdc.timestamp_ns;
            }
        }
    }
    if (tdc.type == TDC2_RISE || tdc.type == TDC2_FALL) {
        stats_.total_tdc2_events++;
    }

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
    // Reserved for future metadata-driven features
}

void HitProcessor::updateHitRate() {
    auto now = std::chrono::steady_clock::now();
    uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    constexpr double TOA_UNIT_SECONDS = 1.5625e-9;

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
        hits_at_last_update_ = stats_.total_hits;
        tdc1_events_at_last_update_ = stats_.total_tdc1_events;
        tdc2_events_at_last_update_ = stats_.total_tdc2_events;
        chip_hits_at_last_update_ = chip_hit_totals_;
        chip_tdc1_at_last_update_ = stats_.chip_tdc1_counts;
        last_hit_time_ticks_ = stats_.latest_hit_time_ticks;
        last_tdc1_time_ticks_ = stats_.latest_tdc1_time_ticks;
        return;
    }

    uint64_t elapsed_ns = current_time_ns - last_update_time_ns_;
    if (elapsed_ns > 1'000'000'000) {
        double elapsed_seconds = elapsed_ns / 1e9;

        uint64_t new_hits = stats_.total_hits - hits_at_last_update_;
        double data_span_hits = 0.0;
        if (stats_.latest_hit_time_ticks > last_hit_time_ticks_) {
            data_span_hits = (stats_.latest_hit_time_ticks - last_hit_time_ticks_) * TOA_UNIT_SECONDS;
        }
        if (data_span_hits > 0.0) {
            stats_.hit_rate_hz = new_hits / data_span_hits;
        } else {
            stats_.hit_rate_hz = new_hits / elapsed_seconds;
        }

        uint64_t new_tdc1_events = stats_.total_tdc1_events - tdc1_events_at_last_update_;
        double data_span_tdc1 = 0.0;
        if (stats_.latest_tdc1_time_ticks > last_tdc1_time_ticks_) {
            data_span_tdc1 = (stats_.latest_tdc1_time_ticks - last_tdc1_time_ticks_) * TOA_UNIT_SECONDS;
        }
        if (data_span_tdc1 > 0.0) {
            stats_.tdc1_rate_hz = new_tdc1_events / data_span_tdc1;
        } else {
            stats_.tdc1_rate_hz = new_tdc1_events / elapsed_seconds;
        }

        uint64_t new_tdc2_events = stats_.total_tdc2_events - tdc2_events_at_last_update_;
        stats_.tdc2_rate_hz = new_tdc2_events / elapsed_seconds;

        stats_.chip_hit_rates_hz.clear();
        for (const auto& pair : chip_hit_totals_) {
            uint8_t chip = pair.first;
            uint64_t current_count = pair.second;
            uint64_t last_count = 0;
            auto last_it = chip_hits_at_last_update_.find(chip);
            if (last_it != chip_hits_at_last_update_.end()) {
                last_count = last_it->second;
            }
            uint64_t new_chip_hits = current_count - last_count;
            if (data_span_hits > 0.0) {
                stats_.chip_hit_rates_hz[chip] = new_chip_hits / data_span_hits;
            } else {
                stats_.chip_hit_rates_hz[chip] = new_chip_hits / elapsed_seconds;
            }
        }

        stats_.chip_tdc1_rates_hz.clear();
        for (const auto& pair : stats_.chip_tdc1_counts) {
            uint8_t chip = pair.first;
            uint64_t current_count = pair.second;
            uint64_t last_count = 0;
            auto last_it = chip_tdc1_at_last_update_.find(chip);
            if (last_it != chip_tdc1_at_last_update_.end()) {
                last_count = last_it->second;
            }
            uint64_t new_tdc1_events_chip = current_count - last_count;
            if (data_span_tdc1 > 0.0) {
                stats_.chip_tdc1_rates_hz[chip] = new_tdc1_events_chip / data_span_tdc1;
            } else {
                stats_.chip_tdc1_rates_hz[chip] = new_tdc1_events_chip / elapsed_seconds;
            }
        }

        last_update_time_ns_ = current_time_ns;
        hits_at_last_update_ = stats_.total_hits;
        tdc1_events_at_last_update_ = stats_.total_tdc1_events;
        tdc2_events_at_last_update_ = stats_.total_tdc2_events;
        chip_hits_at_last_update_ = chip_hit_totals_;
        chip_tdc1_at_last_update_ = stats_.chip_tdc1_counts;
        last_hit_time_ticks_ = stats_.latest_hit_time_ticks;
        last_tdc1_time_ticks_ = stats_.latest_tdc1_time_ticks;
    }

    // Always refresh per-chip cumulative TDC1 rates
    stats_.chip_tdc1_cumulative_rates_hz.clear();
    for (const auto& pair : stats_.chip_tdc1_counts) {
        uint8_t chip = pair.first;
        uint64_t count = pair.second;
        double chip_span_seconds = 0.0;
        auto min_it = chip_tdc1_min_ticks_.find(chip);
        auto max_it = chip_tdc1_max_ticks_.find(chip);
        if (min_it != chip_tdc1_min_ticks_.end() && max_it != chip_tdc1_max_ticks_.end() &&
            max_it->second > min_it->second) {
            chip_span_seconds = (max_it->second - min_it->second) * TOA_UNIT_SECONDS;
        }
        if (chip_span_seconds > 0.0) {
            stats_.chip_tdc1_cumulative_rates_hz[chip] = count / chip_span_seconds;
        } else if (start_time_ns_ > 0) {
            uint64_t total_elapsed_ns = current_time_ns - start_time_ns_;
            if (total_elapsed_ns > 0) {
                stats_.chip_tdc1_cumulative_rates_hz[chip] = count / (total_elapsed_ns / 1e9);
            } else {
                stats_.chip_tdc1_cumulative_rates_hz[chip] = 0.0;
            }
        } else {
            stats_.chip_tdc1_cumulative_rates_hz[chip] = 0.0;
        }
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

void HitProcessor::finalizeRates() {
    updateHitRate();

    constexpr double TOA_UNIT_SECONDS = 1.5625e-9;

    double data_span_hits = 0.0;
    if (stats_.hit_time_initialized && stats_.latest_hit_time_ticks > stats_.earliest_hit_time_ticks) {
        data_span_hits = (stats_.latest_hit_time_ticks - stats_.earliest_hit_time_ticks) * TOA_UNIT_SECONDS;
    }
    double data_span_tdc1 = 0.0;
    if (stats_.tdc1_time_initialized && stats_.latest_tdc1_time_ticks > stats_.earliest_tdc1_time_ticks) {
        data_span_tdc1 = (stats_.latest_tdc1_time_ticks - stats_.earliest_tdc1_time_ticks) * TOA_UNIT_SECONDS;
    }

    if (stats_.hit_rate_hz == 0.0 && data_span_hits > 0.0) {
        stats_.hit_rate_hz = stats_.total_hits / data_span_hits;
    }
    if (stats_.tdc1_rate_hz == 0.0 && data_span_tdc1 > 0.0) {
        stats_.tdc1_rate_hz = stats_.total_tdc1_events / data_span_tdc1;
    }
    if (stats_.chip_hit_rates_hz.empty() && data_span_hits > 0.0) {
        for (const auto& pair : chip_hit_totals_) {
            stats_.chip_hit_rates_hz[pair.first] = pair.second / data_span_hits;
        }
    }
    if (stats_.chip_tdc1_rates_hz.empty() && data_span_tdc1 > 0.0) {
        for (const auto& pair : stats_.chip_tdc1_counts) {
            stats_.chip_tdc1_rates_hz[pair.first] = pair.second / data_span_tdc1;
        }
    }
}

