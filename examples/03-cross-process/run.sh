#!/bin/bash
# run.sh - Orchestrate cross-process tracing demo

echo "=== Cross-Process Tracing Demo ==="
echo ""
echo "Starting backend service in background..."
./examples/03-cross-process/backend &
BACKEND_PID=$!

# Give backend time to start listening
sleep 1

echo ""
echo "Starting frontend service..."
./examples/03-cross-process/frontend

# Wait for backend to finish
sleep 1
wait $BACKEND_PID

echo ""
echo "=== Demo Complete ==="
