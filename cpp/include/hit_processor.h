#ifndef HIT_PROCESSOR_H
#define HIT_PROCESSOR_H

#include "tpx3_packets.h"
#include <vector>
#include <cstdint>

struct Statistics {
    uint64_t total_hits;
    uint64_t total_chunks;
    uint64_t total_tdc_events;
    uint64_t total_control_packets;
    double hit_rate_hz;  // Hits per second
};

class HitProcessor {
public:
    HitProcessor();
    
    void addHit(const PixelHit& hit);
    void addTdcEvent(const TDCEvent& tdc);
    void incrementChunkCount();
    void processChunkMetadata(const ChunkMetadata& metadata);
    
    const std::vector<PixelHit>& getHits() const { return hits_; }
    const Statistics& getStatistics() const { return stats_; }
    
    void clearHits();
    void resetStatistics();
    
private:
    std::vector<PixelHit> hits_;
    Statistics stats_;
    uint64_t last_update_time_ns_;
    
    void updateHitRate();
};

#endif // HIT_PROCESSOR_H

