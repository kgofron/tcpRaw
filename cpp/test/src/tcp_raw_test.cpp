#include "tcp_server.h"
#include "ring_buffer.h"
#include "tpx3_decoder.h"
#include "tpx3_packets.h"
#include "packet_reorder_buffer.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <bitset>
#include <cstring>
#include <cmath>
#include <sstream>
#include <memory>

// Analysis statistics
struct AnalysisStats {
    // Basic metrics
    uint64_t total_bytes = 0;
    uint64_t total_words = 0;
    uint64_t incomplete_words = 0;
    
    // Timing
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_stats_time;
    
    // Throughput
    double current_rate_mbps = 0.0;
    double peak_rate_mbps = 0.0;
    double avg_rate_mbps = 0.0;
    
    // Packet statistics
    uint64_t total_chunks = 0;
    std::map<uint8_t, uint64_t> packet_type_counts;
    std::map<uint8_t, uint64_t> packet_type_bytes;
    
    // Packet order tracking (global - across all chunks)
    uint64_t last_packet_id = 0;
    uint64_t missing_packet_ids = 0;
    uint64_t duplicate_packet_ids = 0;
    uint64_t out_of_order_packet_ids = 0;
    std::set<uint64_t> seen_packet_ids;
    bool first_packet_id_seen = false;
    
    // Per-chunk packet ID tracking (if protocol allows resets)
    std::map<uint64_t, std::set<uint64_t>> chunk_packet_ids;  // chunk_id -> set of packet_ids
    uint64_t current_chunk_id = 0;
    uint64_t duplicate_packet_ids_per_chunk = 0;  // Duplicates within same chunk
    
    // Protocol violations
    uint64_t protocol_violations = 0;
    uint64_t invalid_packet_types = 0;
    uint64_t invalid_chunk_headers = 0;
    uint64_t invalid_chunk_sizes = 0;
    uint64_t incomplete_chunks = 0;
    uint64_t invalid_bit_patterns = 0;
    
    // Categorized violation counts
    uint64_t pixel_violations = 0;      // Invalid pixel packet fields
    uint64_t tdc_violations = 0;        // Invalid TDC packet fields
    uint64_t global_time_violations = 0; // Invalid global time packets
    uint64_t spidr_violations = 0;      // Invalid SPIDR packets
    uint64_t tpx3_control_violations = 0; // Invalid TPX3 control packets
    uint64_t extra_ts_violations = 0;   // Invalid extra timestamp packets
    uint64_t reserved_bit_violations = 0; // Reserved bits set when should be 0
    
    // Detailed violation tracking
    std::vector<std::string> violation_details;
    size_t max_violation_details = 100;
    
    // Per-chip statistics
    std::map<uint8_t, uint64_t> chip_chunks;
    std::map<uint8_t, uint64_t> chip_packets;
    
    // Buffer overruns
    uint64_t buffer_overruns = 0;
};

// Protocol validator
class ProtocolValidator {
public:
    static bool validatePacketType(uint8_t type, uint64_t /*word*/) {
        const std::set<uint8_t> valid_types = {0x5, 0x6, 0xa, 0xb, 0x21, 0x44, 0x45, 0x50, 0x51, 0x71};
        return valid_types.count(type) > 0;
    }
    
    static bool validateChunkHeader(uint64_t word, AnalysisStats& stats) {
        TPX3ChunkHeader header;
        header.data = word;
        
        if (!header.isValid()) {
            stats.invalid_chunk_headers++;
            return false;
        }
        
        uint16_t chunk_size = header.chunkSize();
        uint8_t chip_index = header.chipIndex();
        
        // Validate chunk size (must be multiple of 8, non-zero)
        // Note: uint16_t max is 65535, so no upper bound check needed
        if (chunk_size % 8 != 0 || chunk_size == 0) {
            stats.invalid_chunk_sizes++;
            return false;
        }
        
        // Chip index should be reasonable (0-255, but typically 0-15)
        // Note: uint8_t is always 0-255, so this check is always false, but kept for documentation
        (void)chip_index;  // Suppress unused warning
        
        return true;
    }
    
