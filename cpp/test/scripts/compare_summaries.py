#!/usr/bin/env python3
"""
Compare two TPX3 parser summary files and highlight deltas for key metrics.

Usage:
    python compare_summaries.py --baseline fileB896G.md --candidate tcpB896G.md
"""

import argparse
import math
import re
from pathlib import Path
from typing import Dict, Tuple

NUMBER_RE = re.compile(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?")


def parse_summary(path: Path) -> Dict[str, object]:
    stats: Dict[str, object] = {
        "total_bytes": None,
        "total_hits": None,
        "total_tdc": None,
        "per_chip_tdc": {},
        "per_chip_tdc_inst": {},
        "per_chip_tdc_cum": {},
        "started_mid_stream": False,
    }

    chip_tdc_line = re.compile(
        r"Chip\s+(\d+):\s+([0-9.]+)\s+Hz instant,\s+([0-9.]+)\s+Hz cumulative \(total:\s+(\d+)\)"
    )
    chip_tdc_legacy_line = re.compile(
        r"Chip\s+(\d+):\s+([0-9.]+)\s+Hz\s+\(total:\s+(\d+)\)"
    )

    with path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if line.startswith("Total bytes processed:"):
                match = NUMBER_RE.search(line.replace(",", ""))
                if match:
                    stats["total_bytes"] = int(float(match.group()))
            elif line.startswith("Total hits:"):
                match = NUMBER_RE.search(line.replace(",", ""))
                if match:
                    stats["total_hits"] = int(float(match.group()))
            elif line.startswith("Total TDC events:"):
                match = NUMBER_RE.search(line.replace(",", ""))
                if match:
                    stats["total_tdc"] = int(float(match.group()))
            elif line.startswith("⚠ Detected data before first chunk header"):
                stats["started_mid_stream"] = True
            else:
                chip_match = chip_tdc_line.search(line)
                if chip_match:
                    chip = int(chip_match.group(1))
                    inst = float(chip_match.group(2))
                    cum = float(chip_match.group(3))
                    total = int(chip_match.group(4))
                    stats["per_chip_tdc"][chip] = total
                    stats["per_chip_tdc_inst"][chip] = inst
                    stats["per_chip_tdc_cum"][chip] = cum
                else:
                    legacy_match = chip_tdc_legacy_line.search(line)
                    if legacy_match:
                        chip = int(legacy_match.group(1))
                        inst = float(legacy_match.group(2))
                        total = int(legacy_match.group(3))
                        stats["per_chip_tdc"][chip] = total
                        stats["per_chip_tdc_inst"][chip] = inst
                        # Legacy summaries do not include cumulative, fallback to instant
                        stats["per_chip_tdc_cum"][chip] = inst
    return stats


def format_delta(baseline: float, candidate: float) -> str:
    if baseline is None or candidate is None:
        return "n/a"
    diff = candidate - baseline
    if baseline == 0:
        pct = math.inf if candidate != 0 else 0.0
    else:
        pct = (diff / baseline) * 100.0
    return f"{candidate:,.0f} (Δ {diff:+,.0f}, {pct:+.1f}%)"


def main() -> None:
    parser = argparse.ArgumentParser(description="Compare TPX3 parser summaries.")
    parser.add_argument("--baseline", required=True, type=Path, help="Reference summary (e.g. file replay)")
    parser.add_argument("--candidate", required=True, type=Path, help="Live stream summary to compare")
    args = parser.parse_args()

    baseline_stats = parse_summary(args.baseline)
    candidate_stats = parse_summary(args.candidate)

    print("=== Summary Comparison ===")
    print(f"Baseline : {args.baseline}")
    print(f"Candidate: {args.candidate}\n")

    print("Totals:")
    print(f"  Bytes : {format_delta(baseline_stats['total_bytes'], candidate_stats['total_bytes'])}")
    print(f"  Hits  : {format_delta(baseline_stats['total_hits'], candidate_stats['total_hits'])}")
    print(f"  TDC   : {format_delta(baseline_stats['total_tdc'], candidate_stats['total_tdc'])}\n")

    print("Per-chip TDC1 totals:")
    chips = sorted(set(baseline_stats["per_chip_tdc"]).union(candidate_stats["per_chip_tdc"]))
    if chips:
        for chip in chips:
            base_val = baseline_stats["per_chip_tdc"].get(chip)
            cand_val = candidate_stats["per_chip_tdc"].get(chip)
            diff_str = format_delta(float(base_val or 0), float(cand_val or 0))
            print(f"  Chip {chip}: {diff_str}")
    else:
        print("  (No per-chip TDC1 totals found)")

    print("\nPer-chip TDC1 cumulative rate (Hz):")
    if chips:
        for chip in chips:
            base_val = baseline_stats["per_chip_tdc_cum"].get(chip)
            cand_val = candidate_stats["per_chip_tdc_cum"].get(chip)
            diff_str = format_delta(base_val if base_val is not None else 0.0,
                                    cand_val if cand_val is not None else 0.0)
            print(f"  Chip {chip}: {diff_str}")
    else:
        print("  (No per-chip rate data found)")

    if candidate_stats["started_mid_stream"]:
        print("\nWarning: Candidate run detected mid-stream attachment (data before chunk header).")

    if candidate_stats["total_bytes"] and baseline_stats["total_bytes"]:
        ratio = candidate_stats["total_bytes"] / baseline_stats["total_bytes"]
        print(f"\nCandidate captured {ratio:.2%} of baseline bytes.")


if __name__ == "__main__":
    main()

