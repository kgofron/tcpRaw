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

**Observation:**
- Duplicate packet IDs detected in consistent pattern
- Count grows proportionally with data received:
  - After ~5s: 4,718 duplicates
  - After ~10s: 9,356 duplicates  
  - After ~15s: 13,650 duplicates
- Ratio: ~3 duplicates per unique packet ID

**Pattern:**
- Packet IDs appear to reset at regular intervals
- Example: Packet ID 0 appears at words 1,122, 2,242, 3,362 (every ~1,120 words)
- Pattern repeats for IDs 1, 2, 3, etc.

**Question:**
Is this expected behavior? Should SPIDR packet IDs:
- **A)** Reset per frame/chunk boundary (current behavior)?
- **B)** Increment continuously across frames (different from current)?

### 2. Protocol Violations

**Observation:**
- Protocol violations detected: 12,582 → 36,402 (growing)
- Ratio to duplicates: ~2.67 violations per duplicate
- Suggests violations may be related to duplicate packet detection

**Need Clarification:**
- What specific violations are occurring?
- Are violations related to duplicate packet IDs?
- Are there other protocol conformance issues?

### 3. Out-of-Order Packets

**Observation:**
- Minimal out-of-order packets: 3-4 total
- Likely due to TCP/IP packet reordering (normal)
- Not a significant concern

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

The protocol analysis tool can provide:
- Detailed packet ID sequence analysis
- Per-frame/chunk packet ID tracking (if protocol clarified)
- Specific protocol violation details
- Bit-level field validation
- Per-chip statistics

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
   - Log specific violation types (not just counts)
   - Identify which protocol rules are being violated
   - Determine if violations are critical

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

