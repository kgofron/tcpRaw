#include "packet_reorder_buffer.h"
#include <algorithm>

PacketReorderBuffer::PacketReorderBuffer(size_t max_buffer_size, bool chunk_aware)
    : max_buffer_size_(max_buffer_size)
    , chunk_aware_(chunk_aware)
    , next_expected_id_(0)
    , oldest_allowed_id_(0)
    , current_chunk_id_(0)
    , first_packet_seen_(false)
{
}

bool PacketReorderBuffer::processPacket(uint64_t word, uint64_t packet_id, uint64_t chunk_id,
                                         ProcessCallback callback) {
    stats_.total_packets++;
    
    // Chunk-aware: reset state on chunk boundary
    if (chunk_aware_ && chunk_id != current_chunk_id_ && chunk_id > 0) {
        // Flush any remaining packets from previous chunk
        flush(callback);
        // Reset for new chunk
        resetForNewChunk(chunk_id);
    }
    
    // Fast path: packet is exactly what we expect (most common case)
    if (!first_packet_seen_ || packet_id == next_expected_id_) {
        if (!first_packet_seen_) {
            first_packet_seen_ = true;
            next_expected_id_ = packet_id + 1;
            oldest_allowed_id_ = (packet_id >= max_buffer_size_) 
                ? (packet_id - max_buffer_size_) 
                : 0;
        } else {
            next_expected_id_++;
            updateOldestAllowed();
        }
        
        stats_.packets_processed_immediately++;
        callback(word, packet_id, chunk_id);
        return true;
    }
    
    // Packet is out of order - check if it's too old
    if (first_packet_seen_ && packet_id < oldest_allowed_id_) {
        // Too old, likely duplicate or from previous chunk
        stats_.packets_dropped_too_old++;
        return false;
    }
    
    // Check if packet is ahead of expected (acceptable, buffer it)
    if (packet_id > next_expected_id_) {
        // Calculate reorder distance
        uint64_t distance = packet_id - next_expected_id_;
        if (distance > stats_.max_reorder_distance) {
            stats_.max_reorder_distance = distance;
        }
        
        // Check buffer capacity
        if (buffer_.size() >= max_buffer_size_) {
            stats_.buffer_overflows++;
            // Don't buffer if full - process immediately
            callback(word, packet_id, chunk_id);
            return false;
        }
        
        // Buffer the packet
        buffer_[packet_id] = OutOfOrderPacket(word, packet_id, chunk_id);
        stats_.packets_reordered++;
        
        // Check if we can now release next_expected_id
        releaseConsecutivePackets(callback);
        return false;
    }
    
    // Packet is behind expected but within window (late arrival)
    if (packet_id < next_expected_id_ && packet_id >= oldest_allowed_id_) {
        uint64_t distance = next_expected_id_ - packet_id - 1;
        if (distance > stats_.max_reorder_distance) {
            stats_.max_reorder_distance = distance;
        }
        
        // Check buffer capacity
        if (buffer_.size() >= max_buffer_size_) {
            stats_.buffer_overflows++;
            // Buffer full - drop this late packet
            return false;
        }
        
        // Buffer the late packet
        buffer_[packet_id] = OutOfOrderPacket(word, packet_id, chunk_id);
        stats_.packets_reordered++;
        
        // Check if this fills a gap allowing us to release packets
        releaseConsecutivePackets(callback);
        return false;
    }
    
    // Should not reach here, but process anyway
    callback(word, packet_id, chunk_id);
    return false;
}

void PacketReorderBuffer::releaseConsecutivePackets(ProcessCallback callback) {
    // Release all consecutive packets starting from next_expected_id
    while (true) {
        auto it = buffer_.find(next_expected_id_);
        if (it == buffer_.end()) {
            break; // No more consecutive packets
        }
        
        // Found next expected packet - release it
        const OutOfOrderPacket& packet = it->second;
        callback(packet.word, packet.packet_id, packet.chunk_id);
        
        // Remove from buffer and advance
        buffer_.erase(it);
        next_expected_id_++;
        updateOldestAllowed();
    }
}

void PacketReorderBuffer::flush(ProcessCallback callback) {
    // Process all buffered packets in order
    if (buffer_.empty()) {
        return;
    }
    
    // Extract all packet IDs and sort
    std::vector<uint64_t> packet_ids;
    packet_ids.reserve(buffer_.size());
    for (const auto& pair : buffer_) {
        packet_ids.push_back(pair.first);
    }
    std::sort(packet_ids.begin(), packet_ids.end());
    
    // Process in sorted order
    for (uint64_t id : packet_ids) {
        const OutOfOrderPacket& packet = buffer_[id];
        callback(packet.word, packet.packet_id, packet.chunk_id);
    }
    
    buffer_.clear();
    
    // Reset state
    first_packet_seen_ = false;
    next_expected_id_ = 0;
    oldest_allowed_id_ = 0;
}

void PacketReorderBuffer::resetForNewChunk(uint64_t new_chunk_id) {
    // Flush buffer first (if not already empty)
    if (!buffer_.empty()) {
        // Packets will be processed but callback needs to handle chunk boundaries
        buffer_.clear();
    }
    
    current_chunk_id_ = new_chunk_id;
    first_packet_seen_ = false;
    next_expected_id_ = 0;
    oldest_allowed_id_ = 0;
}

void PacketReorderBuffer::updateOldestAllowed() {
    if (next_expected_id_ >= max_buffer_size_) {
        oldest_allowed_id_ = next_expected_id_ - max_buffer_size_;
    } else {
        oldest_allowed_id_ = 0;
    }
}

