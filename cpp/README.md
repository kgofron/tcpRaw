# TPX3 Raw Data Parser

**Author:** Kazimierz Gofron  
**Institution:** Oak Ridge National Laboratory  
**Created:** November 2, 2025  
**Modified:** November 8, 2025

C++ implementation for parsing Timepix3 raw data from TCP stream, with support for all packet types and experimental time extension.

## Overview

This program receives TPX3 raw data via TCP socket connection on port 8085, decodes all packet types including:
- Pixel data (count_fb and standard modes)
- TDC events
- Global time packets
- SPIDR control packets
- TPX3 control packets
- Experimental extra timestamp packets

## Features

- **TCP Client**: Connects to SERVAL/ADTimePix3 driver with automatic reconnection
- **Complete Packet Decoding**: Supports all TPX3 packet types from the SERVAL manual
- **Timestamp Extension**: Uses experimental extra packets to extend timestamps up to 325 days
- **Statistics Tracking**: Real-time hit counting and rate calculation (instant and cumulative)
- **TDC1/TDC2 Rate Tracking**: Separate rate tracking for TDC1 and TDC2 events
- **Per-Chip Statistics**: Individual hit rates for each chip (0-3)
- **Parallel Decode Pipeline**: Per-chip worker threads keep up with high-rate streams
- **Efficient Buffering**: 1MB socket reads with incomplete word buffering to minimize syscalls
- **Connection Monitoring**: Comprehensive connection statistics and error tracking
- **Packet Reordering**: Optional chunk-aware packet reordering for out-of-order packets
- **High-Rate Performance**: Configurable statistics output for rates up to 140 MHz
- **Data Integrity Verification**: Final summary compares parser received bytes with SERVAL file size
- **Future-Ready Architecture**: Designed for 3D clustering and event classification

## Building

### Requirements
- C++17 compatible compiler (GCC 7+ or Clang 5+)
- Make

### Compilation

```bash
cd cpp
make
```

This creates the executable at `bin/tpx3_parser`.

### Debug Build

```bash
make debug
```

### Clean

```bash
make clean
```

## Running

### Setup with SERVAL

SERVAL must be configured to listen for incoming connections:

```bash
curl -X PUT http://localhost:8081/server/destination \
  -H 'Content-Type: application/json' \
  -d '{"Raw":[{"Base":"tcp://listen@127.0.0.1:8085","SplitStrategy":"SINGLE_FILE","QueueSize":16384}]}'
```

### Start the Parser

**Basic usage:**
```bash
./bin/tpx3_parser
```

**With options:**
```bash
./bin/tpx3_parser --host 127.0.0.1 --port 8085 --stats-time 10 --exit-on-disconnect
```

**High-rate performance mode:**
```bash
./bin/tpx3_parser --stats-final-only --reorder
```

**Maximum performance mode:**
```bash
./bin/tpx3_parser --stats-disable --reorder
```

**Using the convenience script:**
```bash
./test/scripts/run_parser.sh --host 127.0.0.1 --port 8085 --stats-time 10
```

The parser will:
1. Initialize TCP client
2. Connect to SERVAL on 127.0.0.1:8085
3. Process incoming raw data chunks
4. Print statistics according to configured intervals
5. Print final summary when connection closes

## Command-Line Options

```bash
./bin/tpx3_parser [OPTIONS]
```

**Connection options:**
- `--host HOST` - TCP server host (default: 127.0.0.1)
- `--port PORT` - TCP server port (default: 8085)

**Reordering options:**
- `--reorder` - Enable packet reordering
- `--reorder-window SIZE` - Reorder buffer window size (default: 1000)

**Statistics options (for high-rate performance):**
- `--stats-interval N` - Print stats every N packets (default: 1000, 0=disable)
- `--stats-time N` - Print status every N seconds (default: 10, 0=disable)
- `--stats-final-only` - Only print final statistics (no periodic)
- `--stats-disable` - Disable all statistics printing
- `--recent-hit-count N` - Retain the last N hits for the summary (default: 10, 0=disable)

