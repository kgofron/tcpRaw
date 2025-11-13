/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 8, 2025
 */

#include "tcp_server.h"
#include "tpx3_decoder.h"
#include "timestamp_extension.h"
#include "hit_processor.h"
#include "tpx3_packets.h"
#include "packet_reorder_buffer.h"

#include <iostream>
#include <cstring>
#include <iomanip>
#include <string>
#include <bitset>
#include <chrono>
#include <memory>
#include <csignal>
#include <atomic>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <thread>
#include <condition_variable>
#include <queue>

static std::string format_type_label(const std::string& prefix, uint8_t type) {
    std::ostringstream oss;
    oss << prefix << " (0x" << std::hex << std::uppercase << std::setfill('0')
        << std::setw(2) << static_cast<int>(type) << ")";
    return oss.str();
}

void process_packet(uint64_t word, uint8_t chip_index, HitProcessor& processor, const ChunkMetadata& chunk_meta, bool enable_accounting = true);

struct StreamState {
    bool in_chunk = false;
    size_t chunk_words_remaining = 0;
    uint8_t chip_index = 0;
    uint64_t current_chunk_id = 0;
    uint64_t local_chunk_count = 0;  // Local counter to avoid mutex locks
    uint64_t pending_chunk_updates = 0;  // Batch chunk count updates
    ChunkMetadata chunk_meta{};
    std::vector<ExtraTimestamp> extra_timestamps;
    bool saw_first_chunk_header = false;
    bool mid_stream_flagged = false;
    std::vector<uint64_t> batch_buffer;  // Batch buffer for dispatcher submissions

    StreamState() {
        extra_timestamps.reserve(3);
        batch_buffer.reserve(128);  // Pre-allocate batch buffer
    }
};

struct DecodeTask {
    uint64_t word = 0;
    uint8_t chip_index = 0;
    ChunkMetadata chunk_meta{};
};

// Thread-safe queue for raw data buffers between network and processing threads
class RawDataQueue {
public:
    struct Buffer {
        std::vector<uint8_t> data;
        size_t size = 0;
        
        Buffer() = default;
        Buffer(const uint8_t* src, size_t len) : data(src, src + len), size(len) {}
    };
    
    RawDataQueue(size_t max_buffers = 100) 
        : max_buffers_(max_buffers), 
          stop_(false),
          dropped_buffers_(0) {}
    
    // Push a buffer (non-blocking, drops if full)
    // Returns true if successfully enqueued, false if dropped
    bool push(const uint8_t* data, size_t size) {
        if (stop_.load(std::memory_order_acquire)) {
            return false;
        }
        
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Drop oldest buffer if queue is full (flow control)
        if (queue_.size() >= max_buffers_) {
            queue_.pop();
            dropped_buffers_.fetch_add(1, std::memory_order_relaxed);
        }
        
        queue_.emplace(data, size);
        lock.unlock();
        cond_.notify_one();
        return true;
    }
    
    // Pop a buffer (blocking with timeout)
    // Returns true if buffer was retrieved, false if timeout or stopped
    bool pop(Buffer& buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        bool notified = cond_.wait_for(lock, timeout, [this]() {
            return !queue_.empty() || stop_.load(std::memory_order_acquire);
        });
        
        if (!notified || queue_.empty()) {
            return false;
        }
        
        buffer = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    // Signal shutdown
    void stop() {
        stop_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        cond_.notify_all();
    }
    
    // Check if stopped
    bool isStopped() const {
        return stop_.load(std::memory_order_acquire);
    }
    
    // Get number of dropped buffers
    uint64_t getDroppedBuffers() const {
        return dropped_buffers_.load(std::memory_order_acquire);
    }
    
    // Get current queue size (approximate)
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
private:
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::queue<Buffer> queue_;
    size_t max_buffers_;
    std::atomic<bool> stop_;
    std::atomic<uint64_t> dropped_buffers_;
};

class DecodeDispatcher {
public:
    struct PartialStats {
        uint64_t hits = 0;
        uint64_t tdc1 = 0;
        uint64_t tdc2 = 0;
        uint64_t earliest_hit_tick = std::numeric_limits<uint64_t>::max();
        uint64_t latest_hit_tick = 0;
        uint64_t earliest_tdc1_tick = std::numeric_limits<uint64_t>::max();
        uint64_t latest_tdc1_tick = 0;
        std::array<uint64_t, 4> chip_hits{};
        std::array<uint64_t, 4> chip_tdc1{};
        std::array<uint64_t, 4> chip_tdc2{};
        std::array<uint64_t, 4> chip_tdc1_min{};
        std::array<uint64_t, 4> chip_tdc1_max{};
        std::vector<PixelHit> recent_hits;

        void mergeInto(HitProcessor& processor) {
            if (hits == 0 && tdc1 == 0 && tdc2 == 0 && recent_hits.empty()) {
                return;
            }
            std::lock_guard<std::recursive_mutex> lock(processor.mutex_);
            processor.stats_.total_hits += hits;
            processor.stats_.total_tdc1_events += tdc1;
            processor.stats_.total_tdc2_events += tdc2;
            processor.stats_.total_tdc_events += (tdc1 + tdc2);
            for (size_t chip = 0; chip < 4; ++chip) {
                processor.chip_hit_totals_[chip] += chip_hits[chip];
                processor.stats_.chip_hit_rate_valid[chip] =
                    processor.stats_.chip_hit_rate_valid[chip] || chip_hits[chip] > 0;
                processor.stats_.chip_tdc1_counts[chip] += chip_tdc1[chip];
                if (chip_tdc1[chip] > 0) {
                    processor.stats_.chip_tdc1_present[chip] = true;
                    processor.chip_tdc1_min_ticks_[chip] =
                        std::min(processor.chip_tdc1_min_ticks_[chip], chip_tdc1_min[chip]);
                    processor.chip_tdc1_max_ticks_[chip] =
                        std::max(processor.chip_tdc1_max_ticks_[chip], chip_tdc1_max[chip]);
                }
            }
            if (hits > 0) {
                if (!processor.stats_.hit_time_initialized ||
                    earliest_hit_tick < processor.stats_.earliest_hit_time_ticks) {
                    processor.stats_.earliest_hit_time_ticks = earliest_hit_tick;
                    processor.stats_.hit_time_initialized = true;
                }
                if (latest_hit_tick > processor.stats_.latest_hit_time_ticks) {
                    processor.stats_.latest_hit_time_ticks = latest_hit_tick;
                }
            }
            if (tdc1 > 0) {
                if (!processor.stats_.tdc1_time_initialized ||
                    earliest_tdc1_tick < processor.stats_.earliest_tdc1_time_ticks) {
                    processor.stats_.earliest_tdc1_time_ticks = earliest_tdc1_tick;
                    processor.stats_.tdc1_time_initialized = true;
                }
                if (latest_tdc1_tick > processor.stats_.latest_tdc1_time_ticks) {
                    processor.stats_.latest_tdc1_time_ticks = latest_tdc1_tick;
                }
            }
            if (processor.recent_hit_capacity_ > 0) {
                for (const auto& hit : recent_hits) {
                    if (processor.recent_hits_buffer_.size() != processor.recent_hit_capacity_) {
                        processor.recent_hits_buffer_.assign(
                            processor.recent_hit_capacity_, PixelHit{});
                    }
                    processor.recent_hits_buffer_[processor.recent_hits_head_] = hit;
                    processor.recent_hits_head_ =
                        (processor.recent_hits_head_ + 1) % processor.recent_hit_capacity_;
                    if (processor.recent_hits_size_ < processor.recent_hit_capacity_) {
                        processor.recent_hits_size_++;
                    }
                }
            }
        }

