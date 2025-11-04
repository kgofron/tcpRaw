#!/bin/bash
#
# Author: Kazimierz Gofron
#         Oak Ridge National Laboratory
#
# Created:  November 2, 2025
# Modified: November 4, 2025
#
# Quick comparison wrapper - run from project root
# This script calls the main comparison script

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$SCRIPT_DIR/run_comparison.sh" "${1:-60}"
