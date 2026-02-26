#!/bin/bash
# run.sh - Orchestrate the full-system tracing demo
#
# Starts: collector -> database -> middleware -> frontend
# Then runs visualization and analysis scripts on the output.
#
# Usage:
#   ./examples/05-full-system/run.sh
#   # or from build dir:
#   make run-full-system

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN_DIR="$PROJECT_DIR/build/bin"

# Clean up previous run
rm -f traces.log
rm -rf traces/

echo "=========================================="
echo "  Dapper-Lite Full System Demo"
echo "=========================================="
echo ""

# Track PIDs for cleanup
PIDS=()

cleanup() {
    echo ""
    echo "Cleaning up..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null
    echo "Done."
}
trap cleanup EXIT

# 1. Start collector
echo "[1/4] Starting collector daemon..."
"$BIN_DIR/collector" &
PIDS+=($!)
sleep 1

# 2. Start database service
echo "[2/4] Starting database service..."
"$BIN_DIR/example5-database" &
PIDS+=($!)
sleep 1

# 3. Start middleware service
echo "[3/4] Starting middleware service..."
"$BIN_DIR/example5-middleware" &
PIDS+=($!)
sleep 1

# 4. Run frontend (sends requests, then waits for collector flush)
echo "[4/4] Running frontend (sending requests)..."
echo ""
"$BIN_DIR/example5-frontend"

echo ""
echo "=========================================="
echo "  Trace Analysis"
echo "=========================================="
echo ""

# Run analysis on exported traces
if [ -d "traces" ] && ls traces/*.json >/dev/null 2>&1; then
    echo "--- Latency Analysis ---"
    python3 "$PROJECT_DIR/scripts/analyze_latency.py" traces/
    echo ""

    # Generate waterfall for the first trace
    FIRST_TRACE=$(ls traces/*.json | head -1)
    if [ -n "$FIRST_TRACE" ] && command -v python3 >/dev/null 2>&1; then
        # Check if matplotlib is available
        if python3 -c "import matplotlib" 2>/dev/null; then
            echo "--- Generating Waterfall Chart ---"
            python3 "$PROJECT_DIR/scripts/visualize_trace.py" "$FIRST_TRACE" traces/waterfall.png
            echo ""
        else
            echo "Note: Install matplotlib for waterfall charts:"
            echo "  pip3 install matplotlib"
            echo ""
        fi
    fi
else
    echo "No trace JSON files found. The collector may need more time."
fi

echo "=========================================="
echo "  Demo Complete"
echo "=========================================="