        void reset(size_t recent_capacity) {
            hits = tdc1 = tdc2 = 0;
            earliest_hit_tick = std::numeric_limits<uint64_t>::max();
            latest_hit_tick = 0;
            earliest_tdc1_tick = std::numeric_limits<uint64_t>::max();
            latest_tdc1_tick = 0;
            chip_hits.fill(0);
            chip_tdc1.fill(0);
            chip_tdc2.fill(0);
            chip_tdc1_min.fill(std::numeric_limits<uint64_t>::max());
            chip_tdc1_max.fill(0);
            recent_hits.clear();
            if (recent_capacity > 0) {
                recent_hits.reserve(recent_capacity);
            }
        }
    };

    DecodeDispatcher(size_t num_workers, HitProcessor& processor, size_t recent_cap)
        : processor_(processor),
          stop_(false),
          pending_tasks_(0),
          recent_capacity_(recent_cap) {
        size_t workers = std::max<size_t>(1, num_workers);
        worker_data_.reserve(workers);
        for (size_t i = 0; i < workers; ++i) {
            worker_data_.emplace_back(std::make_unique<WorkerData>(recent_capacity_));
        }
        workers_.reserve(workers);
        for (size_t i = 0; i < workers; ++i) {
            workers_.emplace_back([this, i]() { workerLoop(i); });
        }
    }

    ~DecodeDispatcher() { stop(); }

    void submit(uint64_t word, uint8_t chip_index, const ChunkMetadata& meta) {
        size_t index = chip_index % worker_data_.size();
        pending_tasks_.fetch_add(1, std::memory_order_release);
        auto& data = *worker_data_[index];
        {
            std::lock_guard<std::mutex> lock(data.mutex);
            data.queue.push(DecodeTask{word, chip_index, meta});
        }
        // Notify worker (notify_one is cheap, and ensures workers stay responsive)
        data.cond.notify_one();
    }

    // Batch submit multiple words to reduce mutex contention
    void submitBatch(const std::vector<uint64_t>& words, uint8_t chip_index, const ChunkMetadata& meta) {
        if (words.empty()) return;
        size_t index = chip_index % worker_data_.size();
        pending_tasks_.fetch_add(words.size(), std::memory_order_release);
        auto& data = *worker_data_[index];
        {
            std::lock_guard<std::mutex> lock(data.mutex);
            for (uint64_t word : words) {
                data.queue.push(DecodeTask{word, chip_index, meta});
            }
        }
        // Only notify once after batch submission
        data.cond.notify_one();
    }

    void waitUntilIdle() {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        idle_cv_.wait(lock, [this]() {
            return pending_tasks_.load(std::memory_order_acquire) == 0;
        });
        flushAll();
    }

