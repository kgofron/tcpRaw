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

// Helper function to process a single packet (used by reorder buffer callback)
void process_packet(uint64_t word, uint8_t chip_index, HitProcessor& processor, ChunkMetadata& chunk_meta) {
    // Check full-byte types first (0x50, 0x71, etc. that can't be distinguished by 4-bit)
    uint8_t full_type = (word >> 56) & 0xFF;
    
    if (full_type == SPIDR_PACKET_ID) {
        // SPIDR packet ID (0x50)
        uint64_t packet_count;
        if (decode_spidr_packet_id(word, packet_count)) {
            // Packet count tracking
        }
        return;
    }
    
    if (full_type == TPX3_CONTROL) {
        // TPX3 control (0x71)
        Tpx3ControlCmd cmd;
        if (decode_tpx3_control(word, cmd)) {
            // Control command decoded
        }
        return;
    }
    
    if (full_type == EXTRA_TIMESTAMP || full_type == EXTRA_TIMESTAMP_MPX3) {
        // Extra timestamp packets - handled separately in main processing loop
        return;
    }
    
    if (full_type == GLOBAL_TIME_LOW || full_type == GLOBAL_TIME_HIGH) {
        // GlobalTime gt = decode_global_time(word);
        // Future: Use for time extension
        return;
    }
    
    // For other packets, use 4-bit type
    uint8_t packet_type = (word >> 60) & 0xF;
    processor.incrementPacketType(packet_type);
    
    switch (packet_type) {
        case PIXEL_COUNT_FB:
        case PIXEL_STANDARD: {
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
            try {
                TDCEvent tdc = decode_tdc_data(word);
                processor.addTdcEvent(tdc);
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
            SpidrControl ctrl;
            if (decode_spidr_control(word, ctrl)) {
                processor.incrementChunkCount();
            }
            break;
        }
        
        default:
            // Unknown packet type
            processor.incrementUnknownPacket();
            break;
    }
}

// Process raw data buffer
void process_raw_data(const uint8_t* buffer, size_t bytes, HitProcessor& processor, ChunkMetadata& chunk_meta,
                      PacketReorderBuffer* reorder_buffer = nullptr, uint64_t /* current_chunk_count */ = 0) {
    const uint64_t* data_words = reinterpret_cast<const uint64_t*>(buffer);
    size_t num_words = bytes / 8;
    
    bool in_chunk = false;
    size_t chunk_words_remaining = 0;
    uint8_t chip_index = 0;
    
    // Track extra timestamp packets at end of chunk
    std::vector<ExtraTimestamp> extra_timestamps;
    
    for (size_t i = 0; i < num_words; ++i) {
        uint64_t word = data_words[i];
        
        // Check if this is a chunk header
        TPX3ChunkHeader header;
        header.data = word;
        
        if (header.isValid()) {
            // Found chunk header
            in_chunk = true;
            chunk_words_remaining = header.chunkSize() / 8;
            chip_index = header.chipIndex();
            processor.incrementChunkCount();
            
            // If we have a reorder buffer, reset it for new chunk
            if (reorder_buffer) {
                reorder_buffer->resetForNewChunk(processor.getStatistics().total_chunks);
            }
            
            // Reset chunk metadata
            chunk_meta.has_extra_packets = false;
            extra_timestamps.clear();
            
            continue;
        }
        
        if (!in_chunk || chunk_words_remaining == 0) {
            continue;
        }
        
        chunk_words_remaining--;
        
        // Check if we're near the end of chunk (last 3 words are extra timestamps)
        bool is_near_end = (chunk_words_remaining <= 3);
        
        if (is_near_end && ((word >> 56) == EXTRA_TIMESTAMP || (word >> 56) == EXTRA_TIMESTAMP_MPX3)) {
            // This is an extra timestamp packet
            ExtraTimestamp extra_ts = decode_extra_timestamp(word);
            extra_timestamps.push_back(extra_ts);
            
            // When we have all 3 extra packets, process them
            if (extra_timestamps.size() == 3) {
                chunk_meta.has_extra_packets = true;
                chunk_meta.packet_gen_time_ns = extra_timestamps[0].timestamp_ns;
                chunk_meta.min_timestamp_ns = extra_timestamps[1].timestamp_ns;
                chunk_meta.max_timestamp_ns = extra_timestamps[2].timestamp_ns;
                
                processor.processChunkMetadata(chunk_meta);
            }
        } else {
            // Regular packet processing
            // Check if this is a SPIDR packet ID packet that should be reordered
            uint64_t packet_count;
            bool is_spidr_packet_id = decode_spidr_packet_id(word, packet_count);
            
            if (is_spidr_packet_id && reorder_buffer) {
                // Use reorder buffer for SPIDR packet ID packets
                reorder_buffer->processPacket(word, packet_count, processor.getStatistics().total_chunks,
                    [&processor, &chunk_meta, chip_index](uint64_t w, uint64_t /*id*/, uint64_t /*chunk*/) {
                        // Callback: process reordered packet
                        process_packet(w, chip_index, processor, chunk_meta);
                    });
            } else {
                // Process immediately (not SPIDR packet ID or reordering disabled)
                process_packet(word, chip_index, processor, chunk_meta);
            }
        }
        
        if (chunk_words_remaining == 0) {
            in_chunk = false;
        }
    }
}

void print_statistics(const HitProcessor& processor) {
    const Statistics& stats = processor.getStatistics();
    std::cout << "\n=== Statistics ===" << std::endl;
    std::cout << "Total hits: " << stats.total_hits << std::endl;
    std::cout << "Total chunks: " << stats.total_chunks << std::endl;
    std::cout << "Total TDC events: " << stats.total_tdc_events << std::endl;
    std::cout << "Total control packets: " << stats.total_control_packets << std::endl;
    std::cout << "Total decode errors: " << stats.total_decode_errors << std::endl;
    std::cout << "Total fractional errors: " << stats.total_fractional_errors << std::endl;
    std::cout << "Total unknown packets: " << stats.total_unknown_packets << std::endl;
    std::cout << "Total hit rate: " << std::fixed << std::setprecision(2) 
              << stats.hit_rate_hz << " Hz" << std::endl;
    
    if (!stats.packet_type_counts.empty()) {
        std::cout << "Packet type breakdown:" << std::endl;
        for (const auto& pair : stats.packet_type_counts) {
            uint8_t type = pair.first;
            uint64_t count = pair.second;
            std::cout << "  Type 0x" << std::hex << static_cast<int>(type) << std::dec
                      << " (0b" << std::bitset<4>(type) << "): " << count << std::endl;
        }
    }
    
    if (!stats.chip_hit_rates_hz.empty()) {
        std::cout << "Per-chip hit rates:" << std::endl;
        for (const auto& pair : stats.chip_hit_rates_hz) {
            std::cout << "  Chip " << static_cast<int>(pair.first) 
                      << ": " << std::fixed << std::setprecision(2) 
                      << pair.second << " Hz" << std::endl;
        }
    }
}

void print_recent_hits(const HitProcessor& processor, size_t count) {
    const auto& hits = processor.getHits();
    size_t start = (hits.size() > count) ? (hits.size() - count) : 0;
    
    std::cout << "\n=== Recent Hits (last " << (hits.size() - start) << ") ===" << std::endl;
    for (size_t i = start; i < hits.size(); ++i) {
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
    
    // Parse command line arguments for host and port
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
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [--host HOST] [--port PORT] [--reorder] [--reorder-window SIZE]" << std::endl;
            std::cout << "  --host HOST           TCP server host (default: 127.0.0.1)" << std::endl;
            std::cout << "  --port PORT           TCP server port (default: 8085)" << std::endl;
            std::cout << "  --reorder             Enable packet reordering" << std::endl;
            std::cout << "  --reorder-window SIZE Reorder buffer window size (default: 1000)" << std::endl;
            return 0;
        }
    }
    
    std::cout << "TPX3 Raw Data Parser" << std::endl;
    std::cout << "Connecting to " << host << ":" << port << std::endl;
    std::cout << "Packet reordering: " << (enable_reorder ? "enabled" : "disabled");
    if (enable_reorder) {
        std::cout << " (window size: " << reorder_window_size << ")";
    }
    std::cout << std::endl;
    
    TCPServer server(host, port);
    
    if (!server.initialize()) {
        std::cerr << "Failed to initialize TCP server" << std::endl;
        return 1;
    }
    
    std::cout << "TCP client initialized, connecting to server..." << std::endl;
    std::cout << "Statistics will be printed every 1000 packets processed\n" << std::endl;
    
    HitProcessor processor;
    ChunkMetadata chunk_meta;
    
    // Create reorder buffer if enabled
    std::unique_ptr<PacketReorderBuffer> reorder_buffer;
    if (enable_reorder) {
        reorder_buffer = std::make_unique<PacketReorderBuffer>(reorder_window_size, true);
    }
    
    size_t print_counter = 0;
    const size_t PRINT_INTERVAL = 1000;
    
    auto last_status_print = std::chrono::steady_clock::now();
    uint64_t last_hits = 0;
    
    server.setConnectionCallback([&](bool connected) {
        if (connected) {
            std::cout << "✓ Client connected to server" << std::endl;
            std::cout << "Waiting for data...\n" << std::endl;
        } else {
            std::cout << "✗ Client disconnected" << std::endl;
        }
    });
    
    server.run([&](const uint8_t* data, size_t size) {
        // Process raw data
        process_raw_data(data, size, processor, chunk_meta, 
                        reorder_buffer ? reorder_buffer.get() : nullptr, 0);
        
        // Print periodic statistics
        print_counter += (size / 8);
        if (print_counter >= PRINT_INTERVAL) {
            std::cout << "\n[Periodic Statistics Update]" << std::endl;
            print_statistics(processor);
            std::cout << std::endl;
            print_counter = 0;
        }
        
        // Also print status every 10 seconds
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_status_print).count();
        if (elapsed >= 10) {
            const Statistics& stats = processor.getStatistics();
            uint64_t hits_diff = stats.total_hits - last_hits;
            std::cout << "[Status] Processed " << hits_diff << " hits in last 10s" << std::endl;
            last_hits = stats.total_hits;
            last_status_print = now;
        }
    });
    
    // Print final statistics
    print_statistics(processor);
    print_recent_hits(processor, 10);
    
    return 0;
}