    static bool validatePixelPacket(uint8_t type, uint64_t word, AnalysisStats& stats) {
        if (type != 0xa && type != 0xb) return true;
        
        // Extract and validate fields
        uint64_t pixaddr = get_bits(word, 59, 44);
        auto [x, y] = pixaddr_to_xy(pixaddr);
        
        // X, Y should be in valid range (0-255 for TPX3)
        if (x > 255 || y > 255) {
            stats.protocol_violations++;
            return false;
        }
        
        // Validate ranges for different fields
        (void)get_bits(word, 43, 30);  // ToA or Integrated ToT (validated by decoder)
        (void)get_bits(word, 29, 20);  // ToT or EventCount (validated by decoder)
        uint8_t field3 = get_bits(word, 19, 16);   // FToA or HitCount
        (void)get_bits(word, 15, 0);   // SPIDR time (validated by decoder)
        
        // FToA (standard mode) should be 0-15
        if (type == 0xb && field3 > 15) {
            stats.protocol_violations++;
            return false;
        }
        
        return true;
    }
    
    static bool validateTdcPacket(uint64_t word, AnalysisStats& stats) {
        uint8_t event_type = get_bits(word, 59, 56);
        const std::set<uint8_t> valid_types = {0xf, 0xa, 0xe, 0xb};
        
        if (valid_types.count(event_type) == 0) {
            stats.protocol_violations++;
            return false;
        }
        
        uint16_t trigger_count = get_bits(word, 55, 44);
        // 12 bits = 0-4095, validate reasonable range
        if (trigger_count > 4095) {
            stats.protocol_violations++;
            return false;
        }
        
        uint8_t fract = get_bits(word, 8, 5);
        // fract should be 0-12 (0 is old firmware bug, >12 is error)
        if (fract > 12) {
            stats.protocol_violations++;
            return false;
        }
        
        // Bits 4-0 should be reserved/zero
        uint8_t reserved = get_bits(word, 4, 0);
        if (reserved != 0) {
            stats.reserved_bit_violations++;
            stats.protocol_violations++;
            return false;
        }
        
        return true;
    }
    
    static bool validateGlobalTimePacket(uint8_t type, uint64_t word, AnalysisStats& stats) {
        if (type == 0x44 || type == 0x45) {
            // Verify exact header match
            uint8_t full_header = get_bits(word, 63, 56);
            if (full_header != type) {
                stats.protocol_violations++;
                return false;
            }
        }
        return true;
    }
    
    static bool validateSpidrPacket(uint8_t type, uint64_t word, AnalysisStats& stats) {
        if (type == 0x5) {
            // SPIDR control - verify command
            uint8_t cmd = get_bits(word, 59, 56);
            const std::set<uint8_t> valid_cmds = {0xf, 0xa, 0xc};
            if (valid_cmds.count(cmd) == 0) {
                stats.protocol_violations++;
                return false;
            }
            
            // Verify header bits 63-60 = 0x5
            uint8_t header = get_bits(word, 63, 60);
            if (header != 0x5) {
                stats.protocol_violations++;
                return false;
            }
        } else if (type == 0x50) {
            // SPIDR packet ID - header should be exactly 0x50
            uint8_t header = get_bits(word, 63, 56);
            if (header != 0x50) {
                stats.protocol_violations++;
                return false;
            }
        }
        return true;
    }
    
    static bool validateTpx3Control(uint64_t word, AnalysisStats& stats) {
        uint8_t header = get_bits(word, 63, 56);
        if (header != 0x71) {
            stats.protocol_violations++;
            return false;
        }
        
        uint8_t cmd = get_bits(word, 55, 48);
        if (cmd != 0xa0 && cmd != 0xb0) {
            stats.protocol_violations++;
            return false;
        }
        
        return true;
    }
    
    static bool validateExtraTimestamp(uint64_t word, AnalysisStats& stats) {
        uint8_t header = get_bits(word, 63, 56);
        if (header != 0x51 && header != 0x21) {
            stats.protocol_violations++;
            return false;
        }
        return true;
    }
};

