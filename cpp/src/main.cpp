/*
 * Author: Kazimierz Gofron
 *         Oak Ridge National Laboratory
 *
 * Created:  November 2, 2025
 * Modified: November 4, 2025
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

static std::string format_type_label(const std::string& prefix, uint8_t type) {
    std::ostringstream oss;
    oss << prefix << " (0x" << std::hex << std::uppercase << std::setfill('0')
        << std::setw(2) << static_cast<int>(type) << ")";
    return oss.str();
}

// Helper function to process a single packet (used by reorder buffer callback)
void process_packet(uint64_t word, uint8_t chip_index, HitProcessor& processor, ChunkMetadata& chunk_meta) {
    // Check full-byte types first (0x50, 0x71, etc. that can't be distinguished by 4-bit)
    uint8_t full_type = (word >> 56) & 0xFF;
    
    if (full_type == SPIDR_PACKET_ID) {
        processor.addPacketBytes("SPIDR packet ID (0x50)", 8);
        // SPIDR packet ID (0x50)
        uint64_t packet_count;
        if (decode_spidr_packet_id(word, packet_count)) {
            // Packet count tracking
        }
        return;
    }
    
    if (full_type == TPX3_CONTROL) {
        processor.addPacketBytes("TPX3 control (0x71)", 8);
        // TPX3 control (0x71)
        Tpx3ControlCmd cmd;
        if (decode_tpx3_control(word, cmd)) {
            // Control command decoded
        }
        return;
    }
    
    if (full_type == EXTRA_TIMESTAMP || full_type == EXTRA_TIMESTAMP_MPX3) {
        processor.addPacketBytes(format_type_label("Extra timestamp", full_type), 8);
        // Extra timestamp packets - handled separately in main processing loop
        return;
    }
    
    if (full_type == GLOBAL_TIME_LOW || full_type == GLOBAL_TIME_HIGH) {
        processor.addPacketBytes(format_type_label("Global time", full_type), 8);
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
            if (packet_type == PIXEL_COUNT_FB) {
                processor.addPacketBytes("Pixel count_fb (0x0a)", 8);
            } else {
                processor.addPacketBytes("Pixel standard (0x0b)", 8);
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
            processor.addPacketBytes("TDC data (0x06)", 8);
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
            processor.addPacketBytes("SPIDR control (0x05)", 8);
            SpidrControl ctrl;
            if (decode_spidr_control(word, ctrl)) {
                processor.incrementChunkCount();
            }
            break;
        }
        
        default: {
            std::ostringstream label;
            label << "Unknown packet type (0x" << std::hex << std::uppercase
                  << static_cast<int>(packet_type) << ")";
            processor.addPacketBytes(label.str(), 8);
            // Unknown packet type
            processor.incrementUnknownPacket();
            break;
        }
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
            processor.addPacketBytes("Chunk header", 8);
            // Note: chunk size includes the header word itself
            // So we set chunk_words_remaining to chunkSize/8, which includes header
            // We then continue to skip the header, so we process (chunkSize/8 - 1) data words
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
            uint8_t extra_type = static_cast<uint8_t>((word >> 56) & 0xFF);
            processor.addPacketBytes(format_type_label("Extra timestamp", extra_type), 8);
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
    
    if (!stats.chip_hit_rates_hz.empty()) {
        std::cout << "Per-chip hit rates:" << std::endl;
        for (const auto& pair : stats.chip_hit_rates_hz) {
            std::cout << "  Chip " << static_cast<int>(pair.first) 
                      << ": " << std::fixed << std::setprecision(2) 
                      << pair.second << " Hz" << std::endl;
        }
    }
    
    if (!stats.chip_tdc1_rates_hz.empty()) {
        std::cout << "Per-chip TDC1 rates (averaged per chip, for diagnostics):" << std::endl;
        for (const auto& pair : stats.chip_tdc1_rates_hz) {
            uint8_t chip = pair.first;
            uint64_t total_count = stats.chip_tdc1_counts.count(chip) 
                ? stats.chip_tdc1_counts.at(chip) : 0;
            std::cout << "  Chip " << static_cast<int>(chip) 
                      << ": " << std::fixed << std::setprecision(2) 
                      << pair.second << " Hz (total: " << total_count << ")" << std::endl;
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
    size_t stats_interval = 1000;  // Print stats every N packets
    int stats_time_interval = 10;  // Print status every N seconds (0 = disable)
    bool stats_final_only = false; // Only print final statistics
    bool stats_disable = false;    // Completely disable statistics printing
    bool exit_on_disconnect = false; // Exit after connection closes (don't auto-reconnect)
    std::string input_file;
    bool file_mode = false;
    
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
    
    HitProcessor processor;
    ChunkMetadata chunk_meta;
    
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
        std::ifstream input(input_file, std::ios::binary);
        if (!input) {
            std::cerr << "Failed to open input file: " << input_file << std::endl;
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
                    process_raw_data(leftover.data(), 8, processor, chunk_meta,
                        reorder_buffer ? reorder_buffer.get() : nullptr, 0);
                    total_packets_received += 1;
                    words_processed_this_chunk += 1;
                    leftover.clear();
                }
            }
            
            size_t aligned = (remaining / 8) * 8;
            if (aligned > 0) {
                process_raw_data(data_ptr, aligned, processor, chunk_meta,
                        reorder_buffer ? reorder_buffer.get() : nullptr, 0);
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
    } else {
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
        signal(SIGINT, [](int) {
            if (g_server) {
                g_server->stop();
            }
            std::cout << "\n[SIGINT] Received interrupt signal, shutting down gracefully..." << std::endl;
        });
        
        server.setConnectionCallback([&](bool connected) {
            if (connected) {
                std::cout << "✓ Client connected to server" << std::endl;
                std::cout << "Waiting for data...\n" << std::endl;
            } else {
                std::cout << "✗ Client disconnected" << std::endl;
                if (exit_on_disconnect) {
                    server.stop();
                }
            }
        });
        
        server.run([&](const uint8_t* data, size_t size) {
            if (!first_data_received) {
                first_data_received = true;
                first_data_time = std::chrono::steady_clock::now();
                std::cout << "[TCP] First data received: " << size << " bytes" << std::endl;
            }
            
            total_bytes_received += size;
            total_packets_received += (size / 8);
            
            process_raw_data(data, size, processor, chunk_meta,
                            reorder_buffer ? reorder_buffer.get() : nullptr, 0);
            
            if (!stats_disable && stats_interval > 0 && !stats_final_only) {
                print_counter += (size / 8);
                if (print_counter >= stats_interval) {
                    std::cout << "\n[Periodic Statistics Update]" << std::endl;
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
        });
        
        if (!first_data_received) {
            std::cout << "\n[WARNING] No data was received from SERVAL!" << std::endl;
            std::cout << "Possible causes:" << std::endl;
            std::cout << "  1. SERVAL is not configured to send data to port " << port << std::endl;
            std::cout << "  2. SERVAL is not actively sending data" << std::endl;
            std::cout << "  3. Check SERVAL configuration and status" << std::endl;
        }
        
        conn_stats = server.getConnectionStats();
        bytes_dropped_incomplete = conn_stats.bytes_dropped_incomplete;
        total_bytes_received = conn_stats.bytes_received;
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
        print_statistics(processor);
        print_recent_hits(processor, 10);
    }
    
    if (file_mode) {
        std::cout << "\nSource file: " << input_file << std::endl;
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