    void stop() {
        bool expected = false;
        if (!stop_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        for (auto& data : worker_data_) {
            data->cond.notify_all();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        flushAll();
    }

    void flushAll() {
        for (auto& data : worker_data_) {
            flushWorker(*data);
        }
    }

private:
    struct WorkerData {
        explicit WorkerData(size_t recent_capacity) : stats() {
            stats.reset(recent_capacity);
        }
        std::mutex mutex;
        std::condition_variable cond;
        std::queue<DecodeTask> queue;
        std::mutex stats_mutex;
        PartialStats stats;
    };

    HitProcessor& processor_;
    std::vector<std::thread> workers_;
    std::vector<std::unique_ptr<WorkerData>> worker_data_;
    std::atomic<bool> stop_;
    std::atomic<size_t> pending_tasks_;
    std::mutex pending_mutex_;
    std::condition_variable idle_cv_;
    size_t recent_capacity_;

    void workerLoop(size_t index) {
        while (true) {
            DecodeTask task;
            {
                auto& data = *worker_data_[index];
                std::unique_lock<std::mutex> lock(data.mutex);
                data.cond.wait(lock, [this, &data]() {
                    return stop_.load(std::memory_order_acquire) || !data.queue.empty();
                });

                if (stop_.load(std::memory_order_acquire) && data.queue.empty()) {
                    break;
                }

                if (!data.queue.empty()) {
                    task = data.queue.front();
                    data.queue.pop();
                } else {
                    continue;
                }
            }

            processDecoded(task, *worker_data_[index]);

            size_t remaining =
                pending_tasks_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0) {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                idle_cv_.notify_all();
            }
        }
    }

    void processDecoded(const DecodeTask& task, WorkerData& data) {
        PartialStats& stats = data.stats;
        uint8_t full_type = (task.word >> 56) & 0xFF;
        if (full_type == SPIDR_PACKET_ID || full_type == TPX3_CONTROL ||
            full_type == EXTRA_TIMESTAMP || full_type == EXTRA_TIMESTAMP_MPX3 ||
            full_type == GLOBAL_TIME_LOW || full_type == GLOBAL_TIME_HIGH) {
            process_packet(task.word, task.chip_index, processor_, task.chunk_meta);
            return;
        }
        uint8_t packet_type = (task.word >> 60) & 0xF;
        switch (packet_type) {
            case PIXEL_COUNT_FB:
            case PIXEL_STANDARD: {
                try {
                    PixelHit hit = decode_pixel_data(task.word, task.chip_index);
                    if (task.chunk_meta.has_extra_packets) {
                        uint64_t truncated_toa = hit.toa_ns & 0x3FFFFFFF;
                        hit.toa_ns =
                            extend_timestamp(truncated_toa, task.chunk_meta.min_timestamp_ns, 30);
                    }
                    std::lock_guard<std::mutex> lock(data.stats_mutex);
                    stats.hits++;
                    stats.chip_hits[hit.chip_index]++;
                    stats.earliest_hit_tick =
                        std::min(stats.earliest_hit_tick, hit.toa_ns);
                    stats.latest_hit_tick =
                        std::max(stats.latest_hit_tick, hit.toa_ns);
                    if (recent_capacity_ > 0 &&
                        stats.recent_hits.size() < recent_capacity_) {
                        stats.recent_hits.push_back(hit);
                    }
                } catch (...) {
                    process_packet(task.word, task.chip_index, processor_, task.chunk_meta);
                }
                break;
            }
            case TDC_DATA: {
                try {
                    TDCEvent tdc = decode_tdc_data(task.word);
                    std::lock_guard<std::mutex> lock(data.stats_mutex);
                    if (tdc.type == TDC1_RISE || tdc.type == TDC1_FALL) {
                        stats.tdc1++;
                        stats.chip_tdc1[task.chip_index]++;
                        stats.earliest_tdc1_tick =
                            std::min(stats.earliest_tdc1_tick, tdc.timestamp_ns);
                        stats.latest_tdc1_tick =
                            std::max(stats.latest_tdc1_tick, tdc.timestamp_ns);
                        stats.chip_tdc1_min[task.chip_index] =
                            std::min(stats.chip_tdc1_min[task.chip_index], tdc.timestamp_ns);
                        stats.chip_tdc1_max[task.chip_index] =
                            std::max(stats.chip_tdc1_max[task.chip_index], tdc.timestamp_ns);
                    } else if (tdc.type == TDC2_RISE || tdc.type == TDC2_FALL) {
                        stats.tdc2++;
                        stats.chip_tdc2[task.chip_index]++;
                    }
                } catch (...) {
                    process_packet(task.word, task.chip_index, processor_, task.chunk_meta);
                }
                break;
            }
            default:
                process_packet(task.word, task.chip_index, processor_, task.chunk_meta);
                break;
        }
    }

    void flushWorker(WorkerData& data) {
        PartialStats local;
        {
            std::lock_guard<std::mutex> lock(data.stats_mutex);
            local = data.stats;
            data.stats.reset(recent_capacity_);
        }
        local.mergeInto(processor_);
    }
};

// Helper function to process a single packet (used by reorder buffer callback)
void process_packet(uint64_t word, uint8_t chip_index, HitProcessor& processor, const ChunkMetadata& chunk_meta, bool enable_accounting) {
    // Check full-byte types first (0x50, 0x71, etc. that can't be distinguished by 4-bit)
    uint8_t full_type = (word >> 56) & 0xFF;
    
    if (full_type == SPIDR_PACKET_ID) {
        if (enable_accounting) {
            processor.addPacketBytes("SPIDR packet ID (0x50)", 8);
        }
        // SPIDR packet ID (0x50)
        uint64_t packet_count;
        if (decode_spidr_packet_id(word, packet_count)) {
            // Packet count tracking
        }
        return;
    }
    
    if (full_type == TPX3_CONTROL) {
        if (enable_accounting) {
            processor.addPacketBytes("TPX3 control (0x71)", 8);
        }
        // TPX3 control (0x71)
        Tpx3ControlCmd cmd;
        if (decode_tpx3_control(word, cmd)) {
            // Control command decoded
        }
        return;
    }
    
    if (full_type == EXTRA_TIMESTAMP || full_type == EXTRA_TIMESTAMP_MPX3) {
        if (enable_accounting) {
            processor.addPacketBytes(format_type_label("Extra timestamp", full_type), 8);
        }
        // Extra timestamp packets - handled separately in main processing loop
        return;
    }
    
    if (full_type == GLOBAL_TIME_LOW || full_type == GLOBAL_TIME_HIGH) {
        if (enable_accounting) {
            processor.addPacketBytes(format_type_label("Global time", full_type), 8);
        }
        // GlobalTime gt = decode_global_time(word);
        // Future: Use for time extension
        return;
    }
    
    // For other packets, use 4-bit type
    uint8_t packet_type = (word >> 60) & 0xF;
    if (enable_accounting) {
        processor.incrementPacketType(packet_type);
    }
    
    switch (packet_type) {
        case PIXEL_COUNT_FB:
        case PIXEL_STANDARD: {
            if (enable_accounting) {
                if (packet_type == PIXEL_COUNT_FB) {
                    processor.addPacketBytes("Pixel count_fb (0x0a)", 8);
                } else {
                    processor.addPacketBytes("Pixel standard (0x0b)", 8);
                }
            }
            try {
                PixelHit hit = decode_pixel_data(word, chip_index);
                
                // Apply timestamp extension if we have chunk metadata
                if (chunk_meta.has_extra_packets) {
                    // Extract 30-bit timestamp
                    uint64_t truncated_toa = hit.toa_ns & 0x3FFFFFFF;
                    hit.toa_ns = extend_timestamp(truncated_toa, chunk_meta.min_timestamp_ns, 30);
                }
                
                processor.addHit(hit);
            } catch (const std::exception& e) {
                processor.incrementDecodeError();
                // Only print first few errors to avoid flooding output
                static int pixel_error_count = 0;
                if (pixel_error_count++ < 5) {
                    std::cerr << "Error decoding pixel data: " << e.what() << std::endl;
                }
            }
            break;
        }
        
        case TDC_DATA: {
            if (enable_accounting) {
                processor.addPacketBytes("TDC data (0x06)", 8);
            }
            try {
                TDCEvent tdc = decode_tdc_data(word);
                processor.addTdcEvent(tdc, chip_index);
            } catch (const std::exception& e) {
                processor.incrementDecodeError();
                // Check if this is a fractional error
                std::string error_msg = e.what();
                if (error_msg.find("fractional") != std::string::npos) {
                    processor.incrementFractionalError();
                }
                // Only print first few errors to avoid flooding output
                static int tdc_error_count = 0;
                if (tdc_error_count++ < 5) {
                    std::cerr << "Error decoding TDC data: " << error_msg << std::endl;
                }
            }
            break;
        }
        
        case SPIDR_CONTROL: {
            if (enable_accounting) {
                processor.addPacketBytes("SPIDR control (0x05)", 8);
            }
            SpidrControl ctrl;
            if (decode_spidr_control(word, ctrl)) {
                processor.incrementChunkCount();
            }
            break;
        }
        
        default: {
            if (enable_accounting) {
                std::ostringstream label;
                label << "Unknown packet type (0x" << std::hex << std::uppercase
                      << static_cast<int>(packet_type) << ")";
                processor.addPacketBytes(label.str(), 8);
                processor.incrementUnknownPacket();
            }
            break;
        }
    }
}

// Flush batch buffer to dispatcher or process directly
static void flushBatch(StreamState& state, HitProcessor& processor, DecodeDispatcher* dispatcher, bool enable_accounting) {
    if (state.batch_buffer.empty()) return;
    
    if (dispatcher) {
        dispatcher->submitBatch(state.batch_buffer, state.chip_index, state.chunk_meta);
    } else {
        for (uint64_t word : state.batch_buffer) {
            process_packet(word, state.chip_index, processor, state.chunk_meta, enable_accounting);
        }
    }
    state.batch_buffer.clear();
}

// Process raw data buffer
void process_raw_data(const uint8_t* buffer, size_t bytes, HitProcessor& processor, StreamState& state,
                      DecodeDispatcher* dispatcher, PacketReorderBuffer* reorder_buffer = nullptr,
                      bool enable_accounting = true) {
    const uint64_t* data_words = reinterpret_cast<const uint64_t*>(buffer);
    size_t num_words = bytes / 8;
    constexpr size_t BATCH_SIZE = 128;  // Batch size for dispatcher submissions
    
    for (size_t i = 0; i < num_words; ++i) {
        uint64_t word = data_words[i];
        
        // Fast inline chunk header check (avoid struct creation on hot path)
        // TPX3_MAGIC is 0x33585054 ('TPX3' in little-endian)
        if ((word & 0xFFFFFFFFULL) == TPX3_MAGIC) {
            // Flush any pending batch before starting new chunk
            flushBatch(state, processor, dispatcher, enable_accounting);
            
            // Found chunk header - inline field access to avoid struct creation
            if (enable_accounting) {
                processor.addPacketBytes("Chunk header", 8);
            }
            state.saw_first_chunk_header = true;
            // Note: chunk size includes the header word itself
            // So we set chunk_words_remaining to chunkSize/8, which includes header
            // We then continue to skip the header, so we process (chunkSize/8 - 1) data words
            state.in_chunk = true;
            // Inline field access: chunkSize() = (word >> 48) & 0xFFFF, chipIndex() = (word >> 32) & 0xFF
            state.chunk_words_remaining = ((word >> 48) & 0xFFFF) / 8;
            state.chip_index = (word >> 32) & 0xFF;
            
            // Use local counter to avoid mutex lock on getStatistics()
            // This eliminates the expensive getStatistics() call that acquires a mutex
            state.local_chunk_count++;
            state.current_chunk_id = state.local_chunk_count;
            state.pending_chunk_updates++;
            
            // Batch update chunk count to reduce mutex contention (update every 100 chunks)
            // In performance mode, batch updates significantly reduce lock contention
            // Instead of 100 mutex locks, we use 1 mutex lock per 100 chunks
            constexpr uint64_t CHUNK_UPDATE_BATCH = 100;
            if (state.pending_chunk_updates >= CHUNK_UPDATE_BATCH) {
                // Batch update: increment by pending count in a single mutex lock
                processor.incrementChunkCountBatch(state.pending_chunk_updates);
                state.pending_chunk_updates = 0;
            }
            
            // If we have a reorder buffer, reset it for new chunk
            if (reorder_buffer) {
                reorder_buffer->resetForNewChunk(state.current_chunk_id);
            }
            
            // Reset chunk metadata
            state.chunk_meta = {};
            state.extra_timestamps.clear();
            
            continue;
        }
        
        if (!state.in_chunk || state.chunk_words_remaining == 0) {
            if (!state.saw_first_chunk_header && !state.mid_stream_flagged) {
                processor.markMidStreamStart();
                state.mid_stream_flagged = true;
            }
            if (enable_accounting) {
                processor.addPacketBytes("Unassigned (outside chunk)", 8);
            }
            continue;
        }
        
        state.chunk_words_remaining--;
        
        // Fast path: Check packet type byte first (most words are pixel data)
        uint8_t full_type = (word >> 56) & 0xFF;
        
        // Check if we're near the end of chunk (last 3 words are extra timestamps)
        bool is_near_end = (state.chunk_words_remaining <= 3);
        
        if (is_near_end && (full_type == EXTRA_TIMESTAMP || full_type == EXTRA_TIMESTAMP_MPX3)) {
            // Flush batch before processing extra timestamp (chunk_meta may change)
            flushBatch(state, processor, dispatcher, enable_accounting);
            
            // Extra timestamp packet (rare - only at end of chunk)
            uint8_t extra_type = static_cast<uint8_t>(full_type);
            if (enable_accounting) {
                processor.addPacketBytes(format_type_label("Extra timestamp", extra_type), 8);
            }
            ExtraTimestamp extra_ts = decode_extra_timestamp(word);
            state.extra_timestamps.push_back(extra_ts);
            
            // When we have all 3 extra packets, process them
            if (state.extra_timestamps.size() == 3) {
                state.chunk_meta.has_extra_packets = true;
                state.chunk_meta.packet_gen_time_ns = state.extra_timestamps[0].timestamp_ns;
                state.chunk_meta.min_timestamp_ns = state.extra_timestamps[1].timestamp_ns;
                state.chunk_meta.max_timestamp_ns = state.extra_timestamps[2].timestamp_ns;
                
                processor.processChunkMetadata(state.chunk_meta);
            }
        } else if (full_type == SPIDR_PACKET_ID && reorder_buffer) {
            // Flush batch before processing SPIDR packet ID (needs reordering)
            flushBatch(state, processor, dispatcher, enable_accounting);
            
            // SPIDR packet ID packet (needs reordering) - decode and reorder
            uint64_t packet_count = 0;
            if (decode_spidr_packet_id(word, packet_count)) {
                reorder_buffer->processPacket(word, packet_count, state.current_chunk_id,
                    [&processor, &state, dispatcher, enable_accounting](uint64_t w, uint64_t /*id*/, uint64_t /*chunk*/) {
                        // Callback: process reordered packet
                        if (dispatcher) {
                            dispatcher->submit(w, state.chip_index, state.chunk_meta);
                        } else {
                            process_packet(w, state.chip_index, processor, state.chunk_meta, enable_accounting);
                        }
                    });
            } else {
                // Decode failed, submit directly
                if (dispatcher) {
                    dispatcher->submit(word, state.chip_index, state.chunk_meta);
                } else {
                    process_packet(word, state.chip_index, processor, state.chunk_meta, enable_accounting);
                }
            }
        } else {
            // Fast path: Regular packet (most common case - pixel data, TDC, control, etc.)
            // Collect in batch buffer to reduce mutex contention
            state.batch_buffer.push_back(word);
            
            // Flush batch when it reaches BATCH_SIZE
            if (state.batch_buffer.size() >= BATCH_SIZE) {
                flushBatch(state, processor, dispatcher, enable_accounting);
            }
        }
        
        if (state.chunk_words_remaining == 0) {
            // Flush batch at chunk boundary
            flushBatch(state, processor, dispatcher, enable_accounting);
            state.in_chunk = false;
        }
    }
    
    // Flush any remaining batch at end of buffer
    flushBatch(state, processor, dispatcher, enable_accounting);
    
    // Flush pending chunk count updates
    if (state.pending_chunk_updates > 0) {
        processor.incrementChunkCountBatch(state.pending_chunk_updates);
        state.pending_chunk_updates = 0;
    }
    
    if (reorder_buffer) {
        const auto& reorder_stats = reorder_buffer->getStatistics();
        processor.updateReorderStats(
            reorder_stats.packets_reordered,
            reorder_stats.max_reorder_distance,
            reorder_stats.buffer_overflows,
            reorder_stats.packets_dropped_too_old);
    }
}

void print_statistics(const HitProcessor& processor) {
    const Statistics& stats = processor.getStatistics();
    
    // Calculate elapsed time for display (estimated from cumulative rate)
    double elapsed_seconds = 0.0;
    if (stats.cumulative_hit_rate_hz > 0.0) {
        elapsed_seconds = stats.total_hits / stats.cumulative_hit_rate_hz;
    }
    
    std::cout << "\n=== Statistics ===" << std::endl;
    if (elapsed_seconds > 0.0) {
        std::cout << "Elapsed time: " << std::fixed << std::setprecision(1) 
                  << elapsed_seconds << " s (" << (elapsed_seconds / 60.0) << " min)" << std::endl;
    }
    std::cout << "Total hits: " << stats.total_hits << std::endl;
    std::cout << "Total chunks: " << stats.total_chunks << std::endl;
    std::cout << "Total TDC events: " << stats.total_tdc_events << std::endl;
    std::cout << "Total control packets: " << stats.total_control_packets << std::endl;
    std::cout << "Total decode errors: " << stats.total_decode_errors << std::endl;
    std::cout << "Total fractional errors: " << stats.total_fractional_errors << std::endl;
    std::cout << "Total unknown packets: " << stats.total_unknown_packets << std::endl;
    std::cout << "Hit rate (instant): " << std::fixed << std::setprecision(2) 
              << stats.hit_rate_hz << " Hz" << std::endl;
    std::cout << "Hit rate (cumulative avg): " << std::fixed << std::setprecision(2) 
              << stats.cumulative_hit_rate_hz << " Hz" << std::endl;
    std::cout << "Tdc1 rate (instant): " << std::fixed << std::setprecision(2) 
              << stats.tdc1_rate_hz << " Hz" << std::endl;
    std::cout << "Tdc1 rate (cumulative avg, detector-wide): " << std::fixed << std::setprecision(2) 
              << stats.cumulative_tdc1_rate_hz << " Hz" << std::endl;
    std::cout << "Tdc2 rate (instant): " << std::fixed << std::setprecision(2) 
              << stats.tdc2_rate_hz << " Hz" << std::endl;
    std::cout << "Tdc2 rate (cumulative avg): " << std::fixed << std::setprecision(2) 
              << stats.cumulative_tdc2_rate_hz << " Hz" << std::endl;
    
    constexpr double TOA_UNIT_SECONDS = 1.5625e-9;
    if (stats.hit_time_initialized && stats.latest_hit_time_ticks > stats.earliest_hit_time_ticks) {
        double span_seconds = (stats.latest_hit_time_ticks - stats.earliest_hit_time_ticks) * TOA_UNIT_SECONDS;
        std::cout << "Data span (hits): " << std::fixed << std::setprecision(3)
                  << span_seconds << " s" << std::endl;
    } else {
        std::cout << "Data span (hits): <insufficient span>" << std::endl;
    }
    if (stats.tdc1_time_initialized && stats.latest_tdc1_time_ticks > stats.earliest_tdc1_time_ticks) {
        double span_seconds = (stats.latest_tdc1_time_ticks - stats.earliest_tdc1_time_ticks) * TOA_UNIT_SECONDS;
        std::cout << "Data span (tdc1): " << std::fixed << std::setprecision(3)
                  << span_seconds << " s" << std::endl;
    } else if (stats.total_tdc1_events > 0) {
        std::cout << "Data span (tdc1): <insufficient span>" << std::endl;
    }
    if (stats.started_mid_stream) {
        std::cout << "⚠ Detected data before first chunk header (attached mid-stream)." << std::endl;
    }
    
    std::cout << "Out-of-order packets (reordered): " << stats.total_reordered_packets << std::endl;
    std::cout << "Max reorder distance: " << stats.reorder_max_distance << std::endl;
    std::cout << "Reorder buffer overflows: " << stats.reorder_buffer_overflows << std::endl;
    std::cout << "Packets dropped as too old: " << stats.reorder_packets_dropped_too_old << std::endl;
    
    if (!stats.packet_type_counts.empty()) {
        std::cout << "Packet type breakdown:" << std::endl;
        for (const auto& pair : stats.packet_type_counts) {
            uint8_t type = pair.first;
            uint64_t count = pair.second;
            std::cout << "  Type 0x" << std::hex << static_cast<int>(type) << std::dec
                      << " (0b" << std::bitset<4>(type) << "): " << count << std::endl;
        }
    }
    
    bool any_chip_hit_rate = false;
    for (size_t chip = 0; chip < stats.chip_hit_rate_valid.size(); ++chip) {
        if (stats.chip_hit_rate_valid[chip]) {
            any_chip_hit_rate = true;
            break;
        }
    }
    if (any_chip_hit_rate) {
        std::cout << "Per-chip hit rates:" << std::endl;
        for (size_t chip = 0; chip < stats.chip_hit_rates_hz.size(); ++chip) {
            if (!stats.chip_hit_rate_valid[chip]) {
                continue;
            }
            std::cout << "  Chip " << chip
                      << ": " << std::fixed << std::setprecision(2)
                      << stats.chip_hit_rates_hz[chip] << " Hz" << std::endl;
        }
    }
    
    bool any_chip_tdc1 = false;
    for (size_t chip = 0; chip < stats.chip_tdc1_present.size(); ++chip) {
        if (stats.chip_tdc1_present[chip]) {
            any_chip_tdc1 = true;
            break;
        }
    }
    if (any_chip_tdc1) {
        std::cout << "Per-chip TDC1 rates (averaged per chip, for diagnostics):" << std::endl;
        for (size_t chip = 0; chip < stats.chip_tdc1_rates_hz.size(); ++chip) {
            if (!stats.chip_tdc1_present[chip]) {
                continue;
            }
            uint64_t total_count = stats.chip_tdc1_counts[chip];
            double cumulative = stats.chip_tdc1_cumulative_rates_hz[chip];
            std::cout << "  Chip " << chip
                      << ": " << std::fixed << std::setprecision(2)
                      << stats.chip_tdc1_rates_hz[chip] << " Hz instant, "
                      << std::setprecision(2) << cumulative << " Hz cumulative"
                      << " (total: " << total_count << ")" << std::endl;
        }
        // Note: Per-chip rates sum may not equal detector-wide rate if different chips
        // have different activity periods. Detector-wide cumulative rate matches SERVAL.
    }
    
    if (!stats.packet_byte_totals.empty()) {
        std::cout << "\n=== Packet Accounting ===" << std::endl;
        std::cout << std::setfill(' ');
        std::cout << std::left << std::setw(35) << "Category"
                  << std::right << std::setw(18) << "Bytes"
                  << std::setw(12) << "%" << std::endl;
        std::cout << std::string(65, '-') << std::endl;
        double total_bytes = static_cast<double>(stats.total_bytes_accounted);
        for (const auto& entry : stats.packet_byte_totals) {
            double pct = (total_bytes > 0.0)
                ? (static_cast<double>(entry.second) * 100.0 / total_bytes)
                : 0.0;
            std::cout << std::left << std::setw(35) << entry.first
                      << std::right << std::setw(18) << entry.second
                      << std::setw(11) << std::fixed << std::setprecision(2) << pct << std::endl;
        }
        std::cout << std::string(65, '-') << std::endl;
        std::cout << std::left << std::setw(35) << "Total"
                  << std::right << std::setw(18) << stats.total_bytes_accounted
                  << std::setw(11) << "100.00" << std::endl;
    }
}

void print_recent_hits(const HitProcessor& processor, size_t count) {
    auto hits = processor.getHits();
    size_t total = hits.size();
    size_t start = (total > count) ? (total - count) : 0;
    size_t to_show = total - start;
    
    std::cout << "\n=== Recent Hits (last " << to_show << ") ===" << std::endl;
    if (to_show == 0) {
        std::cout << "(recent hit history disabled)" << std::endl;
        return;
    }
    
    for (size_t i = start; i < total; ++i) {
        const PixelHit& hit = hits[i];
        std::cout << "Chip " << static_cast<int>(hit.chip_index) 
                  << ", X=" << hit.x << ", Y=" << hit.y
                  << ", ToA=" << hit.toa_ns << " (1.5625ns units)"
                  << ", ToT=" << hit.tot_ns << " ns"
                  << " [" << (hit.is_count_fb ? "count_fb" : "standard") << "]" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 8085;
    bool enable_reorder = false;
    size_t reorder_window_size = 1000;
    size_t stats_interval = 1000;  // Print stats every N packets
    int stats_time_interval = 10;  // Print status every N seconds (0 = disable)
    bool stats_final_only = false; // Only print final statistics
    bool stats_disable = false;    // Completely disable statistics printing
    size_t recent_hit_count = 10;  // Number of recent hits to retain (0 = disable)
    bool exit_on_disconnect = false; // Exit after connection closes (don't auto-reconnect)
    size_t decoder_workers = 0;    // 0 = auto (stream=4, file=1)
    bool decoder_workers_overridden = false;
    size_t queue_size = 2000;      // Queue size for producer/consumer pipeline (default: 2000 buffers)
    std::string input_file;
    bool file_mode = false;
    std::filesystem::path file_path;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--reorder") {
            enable_reorder = true;
        } else if (arg == "--reorder-window" && i + 1 < argc) {
            reorder_window_size = std::stoul(argv[++i]);
        } else if (arg == "--stats-interval" && i + 1 < argc) {
            stats_interval = std::stoul(argv[++i]);
        } else if (arg == "--stats-time" && i + 1 < argc) {
            stats_time_interval = std::stoi(argv[++i]);
        } else if (arg == "--stats-final-only") {
            stats_final_only = true;
            stats_interval = 0;  // Disable periodic stats
        } else if (arg == "--stats-disable") {
            stats_disable = true;
            stats_interval = 0;
            stats_time_interval = 0;
        } else if (arg == "--recent-hit-count" && i + 1 < argc) {
            recent_hit_count = std::stoul(argv[++i]);
        } else if (arg == "--decoder-workers" && i + 1 < argc) {
            decoder_workers = std::stoul(argv[++i]);
            decoder_workers_overridden = true;
        } else if (arg == "--queue-size" && i + 1 < argc) {
            queue_size = std::stoul(argv[++i]);
        } else if (arg == "--exit-on-disconnect") {
            exit_on_disconnect = true;
        } else if (arg == "--input-file" && i + 1 < argc) {
            input_file = argv[++i];
            file_mode = true;
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]" << std::endl;
            std::cout << "Connection options:" << std::endl;
            std::cout << "  --host HOST           TCP server host (default: 127.0.0.1)" << std::endl;
            std::cout << "  --port PORT           TCP server port (default: 8085)" << std::endl;
            std::cout << "  --input-file PATH     Read data from .tpx3 file instead of TCP" << std::endl;
            std::cout << "Reordering options:" << std::endl;
            std::cout << "  --reorder             Enable packet reordering" << std::endl;
            std::cout << "  --reorder-window SIZE Reorder buffer window size (default: 1000)" << std::endl;
            std::cout << "Statistics options (for high-rate performance):" << std::endl;
            std::cout << "  --stats-interval N    Print stats every N packets (default: 1000, 0=disable)" << std::endl;
            std::cout << "  --stats-time N        Print status every N seconds (default: 10, 0=disable)" << std::endl;
            std::cout << "  --stats-final-only    Only print final statistics (no periodic)" << std::endl;
            std::cout << "  --stats-disable       Disable all statistics printing" << std::endl;
            std::cout << "  --recent-hit-count N  Retain N recent hits for summary (default: 10, 0=disable)" << std::endl;
            std::cout << "Performance options:" << std::endl;
            std::cout << "  --decoder-workers N   Number of parallel decoder workers (default: auto)" << std::endl;
            std::cout << "  --queue-size N        Queue size for producer/consumer pipeline (default: 2000)" << std::endl;
            std::cout << "Other options:" << std::endl;
            std::cout << "  --exit-on-disconnect  Exit after connection closes (don't auto-reconnect)" << std::endl;
            std::cout << "  --help                Show this help message" << std::endl;
            return 0;
        }
    }
    
