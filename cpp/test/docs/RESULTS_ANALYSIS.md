# Test Results Analysis: Protocol Violations and Duplicate Packet IDs

## Summary

The test results show several significant issues:

- **Duplicate Packet IDs: 4,582** → growing to 14,043
- **Protocol Violations: 12,220** → growing to 37,450  
- **Out-of-order Packet IDs: 2** (minor)

## Detailed Analysis

### 1. Duplicate Packet IDs (4,582 ⚠️)

**Pattern Observed:**
- Duplicates occur at regular intervals (~1,120 words apart)
- Example pattern: Packet ID 0 appears at words 1,122, 2,242, 3,362
- Packet ID 1 appears at words 5,602, 6,722, 7,842
- Each ID appears approximately 3 times in the observed pattern

**Analysis:**
```
Seen: 1,528 unique SPIDR packet IDs
Duplicates: 4,582 occurrences
Ratio: ~3 duplicates per unique ID
```

**Possible Causes:**

1. **SPIDR Packet Counter Resets (MOST LIKELY)**
   - Packet ID counter may reset at frame/chunk boundaries
   - This could be **normal behavior** if SPIDR resets per frame
   - Each frame starts with packet ID 0, 1, 2, etc.

2. **Per-Chip Packet Counters**
   - If each chip has its own packet ID counter, duplicates are expected
   - 4 chips × ~1,527 chunks per chip = 6,108 total chunks
   - This matches approximately with the duplicate pattern

3. **TCP/IP Duplication**
   - Less likely - TCP should prevent duplication
   - Would require network-level issues

### 2. Protocol Violations (12,220 ⚠️)

**Observation:**
- Protocol violations ≈ 2.67× duplicate packet ID count
- This suggests violations may be **related** to duplicate detection

**Potential Violation Sources:**

1. **Duplicate Packet ID Validation**
   - The validator flags duplicate packet IDs as protocol violations
   - This may be a **false positive** if duplicates are expected behavior

2. **Other Protocol Checks**
   - Bit field validation failures
   - Reserved bit violations
   - Range check failures

**Need to Investigate:**
- What specific violations are being flagged?
- Are violations correlated with duplicate packet IDs?
- Are there other types of violations?

### 3. Out-of-order Packet IDs (2 ⚠️)

**Analysis:**
- Only 2 out-of-order packets detected
- This is **negligible** - likely due to TCP/IP packet reordering
- Acceptable for TCP transmission

## Interpretation

### Normal Behavior Hypothesis

If SPIDR packet IDs reset per frame/chunk:

✅ **Expected Pattern:**
- Each frame/chunk may start with packet ID 0
- Packet IDs increment within each frame/chunk
- Duplicates are **normal** because IDs reset per frame

✅ **What This Means:**
- The duplicate detection is flagging **expected behavior**
- Protocol violations related to duplicates are **false positives**
- The test tool needs to account for per-frame/chunk resets

### Abnormal Behavior Hypothesis

If SPIDR packet IDs should increment continuously:

❌ **Problem:**
- Packet counter is resetting unexpectedly
- Could indicate firmware bug or configuration issue
- Data may be getting duplicated in transmission chain

❌ **Action Required:**
- Investigate SERVAL/SPIDR configuration
- Check if packet IDs should be continuous or per-frame
- Verify firmware version and behavior

## Recommendations

### 1. Verify Expected Behavior

**Check SERVAL/SPIDR Documentation:**
- Should packet IDs reset per frame/chunk?
- Should packet IDs increment continuously?
- What is the expected packet ID behavior?

### 2. Update Test Tool Logic

**If Packet IDs Reset Per Frame (Normal):**
- Modify duplicate detection to account for frame/chunk boundaries
- Only flag duplicates within the same frame/chunk
- Reset tracking at frame/chunk boundaries

**If Packet IDs Should Be Continuous (Abnormal):**
- Flag duplicates as real errors
- Investigate SERVAL/SPIDR configuration
- Report to firmware developers

### 3. Investigate Protocol Violations

**Add Detailed Violation Logging:**
- Log specific violation types (not just counts)
- Track which packet types have violations
- Correlate violations with duplicate packet IDs

### 4. Compare with Real-Time Parser

**Check Parser Statistics:**
- Does parser report similar issues?
- Does parser handle duplicates differently?
- Are there discrepancies in chunk/packet counts?

## Next Steps

1. **Immediate:**
   - Check SERVAL/SPIDR documentation for packet ID behavior
   - Verify if duplicates are expected or errors

2. **Short-term:**
   - Add detailed violation type reporting
   - Modify duplicate detection to account for frame boundaries
   - Compare parser vs. test tool statistics

3. **Long-term:**
   - Update protocol validator if duplicates are normal
   - Add frame/chunk-aware tracking
   - Create violation categorization

## Current Status

**Test Tool Results:**
- ✅ Data collection working correctly
- ✅ Protocol validation active
- ⚠️ Duplicate detection may be flagging normal behavior
- ⚠️ Need to verify expected packet ID behavior

**Action Required:**
- Determine if packet ID duplicates are expected
- Update validation logic accordingly
- Add frame/chunk boundary awareness

