#!/bin/bash
# Script to run both tools simultaneously on two different SERVAL TCP sockets
# This allows true parallel comparison of the same data stream
# Run from project root directory

set -e

# Get the script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# SERVAL should be configured with two TCP sockets:
# tcp://listen@localhost:8085  (for real-time parser)
# tcp://listen@localhost:8086  (for test tool)
PARSER_PORT=${PARSER_PORT:-8085}  # Port for real-time parser
TEST_PORT=${TEST_PORT:-8086}      # Port for test tool
TEST_TOOL="$PROJECT_ROOT/cpp/bin/tcp_raw_test"
PARSER_TOOL="$PROJECT_ROOT/cpp/bin/tpx3_parser"
DURATION=${1:-60}  # Default 60 seconds

echo "=== TCP Raw Data Tool Comparison ==="
echo "Duration: ${DURATION} seconds"
echo "SERVAL Configuration Required:"
echo "  Socket 1: tcp://listen@localhost:${PARSER_PORT} (for parser)"
echo "  Socket 2: tcp://listen@localhost:${TEST_PORT} (for test tool)"
echo ""

# Create results directory with timestamp in test/results
RESULTS_DIR="$PROJECT_ROOT/cpp/test/results/comparison_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

TEST_OUTPUT="$RESULTS_DIR/test_tool.log"
PARSER_OUTPUT="$RESULTS_DIR/parser.log"
COMPARISON_REPORT="$RESULTS_DIR/comparison_report.txt"

# Start both tools simultaneously on different ports
echo "Starting test tool on port ${TEST_PORT}..."
cd "$PROJECT_ROOT"
timeout ${DURATION} ${TEST_TOOL} --mode buffer --host 127.0.0.1 --port ${TEST_PORT} --analyze --stats-interval 5 --duration ${DURATION} > "$TEST_OUTPUT" 2>&1 &
TEST_PID=$!

sleep 1

echo "Starting real-time parser on port ${PARSER_PORT}..."
timeout ${DURATION} ${PARSER_TOOL} --host 127.0.0.1 --port ${PARSER_PORT} > "$PARSER_OUTPUT" 2>&1 &
PARSER_PID=$!

echo "Both tools started (PIDs: $TEST_PID, $PARSER_PID)"
echo "Results will be saved to: $RESULTS_DIR"
echo "Waiting for completion..."
echo ""

# Wait for both
wait $TEST_PID 2>/dev/null
TEST_EXIT=$?
wait $PARSER_PID 2>/dev/null  
PARSER_EXIT=$?

echo ""
echo "=== Comparison Results ==="
echo ""

# Extract and compare key statistics
echo "1. Total Bytes/Words Received:"
TEST_BYTES=$(grep "Total bytes:" "$TEST_OUTPUT" | awk '{print $3}' | head -1 || echo "N/A")
TEST_WORDS=$(grep "Total words:" "$TEST_OUTPUT" | awk '{print $3}' | head -1 || echo "N/A")
echo "   Test Tool:  bytes=$TEST_BYTES, words=$TEST_WORDS"

PARSER_OUT=$(cat "$PARSER_OUTPUT" | grep -E "(Total hits|Total chunks)" | head -2 || echo "")
echo "   Parser:"
if [ -n "$PARSER_OUT" ]; then
    echo "$PARSER_OUT" | sed 's/^/     /'
else
    echo "     (no data received)"
fi

echo ""
echo "2. Chunk Counts:"
TEST_CHUNKS=$(grep "Total chunks:" "$TEST_OUTPUT" | awk '{print $3}' | head -1 || echo "N/A")
PARSER_CHUNKS=$(grep "Total chunks:" "$PARSER_OUTPUT" | awk '{print $3}' | head -1 || echo "N/A")
echo "   Test Tool: $TEST_CHUNKS"
echo "   Parser:    $PARSER_CHUNKS"
if [ "$TEST_CHUNKS" != "N/A" ] && [ "$PARSER_CHUNKS" != "N/A" ] && [ "$TEST_CHUNKS" != "$PARSER_CHUNKS" ]; then
    echo "   ⚠️  WARNING: Chunk counts differ!"
fi

echo ""
echo "3. Throughput:"
TEST_RATE=$(grep "Average rate:" "$TEST_OUTPUT" | grep -oE "[0-9.]+ [MG]bps" | head -1 || echo "N/A")
echo "   Test Tool: $TEST_RATE"
PARSER_RATE=$(grep "Total hit rate:" "$PARSER_OUTPUT" | awk '{print $4}' | head -1 || echo "N/A")
if [ "$PARSER_RATE" != "N/A" ]; then
    echo "   Parser:    ~$PARSER_RATE Hz (hit rate)"
fi

echo ""
echo "4. Errors/Violations:"
TEST_VIOLATIONS=$(grep "Protocol violations:" "$TEST_OUTPUT" | awk '{print $3}' | head -1 || echo "N/A")
TEST_ERRORS=$(grep "Invalid packet types:" "$TEST_OUTPUT" | awk '{print $4}' | head -1 || echo "N/A")
echo "   Test Tool:  violations=$TEST_VIOLATIONS, invalid_types=$TEST_ERRORS"

PARSER_ERRORS=$(grep "Total decode errors:" "$PARSER_OUTPUT" | awk '{print $4}' | head -1 || echo "N/A")
PARSER_FRAC=$(grep "Total fractional errors:" "$PARSER_OUTPUT" | awk '{print $4}' | head -1 || echo "N/A")
echo "   Parser:     decode_errors=$PARSER_ERRORS, fractional_errors=$PARSER_FRAC"

echo ""
echo "5. Packet Order Analysis (Test Tool Only):"
MISSING=$(grep "Missing packet IDs:" "$TEST_OUTPUT" | awk '{print $4}' | head -1 || echo "N/A")
DUPES=$(grep "Duplicate packet IDs:" "$TEST_OUTPUT" | awk '{print $4}' | head -1 || echo "N/A")
OUT_OF_ORDER=$(grep "Out-of-order packet IDs:" "$TEST_OUTPUT" | awk '{print $4}' | head -1 || echo "N/A")
echo "   Missing:    $MISSING"
echo "   Duplicates: $DUPES"
echo "   Out-of-order: $OUT_OF_ORDER"

echo ""
echo "=== Results Saved ==="
echo "Results directory: $RESULTS_DIR"
echo "  Test Tool Output:  $TEST_OUTPUT"
echo "  Parser Output:      $PARSER_OUTPUT"
echo "  Comparison Report: $COMPARISON_REPORT"
echo ""
echo "To view results:"
echo "  cat $TEST_OUTPUT"
echo "  cat $PARSER_OUTPUT"
echo "  cat $COMPARISON_REPORT"
echo ""

# Create detailed comparison report
{
    echo "=== Tool Comparison Report ==="
    echo "Generated: $(date)"
    echo "Duration: ${DURATION} seconds"
    echo "SERVAL Sockets:"
    echo "  Parser:    tcp://listen@localhost:${PARSER_PORT}"
    echo "  Test Tool: tcp://listen@localhost:${TEST_PORT}"
    echo ""
    echo "=== Test Tool Statistics ==="
    cat "$TEST_OUTPUT"
    echo ""
    echo "=== Real-Time Parser Statistics ==="
    cat "$PARSER_OUTPUT"
} > "$COMPARISON_REPORT"

echo "Detailed comparison report saved to: $COMPARISON_REPORT"
