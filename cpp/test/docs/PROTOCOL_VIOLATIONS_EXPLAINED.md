# Protocol Violations Explained

## What Are Protocol Violations?

**Protocol violations** are separate from **duplicate packet IDs**. They indicate that packet fields do not conform to the TPX3 protocol specification.

### Duplicate Packet IDs vs. Protocol Violations

**Duplicate Packet IDs (Global):**
- **NOT a protocol violation** - they're tracked separately
- Indicates the same packet ID appears in multiple chunks
- May be **expected behavior** if packet IDs reset per chunk
- Currently: ~22,574 duplicates observed

**Protocol Violations:**
- Checks if packet **structure and fields** conform to specification
- Examples:
  - Invalid packet type headers
  - Fields out of documented range
  - Reserved bits set incorrectly
  - Invalid command codes
- Currently: 60,198 total violations, with 30,099 (50%) from SPIDR packets

## SPIDR Packet Violations (50% of Total)

The test shows **SPIDR packets (0x5, 0x50): 30,099 violations (50.0%)**

### What This Means

SPIDR validation checks:

**For SPIDR Control (0x5):**
- Header bits 63-60 must equal 0x5
- Command bits 59-56 must be one of: 0xf (shutter open), 0xa (shutter close), 0xc (heartbeat)

**For SPIDR Packet ID (0x50):**
- Header bits 63-56 must equal exactly 0x50

### Possible Causes

1. **False Positives (Likely):**
   - Validation logic may be too strict
   - May be checking fields that are valid but not recognized
   - Packet may be valid but validation doesn't account for all cases

2. **Real Issues:**
   - Packets may have incorrect headers
   - Command codes may be out of expected range
   - Reserved bits may be used by firmware

3. **Validation Bug:**
   - The validation function may be called incorrectly
   - May be validating packets that don't need validation
   - Logic error in header checking

## Investigation Needed

To determine if violations are real or false positives:

1. **Examine violation details** (first 20 logged violations)
2. **Check actual packet data** for SPIDR packets that fail validation
3. **Compare with SERVAL documentation** for valid SPIDR packet formats
4. **Verify validation logic** matches actual protocol specification

## Out-of-Order Packets

**Out-of-order packet IDs: 4**

These indicate SPIDR packet ID packets received in wrong sequence (e.g., ID 10 received before ID 9).

### Current Behavior
- Minimal: Only 4 occurrences
- Likely due to TCP packet reordering (normal network behavior)

### Reordering Feature

**Planned Enhancement:** Add ability to reorder packets by SPIDR packet ID sequence.

This would:
- Buffer out-of-order packets
- Sort by packet ID before processing
- Ensure correct sequence for analysis
- May require reordering window (e.g., buffer up to N packets)

## Summary

- **Duplicate Packet IDs:** Separate metric, may be expected
- **Protocol Violations:** Field/structure conformance issues
- **SPIDR 50% violations:** Need investigation - could be false positives or real issues
- **Out-of-order:** Minimal, can be handled with reordering