void analyzeWord(uint64_t word, AnalysisStats& stats, bool in_chunk, 
                 size_t /*chunk_words_remaining*/, uint8_t chip_index) {
    stats.total_words++;
    
    // Extract packet type (lower 4 bits of upper byte)
    uint8_t packet_type = (word >> 60) & 0xF;
    
    // For full-byte packet types (0x50, 0x44, 0x45, etc.), check full byte first
    uint8_t full_byte_type = (word >> 56) & 0xFF;
    
    // Check if this is a chunk header
    TPX3ChunkHeader header;
    header.data = word;
    
    if (header.isValid()) {
        // Validate chunk header
        if (!ProtocolValidator::validateChunkHeader(word, stats)) {
            if (stats.violation_details.size() < stats.max_violation_details) {
                std::stringstream ss;
                ss << "Invalid chunk header at word " << stats.total_words 
                   << ": size=" << header.chunkSize() << ", chip=" << static_cast<int>(header.chipIndex());
                stats.violation_details.push_back(ss.str());
            }
        }
        stats.total_chunks++;
        stats.chip_chunks[header.chipIndex()]++;
        
        // New chunk - create unique chunk ID and reset per-chunk tracking
        stats.current_chunk_id = stats.total_chunks;  // Use chunk count as unique ID
        stats.chunk_packet_ids[stats.current_chunk_id] = std::set<uint64_t>();
        
        return;
    }
    
    // Check packet type validity
    if (!ProtocolValidator::validatePacketType(packet_type, word)) {
        // Also check full-byte packet types for special cases
        // Note: full_byte_type already extracted above
        if (full_byte_type != 0x21 && full_byte_type != 0x44 && full_byte_type != 0x45 && 
            full_byte_type != 0x50 && full_byte_type != 0x51 && full_byte_type != 0x71) {
            stats.invalid_packet_types++;
            if (stats.violation_details.size() < stats.max_violation_details) {
                std::stringstream ss;
                ss << "Invalid packet type 0x" << std::hex << static_cast<int>(packet_type) 
                   << " at word " << std::dec << stats.total_words;
                stats.violation_details.push_back(ss.str());
            }
        }
    }
    
    // Count packet type
    stats.packet_type_counts[packet_type]++;
    stats.packet_type_bytes[packet_type] += 8;
    
    // Track SPIDR packet ID for order analysis
    uint64_t packet_count;
    if (decode_spidr_packet_id(word, packet_count)) {
        // Track both globally (across all chunks) and per-chunk
        // Global tracking (across all chunks)
        if (!stats.first_packet_id_seen) {
            stats.first_packet_id_seen = true;
            stats.last_packet_id = packet_count;
            stats.seen_packet_ids.insert(packet_count);
        } else {
            // Check for global duplicates
            if (stats.seen_packet_ids.count(packet_count) > 0) {
                stats.duplicate_packet_ids++;
                if (stats.violation_details.size() < stats.max_violation_details) {
                    std::stringstream ss;
                    ss << "Duplicate packet ID " << packet_count << " at word " << stats.total_words
                       << " (chunk " << stats.current_chunk_id << ")";
                    stats.violation_details.push_back(ss.str());
                }
            } else {
                stats.seen_packet_ids.insert(packet_count);
            }
            
            // Check for gaps (missing sequence numbers) - only if not reset
            if (packet_count > stats.last_packet_id + 1) {
                // Only flag as missing if gap is small (< 1000)
                // Large gaps likely indicate chunk boundary resets
                if (packet_count - stats.last_packet_id < 1000) {
                    uint64_t gap = packet_count - stats.last_packet_id - 1;
                    stats.missing_packet_ids += gap;
                    if (stats.violation_details.size() < stats.max_violation_details) {
                        std::stringstream ss;
                        ss << "Missing " << gap << " packet IDs between " << stats.last_packet_id 
                           << " and " << packet_count << " at word " << stats.total_words;
                        stats.violation_details.push_back(ss.str());
                    }
                }
            }
            
            // Check for out-of-order (packet_count < last_packet_id)
            // Only flag if difference is small - large drops likely indicate reset
            if (packet_count < stats.last_packet_id && (stats.last_packet_id - packet_count < 1000)) {
                stats.out_of_order_packet_ids++;
                if (stats.violation_details.size() < stats.max_violation_details) {
                    std::stringstream ss;
                    ss << "Out-of-order packet ID " << packet_count << " < " << stats.last_packet_id 
                       << " at word " << stats.total_words << " (chunk " << stats.current_chunk_id << ")";
                    stats.violation_details.push_back(ss.str());
                }
            }
            
            stats.last_packet_id = packet_count;
        }
        
        // Per-chunk tracking (to detect duplicates within same chunk)
        if (stats.current_chunk_id > 0) {
            auto& chunk_ids = stats.chunk_packet_ids[stats.current_chunk_id];
            if (chunk_ids.count(packet_count) > 0) {
                stats.duplicate_packet_ids_per_chunk++;
                // This is a real error - duplicate within same chunk
            } else {
                chunk_ids.insert(packet_count);
            }
        }
    }
    
    // Validate protocol conformance based on packet type
    bool valid = true;
    switch (packet_type) {
        case 0xa:
        case 0xb:
            valid = ProtocolValidator::validatePixelPacket(packet_type, word, stats);
            if (!valid) stats.pixel_violations++;
            if (in_chunk) stats.chip_packets[chip_index]++;
            break;
        case 0x6:
            valid = ProtocolValidator::validateTdcPacket(word, stats);
            if (!valid) stats.tdc_violations++;
            if (in_chunk) stats.chip_packets[chip_index]++;
            break;
        case 0x44:
        case 0x45:
            valid = ProtocolValidator::validateGlobalTimePacket(packet_type, word, stats);
            if (!valid) stats.global_time_violations++;
            break;
        case 0x5:
            // Packet type 0x5 could be SPIDR control (0x5) OR SPIDR packet ID (0x50)
            // Check full byte to distinguish
            if (full_byte_type == 0x50) {
                // This is SPIDR packet ID (0x50), not control (0x5)
                valid = ProtocolValidator::validateSpidrPacket(0x50, word, stats);
                if (!valid) stats.spidr_violations++;
            } else {
                // This is SPIDR control (0x5)
                valid = ProtocolValidator::validateSpidrPacket(0x5, word, stats);
                if (!valid) stats.spidr_violations++;
            }
            break;
        default:
            // Check for special full-byte packet types
            if (full_byte_type == 0x50) {
                // 0x50 is SPIDR packet ID - validate it
                valid = ProtocolValidator::validateSpidrPacket(0x50, word, stats);
                if (!valid) stats.spidr_violations++;
                // Note: 0x50 packets are also tracked for duplicate detection above
                // Duplicate detection is separate from protocol violations
            } else if (full_byte_type == 0x71) {
                valid = ProtocolValidator::validateTpx3Control(word, stats);
                if (!valid) stats.tpx3_control_violations++;
            } else if (full_byte_type == 0x51 || full_byte_type == 0x21) {
                valid = ProtocolValidator::validateExtraTimestamp(word, stats);
                if (!valid) stats.extra_ts_violations++;
            } else if (full_byte_type == 0x44 || full_byte_type == 0x45) {
                // Global time packets - already handled above but double-check
                valid = ProtocolValidator::validateGlobalTimePacket(packet_type, word, stats);
                if (!valid) stats.global_time_violations++;
            }
            break;
    }
    
    if (!valid) {
        stats.protocol_violations++;
    }
}