    std::cout << "TPX3 Raw Data Parser" << std::endl;
    if (file_mode) {
        std::cout << "Reading from file: " << input_file << std::endl;
    } else {
        std::cout << "Connecting to " << host << ":" << port << std::endl;
    }
    std::cout << "Packet reordering: " << (enable_reorder ? "enabled" : "disabled");
    if (enable_reorder) {
        std::cout << " (window size: " << reorder_window_size << ")";
    }
    std::cout << std::endl;
    
    if (stats_disable) {
        std::cout << "Statistics: disabled (performance mode)" << std::endl;
    } else if (stats_final_only) {
        std::cout << "Statistics: final only (performance mode)" << std::endl;
    } else {
        if (stats_interval > 0) {
            std::cout << "Statistics: every " << stats_interval << " packets";
        } else {
            std::cout << "Statistics: periodic disabled";
        }
        if (stats_time_interval > 0) {
            std::cout << ", status every " << stats_time_interval << " seconds";
        }
        std::cout << std::endl;
    }
    if (recent_hit_count == 0) {
        std::cout << "Recent hit history: disabled" << std::endl;
    } else {
        std::cout << "Recent hit history: retaining last " << recent_hit_count << " hits" << std::endl;
    }
    
    HitProcessor processor;
    processor.setRecentHitCapacity(recent_hit_count);
    StreamState stream_state;
    size_t worker_count = decoder_workers;
    if (!decoder_workers_overridden) {
        if (file_mode) {
            worker_count = 1;
        } else {
            worker_count = std::max<size_t>(4, std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 4);
        }
    } else if (worker_count == 0) {
        worker_count = file_mode ? 1 : std::max<size_t>(4, std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 4);
    }
    
