#!/bin/bash
# Shell script to run TPX3 parser with common options
# Usage: ./run_parser.sh [options]

# Get script directory and project root
# Script is at: cpp/test/scripts/run_parser.sh
# Project root is at: tcpRaw/ (parent of cpp/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PARSER="$PROJECT_ROOT/cpp/bin/tpx3_parser"

# Default options
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8085}"
STATS_TIME="${STATS_TIME:-10}"
STATS_INTERVAL="${STATS_INTERVAL:-0}"  # 0 = disable packet-based stats
REORDER="${REORDER:-false}"
REORDER_WINDOW="${REORDER_WINDOW:-1000}"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --host)
            HOST="$2"
            shift 2
            ;;
        --port)
            PORT="$2"
            shift 2
            ;;
        --stats-time)
            STATS_TIME="$2"
            shift 2
            ;;
        --stats-interval)
            STATS_INTERVAL="$2"
            shift 2
            ;;
        --stats-final-only)
            STATS_FINAL_ONLY=true
            shift
            ;;
        --stats-disable)
            STATS_DISABLE=true
            shift
            ;;
        --reorder)
            REORDER=true
            shift
            ;;
        --reorder-window)
            REORDER_WINDOW="$2"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --host HOST           TCP server host (default: 127.0.0.1)"
            echo "  --port PORT           TCP server port (default: 8085)"
            echo "  --stats-time N        Print status every N seconds (default: 10, 0=disable)"
            echo "  --stats-interval N    Print stats every N packets (default: 0=disable)"
            echo "  --stats-final-only    Only print final statistics"
            echo "  --stats-disable       Disable all statistics printing"
            echo "  --reorder             Enable packet reordering"
            echo "  --reorder-window N   Reorder buffer window size (default: 1000)"
            echo "  --help                Show this help message"
            echo ""
            echo "Environment variables:"
            echo "  HOST, PORT, STATS_TIME, STATS_INTERVAL, REORDER, REORDER_WINDOW"
            echo ""
            echo "Examples:"
            echo "  $0 --stats-time 10"
            echo "  $0 --stats-final-only --reorder"
            echo "  $0 --stats-disable --host 127.0.0.1 --port 8085"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Build command
CMD="$PARSER --host $HOST --port $PORT"

# Add statistics options
if [[ "${STATS_DISABLE:-false}" == "true" ]]; then
    CMD="$CMD --stats-disable"
elif [[ "${STATS_FINAL_ONLY:-false}" == "true" ]]; then
    CMD="$CMD --stats-final-only"
else
    # Always pass stats-interval to override parser default (1000)
    # If user wants packet-based stats, they must explicitly set --stats-interval
    if [[ "$STATS_INTERVAL" != "0" ]]; then
        CMD="$CMD --stats-interval $STATS_INTERVAL"
    else
        # Explicitly disable packet-based stats
        CMD="$CMD --stats-interval 0"
    fi
    if [[ "$STATS_TIME" != "0" ]]; then
        CMD="$CMD --stats-time $STATS_TIME"
    fi
fi

# Add reordering options
if [[ "$REORDER" == "true" ]]; then
    CMD="$CMD --reorder --reorder-window $REORDER_WINDOW"
fi

# Check if parser exists
if [[ ! -f "$PARSER" ]]; then
    echo "Error: Parser not found at $PARSER"
    echo "Please build the parser first: cd $PROJECT_ROOT/cpp && make"
    exit 1
fi

# Run parser
echo "Running: $CMD"
echo ""
exec $CMD

