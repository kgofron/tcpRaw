# TPX3 Raw Data Parser and EPICS IOC

**Author:** Kazimierz Gofron  
**Institution:** Oak Ridge National Laboratory  
**Created:** November 2, 2025  
**Modified:** November 4, 2025

C++ implementation for parsing Timepix3 raw data with support for all packet types, experimental time extension, and architecture for future 3D clustering and EPICS integration.

## Project Structure

```
tcpRaw/
├── cpp/                           # C++ implementation
│   ├── src/                       # Source files
│   ├── include/                   # Header files
│   ├── bin/                       # Compiled executable
│   ├── build/                     # Object files
│   ├── Makefile                   # Build system
│   └── README.md                  # C++ build/run instructions
├── python/                        # Reference Python implementation
│   ├── raw-stream-kaz.py         # Original parser
│   └── raw_tcp_port.sh           # Helper script
├── documentation/                 # Documentation
│   ├── TPX3_EPICS_IOC_Plan.md   # Initial implementation plan
│   └── Clustering_Architecture.md # Future clustering design
└── README.md                      # This file
```

## Quick Start

### Build the Parser

```bash
cd cpp
make
```

### Run

**Configure SERVAL:**
```bash
curl -X PUT http://localhost:8081/server/destination \
  -H 'Content-Type: application/json' \
  -d '{"Raw":[{"Base":"tcp://listen@127.0.0.1:8085","SplitStrategy":"SINGLE_FILE","QueueSize":16384}]}'
```

**Run the parser:**
```bash
# Basic usage
./cpp/bin/tpx3_parser

# With options (recommended)
./cpp/bin/tpx3_parser --host 127.0.0.1 --port 8085 --stats-time 10 --exit-on-disconnect

# Using convenience script
./cpp/test/scripts/run_parser.sh --host 127.0.0.1 --port 8085 --stats-time 10

# High-rate performance mode
./cpp/bin/tpx3_parser --stats-final-only --reorder
```

See [cpp/README.md](cpp/README.md) for complete documentation.

## Features

### ✅ Phase 1: Core Parser (Completed)

- **TCP Client**: Connects to SERVAL on 127.0.0.1:8085 with automatic reconnection
- **Complete Packet Decoding**: All TPX3 packet types from SERVAL manual
  - Pixel data (0xa count_fb, 0xb standard)
  - TDC data (0x6) with TDC1/TDC2 rate tracking
  - Global time (0x44, 0x45)
  - SPIDR control (0x50, 0x5)
  - TPX3 control (0x71)
- **Experimental Time Extension**: Decodes extra timestamp packets
  - Packet generation timestamp
  - Minimum/maximum event timestamps
  - Timestamp extension up to 325 days
- **Statistics**: Real-time hit counting and rate calculation
  - Instant rates (rolling average over ~1s window)
  - Cumulative average rates (total/elapsed time, matches SERVAL)
  - Per-chip hit rates (chips 0-3)
  - TDC1/TDC2 event rates
- **Data Integrity**: 100% data integrity verified
  - Incomplete word buffering (handles TCP fragmentation)
  - Connection monitoring and statistics
  - Final summary compares parser received bytes with SERVAL file size
- **Performance**: Optimized for high-rate performance (up to 140 MHz)
  - Configurable statistics output (reduce overhead)
  - Efficient buffering (8-byte aligned data processing)
  - Packet reordering (optional, chunk-aware)
- **Connection Monitoring**: Comprehensive connection statistics
  - Tracks connection attempts, disconnections, errors
  - Monitors bytes received and dropped
  - Real-time connection status logging

### 🎯 Phase 2: Future Enhancements (Planned)

See [Clustering_Architecture.md](documentation/Clustering_Architecture.md) for detailed design.

- **Time Alignment**: Sort hits by timestamp
- **3D Clustering**: Spatial-temporal clustering with DBSCAN
- **Centroid Extraction**: Compute cluster centroids
- **Event Classification**: Identify particle types
  - Neutrons
  - Electrons
  - X-rays
  - Ions
- **EPICS Integration**: Publish results via Process Variables

## Implementation Details

### Packet Decoding

Based on SERVAL Manual Appendix: file formats:
- All data is little-endian
- Chunks consist of 8-byte header followed by 8-byte words
- Packet type identified by most significant nibble
- Time units: 1.5625ns (640 MHz clock) for extended timestamps

### Timestamp Extension

Implements the wraparound-safe algorithm from the manual:

```cpp
uint64_t extend_timestamp(uint64_t timestamp, uint64_t minimum_timestamp, uint64_t n_bits) {
    uint64_t bit_mask = (1ULL << n_bits) - 1;
    uint64_t delta_t = ((timestamp - minimum_timestamp) & bit_mask);
    return minimum_timestamp + delta_t;
}
```

### Coordinate Decoding

PixAddr to (x,y) conversion per Table 6.6:
- Double column (bits 15-9): 128 columns, left to right
- Super pixel (bits 8-3): 64 per column, bottom to top
- Pixel index (bits 2-0): 8 per super pixel

## Comparison with Python Reference

The C++ implementation follows the structure of `python/raw-stream-kaz.py`:

| Feature | Python | C++ |
|---------|--------|-----|
| TCP Server | ✅ | ✅ |
| Pixel Decoding (0xb) | ✅ | ✅ |
| Pixel Decoding (0xa) | ❌ | ✅ |
| TDC Decoding | ✅ | ✅ |
| Extra Timestamps | ❌ | ✅ |
| All Control Packets | ❌ | ✅ |
| Statistics | Basic | Enhanced |

## Build Requirements

- C++17 compiler (GCC 7+, Clang 5+)
- Make
- POSIX sockets (Linux/macOS)

## Development

### Build Options

```bash
make          # Standard build
make debug    # Debug build with symbols
make clean    # Clean build artifacts
make run      # Build and run
```

### Code Organization

- `tpx3_packets.h`: Packet structure definitions
- `tpx3_decoder.cpp`: Bit manipulation and packet decoding
- `tcp_server.cpp`: TCP connection handling
- `timestamp_extension.cpp`: Time extension algorithms
- `hit_processor.cpp`: Statistics and buffering
- `main.cpp`: Integration and control flow

## Testing

### Unit Testing (Planned)

- Bit manipulation functions
- Coordinate decoding
- Timestamp extension edge cases
- Packet decoding validation

### Integration Testing (Planned)

- Recorded data playback
- Comparison with Python output
- Performance benchmarking

## Documentation

- **TPX3_EPICS_IOC_Plan.md**: Complete implementation plan
- **Clustering_Architecture.md**: 3D clustering and event classification design
- **cpp/README.md**: C++ build and usage instructions

## References

- SERVAL Manual - TPX3 raw file format specification
- Extra packets for time extension documentation
- ADTimePix3 EPICS driver documentation
- Timepix3 clustering papers

## License

[Specify license]

## Authors

**Primary Author:** Kazimierz Gofron, Oak Ridge National Laboratory  
**Created:** November 2, 2025

Based on Python implementation by Amsterdam Scientific Instruments.

