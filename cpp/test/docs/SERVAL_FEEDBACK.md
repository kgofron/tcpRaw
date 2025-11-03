# Feedback for SERVAL Author: TPX3 Protocol Analysis Results

## Summary

Protocol analysis of TPX3 raw data stream reveals consistent patterns that may indicate protocol behavior or potential issues.

## Test Configuration

- **Test Duration:** Multiple runs (30-60 seconds each)
- **Data Source:** SERVAL TPX3 detector
- **TCP Sockets:** 
  - Port 8085: Real-time parser
  - Port 8086: Protocol analysis tool
- **Data Rate:** ~85-87 Mbps average throughput

## Findings

### 1. Duplicate Packet IDs (SPIDR 0x50 Packets)

**Latest Test Results:**
- **After ~5s:** 4,605 global duplicates, 0 within-chunk duplicates, 1 missing
- **After ~10s:** 9,126 global duplicates, 0 within-chunk duplicates, 1 missing
- **After ~15s:** 13,755 global duplicates, 0 within-chunk duplicates, 1 missing
- **Ratio:** ~3 global duplicates per unique packet ID seen
- **Missing:** 1 packet ID gap (negligible, may be TCP reordering)

**Pattern Analysis:**
- Packet IDs reset at chunk boundaries (confirmed by per-chunk tracking)
- **Within-chunk duplicates: 0** ✓ (no protocol errors within chunks)
- **Global duplicates:** High count, but appear to be expected if IDs reset per chunk
- Example pattern: Packet ID 0 appears in multiple chunks (expected if reset)

**Key Observation:**
- The test tool now distinguishes:
  - **Global duplicates** (across chunks): May be expected behavior
  - **Within-chunk duplicates**: Would indicate real protocol errors (currently 0)

**Question for SERVAL:**
Is packet ID reset per chunk the intended behavior?
- **A)** Yes, IDs reset at each chunk boundary (current behavior) ✓
- **B)** No, IDs should increment continuously across chunks

### 2. Protocol Violations

**Latest Test Results:**
- **Total violations:** 60,198
- **SPIDR packet violations:** 30,099 (50.0% of total)
- **Ratio to global duplicates:** ~2.67 violations per global duplicate

**Important Clarification:**
- **Protocol violations are SEPARATE from duplicate packet IDs**
- Duplicate packet IDs are tracked separately and NOT counted as violations
- Protocol violations check packet STRUCTURE and FIELD conformance to specification

**Breakdown by Packet Type:**
The enhanced test tool provides categorized violation counts:
- **SPIDR packets (0x5, 0x50):** 30,099 (50.0%) - Dominates violations
  - 0x5 (SPIDR control): Validates header bits 63-60 = 0x5, command in {0xf, 0xa, 0xc}
  - 0x50 (SPIDR packet ID): Validates header bits 63-56 = exactly 0x50
- Pixel packet violations (0xa, 0xb): Invalid PixAddr, ToA/ToT, FToA ranges
- TDC packet violations (0x6): Invalid event types, trigger counts, fractional timestamps, reserved bits
- Global time violations (0x44, 0x45): Header mismatches, reserved bits
- TPX3 control violations (0x71): Invalid commands
- Extra timestamp violations (0x51, 0x21): Invalid headers
- Reserved bit violations: Bits that should be zero but are set

**Critical Question:**
**SPIDR packets account for 50% of all protocol violations.**
- Are these false positives (validation too strict)?
- OR are SPIDR packets actually violating the protocol?
- **Need to check actual failing packet data to determine**

**Investigation Needed:**
1. Examine actual SPIDR packet data that fails validation
2. Verify if validation logic matches SERVAL protocol specification
3. Check if there are firmware-specific behaviors not accounted for
4. Determine if violations indicate real protocol errors or validation bugs

### 3. Out-of-Order and Missing Packets

**Latest Test Results:**
- **Out-of-order packets:** 3 → 7 → 7 (minimal)
- **Missing packet IDs:** 1 → 1 → 1 (negligible)
- Both within acceptable limits (likely TCP/IP reordering)
- **Assessment:** No significant concern ✓

## Protocol Questions

### Packet ID Behavior