**Control options:**
- `--exit-on-disconnect` - Exit after connection closes (don't auto-reconnect)
- `--help` - Show help message

## Output

The program prints periodic statistics (if enabled):

```
=== Statistics ===
Elapsed time: 34.8 s (0.6 min)
Total hits: 64347603
Total chunks: 125300
Hit rate (instant): 2603938.60 Hz
Hit rate (cumulative avg): 1849768.72 Hz
Tdc1 rate (instant): 2.00 Hz
Tdc1 rate (cumulative avg): 1.98 Hz
Per-chip hit rates:
  Chip 0: 654007.58 Hz
  Chip 1: 652308.62 Hz
  Chip 2: 657610.49 Hz
  Chip 3: 640011.91 Hz
```

Final summary on exit:

```
============================================================
=== FINAL SUMMARY ===
============================================================
Total bytes received: 1122008160 (1070.03 MB)
Total packets (words) received: 140251020

=== Final Statistics ===
[... processing statistics ...]

=== Connection Statistics ===
Connection attempts: 1
Successful connections: 1
Disconnections: 1
Total bytes received: 1122008160 (1070.03 MB)
Bytes dropped (incomplete words): 0

============================================================
Data Reception Summary:
  Parser received: 1070.03 MB
  (1122008160 bytes)
  Compare with SERVAL .tpx3 file size to check for data loss.
============================================================
```

## Architecture

### File Structure

```
cpp/
├── src/
│   ├── main.cpp              # Entry point, TCP server loop
│   ├── tpx3_decoder.cpp      # Packet decoding logic
│   ├── tcp_server.cpp        # TCP connection handling
│   ├── timestamp_extension.cpp # Time extension algorithms
│   ├── hit_processor.cpp     # Hit buffering and statistics
│   ├── packet_reorder_buffer.cpp # Packet reordering for out-of-order packets
│   └── ring_buffer.cpp       # Lock-free ring buffer implementation
├── include/
│   ├── tpx3_packets.h        # Packet structure definitions
│   ├── tpx3_decoder.h
│   ├── tcp_server.h
│   ├── hit_processor.h
│   ├── packet_reorder_buffer.h
│   └── ring_buffer.h
├── test/
│   ├── src/
│   │   └── tcp_raw_test.cpp  # Comprehensive protocol analysis tool
│   ├── scripts/
│   │   └── run_parser.sh     # Convenience script for running parser
│   └── docs/
│       ├── SERVAL_FEEDBACK.md # Feedback for SERVAL developers
│       └── TEST_RESULTS_SUMMARY.md # Test results documentation
├── Makefile
└── README.md
```

### Key Components

- **TCPServer**: Handles TCP connection lifecycle and data reception (client mode)
  - Automatic reconnection on disconnect
  - Incomplete word buffering (handles TCP fragmentation)
  - Connection statistics and monitoring
  - TCP keepalive configuration
- **TPX3Decoder**: Decodes all packet types according to SERVAL manual
  - All packet types: pixel, TDC, SPIDR, global time, TPX3 control
  - Error handling for fractional TDC errors
  - Protocol validation
- **Timestamp Extension**: Implements wraparound-safe timestamp extension
  - Uses extra timestamp packets (0x51, 0x21)
  - Extends 30-bit timestamps up to 325 days
- **HitProcessor**: Buffers hits and tracks statistics
  - Instant and cumulative rate calculation
  - Per-chip hit rate tracking
  - TDC1/TDC2 rate tracking
  - Efficient rate calculation (throttled to reduce overhead)
- **PacketReorderBuffer**: Chunk-aware packet reordering
  - Handles out-of-order SPIDR packets
  - Configurable window size
  - Statistics tracking

## Future Extensions

The architecture is designed to support:

1. **3D Spatial Clustering**: Group hits by spatial and temporal proximity
2. **Event Classification**: Identify particle types (neutron/electron/X-ray/ion)
3. **EPICS Integration**: Publish data via EPICS Process Variables
4. **Hardware Acceleration**: Intel IPP or OpenCV for clustering algorithms

## References

- SERVAL Manual - Appendix: file formats (TPX3 raw file format)
- Extra packets for time extension documentation
- ADTimePix3 EPICS driver documentation

## Notes

- All data is processed in 8-byte aligned blocks
- Little-endian byte order throughout
- Timestamps are in 1.5625ns units (640 MHz clock) after extension
- ToT values are in nanoseconds (25ns clock units)

## License

See project root LICENSE file.

