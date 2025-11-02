# TPX3 Raw Data Parser

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

- **TCP Client**: Connects to SERVAL/ADTimePix3 driver
- **Complete Packet Decoding**: Supports all TPX3 packet types from the SERVAL manual
- **Timestamp Extension**: Uses experimental extra packets to extend timestamps up to 325 days
- **Statistics Tracking**: Real-time hit counting and rate calculation
- **Efficient Buffering**: 8-byte aligned data processing
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

```bash
./bin/tpx3_parser
```

The parser will:
1. Initialize TCP client
2. Connect to SERVAL on 127.0.0.1:8085
3. Process incoming raw data chunks
4. Print statistics every 1000 processed hits

## Output

The program prints periodic statistics:

```
=== Statistics ===
Total hits: 5000
Total chunks: 25
Total TDC events: 120
Hit rate: 15000.00 Hz
```

And recent hit information:

```
=== Recent Hits (last 10) ===
Chip 0, X=128, Y=256, ToA=12345678 (1.5625ns units), ToT=500 ns [standard]
...
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
│   └── hit_processor.cpp     # Hit buffering and statistics
├── include/
│   ├── tpx3_packets.h        # Packet structure definitions
│   ├── tpx3_decoder.h
│   ├── tcp_server.h
│   └── hit_processor.h
├── Makefile
└── README.md
```

### Key Components

- **TCPServer**: Handles TCP connection lifecycle and data reception (client mode)
- **TPX3Decoder**: Decodes all packet types according to SERVAL manual
- **Timestamp Extension**: Implements wraparound-safe timestamp extension
- **HitProcessor**: Buffers hits and tracks statistics

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

