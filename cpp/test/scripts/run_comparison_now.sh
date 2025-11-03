#!/bin/bash
# Quick comparison wrapper - run from project root
# This script calls the main comparison script

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$SCRIPT_DIR/run_comparison.sh" "${1:-60}"
