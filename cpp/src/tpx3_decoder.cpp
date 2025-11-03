#include "tpx3_decoder.h"
#include <stdexcept>

PixelHit decode_pixel_data(uint64_t data, uint8_t chip_index) {
    uint8_t packet_type = (data >> 60) & 0xF;
    
    if (packet_type == 0xa) {
        return decode_pixel_data_count_fb(data, chip_index);
    } else if (packet_type == 0xb) {
        return decode_pixel_data_standard(data, chip_index);
    }
    
    throw std::runtime_error("Invalid pixel packet type");
}

PixelHit decode_pixel_data_count_fb(uint64_t data, uint8_t chip_index) {
    PixelHit hit;
    hit.is_count_fb = true;
    hit.chip_index = chip_index;
    
    // Extract PixAddr (bits 59-44)
    uint64_t pixaddr = get_bits(data, 59, 44);
    std::tie(hit.x, hit.y) = pixaddr_to_xy(pixaddr);
    
    // Extract Integrated ToT (bits 43-30) in 25ns units
    uint16_t integrated_tot = get_bits(data, 43, 30);
    
    // Extract EventCount (bits 29-20)
    uint16_t event_count = get_bits(data, 29, 20);
    
    // Extract HitCount (bits 19-16) - not currently used
    // uint8_t hit_count = get_bits(data, 19, 16);
    
    // SPIDR time (bits 15-0) in 0.4096ms units
    uint16_t spidr_time = get_bits(data, 15, 0);
    
    // For count_fb mode, we use integrated ToT as ToT
    hit.tot_ns = integrated_tot * 25; // Convert to nanoseconds
    
    // Calculate TOA: ((SPIDR_time << 14) + EventCount) << 4)
    uint64_t toa_counts = ((static_cast<uint64_t>(spidr_time) << 14) + event_count) << 4;
    hit.toa_ns = toa_counts; // Already in 1.5625ns units
    
    return hit;
}

PixelHit decode_pixel_data_standard(uint64_t data, uint8_t chip_index) {
    PixelHit hit;
    hit.is_count_fb = false;
    hit.chip_index = chip_index;
    
    // Extract PixAddr (bits 59-44)
    uint64_t pixaddr = get_bits(data, 59, 44);
    std::tie(hit.x, hit.y) = pixaddr_to_xy(pixaddr);
    
    // Extract ToA (bits 43-30) in 25ns units
    uint16_t toa = get_bits(data, 43, 30);
    
    // Extract ToT (bits 29-20) in 25ns units
    uint16_t tot = get_bits(data, 29, 20);
    hit.tot_ns = tot * 25; // Convert to nanoseconds
    
    // Extract FToA (bits 19-16) in -1.5625ns units (negative!)
    uint8_t ftoa = get_bits(data, 19, 16);
    
    // Extract SPIDR time (bits 15-0) in 0.4096ms units
    uint16_t spidr_time = get_bits(data, 15, 0);
    
    // Calculate full TOA: ((SPIDR_time << 14) + ToA) << 4) - FToA
    // This gives time in 1.5625ns units
    hit.toa_ns = (((static_cast<uint64_t>(spidr_time) << 14) + toa) << 4) - ftoa;
    
    return hit;
}

TDCEvent decode_tdc_data(uint64_t data) {
    TDCEvent tdc;
    
    // TDC event type (bits 59-56)
    uint8_t event_type = get_bits(data, 59, 56);
    tdc.type = static_cast<TDCEventType>(event_type);
    
    // Trigger count (bits 55-44)
    tdc.trigger_count = get_bits(data, 55, 44);
    
    // Timestamp (bits 43-9) in 3.125ns units
    uint64_t tdc_coarse = get_bits(data, 43, 9);
    
    // Fine timestamp (bits 8-5), values 1-12 (but 0 seen on old firmware)
    uint8_t fract = get_bits(data, 8, 5);
    
    // Bug: fract is sometimes 0 for older firmware but it should be 1 <= fract <= 12
    // Python assert treats 0 as an error, but we'll accept it and treat as 1
    if (fract == 0) {
        fract = 1;  // Handle old firmware bug
    } else if (fract > 12) {
        throw std::runtime_error("Invalid fractional TDC part: " + std::to_string(fract));
    }
    
    tdc.fine_timestamp = fract;
    
    // Convert TDC to 1.5625ns units (640 MHz clock)
    // Formula from manual: (tdcCoarse << 1) | ((fract-1) // 6)
    tdc.timestamp_ns = (tdc_coarse << 1) | ((fract - 1) / 6);
    
    return tdc;
}

GlobalTime decode_global_time(uint64_t data) {
    GlobalTime gt;
    
    // Packet type (bits 63-56)
    uint8_t packet_type = get_bits(data, 63, 56);
    
    if (packet_type == 0x44) {
        gt.is_high_word = false;
        // Timestamp (bits 47-16) in 25ns units
        gt.time_value = get_bits(data, 47, 16);
    } else if (packet_type == 0x45) {
        gt.is_high_word = true;
        // Timestamp (bits 31-16) in 107.374182s units
        gt.time_value = get_bits(data, 31, 16);
    }
    
    // SPIDR time (bits 15-0) in 0.4096ms units
    gt.spidr_time = get_bits(data, 15, 0);
    
    return gt;
}

bool decode_spidr_packet_id(uint64_t data, uint64_t& packet_count) {
    // Check if this is a packet ID packet (0x50)
    if ((data >> 56) != 0x50) {
        return false;
    }
    
    // Packet count (bits 47-0)
    packet_count = get_bits(data, 47, 0);
    return true;
}

bool decode_spidr_control(uint64_t data, SpidrControl& ctrl) {
    // Check if this is a control packet (0x5)
    if ((data >> 60) != 0x5) {
        return false;
    }
    
    // Control command (bits 59-56)
    uint8_t cmd = get_bits(data, 59, 56);
    
    // Check for valid control commands
    if (cmd == SPIDR_SHUTTER_OPEN || cmd == SPIDR_SHUTTER_CLOSE || cmd == SPIDR_HEARTBEAT) {
        ctrl.command = static_cast<SpidrControlCmd>(cmd);
        // Timestamp (bits 45-12) in 25ns units
        ctrl.timestamp_ns = get_bits(data, 45, 12) * 25ULL;
        return true;
    }
    
    return false;
}

bool decode_tpx3_control(uint64_t data, Tpx3ControlCmd& cmd) {
    // Check if this is a TPX3 control packet (0x71)
    if ((data >> 56) != 0x71) {
        return false;
    }
    
    // Control command (bits 55-48)
    uint8_t control_cmd = get_bits(data, 55, 48);
    
    if (control_cmd == TPX3_END_SEQUENTIAL || control_cmd == TPX3_END_DATA_DRIVEN) {
        cmd = static_cast<Tpx3ControlCmd>(control_cmd);
        return true;
    }
    
    return false;
}

ExtraTimestamp decode_extra_timestamp(uint64_t data) {
    ExtraTimestamp ts;
    
    // Header (bits 63-56)
    uint8_t header = get_bits(data, 63, 56);
    ts.is_tpx3 = (header == 0x51);
    
    // Error flag (bit 55)
    ts.error_flag = get_bits(data, 55, 55) != 0;
    
    // Overflow flag (bit 54)
    ts.overflow_flag = get_bits(data, 54, 54) != 0;
    
    // Timestamp (bits 53-0) in 1.5625ns units
    ts.timestamp_ns = get_bits(data, 53, 0);
    
    return ts;
}

