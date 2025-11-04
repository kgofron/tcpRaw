#!/bin/bash
#
# Author: Kazimierz Gofron
#         Oak Ridge National Laboratory
#
# Created:  November 2, 2025
# Modified: November 4, 2025
#
# Script to run both tools simultaneously and compare their statistics
# This uses a TCP stream duplicator to feed the same data to both tools

set -e

PORT=8085
TEST_TOOL="./cpp/bin/tcp_raw_test"
PARSER_TOOL="./cpp/bin/tpx3_parser"
DURATION=${1:-60}  # Default 60 seconds

echo "=== TCP Raw Data Tool Comparison ==="
echo "Duration: ${DURATION} seconds"
echo ""

# Create a temporary directory for outputs
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

TEST_OUTPUT="$TMPDIR/test_tool.log"
PARSER_OUTPUT="$TMPDIR/parser.log"
COMPARISON="$TMPDIR/comparison.txt"

echo "Output files:"
echo "  Test tool: $TEST_OUTPUT"
echo "  Parser: $PARSER_OUTPUT"
echo "  Comparison: $COMPARISON"
echo ""

# Function to extract statistics from test tool output
extract_test_stats() {
    local file=$1
    grep -E "(Total bytes|Total words|Total chunks|Average rate|Peak rate|Protocol violations|Missing packet IDs|Duplicate packet IDs|Out-of-order packet IDs|Invalid packet types|Buffer overruns)" "$file" || echo "No statistics found"
}

# Function to extract statistics from parser output
extract_parser_stats() {
    local file=$1
    grep -E "(Total hits|Total chunks|Total TDC events|Total decode errors|Total hit rate|Per-chip hit rates)" "$file" || echo "No statistics found"
}

echo "Starting tools..."
echo "Note: If the server only accepts one connection, only one tool will connect."
echo "In that case, run them sequentially or use a TCP stream duplicator."
echo ""

# Start test tool in background
echo "Starting test tool..."
timeout ${DURATION} ${TEST_TOOL} --mode buffer --analyze --stats-interval 5 --duration ${DURATION} > "$TEST_OUTPUT" 2>&1 &
TEST_PID=$!

# Start parser in background
echo "Starting real-time parser..."
timeout ${DURATION} ${PARSER_TOOL} > "$PARSER_OUTPUT" 2>&1 &
PARSER_PID=$!

echo "Both tools started (PIDs: $TEST_PID, $PARSER_PID)"
echo "Waiting for completion or timeout..."
echo ""

# Wait for both processes
wait $TEST_PID 2>/dev/null
TEST_EXIT=$?
wait $PARSER_PID 2>/dev/null
PARSER_EXIT=$?

echo ""
echo "=== Test Tool Statistics ==="
extract_test_stats "$TEST_OUTPUT"
echo ""

echo "=== Real-Time Parser Statistics ==="
extract_parser_stats "$PARSER_OUTPUT"
echo ""

# Create comparison report
{
    echo "=== Tool Comparison Report ==="
    echo "Generated: $(date)"
    echo ""
    echo "=== Test Tool Output ==="
    extract_test_stats "$TEST_OUTPUT"
    echo ""
    echo "=== Real-Time Parser Output ==="
    extract_parser_stats "$PARSER_OUTPUT"
    echo ""
    echo "=== Full Test Tool Output ==="
    cat "$TEST_OUTPUT"
    echo ""
    echo "=== Full Parser Output ==="
    cat "$PARSER_OUTPUT"
} > "$COMPARISON"

echo "=== Comparison Report ==="
echo "Full comparison saved to: $COMPARISON"
echo ""
echo "Key Metrics to Compare:"
echo "1. Total bytes/words received"
echo "2. Chunk counts"
echo "3. Packet type distributions"
echo "4. Error/violation counts"
echo "5. Per-chip statistics"
echo ""

# Show key differences
echo "=== Quick Differences ==="
TEST_CHUNKS=$(grep "Total chunks:" "$TEST_OUTPUT" | awk '{print $3}' | head -1 || echo "0")
PARSER_CHUNKS=$(grep "Total chunks:" "$PARSER_OUTPUT" | awk '{print $3}' | head -1 || echo "0")

if [ -n "$TEST_CHUNKS" ] && [ -n "$PARSER_CHUNKS" ] && [ "$TEST_CHUNKS" != "0" ] && [ "$PARSER_CHUNKS" != "0" ]; then
    echo "Chunk count - Test tool: $TEST_CHUNKS, Parser: $PARSER_CHUNKS"
    if [ "$TEST_CHUNKS" != "$PARSER_CHUNKS" ]; then
        echo "  WARNING: Chunk counts differ!"
    fi
fi

TEST_WORDS=$(grep "Total words:" "$TEST_OUTPUT" | awk '{print $3}' | head -1 || echo "0")
if [ -n "$TEST_WORDS" ] && [ "$TEST_WORDS" != "0" ]; then
    echo "Test tool words: $TEST_WORDS"
fi

PARSER_HITS=$(grep "Total hits:" "$PARSER_OUTPUT" | awk '{print $3}' | head -1 || echo "0")
if [ -n "$PARSER_HITS" ] && [ "$PARSER_HITS" != "0" ]; then
    echo "Parser hits: $PARSER_HITS"
fi

echo ""
echo "For detailed comparison, see: $COMPARISON"

