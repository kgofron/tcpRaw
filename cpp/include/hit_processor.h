#ifndef HIT_PROCESSOR_H
#define HIT_PROCESSOR_H

#include "tpx3_packets.h"
#include <vector>
#include <cstdint>
#include <map>

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
    double hit_rate_hz;  // Total hits per second
    double tdc1_rate_hz;  // TDC1 events per second
    double tdc2_rate_hz;  // TDC2 events per second
    std::map<uint8_t, double> chip_hit_rates_hz;  // Per-chip hit rates
};

class HitProcessor {
public:
    HitProcessor();
    
    void addHit(const PixelHit& hit);
    void addTdcEvent(const TDCEvent& tdc);
    void incrementChunkCount();
    void processChunkMetadata(const ChunkMetadata& metadata);
    void incrementDecodeError();
    void incrementFractionalError();
    void incrementUnknownPacket();
    void incrementPacketType(uint8_t packet_type);
    
    const std::vector<PixelHit>& getHits() const { return hits_; }
    const Statistics& getStatistics() const { return stats_; }
    
    void clearHits();
    void resetStatistics();
    
private:
    std::vector<PixelHit> hits_;
    Statistics stats_;
    uint64_t last_update_time_ns_;
    uint64_t hits_at_last_update_;
    uint64_t tdc1_events_at_last_update_;
    uint64_t tdc2_events_at_last_update_;
    std::map<uint8_t, uint64_t> chip_hits_at_last_update_;
    uint64_t calls_since_last_update_;
    
    void updateHitRate();
};

#endif // HIT_PROCESSOR_H

