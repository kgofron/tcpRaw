/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 4, 2025
 */

#include "ring_buffer.h"
#include <algorithm>
#include <cstring>

RingBuffer::RingBuffer(size_t size_bytes)
    : size_(roundUpToPowerOf2(size_bytes))
    , buffer_(size_)
    , head_(0)
    , tail_(0)
{
    // Verify power of 2
    if ((size_ & (size_ - 1)) != 0) {
        // Should not happen if roundUpToPowerOf2 works correctly
        size_ = roundUpToPowerOf2(size_bytes);
    }
}

RingBuffer::~RingBuffer() = default;

size_t RingBuffer::roundUpToPowerOf2(size_t n) {
    if (n == 0) return 1;
    if ((n & (n - 1)) == 0) return n; // Already power of 2
    
    // Find the next power of 2
    size_t power = 1;
    while (power < n) {
        power <<= 1;
        if (power == 0) {
            // Overflow - return max possible
            return 1ULL << 63;
        }
    }
    return power;
}

size_t RingBuffer::write(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return 0;
    }
    
    // Load current tail and head
    size_t current_tail = tail_.load(std::memory_order_relaxed);
    size_t current_head = head_.load(std::memory_order_acquire);
    
    // Calculate available space
    // Keep one byte free to distinguish empty from full
    size_t available_space = (current_head > current_tail) 
        ? (current_head - current_tail - 1)
        : (size_ - (current_tail - current_head) - 1);
    
    // Limit write size to available space
    size_t write_size = std::min(size, available_space);
    if (write_size == 0) {
        return 0; // Buffer is full
    }
    
    // Calculate write positions using mask for indexing
    size_t tail_masked = current_tail & mask();
    size_t space_to_end = size_ - tail_masked;
    
    if (write_size <= space_to_end) {
        // Single contiguous write
        std::memcpy(&buffer_[tail_masked], data, write_size);
    } else {
        // Wrapped write (two parts)
        std::memcpy(&buffer_[tail_masked], data, space_to_end);
        std::memcpy(&buffer_[0], data + space_to_end, write_size - space_to_end);
    }
    
    // Update tail atomically (keep as unbounded counter, mask only for indexing)
    tail_.store(current_tail + write_size, std::memory_order_release);
    
    return write_size;
}

size_t RingBuffer::read(uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return 0;
    }
    
    // Load current head and tail
    size_t current_head = head_.load(std::memory_order_relaxed);
    size_t current_tail = tail_.load(std::memory_order_acquire);
    
    // Calculate available data
    size_t available_data = (current_tail >= current_head)
        ? (current_tail - current_head)
        : 0;
    
    // Limit read size to available data
    size_t read_size = std::min(size, available_data);
    if (read_size == 0) {
        return 0; // Buffer is empty
    }
    
    // Calculate read positions using mask for indexing
    size_t head_masked = current_head & mask();
    size_t space_to_end = size_ - head_masked;
    
    if (read_size <= space_to_end) {
        // Single contiguous read
        std::memcpy(data, &buffer_[head_masked], read_size);
    } else {
        // Wrapped read (two parts)
        std::memcpy(data, &buffer_[head_masked], space_to_end);
        std::memcpy(data + space_to_end, &buffer_[0], read_size - space_to_end);
    }
    
    // Update head atomically (keep as unbounded counter, mask only for indexing)
    head_.store(current_head + read_size, std::memory_order_release);
    
    return read_size;
}

size_t RingBuffer::available() const {
    size_t current_head = head_.load(std::memory_order_acquire);
    size_t current_tail = tail_.load(std::memory_order_relaxed);
    
    if (current_tail >= current_head) {
        return current_tail - current_head;
    } else {
        // Head wrapped around (shouldn't happen in normal operation)
        return 0;
    }
}

size_t RingBuffer::free() const {
    size_t current_head = head_.load(std::memory_order_acquire);
    size_t current_tail = tail_.load(std::memory_order_relaxed);
    
    if (current_head > current_tail) {
        return current_head - current_tail - 1;
    } else {
        return size_ - (current_tail - current_head) - 1;
    }
}

bool RingBuffer::isFull() const {
    return free() == 0;
}

bool RingBuffer::isEmpty() const {
    return available() == 0;
}

void RingBuffer::reset() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
}