    std::unique_ptr<DecodeDispatcher> dispatcher;
    if (worker_count > 1) {
        dispatcher = std::make_unique<DecodeDispatcher>(worker_count, processor, recent_hit_count);
    }
    
    std::unique_ptr<PacketReorderBuffer> reorder_buffer;
    if (enable_reorder) {
        reorder_buffer = std::make_unique<PacketReorderBuffer>(reorder_window_size, true);
    }
    
    uint64_t total_bytes_received = 0;
    uint64_t total_packets_received = 0;
    uint64_t bytes_dropped_incomplete = 0;
    bool first_data_received = false;
    auto first_data_time = std::chrono::steady_clock::now();
    size_t print_counter = 0;
    auto last_status_print = std::chrono::steady_clock::now();
    uint64_t last_hits = 0;
    TCPServer::ConnectionStats conn_stats{};
    
    if (file_mode) {
        file_path = std::filesystem::absolute(std::filesystem::path(input_file));
        std::ifstream input(file_path, std::ios::binary);
        if (!input) {
            std::error_code ec(errno, std::generic_category());
            std::cerr << "Failed to open input file: " << file_path << " (" << ec.message() << ")" << std::endl;
            return 1;
        }
        std::cout << "Processing file...\n" << std::endl;
        const size_t buffer_size = 4 * 1024 * 1024;
        std::vector<uint8_t> buffer(buffer_size);
        std::vector<uint8_t> leftover;
        leftover.reserve(8);
        
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
            std::streamsize read = input.gcount();
            if (read <= 0) {
                break;
            }
            
            if (!first_data_received) {
                first_data_received = true;
                first_data_time = std::chrono::steady_clock::now();
                std::cout << "[FILE] First data chunk: " << read << " bytes" << std::endl;
            }
            
            total_bytes_received += static_cast<uint64_t>(read);
            const uint8_t* data_ptr = buffer.data();
            size_t remaining = static_cast<size_t>(read);
            size_t words_processed_this_chunk = 0;
            
            if (!leftover.empty()) {
                size_t needed = 8 - leftover.size();
                size_t to_copy = std::min(needed, remaining);
                leftover.insert(leftover.end(), data_ptr, data_ptr + to_copy);
                data_ptr += to_copy;
                remaining -= to_copy;
                if (leftover.size() == 8) {
                    process_raw_data(leftover.data(), 8, processor, stream_state,
                                dispatcher ? dispatcher.get() : nullptr,
                                reorder_buffer ? reorder_buffer.get() : nullptr,
                                !stats_final_only);
                    total_packets_received += 1;
                    words_processed_this_chunk += 1;
                    leftover.clear();
                }
            }
            
            size_t aligned = (remaining / 8) * 8;
            if (aligned > 0) {
                process_raw_data(data_ptr, aligned, processor, stream_state,
                        dispatcher ? dispatcher.get() : nullptr,
                        reorder_buffer ? reorder_buffer.get() : nullptr,
                        !stats_final_only);
                size_t words = aligned / 8;
                total_packets_received += words;
                words_processed_this_chunk += words;
                data_ptr += aligned;
                remaining -= aligned;
            }
            
            if (remaining > 0) {
                leftover.assign(data_ptr, data_ptr + remaining);
            }
            
            if (!stats_disable && stats_interval > 0 && !stats_final_only) {
                print_counter += words_processed_this_chunk;
                if (print_counter >= stats_interval) {
                    std::cout << "\n[Periodic Statistics Update]" << std::endl;
                    if (dispatcher) {
                        dispatcher->waitUntilIdle();
                    }
                    processor.finalizeRates();
                    print_statistics(processor);
                    std::cout << std::endl;
                    print_counter = 0;
                }
            }
            
            if (!stats_disable && stats_time_interval > 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_status_print).count();
                if (elapsed >= stats_time_interval) {
                    if (dispatcher) {
                        dispatcher->flushAll();
                    }
                    const Statistics& stats = processor.getStatistics();
                    uint64_t hits_diff = stats.total_hits - last_hits;
                    std::cout << "[Status] Processed " << hits_diff << " hits in last "
                              << stats_time_interval << "s" << std::endl;
                    std::cout << "[Status] Total bytes processed: " << total_bytes_received
                              << " (" << (total_bytes_received / 1024.0 / 1024.0) << " MB)" << std::endl;
                    std::cout << "[Status] Total packets (words) processed: " << total_packets_received << std::endl;
                    last_hits = stats.total_hits;
                    last_status_print = now;
                }
            }
        }
        
        if (input.bad()) {
            std::cerr << "Error reading input file: " << input_file << std::endl;
            return 1;
        }
        
        if (!leftover.empty()) {
            bytes_dropped_incomplete += leftover.size();
            std::cerr << "[WARNING] Ignoring " << leftover.size()
                      << " trailing byte(s) not forming a full 8-byte word" << std::endl;
        }
        
        if (dispatcher) {
            dispatcher->waitUntilIdle();
        }
    } else {
        // Producer/consumer pipeline: network thread pushes to queue, processing thread drains it
        RawDataQueue data_queue(queue_size);  // Configurable queue size (default: 2000 buffers)
        std::atomic<bool> processing_active{true};
        
        std::cout << "Queue size: " << queue_size << " buffers" << std::endl;
        // Note: Chunk parsing is inherently sequential (chunks can span buffers),
        // so we use a single processing thread. Parallelism is achieved via DecodeDispatcher.
        
        TCPServer server(host, port);
        
        if (!server.initialize()) {
            std::cerr << "Failed to initialize TCP server" << std::endl;
            return 1;
        }
        
        std::cout << "TCP client initialized, connecting to server..." << std::endl;
        if (!stats_disable && !stats_final_only) {
            std::cout << "Waiting for data...\n" << std::endl;
        } else {
            std::cout << "Waiting for data (high-rate mode)...\n" << std::endl;
        }
        
        static TCPServer* g_server = &server;
        static RawDataQueue* g_queue = &data_queue;
        static std::atomic<bool>* g_processing = &processing_active;
        
        signal(SIGINT, [](int) {
            if (g_server) {
                g_server->stop();
            }
            if (g_queue) {
                g_queue->stop();
            }
            if (g_processing) {
                g_processing->store(false);
            }
            std::cout << "\n[SIGINT] Received interrupt signal, shutting down gracefully..." << std::endl;
        });
        
        server.setConnectionCallback([&](bool connected) {
            if (connected) {
                std::cout << "✓ Client connected to server" << std::endl;
                std::cout << "Waiting for data...\n" << std::endl;
            } else {
                std::cout << "✗ Client disconnected" << std::endl;
                // Signal queue to stop when connection closes
                data_queue.stop();
                if (exit_on_disconnect) {
                    server.stop();
                    processing_active.store(false);
                }
            }
        });
        
        // Single processing thread: pulls from queue and processes data
        // Chunk parsing is sequential (chunks can span buffers), so we use one thread.
        // Parallelism is achieved via DecodeDispatcher for actual decoding.
        std::thread processing_thread([&]() {
            RawDataQueue::Buffer buffer;
            // Continue processing until queue is stopped AND empty
            while (true) {
                if (data_queue.pop(buffer, std::chrono::milliseconds(100))) {
                    // Successfully popped a buffer, process it
                    if (!first_data_received) {
                        first_data_received = true;
                        first_data_time = std::chrono::steady_clock::now();
                        std::cout << "[TCP] First data received: " << buffer.size << " bytes" << std::endl;
                    }
                    
                    // Update counters
                    total_bytes_received += buffer.size;
                    total_packets_received += (buffer.size / 8);
                    
                    // Process data (no mutex needed - single thread)
                    // Disable packet accounting in performance mode (--stats-final-only)
                    process_raw_data(buffer.data.data(), buffer.size, processor, stream_state,
                                    dispatcher ? dispatcher.get() : nullptr,
                                    reorder_buffer ? reorder_buffer.get() : nullptr,
                                    !stats_final_only);
                    
                    // Handle statistics printing
                    if (!stats_disable && stats_interval > 0 && !stats_final_only) {
                        print_counter += (buffer.size / 8);
                        if (print_counter >= stats_interval) {
                            std::cout << "\n[Periodic Statistics Update]" << std::endl;
                            if (dispatcher) {
                                dispatcher->waitUntilIdle();
                            }
                            processor.finalizeRates();
                            print_statistics(processor);
                            std::cout << std::endl;
                            print_counter = 0;
                        }
                    }
                    
                    if (!stats_disable && stats_time_interval > 0) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - last_status_print).count();
                        if (elapsed >= stats_time_interval) {
                            if (dispatcher) {
                                dispatcher->flushAll();
                            }
                            const Statistics& stats = processor.getStatistics();
                            uint64_t hits_diff = stats.total_hits - last_hits;
                            std::cout << "[Status] Processed " << hits_diff << " hits in last "
                                      << stats_time_interval << "s" << std::endl;
                            std::cout << "[Status] Total bytes received: " << total_bytes_received
                                      << " (" << (total_bytes_received / 1024.0 / 1024.0) << " MB)" << std::endl;
                            std::cout << "[Status] Total packets (words) received: " << total_packets_received << std::endl;
                            last_hits = stats.total_hits;
                            last_status_print = now;
                        }
                    }
                } else {
                    // pop() returned false - check if we should exit
                    // Exit if queue is stopped AND empty (no more data to process)
                    if (data_queue.isStopped() && data_queue.size() == 0) {
                        break;
                    }
                    // Otherwise, continue (might be a timeout, more data could arrive)
                }
            }
        });
        
        // Network thread: pushes data to queue (non-blocking)
        server.run([&](const uint8_t* data, size_t size) {
            // Push to queue immediately and return (non-blocking)
            // This allows the network thread to quickly return to recv()
            data_queue.push(data, size);
        });
        
        // Network thread finished, signal processing thread to stop
        data_queue.stop();
        processing_active.store(false, std::memory_order_release);
        
        // Wait for processing thread to finish draining the queue
        if (processing_thread.joinable()) {
            processing_thread.join();
        }
        
        g_server = nullptr;
        g_queue = nullptr;
        g_processing = nullptr;
        
        if (!first_data_received) {
            std::cout << "\n[WARNING] No data was received from SERVAL!" << std::endl;
            std::cout << "Possible causes:" << std::endl;
            std::cout << "  1. SERVAL is not configured to send data to port " << port << std::endl;
            std::cout << "  2. SERVAL is not actively sending data" << std::endl;
            std::cout << "  3. Check SERVAL configuration and status" << std::endl;
        }
        
        conn_stats = server.getConnectionStats();
        bytes_dropped_incomplete = conn_stats.bytes_dropped_incomplete;
        // Note: total_bytes_received is updated by the processing thread
        // conn_stats.bytes_received reflects bytes received from socket (may differ if buffers dropped)
        
        // Report queue statistics
        uint64_t dropped = data_queue.getDroppedBuffers();
        if (dropped > 0) {
            std::cout << "\n⚠️  WARNING: " << dropped 
                      << " buffer(s) were dropped due to queue full (size: " << queue_size << ")!" << std::endl;
            std::cout << "   Consider increasing queue size (--queue-size N) or decoder workers (--decoder-workers N)." << std::endl;
            std::cout << "   Dropped buffers indicate chunk parsing cannot keep up with network receive rate." << std::endl;
            std::cout << "   Note: Parallelism is achieved via DecodeDispatcher workers for actual decoding." << std::endl;
        }
        
        if (dispatcher) {
            dispatcher->waitUntilIdle();
        }
    }
    
    if (!first_data_received) {
        if (file_mode) {
            std::cout << "\n[WARNING] The input file contained no data." << std::endl;
        }
        // For TCP mode a message has already been printed above.
    }
    
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "=== FINAL SUMMARY ===" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "Total bytes processed: " << total_bytes_received
              << " (" << std::fixed << std::setprecision(2)
              << (total_bytes_received / 1024.0 / 1024.0) << " MB)" << std::endl;
    std::cout << "Total packets (words) processed: " << total_packets_received << std::endl;
    if (bytes_dropped_incomplete > 0) {
        std::cout << "Bytes dropped (incomplete words): " << bytes_dropped_incomplete
                  << " (" << std::fixed << std::setprecision(2)
                  << (bytes_dropped_incomplete / 1024.0) << " KB)" << std::endl;
    }
    std::cout << std::endl;
    
    if (!stats_disable) {
        std::cout << "=== Final Statistics ===" << std::endl;
        if (dispatcher) {
            dispatcher->waitUntilIdle();
        }
        processor.finalizeRates();
        print_statistics(processor);
        print_recent_hits(processor, 10);
    }
    
    if (file_mode) {
        std::cout << "\nSource file: " << file_path << std::endl;
    } else {
        std::cout << "\n=== Connection Statistics ===" << std::endl;
        std::cout << "Connection attempts: " << conn_stats.connection_attempts << std::endl;
        std::cout << "Successful connections: " << conn_stats.successful_connections << std::endl;
        std::cout << "Disconnections: " << conn_stats.disconnections << std::endl;
        std::cout << "Reconnect errors: " << conn_stats.reconnect_errors << std::endl;
        std::cout << "recv() errors: " << conn_stats.recv_errors << std::endl;
        
        if (conn_stats.bytes_dropped_incomplete > 0) {
            std::cout << "\n⚠️  WARNING: " << conn_stats.bytes_dropped_incomplete
                      << " bytes were dropped due to incomplete 8-byte words!" << std::endl;
            std::cout << "   This may indicate TCP packet fragmentation issues." << std::endl;
        }
        
        if (conn_stats.disconnections > 0) {
            std::cout << "\n⚠️  WARNING: " << conn_stats.disconnections
                      << " disconnection(s) detected. This may cause data loss!" << std::endl;
        }
    }
    
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << (file_mode ? "Data Processing Summary:" : "Data Reception Summary:") << std::endl;
    std::cout << "  Parser processed: " << std::fixed << std::setprecision(2)
              << (total_bytes_received / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "  (" << total_bytes_received << " bytes)" << std::endl;
    std::cout << std::endl;
    if (!file_mode) {
        std::cout << "  To check for data loss:" << std::endl;
        std::cout << "  1. Compare with SERVAL .tpx3 file size" << std::endl;
        std::cout << "  2. If parser received < file size, data was lost" << std::endl;
        std::cout << "  3. Possible causes: TCP buffer overruns, processing bottleneck" << std::endl;
    } else {
        std::cout << "  Compare these totals with live TCP capture to detect discrepancies." << std::endl;
    }
    std::cout << std::string(60, '=') << std::endl;
    
    return 0;
}

