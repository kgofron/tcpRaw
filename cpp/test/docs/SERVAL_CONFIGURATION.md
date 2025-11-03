# SERVAL Configuration for Dual Socket Comparison

## Overview

To run both the test tool and real-time parser simultaneously on the same data stream, SERVAL needs to be configured with **two TCP sockets**. This allows each tool to connect independently and receive the same data stream.

## Required SERVAL Configuration

Configure SERVAL with two TCP socket listeners:

1. **Socket 1 (Parser)**: `tcp://listen@localhost:8085`
   - Used by: `tpx3_parser` (real-time parser)
   
2. **Socket 2 (Test Tool)**: `tcp://listen@localhost:8086`
   - Used by: `tcp_raw_test` (analysis tool)

## Configuration Example

In your SERVAL configuration file, add both sockets:

```yaml
# SERVAL Configuration
outputs:
  - type: tcp
    address: tcp://listen@localhost:8085
    name: parser_socket
  - type: tcp
    address: tcp://listen@localhost:8086
    name: test_tool_socket
```

Or if using command-line/SERVAL interface:
- `tcp://listen@localhost:8085` (for parser)
- `tcp://listen@localhost:8086` (for test tool)

## Running the Comparison

Once SERVAL is configured with both sockets:

```bash
# Run the comparison script
./run_comparison.sh 60

# Or run tools manually:
# Terminal 1: Test tool (connects to port 8086)
./cpp/bin/tcp_raw_test --port 8086 --analyze --stats-interval 5

# Terminal 2: Parser (connects to port 8085)
./cpp/bin/tpx3_parser --port 8085
```

## Port Configuration

Both tools support configurable ports:

- **Test Tool**: `./cpp/bin/tcp_raw_test --port 8086 --host 127.0.0.1`
- **Parser**: `./cpp/bin/tpx3_parser --port 8085 --host 127.0.0.1`

The default ports are:
- Test tool: 8086 (configurable)
- Parser: 8085 (configurable)

## Verification

After configuring SERVAL:

1. Check that both ports are listening:
   ```bash
   netstat -tlnp | grep -E "808[56]"
   # or
   ss -tlnp | grep -E "808[56]"
   ```

2. Run the comparison script:
   ```bash
   ./run_comparison.sh 30
   ```

3. Both tools should connect and receive data simultaneously.

## Benefits of Dual Socket Configuration

- **True parallel analysis**: Both tools receive the same data stream
- **Independent processing**: No interference between tools
- **Accurate comparison**: Same data = fair comparison
- **No data loss**: Each tool has its own connection

## Troubleshooting

If only one tool connects:
- Check SERVAL configuration has both sockets enabled
- Verify both ports are listening (use `netstat` or `ss`)
- Check firewall rules for both ports
- Ensure SERVAL is configured to allow multiple connections

If tools don't receive data:
- Verify SERVAL is actively sending data
- Check connection logs in both tools
- Ensure SERVAL output format matches expected TPX3 format

