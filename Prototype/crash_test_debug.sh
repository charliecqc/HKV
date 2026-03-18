#!/bin/bash
set -e

CRASH_AFTER=${1:-30}
PMEM="/mnt/pmem0"
THREADS=16
BINARY="./project"
INSERT_LOG="/tmp/debug_insert.log"
LOOKUP_LOG="/tmp/debug_lookup.log"

echo "=== Phase 1: Insert + Crash after ${CRASH_AFTER}s ==="

# Start insert
$BINARY a zipf $THREADS $PMEM > "$INSERT_LOG" 2>&1 &
INSERT_PID=$!
echo "Insert PID: $INSERT_PID"

# Wait for pools
echo "Waiting for pools..."
for i in $(seq 1 300); do
    if grep -q "Populating" "$INSERT_LOG" 2>/dev/null; then
        echo "Pools ready after ${i}s"
        break
    fi
    if ! kill -0 $INSERT_PID 2>/dev/null; then
        echo "ERROR: Insert process died before pools were ready"
        cat "$INSERT_LOG"
        exit 1
    fi
    sleep 1
done

echo "Letting insert run for ${CRASH_AFTER} seconds..."
sleep $CRASH_AFTER

# Crash it
kill -9 $INSERT_PID 2>/dev/null || true
wait $INSERT_PID 2>/dev/null || true
echo "Crashed insert process"

echo "--- Insert log tail ---"
tail -5 "$INSERT_LOG"

echo ""
echo "=== Phase 2: Recovery + YCSB_C Lookup ==="

# Run lookup (which triggers recovery)
$BINARY c zipf $THREADS $PMEM > "$LOOKUP_LOG" 2>&1 &
LOOKUP_PID=$!
echo "Lookup PID: $LOOKUP_PID"

# Wait for it to finish (with timeout)
TIMEOUT=600
for i in $(seq 1 $TIMEOUT); do
    if ! kill -0 $LOOKUP_PID 2>/dev/null; then
        LOOKUP_EXIT=$?
        wait $LOOKUP_PID 2>/dev/null
        LOOKUP_EXIT=$?
        echo "Lookup process finished after ${i}s (exit: $LOOKUP_EXIT)"
        break
    fi
    if [ $i -eq $TIMEOUT ]; then
        echo "TIMEOUT: Lookup process didn't finish in ${TIMEOUT}s"
        kill -9 $LOOKUP_PID 2>/dev/null || true
    fi
    sleep 1
done

echo ""
echo "=== Results ==="
echo "--- Lookup log ---"
cat "$LOOKUP_LOG"

# Check dmesg for segfaults
echo ""
echo "--- Recent dmesg (last 5 project entries) ---"
dmesg 2>/dev/null | grep project | tail -5 || echo "(no dmesg access)"