void printStatistics(const AnalysisStats& stats, bool detailed = false) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_total = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - stats.start_time).count() / 1000.0;
    
    std::cout << "\n=== TCP Raw Data Analysis ===" << std::endl;
    std::cout << "Duration: " << elapsed_total << " s" << std::endl;
    std::cout << "Total bytes: " << stats.total_bytes << " (" 
              << (stats.total_bytes / 1024.0 / 1024.0 / 1024.0) << " GB)" << std::endl;
    std::cout << "Total words: " << stats.total_words << std::endl;
    std::cout << "Incomplete words: " << stats.incomplete_words << std::endl;
    
    if (elapsed_total > 0) {
        double rate_mbps = (stats.total_bytes * 8.0) / (elapsed_total * 1e6);
        double rate_gbps = rate_mbps / 1000.0;
        std::cout << "Average rate: " << std::fixed << std::setprecision(2) 
                  << rate_mbps << " Mbps (" << rate_gbps << " Gbps)" << std::endl;
        std::cout << "Peak rate: " << stats.peak_rate_mbps << " Mbps" << std::endl;
    }
    
    std::cout << "\n=== Packet Statistics ===" << std::endl;
    std::cout << "Total chunks: " << stats.total_chunks << std::endl;
    std::cout << "Packet type breakdown:" << std::endl;
    if (stats.packet_type_counts.empty()) {
        std::cout << "  (no packets processed yet)" << std::endl;
    } else {
        for (const auto& pair : stats.packet_type_counts) {
            uint8_t type = pair.first;
            uint64_t count = pair.second;
            double percentage = (stats.total_words > 0) ? (count * 100.0 / stats.total_words) : 0.0;
            std::cout << "  Type 0x" << std::hex << static_cast<int>(type) << std::dec
                      << " (0b" << std::bitset<4>(type) << "): " << count 
                      << " (" << std::fixed << std::setprecision(2) << percentage << "%)" << std::endl;
        }
    }
    
    std::cout << "\n=== Packet Order Analysis ===" << std::endl;
    if (stats.seen_packet_ids.empty()) {
        std::cout << "No SPIDR packet ID packets found (0x50 packets)" << std::endl;
    } else {
        std::cout << "SPIDR packet IDs seen (global): " << stats.seen_packet_ids.size() << std::endl;
        std::cout << "Missing packet IDs: " << stats.missing_packet_ids 
                  << (stats.missing_packet_ids > 0 ? " ⚠️" : " ✓") << std::endl;
        std::cout << "Duplicate packet IDs (global): " << stats.duplicate_packet_ids;
        if (stats.duplicate_packet_ids > 0) {
            std::cout << " ⚠️ (may be expected if IDs reset per chunk)";
        } else {
            std::cout << " ✓";
        }
        std::cout << std::endl;
        std::cout << "Duplicate packet IDs (within chunk): " << stats.duplicate_packet_ids_per_chunk
                  << (stats.duplicate_packet_ids_per_chunk > 0 ? " ⚠️ (ERROR)" : " ✓") << std::endl;
        std::cout << "Out-of-order packet IDs: " << stats.out_of_order_packet_ids 
                  << (stats.out_of_order_packet_ids > 0 ? " ⚠️" : " ✓") << std::endl;
        if (stats.first_packet_id_seen) {
            std::cout << "Last packet ID: " << stats.last_packet_id << std::endl;
            std::cout << "Expected next ID: " << (stats.last_packet_id + 1) << std::endl;
        }
        if (stats.total_chunks > 0) {
            double avg_ids_per_chunk = static_cast<double>(stats.seen_packet_ids.size()) / stats.total_chunks;
            std::cout << "Average packet IDs per chunk: " << std::fixed << std::setprecision(1) 
                      << avg_ids_per_chunk << std::endl;
        }
    }
    
    std::cout << "\n=== Protocol Conformance ===" << std::endl;
    std::cout << "Total protocol violations: " << stats.protocol_violations 
              << (stats.protocol_violations > 0 ? " ⚠️" : " ✓") << std::endl;
    
    if (stats.protocol_violations > 0) {
        std::cout << "\nViolation breakdown by packet type:" << std::endl;
        if (stats.pixel_violations > 0) {
            std::cout << "  Pixel packets (0xa, 0xb): " << stats.pixel_violations 
                      << " (" << std::fixed << std::setprecision(1) 
                      << (100.0 * stats.pixel_violations / stats.protocol_violations) << "%)" << std::endl;
        }
        if (stats.tdc_violations > 0) {
            std::cout << "  TDC packets (0x6): " << stats.tdc_violations 
                      << " (" << std::fixed << std::setprecision(1) 
                      << (100.0 * stats.tdc_violations / stats.protocol_violations) << "%)" << std::endl;
        }
        if (stats.global_time_violations > 0) {
            std::cout << "  Global time packets (0x44, 0x45): " << stats.global_time_violations 
                      << " (" << std::fixed << std::setprecision(1) 
                      << (100.0 * stats.global_time_violations / stats.protocol_violations) << "%)" << std::endl;
        }
        if (stats.spidr_violations > 0) {
            std::cout << "  SPIDR packets (0x5, 0x50): " << stats.spidr_violations 
                      << " (" << std::fixed << std::setprecision(1) 
                      << (100.0 * stats.spidr_violations / stats.protocol_violations) << "%)" << std::endl;
        }
        if (stats.tpx3_control_violations > 0) {
            std::cout << "  TPX3 control packets (0x71): " << stats.tpx3_control_violations 
                      << " (" << std::fixed << std::setprecision(1) 
                      << (100.0 * stats.tpx3_control_violations / stats.protocol_violations) << "%)" << std::endl;
        }
        if (stats.extra_ts_violations > 0) {
            std::cout << "  Extra timestamp packets (0x51, 0x21): " << stats.extra_ts_violations 
                      << " (" << std::fixed << std::setprecision(1) 
                      << (100.0 * stats.extra_ts_violations / stats.protocol_violations) << "%)" << std::endl;
        }
        if (stats.reserved_bit_violations > 0) {
            std::cout << "  Reserved bit violations: " << stats.reserved_bit_violations 
                      << " (" << std::fixed << std::setprecision(1) 
                      << (100.0 * stats.reserved_bit_violations / stats.protocol_violations) << "%)" << std::endl;
        }
    }
    
    std::cout << "\nOther issues:" << std::endl;
    std::cout << "Invalid packet types: " << stats.invalid_packet_types 
              << (stats.invalid_packet_types > 0 ? " ⚠️" : " ✓") << std::endl;
    std::cout << "Invalid chunk headers: " << stats.invalid_chunk_headers 
              << (stats.invalid_chunk_headers > 0 ? " ⚠️" : " ✓") << std::endl;
    std::cout << "Invalid chunk sizes: " << stats.invalid_chunk_sizes 
              << (stats.invalid_chunk_sizes > 0 ? " ⚠️" : " ✓") << std::endl;
    std::cout << "Buffer overruns: " << stats.buffer_overruns 
              << (stats.buffer_overruns > 0 ? " ⚠️" : " ✓") << std::endl;
    
    if (!stats.chip_chunks.empty()) {
        std::cout << "\n=== Per-Chip Statistics ===" << std::endl;
        for (const auto& pair : stats.chip_chunks) {
            auto it = stats.chip_packets.find(pair.first);
            uint64_t packet_count = (it != stats.chip_packets.end()) ? it->second : 0;
            std::cout << "Chip " << static_cast<int>(pair.first) 
                      << ": " << pair.second << " chunks, "
                      << packet_count << " packets" << std::endl;
        }
    }
    
    if (detailed && !stats.violation_details.empty()) {
        std::cout << "\n=== Violation Details (first " << stats.violation_details.size() << ") ===" << std::endl;
        for (size_t i = 0; i < std::min(stats.violation_details.size(), size_t(20)); ++i) {
            std::cout << "  " << stats.violation_details[i] << std::endl;
        }
    }
}

