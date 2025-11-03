#ifndef PACKET_REORDER_BUFFER_H
#define PACKET_REORDER_BUFFER_H

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <functional>

// Structure for out-of-order packet
struct OutOfOrderPacket {
    uint64_t word;
    uint64_t packet_id;
    uint64_t chunk_id;
    
    OutOfOrderPacket() : word(0), packet_id(0), chunk_id(0) {}
    OutOfOrderPacket(uint64_t w, uint64_t id, uint64_t chunk) 
        : word(w), packet_id(id), chunk_id(chunk) {}
};

// High-performance packet reorder buffer with chunk awareness
class PacketReorderBuffer {
public:
    // Callback type for processing reordered packets
    using ProcessCallback = std::function<void(uint64_t word, uint64_t packet_id, uint64_t chunk_id)>;
    
    explicit PacketReorderBuffer(size_t max_buffer_size = 1000, bool chunk_aware = true);
    
    // Process a packet (returns true if packet was processed immediately, false if buffered)
    bool processPacket(uint64_t word, uint64_t packet_id, uint64_t chunk_id, 
                      ProcessCallback callback);
    
    // Flush buffer (process all buffered packets in order, even if gaps exist)
    void flush(ProcessCallback callback);
    
    // Reset state at chunk boundary (if chunk-aware)
    void resetForNewChunk(uint64_t new_chunk_id);
    
    // Check if buffer is empty
    bool isEmpty() const { return buffer_.empty(); }
    
    // Get current buffer size
    size_t size() const { return buffer_.size(); }
    
    // Get statistics
    struct Statistics {
        uint64_t packets_reordered = 0;      // Packets that were buffered and reordered
        uint64_t packets_processed_immediately = 0;  // In-order packets
        uint64_t max_reorder_distance = 0;    // Maximum gap from expected ID
        uint64_t buffer_overflows = 0;        // Packets dropped due to full buffer
        uint64_t packets_dropped_too_old = 0; // Packets dropped because ID < oldest_allowed
        uint64_t total_packets = 0;          // Total packets processed
    };
    
    const Statistics& getStatistics() const { return stats_; }
    void resetStatistics() { stats_ = Statistics(); }
    
private:
    // Internal buffer: packet_id -> packet
    std::unordered_map<uint64_t, OutOfOrderPacket> buffer_;
    
    // Configuration
    size_t max_buffer_size_;
    bool chunk_aware_;
    
    // State
    uint64_t next_expected_id_;
    uint64_t oldest_allowed_id_;
    uint64_t current_chunk_id_;
    bool first_packet_seen_;
    
    // Statistics
    Statistics stats_;
    
    // Helper: Release consecutive packets starting from next_expected_id
    void releaseConsecutivePackets(ProcessCallback callback);
    
    // Helper: Update oldest_allowed_id based on window
    void updateOldestAllowed();
};

#endif // PACKET_REORDER_BUFFER_H

