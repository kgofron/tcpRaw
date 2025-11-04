# TPX3 Protocol Test and Analysis Tools

**Author:** Kazimierz Gofron  
**Institution:** Oak Ridge National Laboratory  
**Created:** November 2, 2025  
**Modified:** November 4, 2025

This directory contains test programs, scripts, documentation, and results for analyzing the TPX3 raw data protocol and comparing with the real-time parser.

## Directory Structure

```
test/
├── src/                    # Test program source code
│   └── tcp_raw_test.cpp    # Comprehensive protocol analysis tool
├── scripts/                # Comparison and test scripts
│   ├── run_comparison.sh   # Main comparison script (dual socket)
│   ├── compare_tools.sh    # Detailed comparison script
│   ├── run_comparison_now.sh # Quick comparison wrapper
│   └── tcp_stream_duplicator.py # TCP stream duplication tool
├── docs/                   # Test documentation
│   ├── RESULTS_ANALYSIS.md # Analysis of test results
│   └── SERVAL_CONFIGURATION.md # SERVAL setup instructions
└── results/                # Test results (timestamped directories)
    └── comparison_YYYYMMDD_HHMMSS/
        ├── test_tool.log
        ├── parser.log
        └── comparison_report.txt
```

## Building the Test Tool

From the `cpp/` directory:

```bash
make tcp_raw_test
# or
make all  # builds both parser and test tool
```

The test tool will be built to `cpp/bin/tcp_raw_test`.

## Running Tests

### Prerequisites

SERVAL must be configured with **two TCP sockets**:
- `tcp://listen@localhost:8085` (for real-time parser)
- `tcp://listen@localhost:8086` (for test tool)

See `docs/SERVAL_CONFIGURATION.md` for detailed setup instructions.

### Quick Comparison

From the project root:

```bash
# Run comparison for 60 seconds
./cpp/test/scripts/run_comparison.sh 60

# Or from cpp/test/scripts
cd cpp/test/scripts
./run_comparison.sh 60
```

### Manual Testing

**Terminal 1 (Test Tool):**
```bash
./cpp/bin/tcp_raw_test --port 8086 --analyze --stats-interval 5 --duration 60
```

**Terminal 2 (Parser):**
```bash
./cpp/bin/tpx3_parser --port 8085
```

## Test Tool Features

The `tcp_raw_test` program provides comprehensive protocol analysis:

### Protocol Conformance
- Packet type validation
- Chunk header validation
- Bit-level field validation
- Reserved bit checking

### Packet Order Analysis
- SPIDR packet ID sequence tracking
- Missing packet detection
- Duplicate packet detection
- Out-of-order packet detection

### Data Integrity
- 8-byte word alignment verification
- Incomplete word detection
- Chunk structure validation

### Statistics
- Throughput metrics (bytes/s, MB/s, GB/s)
- Packet type breakdown
- Per-chip statistics
- Protocol violation details

## Command Line Options

```bash
./cpp/bin/tcp_raw_test [OPTIONS]

Options:
  --mode buffer|disk      Output mode (default: buffer)
  --output FILE           Output file path for disk mode
  --buffer-size SIZE      Ring buffer size in MB (default: 256)
  --host HOST             TCP server host (default: 127.0.0.1)
  --port PORT             TCP server port (default: 8085)
  --duration SECONDS      Run duration (default: 0 = infinite)
  --analyze               Enable detailed packet-level analysis
  --stats-interval SECONDS Statistics print interval (default: 5)
  --help                  Show help message
```

## Results

Test results are saved to `results/comparison_YYYYMMDD_HHMMSS/`:

- **test_tool.log**: Full test tool output with protocol analysis
- **parser.log**: Full parser output with statistics
- **comparison_report.txt**: Combined report for easy comparison

## Analysis

See `docs/RESULTS_ANALYSIS.md` for interpretation of test results including:
- Duplicate packet ID analysis
- Protocol violation investigation
- Packet order issues
- Recommendations for tool updates

## Troubleshooting

**Tools don't connect:**
- Verify SERVAL is running and configured with both sockets
- Check that ports 8085 and 8086 are listening: `netstat -tlnp | grep 808`
- Ensure firewall rules allow connections

**No data received:**
- Check SERVAL is actively sending data
- Verify connection logs show "Connected"
- Check SERVAL output configuration

**Protocol violations found:**
- See `docs/RESULTS_ANALYSIS.md` for interpretation
- Some violations may be expected (e.g., packet ID resets per frame)
- Compare with SERVAL/SPIDR documentation

## Related Files

- Main parser: `cpp/src/main.cpp`
- TCP server: `cpp/src/tcp_server.cpp`
- Protocol decoder: `cpp/src/tpx3_decoder.cpp`
- Ring buffer: `cpp/src/ring_buffer.cpp`

