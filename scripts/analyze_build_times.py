#!/usr/bin/env python3
"""Parse Clang -ftime-trace JSON files and print top-10 slowest compilation units.

Usage:
    python3 scripts/analyze_build_times.py [build_dir]
"""

import json
import os
import sys
from pathlib import Path


def find_time_traces(build_dir: Path) -> list[tuple[Path, Path]]:
    """Find all .cpp source files with corresponding .json time traces."""
    compile_commands = build_dir / "compile_commands.json"
    if not compile_commands.exists():
        print(f"[analyze_build_times] compile_commands.json not found in {build_dir}")
        return []

    with open(compile_commands, encoding="utf-8") as f:
        commands = json.load(f)

    results: list[tuple[Path, Path]] = []
    seen = set()
    for entry in commands:
        output = entry.get("output", "")
        source = entry.get("file", "")
        if not output or not source:
            continue
        trace = Path(output).with_suffix(".json")
        if trace.exists() and str(trace) not in seen:
            seen.add(str(trace))
            results.append((Path(source), trace))
    return results


def parse_trace_duration_sec(trace_path: Path) -> float:
    """Extract total wall time in seconds from a Clang -ftime-trace JSON."""
    try:
        with open(trace_path, encoding="utf-8") as f:
            data = json.load(f)
    except (json.JSONDecodeError, OSError):
        return 0.0

    events = data.get("traceEvents", [])
    max_dur_us = 0
    for event in events:
        name = event.get("name", "")
        # "Total Source" is the most accurate single-TU wall time in newer Clang
        # Fall back to the largest "Total*" event
        if name.startswith("Total"):
            dur_us = event.get("dur", 0)
            if name == "Total Source":
                return dur_us / 1_000_000.0
            if dur_us > max_dur_us:
                max_dur_us = dur_us
    return max_dur_us / 1_000_000.0


def main() -> None:
    build_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build")
    if not build_dir.exists():
        print(f"[analyze_build_times] directory not found: {build_dir}")
        return

    traces = find_time_traces(build_dir)
    if not traces:
        return  # No time-trace files — skip silently (GCC/MSVC build)

    results: list[tuple[Path, float]] = []
    for source, trace in traces:
        dur = parse_trace_duration_sec(trace)
        if dur > 0:
            results.append((source, dur))

    results.sort(key=lambda x: x[1], reverse=True)

    print(f"\n{'=' * 72}")
    print(
        f"  Compile Time Top 10  (total: {len(results)} translation units)"
    )
    print(f"{'=' * 72}")
    for i, (source, dur) in enumerate(results[:10], 1):
        print(f"  {i:2}. {dur:7.2f}s  {source}")
    print(f"{'=' * 72}\n")


if __name__ == "__main__":
    main()
