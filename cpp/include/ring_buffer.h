#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

/**
 * Lock-free ring buffer for high-throughput single-producer/single-consumer pattern.
 * 
 * Thread-safe when used with:
 * - Single producer thread calling write()
 * - Single consumer thread calling read()
 * 
 * The ring buffer uses atomic operations for head/tail pointers to avoid
 * mutex overhead in the fast path.
 */
class RingBuffer {
public:
    /**
     * Construct a ring buffer with specified size.
     * @param size_bytes Size in bytes (will be rounded up to next power of 2)
     */
    explicit RingBuffer(size_t size_bytes);
    
    ~RingBuffer();
    
    // Non-copyable, non-movable
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    
    /**
     * Write data to the ring buffer (producer).
     * @param data Source buffer
     * @param size Number of bytes to write
     * @return Number of bytes actually written (may be less if buffer is full)
     */
    size_t write(const uint8_t* data, size_t size);
    
    /**
     * Read data from the ring buffer (consumer).
     * @param data Destination buffer
     * @param size Maximum number of bytes to read
     * @return Number of bytes actually read
     */
    size_t read(uint8_t* data, size_t size);
    
    /**
     * Get number of bytes available to read.
     * @return Available bytes
     */
    size_t available() const;
    
    /**
     * Get number of bytes free for writing.
     * @return Free bytes
     */
    size_t free() const;
    
    /**
     * Check if buffer is full.
     * @return True if buffer is full
     */
    bool isFull() const;
    
    /**
     * Check if buffer is empty.
     * @return True if buffer is empty
     */
    bool isEmpty() const;
    
    /**
     * Reset the ring buffer (clear all data).
     * Not thread-safe - must ensure no concurrent access.
     */
    void reset();
    
    /**
     * Get the capacity of the ring buffer.
     * @return Capacity in bytes
     */
    size_t capacity() const { return size_; }
    
private:
    // Actual buffer size (must be power of 2 for efficient modulo)
    size_t size_;
    
    // Buffer storage
    std::vector<uint8_t> buffer_;
    
    // Atomic head (read position) and tail (write position)
    // Using relaxed memory ordering for better performance
    mutable std::atomic<size_t> head_;
    mutable std::atomic<size_t> tail_;
    
    /**
     * Compute mask for efficient modulo operation (requires size_ to be power of 2).
     */
    size_t mask() const { return size_ - 1; }
    
    /**
     * Round up to next power of 2.
     */
    static size_t roundUpToPowerOf2(size_t n);
};

#endif // RING_BUFFER_H