void printReorderStatistics(const PacketReorderBuffer::Statistics& reorder_stats) {
    if (reorder_stats.total_packets == 0) {
        return;
    }
    
    std::cout << "\n=== Packet Reordering Statistics ===" << std::endl;
    std::cout << "Total SPIDR packets: " << reorder_stats.total_packets << std::endl;
    std::cout << "Processed immediately (in-order): " << reorder_stats.packets_processed_immediately 
              << " (" << std::fixed << std::setprecision(1) 
              << (100.0 * reorder_stats.packets_processed_immediately / reorder_stats.total_packets) << "%)" << std::endl;
    std::cout << "Reordered (buffered): " << reorder_stats.packets_reordered 
              << " (" << std::fixed << std::setprecision(1) 
              << (100.0 * reorder_stats.packets_reordered / reorder_stats.total_packets) << "%)" << std::endl;
    std::cout << "Max reorder distance: " << reorder_stats.max_reorder_distance << " packets" << std::endl;
    std::cout << "Buffer overflows: " << reorder_stats.buffer_overflows 
              << (reorder_stats.buffer_overflows > 0 ? " ⚠️" : " ✓") << std::endl;
    std::cout << "Packets dropped (too old): " << reorder_stats.packets_dropped_too_old 
              << (reorder_stats.packets_dropped_too_old > 0 ? " ⚠️" : " ✓") << std::endl;
}

