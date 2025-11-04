# TPX3 Raw Data Parser and Processing Pipeline

**Author:** Kazimierz Gofron  
**Institution:** Oak Ridge National Laboratory  
**Created:** November 2, 2025  
**Modified:** November 4, 2025

## Overview
Build a C++ program that receives TPX3 raw data via TCP socket, decodes all packet types (pixel hits, TDC, control, time extension), performs timestamp extension, and establishes the foundation for 3D spatial clustering and event centroid detection.

## Phase 1: Core C++ Parser (Standalone)

### TCP Connection Layer
- Implement TCP server using `tcp://listen@127.0.0.1:8085` mode
- Create socket listener that accepts connections from ADTimePix3 driver
- Implement efficient buffering strategy (similar to Python's recv_into with 8-byte aligned reads)
- Handle connection lifecycle (accept, read loop, graceful disconnect)

### TPX3 Packet Decoder
Based on the format specifications from the manual images, implement decoders for:

**Chunk Structure:**
- Parse 8-byte chunk header (bits 63-48: size, 39-32: chip index, 31-0: 'TPX3' identifier)
- Process 8-byte data words within chunks

**Packet Types:**
1. **Pixel Data (0xa, 0xb)** - Table 6.2
   - Type 0xa: count_fb mode (Integrated ToT, EventCount, HitCount)
   - Type 0xb: standard mode (ToA, ToT, FToA with negative units)
   - Decode PixAddr to (x, y) coordinates using Table 6.6 format
   - Extract timestamps: `((SPIDR_time << 14) + ToA) << 4) - FToA`

2. **TDC Data (0x6)** - Table 6.3
   - Decode TDC event types (0xf/0xa/0xe/0xb for Rise/Fall on TDC1/TDC2)
   - Extract trigger count, timestamp (3.125ns), fine timestamp (260.41666ps steps)
   - Handle fine timestamp error state (value 0)

3. **Global Time (0x44, 0x45)** - Table 6.3
   - Type 0x44: Time Low (25ns units)
   - Type 0x45: Time High (107.374182s units)
   - Used for timestamp extension up to ~81 days

4. **SPIDR Control (0x50, 0x5)** - Table 6.4
   - Packet ID (0x50): packet count tracking
   - Control commands (0x5): shutter open/close, heartbeat

5. **TPX3 Control (0x71)** - Table 6.5
   - End of sequential readout (0xa0)
   - End of data driven readout (0xb0)

**Extra Timestamp Packets (Experimental):**
- Decode three packets appended at chunk end:
  1. Packet generation timestamp (0x51 for TPX3, 0x21 for MPX3)
  2. Minimum event timestamp (1.5625ns units, 54-bit)
  3. Maximum event timestamp (1.5625ns units, 54-bit)
- Check overflow/error flags (bits 54-55)
- Use for timestamp extension up to 325 days

### Timestamp Extension Algorithm
Implement the Rust formula adapted to C++:
```cpp
uint64_t extend_timestamp(uint64_t timestamp, uint64_t minimum_timestamp, uint64_t n_bits) {
    uint64_t bit_mask = (1ULL << n_bits) - 1;
    uint64_t delta_t = ((timestamp - minimum_timestamp) & bit_mask);
    return minimum_timestamp + delta_t;
}
```
- Apply to all 30-bit precision timestamps within chunks
- Handle wraparound correctly

### Data Structures
Create efficient C++ classes:
- `TPX3Header`: chunk header data
- `PixelHit`: x, y, ToA (extended), ToT, chip_index
- `TDCEvent`: type, trigger_count, timestamp (extended)
- `ChunkMetadata`: min/max timestamps, packet generation time
- `ClusterCandidate`: for future 3D clustering (x, y, ToA, ToT)

### Output and Statistics
- Real-time hit counting and rate calculation
- Print diagnostic info (every N hits, similar to Python version)
- Accumulate hits in memory structure for future clustering
- Basic statistics: total hits, TDC events, chunks processed

## Phase 2: Architecture for Future Features

### 3D Clustering Preparation
Design interface for future integration:
- Define `ClusterProcessor` abstract class with methods:
  - `addHit(PixelHit)`: buffer hits for time alignment
  - `processTimeWindow()`: find spatial-temporal clusters
  - `extractCentroids()`: compute cluster centroids
- Prepare data structure for time-sorted hit buffer
- Consider using Intel IPP or OpenCV for:
  - Connected component labeling (spatial clustering)
  - Centroid calculation (cv::moments)
  - 3D distance metrics (IPP vector operations)

### Event Classification Framework
Stub out event processing pipeline:
- `EventClassifier` interface for neutron/electron/X-ray/ion discrimination
- Based on cluster properties: size, ToT distribution, shape
- Placeholder for machine learning integration

### EPICS IOC Integration Hooks
Prepare for future asyn driver:
- Separate parsing logic from I/O (single responsibility)
- Design callback interface for processed data
- Define PV structure outline:
  - Scalars: hit_rate, total_hits, connection_status
  - Waveforms: recent_hits_x[], recent_hits_y[], recent_hits_toa[]
  - Statistics: cluster_count, event_rate by type

## Implementation Details

### File Structure
```
/home/kg1/Documents/src/github/tcpRaw/
├── cpp/
│   ├── src/
│   │   ├── main.cpp              # Entry point, TCP server loop
│   │   ├── tpx3_decoder.cpp      # Packet decoding logic
│   │   ├── tcp_server.cpp        # TCP connection handling
│   │   ├── timestamp_extension.cpp # Time extension algorithms
│   │   └── hit_processor.cpp     # Hit buffering and statistics
│   ├── include/
│   │   ├── tpx3_packets.h        # Packet structure definitions
│   │   ├── tpx3_decoder.h
│   │   ├── tcp_server.h
│   │   └── hit_processor.h
│   ├── Makefile                  # Build system
│   └── README.md                 # Documentation
```

### Build Dependencies
- C++17 standard
- No external dependencies for Phase 1 (use standard library)
- Optional for Phase 2: Intel IPP or OpenCV (decision point later)

### Testing Strategy
- Unit tests for bit manipulation (get_bits, calculate_XY)
- Validate against Python script output using recorded data
- Test timestamp extension edge cases (wraparound)
- Verify extra packet decoding with known samples

## Key Implementation Notes
1. All multi-byte values are little-endian
2. Use bit manipulation macros/functions for clarity
3. Handle firmware version differences (e.g., TDC fractional=0 bug)
4. Validate chunk headers (TPX3 magic number)
5. Process data in 8-byte aligned blocks only

## Processing Goals

### Data Flow
1. **TCP Reception**: Receive raw 8-byte aligned data chunks
2. **Packet Decoding**: Parse and validate all packet types
3. **Timestamp Extension**: Apply time extension to achieve full precision
4. **Hit Buffering**: Store hits with extended timestamps
5. **Time Alignment**: Sort hits by timestamp (future)
6. **3D Clustering**: Group spatially and temporally close hits (future)
7. **Centroid Extraction**: Calculate cluster centroids as events (future)
8. **Event Classification**: Identify particle type (neutron/electron/X-ray/ion) (future)
9. **EPICS Integration**: Publish results to PVs (future)

### Event Processing Requirements
- Time alignment of hits before clustering
- 3D spatial clustering (x, y, time) with configurable thresholds
- Centroid calculation for each cluster
- Event classification based on:
  - Cluster size (number of pixels)
  - Total energy deposition (sum of ToT)
  - Cluster shape/morphology
  - Time structure

## Implementation Todos

1. **tcp-server**: Implement TCP server with listen mode on port 8085, handle connection lifecycle
2. **packet-structures**: Define C++ structs/classes for all TPX3 packet types and chunk headers
3. **bit-manipulation**: Create bit extraction utilities and PixAddr to (x,y) coordinate decoder
4. **pixel-decoder**: Implement pixel data packet decoder (0xa, 0xb) with ToA/ToT extraction
5. **timestamp-extension**: Implement timestamp extension algorithm with wraparound handling
6. **extra-packets**: Decode experimental extra timestamp packets (min/max/generation)
7. **tdc-control-decoders**: Implement TDC, global time, and control packet decoders
8. **hit-processor**: Create hit buffering and statistics tracking system
9. **main-loop**: Integrate all components into main processing loop with diagnostics
10. **clustering-architecture**: Design abstract interfaces for future 3D clustering and event classification

## References
- SERVAL Manual - Appendix: file formats (TPX3 raw file format)
- Extra packets for time extension documentation
- Timestamp extension algorithm (Rust implementation reference)
- ADTimePix3 EPICS driver documentation

