#!/usr/bin/env python3
"""
visualize_trace.py - Waterfall chart generator for Dapper-Lite traces

Reads a JSON trace (produced by export_trace_json) and renders a
waterfall timeline showing span hierarchy, durations, and critical path.

Usage:
    python3 scripts/visualize_trace.py <trace.json> [output.png]

If output path is omitted, displays the chart interactively.
"""

import json
import sys

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import Rectangle


def load_trace(path):
    """Load and validate a trace JSON file."""
    with open(path) as f:
        trace = json.load(f)

    required = ("trace_id", "spans")
    for key in required:
        if key not in trace:
            raise ValueError(f"Missing required field: {key}")

    return trace


def build_span_index(spans):
    """Build a dict mapping span_id -> span for parent lookups."""
    return {s["span_id"]: s for s in spans}


def calculate_depth(span, index):
    """Calculate nesting depth of a span (root = 0)."""
    depth = 0
    current = span
    while current["parent_span_id"] != "0":
        parent = index.get(current["parent_span_id"])
        if parent is None:
            break
        depth += 1
        current = parent
    return depth


def order_spans_dfs(spans, index):
    """Order spans in DFS order for display (parent before children)."""
    children_map = {}
    roots = []
    for s in spans:
        pid = s["parent_span_id"]
        if pid == "0":
            roots.append(s)
        else:
            children_map.setdefault(pid, []).append(s)

    # Sort children by start_ts for consistent ordering
    for children in children_map.values():
        children.sort(key=lambda s: s["start_ts"])
    roots.sort(key=lambda s: s["start_ts"])

    ordered = []

    def dfs(span):
        ordered.append(span)
        for child in children_map.get(span["span_id"], []):
            dfs(child)

    for root in roots:
        dfs(root)

    return ordered


def visualize_trace(trace, output_path=None):
    """Render a waterfall timeline for the given trace."""
    spans = trace["spans"]
    if not spans:
        print("No spans in trace.")
        return

    critical_path_ids = set(trace.get("critical_path", []))
    index = build_span_index(spans)
    ordered = order_spans_dfs(spans, index)

    # Compute time bounds
    min_ts = min(s["start_ts"] for s in spans)
    max_end = max(s["start_ts"] + s["duration_us"] for s in spans)
    total_duration_ms = (max_end - min_ts) / 1000.0

    # Figure sizing: scale height with span count
    n_spans = len(ordered)
    fig_height = max(3, 0.5 * n_spans + 1.5)
    fig, ax = plt.subplots(figsize=(14, fig_height))

    bar_height = 0.7
    colors = {
        "default": "#4a90d9",
        "critical": "#e74c3c",
    }

    for i, span in enumerate(ordered):
        y = n_spans - 1 - i  # Top-to-bottom ordering
        start_ms = (span["start_ts"] - min_ts) / 1000.0
        dur_ms = span["duration_us"] / 1000.0

        is_critical = span["span_id"] in critical_path_ids
        color = colors["critical"] if is_critical else colors["default"]
        edge = "#c0392b" if is_critical else "#2c3e50"

        rect = Rectangle(
            (start_ms, y - bar_height / 2),
            max(dur_ms, total_duration_ms * 0.003),  # minimum visible width
            bar_height,
            facecolor=color,
            edgecolor=edge,
            linewidth=1.0,
            alpha=0.85,
        )
        ax.add_patch(rect)

        # Label: indented name + duration
        depth = calculate_depth(span, index)
        indent = "  " * depth
        label = f"{indent}{span['name']}"
        dur_label = f"{dur_ms:.1f}ms" if dur_ms >= 1 else f"{span['duration_us']}us"

        ax.text(
            start_ms + dur_ms + total_duration_ms * 0.005,
            y,
            dur_label,
            va="center",
            fontsize=7,
            color="#555555",
        )
        ax.text(
            -total_duration_ms * 0.01,
            y,
            label,
            va="center",
            ha="right",
            fontsize=8,
            fontfamily="monospace",
        )

    # Axis configuration
    ax.set_xlim(-total_duration_ms * 0.01, total_duration_ms * 1.15)
    ax.set_ylim(-0.8, n_spans - 0.2)
    ax.set_xlabel("Time (ms)", fontsize=10)
    ax.set_yticks([])

    trace_id = trace.get("trace_id", "unknown")
    duration_us = trace.get("duration_us", max_end - min_ts)
    ax.set_title(
        f"Trace {trace_id}  |  Duration: {duration_us / 1000.0:.1f}ms  |  "
        f"Spans: {n_spans}",
        fontsize=11,
        fontweight="bold",
    )

    # Legend
    legend_items = [
        mpatches.Patch(facecolor=colors["default"], edgecolor="#2c3e50", label="Span"),
        mpatches.Patch(
            facecolor=colors["critical"], edgecolor="#c0392b", label="Critical Path"
        ),
    ]
    ax.legend(handles=legend_items, loc="upper right", fontsize=8)

    ax.grid(axis="x", alpha=0.3, linestyle="--")
    plt.tight_layout()

    if output_path:
        plt.savefig(output_path, dpi=150, bbox_inches="tight")
        print(f"Saved waterfall chart to {output_path}")
    else:
        plt.show()


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <trace.json> [output.png]")
        sys.exit(1)

    trace_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else None

    trace = load_trace(trace_path)
    visualize_trace(trace, output_path)


if __name__ == "__main__":
    main()