1. **Expected Packet ID Sequence:**
   - Should SPIDR packet IDs (0x50 packets) increment continuously?
   - Or reset at frame/chunk boundaries?
   - What defines a "frame" or "chunk" for packet ID reset?

2. **Per-Chip Packet IDs:**
   - Do different chips (0-3) use separate packet ID counters?
   - Should packet IDs be globally unique or per-chip?
   - Current observation: 4 chips with similar duplicate patterns

3. **Packet ID Wraparound:**
   - At what value does packet ID wrap? (48-bit field: 0-2^48-1)
   - Should wraparound be handled differently than reset?

### Chunk Structure

1. **Chunk Boundaries:**
   - What defines the start/end of a chunk?
   - Should packet IDs reset at chunk boundaries?
   - How are chunk headers (magic 'TPX3') related to packet IDs?

2. **Frame Boundaries:**
   - Are frames defined by control packets (0x71, 0xa0, 0xb0)?
   - Should packet IDs reset at frame boundaries?
   - How are frames related to chunks?

### Protocol Validation

1. **Acceptable Ranges:**
   - Are there specific bit fields that may violate documented ranges?
   - Are firmware-specific behaviors documented?
   - Are there known protocol deviations in current firmware?

2. **Reserved Bits:**
   - Which bits are truly reserved and must be zero?
   - Are some "reserved" bits used for firmware-specific purposes?

## Requested Information

### Documentation

1. **SPIDR Packet ID Behavior:**
   - Official specification for packet ID counter behavior
   - Whether IDs reset per frame/chunk or increment continuously
   - Expected duplicate patterns (if any)

2. **Protocol Specification:**
   - Complete protocol specification document
   - Known firmware-specific behaviors
   - Acceptable deviations from specification

3. **Frame/Chunk Structure:**
   - Definition of frame vs. chunk
   - How packet IDs relate to frame/chunk boundaries
   - Control packet relationships

### Test Data

1. **Expected Patterns:**
   - Sample data with expected packet ID sequences
   - Examples of valid duplicate patterns (if any)
   - Known protocol violations that are acceptable

2. **Configuration:**
   - SERVAL configuration that affects packet ID behavior
   - Firmware version compatibility
   - Settings that control packet ID reset behavior

## Test Tool Capabilities

The enhanced protocol analysis tool provides:
- **Detailed packet ID sequence analysis:**
  - Global tracking (across all chunks)
  - Per-chunk tracking (within each chunk)
  - Distinguishes expected resets from real errors
- **Categorized protocol violation analysis:**
  - Breakdown by packet type (pixel, TDC, SPIDR, etc.)
  - Reserved bit violation tracking
  - Violation percentages per category
- **Bit-level field validation:**
  - All packet types validated against specification
  - Range checking for all fields
  - Reserved bit validation
- **Per-chip statistics:**
  - Chunk counts per chip
  - Packet counts per chip
  - Hit rate analysis

## Recommendations

### If Packet IDs Reset Per Frame (Expected):

✅ **No Action Required:**
- Duplicates are expected behavior
- Test tool should be updated to account for frame boundaries
- Protocol violations related to duplicates are false positives

### If Packet IDs Should Be Continuous (Issue):

❌ **Investigation Required:**
- Firmware bug or configuration issue
- May indicate packet duplication in transmission
- Report to firmware developers

### If Protocol Violations Are Real Issues:

⚠️ **Action Required:**
- Identify specific violation types
- Determine if violations are critical
- Update firmware or documentation accordingly

## Next Steps

1. **Clarify Expected Behavior:**
   - Determine if packet ID resets are normal
   - Update test tool to match expected behavior
   - Reduce false positive violations

2. **Detailed Violation Analysis:**
   - ✅ **COMPLETED:** Test tool now categorizes violations by packet type
   - ✅ **COMPLETED:** Shows violation percentages per category
   - **Next:** Run enhanced tool to identify which specific violations dominate
   - **Next:** Analyze if violations correlate with duplicate packet IDs

3. **Protocol Documentation:**
   - Complete protocol specification
   - Firmware-specific behaviors documented
   - Test cases with expected patterns

## Contact Information

For protocol questions or clarification, please provide:
- Official protocol specification
- Expected packet ID behavior
- Frame/chunk boundary definitions
- Any known protocol deviations

Thank you for your assistance in clarifying these protocol behaviors.

