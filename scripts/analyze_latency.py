#!/usr/bin/env python3
"""
analyze_latency.py - Latency analysis for Dapper-Lite traces

Reads one or more JSON trace files and produces:
  1. Per-service (span name) latency statistics
  2. Latency amplification warnings
  3. Critical path summary

Usage:
    python3 scripts/analyze_latency.py <trace1.json> [trace2.json ...]
    python3 scripts/analyze_latency.py traces/           # directory of JSON files
"""

import json
import math
import os
import sys
from collections import defaultdict


def load_traces(paths):
    """Load traces from file paths or directories."""
    traces = []
    for path in paths:
        if os.path.isdir(path):
            for fname in sorted(os.listdir(path)):
                if fname.endswith(".json"):
                    fpath = os.path.join(path, fname)
                    with open(fpath) as f:
                        traces.append(json.load(f))
        else:
            with open(path) as f:
                traces.append(json.load(f))
    return traces


def percentile(values, p):
    """Compute the p-th percentile of a sorted list."""
    if not values:
        return 0.0
    k = (len(values) - 1) * (p / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return values[int(k)]
    return values[int(f)] * (c - k) + values[int(c)] * (k - f)


def analyze_service_latency(traces):
    """Compute per-span-name latency statistics across all traces."""
    latencies = defaultdict(list)

    for trace in traces:
        for span in trace.get("spans", []):
            name = span["name"]
            dur = span["duration_us"]
            latencies[name].append(dur)

    stats = []
    for name, durations in sorted(latencies.items()):
        durations.sort()
        n = len(durations)
        mean = sum(durations) / n
        stats.append(
            {
                "name": name,
                "count": n,
                "mean_us": mean,
                "min_us": durations[0],
                "max_us": durations[-1],
                "p50_us": percentile(durations, 50),
                "p95_us": percentile(durations, 95),
                "p99_us": percentile(durations, 99),
            }
        )

    return stats


def detect_amplification(traces, threshold=5.0):
    """
    Detect latency amplification: cases where a parent span's duration
    is significantly larger than its slowest child, indicating overhead
    or waiting beyond child execution.

    Also detects fan-out amplification: when a parent calls multiple
    children in parallel and the slowest child dominates.
    """
    warnings = []

    for trace in traces:
        spans = trace.get("spans", [])
        index = {s["span_id"]: s for s in spans}

        # Group children by parent
        children_map = defaultdict(list)
        for s in spans:
            if s["parent_span_id"] != "0":
                children_map[s["parent_span_id"]].append(s)

        for span in spans:
            children = children_map.get(span["span_id"], [])
            if not children:
                continue

            parent_dur = span["duration_us"]
            max_child_dur = max(c["duration_us"] for c in children)

            if max_child_dur > 0 and parent_dur > 0:
                # Check for fan-out: parent much slower than average child
                # due to tail latency in one child
                if len(children) > 1:
                    child_durations = sorted(
                        c["duration_us"] for c in children
                    )
                    median_child = percentile(child_durations, 50)

                    if median_child > 0:
                        tail_ratio = max_child_dur / median_child
                        if tail_ratio > threshold:
                            warnings.append(
                                {
                                    "type": "tail_amplification",
                                    "trace_id": trace.get("trace_id", "?"),
                                    "span": span["name"],
                                    "fan_out": len(children),
                                    "median_child_us": median_child,
                                    "max_child_us": max_child_dur,
                                    "ratio": tail_ratio,
                                }
                            )

                # Check for parent overhead (parent much longer than children)
                child_total = sum(c["duration_us"] for c in children)
                overhead = parent_dur - child_total
                if child_total > 0 and overhead > 0:
                    overhead_ratio = parent_dur / child_total
                    if overhead_ratio > threshold:
                        warnings.append(
                            {
                                "type": "parent_overhead",
                                "trace_id": trace.get("trace_id", "?"),
                                "span": span["name"],
                                "parent_us": parent_dur,
                                "children_total_us": child_total,
                                "overhead_ratio": overhead_ratio,
                            }
                        )

    return warnings


def print_service_stats(stats):
    """Pretty-print service latency statistics."""
    if not stats:
        print("No span data found.")
        return

    # Header
    print(f"{'Span Name':<30} {'Count':>6} {'Mean':>10} {'p50':>10} "
          f"{'p95':>10} {'p99':>10} {'Min':>10} {'Max':>10}")
    print("-" * 118)

    for s in stats:
        def fmt(us):
            if us >= 1000:
                return f"{us / 1000:.1f}ms"
            return f"{us:.0f}us"

        print(
            f"{s['name']:<30} {s['count']:>6} {fmt(s['mean_us']):>10} "
            f"{fmt(s['p50_us']):>10} {fmt(s['p95_us']):>10} "
            f"{fmt(s['p99_us']):>10} {fmt(s['min_us']):>10} "
            f"{fmt(s['max_us']):>10}"
        )


def print_amplification_warnings(warnings):
    """Pretty-print latency amplification warnings."""
    if not warnings:
        print("No latency amplification detected.")
        return

    for w in warnings:
        if w["type"] == "tail_amplification":
            print(
                f"  WARNING: Tail amplification in '{w['span']}' "
                f"(trace {w['trace_id']})"
            )
            print(
                f"    Fan-out: {w['fan_out']} children, "
                f"median: {w['median_child_us']:.0f}us, "
                f"max: {w['max_child_us']:.0f}us "
                f"({w['ratio']:.1f}x amplification)"
            )
        elif w["type"] == "parent_overhead":
            print(
                f"  WARNING: Parent overhead in '{w['span']}' "
                f"(trace {w['trace_id']})"
            )
            print(
                f"    Parent: {w['parent_us']:.0f}us, "
                f"children total: {w['children_total_us']:.0f}us "
                f"({w['overhead_ratio']:.1f}x ratio)"
            )
        print()


def print_critical_path_summary(traces):
    """Print critical path info for each trace."""
    for trace in traces:
        trace_id = trace.get("trace_id", "?")
        duration = trace.get("duration_us", 0)
        cpath = trace.get("critical_path", [])
        spans = trace.get("spans", [])
        index = {s["span_id"]: s for s in spans}

        if not cpath:
            continue

        path_names = []
        for sid in cpath:
            s = index.get(sid)
            if s:
                path_names.append(s["name"])

        print(f"  Trace {trace_id} ({duration / 1000.0:.1f}ms):")
        print(f"    Critical path: {' -> '.join(path_names)}")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <trace.json|dir> [...]")
        sys.exit(1)

    traces = load_traces(sys.argv[1:])
    if not traces:
        print("No traces loaded.")
        sys.exit(1)

    total_spans = sum(len(t.get("spans", [])) for t in traces)
    print(f"Loaded {len(traces)} trace(s) with {total_spans} total spans.\n")

    # 1. Service latency statistics
    print("=" * 60)
    print("  Per-Service Latency Statistics")
    print("=" * 60)
    stats = analyze_service_latency(traces)
    print_service_stats(stats)

    # 2. Amplification detection
    print()
    print("=" * 60)
    print("  Latency Amplification Analysis")
    print("=" * 60)
    warnings = detect_amplification(traces)
    print_amplification_warnings(warnings)

    # 3. Critical path summary
    print("=" * 60)
    print("  Critical Path Summary")
    print("=" * 60)
    print_critical_path_summary(traces)


if __name__ == "__main__":
    main()