void processRawData(const uint8_t* buffer, size_t bytes, AnalysisStats& stats, 
                    std::ofstream* out_file = nullptr,
                    PacketReorderBuffer* reorder_buffer = nullptr) {
    stats.total_bytes += bytes;
    
    // Handle incomplete words
    size_t complete_words_bytes = (bytes / 8) * 8;
    size_t incomplete_bytes = bytes - complete_words_bytes;
    if (incomplete_bytes > 0) {
        stats.incomplete_words++;
    }
    
    // Write to file if disk mode
    if (out_file && out_file->is_open()) {
        out_file->write(reinterpret_cast<const char*>(buffer), complete_words_bytes);
    }
    
    // Analyze words
    const uint64_t* data_words = reinterpret_cast<const uint64_t*>(buffer);
    size_t num_words = complete_words_bytes / 8;
    
    bool in_chunk = false;
    size_t chunk_words_remaining = 0;
    uint8_t chip_index = 0;
    
    for (size_t i = 0; i < num_words; ++i) {
        uint64_t word = data_words[i];
        
        // Check if this is a chunk header
        TPX3ChunkHeader header;
        header.data = word;
        bool is_chunk_header = header.isValid();
        
        if (is_chunk_header) {
            in_chunk = true;
            chunk_words_remaining = header.chunkSize() / 8;
            chip_index = header.chipIndex();
        }
        
        if (in_chunk && chunk_words_remaining > 0) {
            chunk_words_remaining--;
            
            // Check if this is a SPIDR packet ID packet that should be reordered
            uint64_t packet_count;
            bool is_spidr_packet_id = decode_spidr_packet_id(word, packet_count);
            
            if (is_spidr_packet_id && reorder_buffer) {
                // Use reorder buffer for SPIDR packet ID packets
                reorder_buffer->processPacket(word, packet_count, stats.current_chunk_id,
                    [&stats, in_chunk, chunk_words_remaining, chip_index](uint64_t w, uint64_t /*id*/, uint64_t /*chunk*/) {
                        // Callback: process reordered packet
                        analyzeWord(w, stats, in_chunk, chunk_words_remaining, chip_index);
                    });
            } else {
                // Process immediately (not SPIDR packet ID or reordering disabled)
                analyzeWord(word, stats, in_chunk, chunk_words_remaining, chip_index);
            }
        } else if (!in_chunk) {
            // Check if this is a SPIDR packet ID packet that should be reordered
            uint64_t packet_count;
            bool is_spidr_packet_id = decode_spidr_packet_id(word, packet_count);
            
            if (is_spidr_packet_id && reorder_buffer) {
                // Use reorder buffer for SPIDR packet ID packets
                reorder_buffer->processPacket(word, packet_count, stats.current_chunk_id,
                    [&stats](uint64_t w, uint64_t /*id*/, uint64_t /*chunk*/) {
                        // Callback: process reordered packet
                        analyzeWord(w, stats, false, 0, 0);
                    });
            } else {
                // Process immediately (not SPIDR packet ID or reordering disabled)
                analyzeWord(word, stats, false, 0, 0);
            }
        }
        
        // Handle reorder buffer reset for chunk headers AFTER processing
        if (is_chunk_header && reorder_buffer) {
            reorder_buffer->resetForNewChunk(stats.total_chunks);
        }
        
        if (chunk_words_remaining == 0) {
            in_chunk = false;
        }
    }
    
    // Update throughput
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - stats.last_stats_time).count() / 1000.0;
    auto elapsed_total = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - stats.start_time).count() / 1000.0;
    
    if (elapsed >= 1.0) {
        if (elapsed_total > 0) {
            double bytes_per_sec = stats.total_bytes / elapsed_total;
            double mbps = (bytes_per_sec * 8.0) / 1e6;
            stats.current_rate_mbps = mbps;
            if (mbps > stats.peak_rate_mbps) {
                stats.peak_rate_mbps = mbps;
            }
        }
        stats.last_stats_time = now;
    }
}

