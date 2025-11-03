# Packet Reordering Feature Plan

## Overview

Add capability to reorder out-of-order SPIDR packet ID (0x50) packets before processing and analysis.

## Problem

**Current Situation:**
- Out-of-order packet IDs: 4-7 occurrences observed
- Minimal but indicates packets can arrive out of sequence
- Likely due to TCP/IP packet reordering (normal network behavior)

**Impact:**
- Analysis assumes packets arrive in order
- Out-of-order packets may affect statistics
- Could cause incorrect sequence analysis

## Solution Design

### Reordering Strategy

**Approach:**
- Buffer incoming packets with SPIDR packet IDs
- Maintain reorder window (e.g., buffer up to N packets)
- When packet arrives, check if sequence gap can be filled
- Output packets in correct ID sequence order

### Implementation Options

**Option 1: Simple Reorder Buffer**
- Fixed-size buffer for out-of-order packets
- Release packets in order when sequence gaps filled
- Pros: Simple, predictable memory usage
- Cons: May drop packets if buffer overflows

**Option 2: Sliding Window**
- Maintain sliding window of packet IDs
- Release packets when window complete
- Pros: Handles gaps better
- Cons: More complex, variable latency

**Option 3: Per-Chunk Reordering**
- Only reorder within chunk boundaries
- Chunk-aware ordering
- Pros: Respects chunk structure
- Cons: May miss inter-chunk reordering

### Recommended: Option 1 with Chunk Awareness

**Design:**
1. Track current expected packet ID
2. Buffer packets that arrive early (ID < expected + window)
3. Output packets in sequence when received
4. Reset reorder state at chunk boundaries (if IDs reset per chunk)

## Implementation Details

### Data Structures

```cpp
struct OutOfOrderPacket {
    uint64_t word;
    uint64_t packet_id;
    size_t word_position;
    uint64_t chunk_id;
};

class PacketReorderBuffer {
    std::map<uint64_t, OutOfOrderPacket> buffer_;  // packet_id -> packet
    uint64_t next_expected_id_;
    uint64_t max_window_size_;
    uint64_t oldest_allowed_id_;
};
```

### Algorithm

1. **On packet arrival with SPIDR packet ID:**
   - If ID == next_expected_id: Process immediately
   - If ID > next_expected_id: Add to buffer, check for gaps
   - If ID < next_expected_id (within window): Add to buffer
   - If ID < oldest_allowed_id: Skip (too old, likely duplicate)

2. **After adding to buffer:**
   - Check if next_expected_id can be released
   - Release consecutive packets starting from next_expected_id
   - Update next_expected_id

3. **On chunk boundary:**
   - If IDs reset per chunk: Reset reorder state
   - Flush buffer (process remaining packets)
   - Start new reorder window

### Configuration

- **Reorder window size:** Default 1000 packets
- **Max buffer size:** Prevent unbounded growth
- **Enable/disable:** Command-line option
- **Chunk-aware:** Respect chunk boundaries if IDs reset

## Integration Points

### Current Code

- `analyzeWord()` function processes words immediately
- SPIDR packet ID tracking in `analyzeWord()`
- Chunk boundary detection already implemented

### Changes Needed

1. **Add reorder buffer class** in separate file
2. **Modify analyzeWord()** to use buffer
3. **Add configuration option** for reordering
4. **Add statistics** for reordering effectiveness

## Statistics to Track

- Packets reordered (count)
- Max reorder distance (how far out of order)
- Buffer overflows (if window too small)
- Average reorder delay
- Packets dropped due to being too old

## Testing

1. **Synthetic test:** Generate out-of-order packets
2. **Real data test:** Verify reordering improves analysis
3. **Performance test:** Measure overhead
4. **Buffer overflow test:** Test with limited window size

## Future Enhancements

1. **Adaptive window size:** Adjust based on observed reordering
2. **Multiple sequence tracking:** If multiple chips have separate sequences
3. **Timestamp-based reordering:** Alternative to packet ID ordering
4. **Configurable timeout:** Drop packets after time limit