void printUsage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [OPTIONS]\n"
              << "Options:\n"
              << "  --mode buffer|disk      Output mode (default: buffer)\n"
              << "  --output FILE            Output file path for disk mode (default: tcp_raw_dump.bin)\n"
              << "  --buffer-size SIZE       Ring buffer size in MB (default: 256)\n"
              << "  --host HOST              TCP server host (default: 127.0.0.1)\n"
              << "  --port PORT              TCP server port (default: 8085)\n"
              << "  --duration SECONDS       Run duration (default: 0 = infinite)\n"
              << "  --analyze                Enable detailed packet-level analysis (slower)\n"
              << "  --stats-interval SECONDS Statistics print interval (default: 5)\n"
              << "  --reorder                Enable packet reordering (default: disabled)\n"
              << "  --reorder-window SIZE     Reorder buffer window size (default: 1000)\n"
              << "  --help                   Show this help message\n";
}

int main(int argc, char* argv[]) {
    // Defaults
    std::string mode = "buffer";
    std::string output_file = "tcp_raw_dump.bin";
    size_t buffer_size_mb = 256;
    std::string host = "127.0.0.1";
    uint16_t port = 8085;
    double duration = 0.0;  // 0 = infinite
    bool detailed_analysis = false;
    double stats_interval = 5.0;
    bool enable_reorder = false;
    size_t reorder_window_size = 1000;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--buffer-size" && i + 1 < argc) {
            buffer_size_mb = std::stoul(argv[++i]);
        } else if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--duration" && i + 1 < argc) {
            duration = std::stod(argv[++i]);
        } else if (arg == "--analyze") {
            detailed_analysis = true;
        } else if (arg == "--stats-interval" && i + 1 < argc) {
            stats_interval = std::stod(argv[++i]);
        } else if (arg == "--reorder") {
            enable_reorder = true;
        } else if (arg == "--reorder-window" && i + 1 < argc) {
            reorder_window_size = std::stoul(argv[++i]);
        } else if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }
    
    std::cout << "TCP Raw Data Test Tool" << std::endl;
    std::cout << "Mode: " << mode << std::endl;
    if (mode == "disk") {
        std::cout << "Output file: " << output_file << std::endl;
    } else {
        std::cout << "Buffer size: " << buffer_size_mb << " MB" << std::endl;
    }
    std::cout << "Connecting to " << host << ":" << port << std::endl;
    std::cout << "Detailed analysis: " << (detailed_analysis ? "enabled" : "disabled") << std::endl;
    std::cout << "Packet reordering: " << (enable_reorder ? "enabled" : "disabled");
    if (enable_reorder) {
        std::cout << " (window size: " << reorder_window_size << ")";
    }
    std::cout << std::endl;
    
    AnalysisStats stats;
    stats.start_time = std::chrono::steady_clock::now();
    stats.last_stats_time = stats.start_time;
    
    // Setup output file if disk mode
    std::ofstream out_file;
    if (mode == "disk") {
        out_file.open(output_file, std::ios::binary);
        if (!out_file.is_open()) {
            std::cerr << "Error: Failed to open output file " << output_file << std::endl;
            return 1;
        }
    }
    
    // Setup ring buffer if buffer mode
    RingBuffer* ring_buffer = nullptr;
    if (mode == "buffer") {
        ring_buffer = new RingBuffer(buffer_size_mb * 1024 * 1024);
    }
    
    // Setup packet reorder buffer if enabled
    std::unique_ptr<PacketReorderBuffer> reorder_buffer;
    if (enable_reorder) {
        reorder_buffer = std::make_unique<PacketReorderBuffer>(reorder_window_size, true);
    }
    
    TCPServer server(host.c_str(), port);
    
    if (!server.initialize()) {
        std::cerr << "Failed to initialize TCP server" << std::endl;
        return 1;
    }
    
    std::cout << "Connected. Starting data collection..." << std::endl;
    std::cout << "Statistics will be printed every " << stats_interval << " seconds" << std::endl;
    std::cout << "Press Ctrl+C to stop early\n" << std::endl;
    
    auto last_print = std::chrono::steady_clock::now();
    auto last_data_check = std::chrono::steady_clock::now();
    uint64_t last_bytes = 0;
    
    server.run([&](const uint8_t* data, size_t size) {
        // Check duration
        if (duration > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - stats.start_time).count();
            if (elapsed >= duration) {
                server.stop();
                return;
            }
        }
        
        // Write to ring buffer if buffer mode
        if (mode == "buffer" && ring_buffer) {
            size_t written = ring_buffer->write(data, size);
            if (written < size) {
                stats.buffer_overruns++;
            }
            
            // Read from ring buffer for analysis (simulate consumer)
            uint8_t read_buffer[8192];
            size_t read_size = ring_buffer->read(read_buffer, sizeof(read_buffer));
            if (read_size > 0) {
                processRawData(read_buffer, read_size, stats, nullptr, 
                              reorder_buffer ? reorder_buffer.get() : nullptr);
            }
        } else {
            // Direct processing for disk mode
            processRawData(data, size, stats, &out_file, 
                          reorder_buffer ? reorder_buffer.get() : nullptr);
        }
        
        // Print periodic statistics
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_print).count();
        
        // Print statistics at regular intervals
        if (elapsed >= static_cast<int>(stats_interval)) {
            std::cout << "\n[Periodic Statistics Update]" << std::endl;
            printStatistics(stats, detailed_analysis);
            if (reorder_buffer) {
                printReorderStatistics(reorder_buffer->getStatistics());
            }
            std::cout << std::endl;
            last_print = now;
        }
        
        // Also print a brief status if data is being received
        auto data_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_data_check).count();
        if (data_elapsed >= 10000) {  // Every 10 seconds
            if (stats.total_bytes > last_bytes) {
                uint64_t bytes_diff = stats.total_bytes - last_bytes;
                double mbps = (bytes_diff * 8.0) / (data_elapsed / 1000.0) / 1e6;
                std::cout << "[Status] Received " << bytes_diff << " bytes in last 10s (~" 
                          << std::fixed << std::setprecision(2) << mbps << " Mbps)" << std::endl;
                last_bytes = stats.total_bytes;
            } else {
                std::cout << "[Warning] No data received in last 10 seconds" << std::endl;
            }
            last_data_check = now;
        }
    });
    
    std::cout << "\n=== Final Statistics ===" << std::endl;
    std::cout << "Data collection completed.\n" << std::endl;
    printStatistics(stats, true);
    if (reorder_buffer) {
        printReorderStatistics(reorder_buffer->getStatistics());
    }
    
    // Cleanup
    if (out_file.is_open()) {
        out_file.close();
    }
    if (ring_buffer) {
        delete ring_buffer;
    }
    
    return 0;
}

