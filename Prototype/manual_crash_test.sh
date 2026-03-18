#!/bin/bash
# manual_crash_test.sh — Single crash recovery test with debug output
# Usage: bash manual_crash_test.sh [crash_after_sec]
set -u

THREADS=16
PMEM=/mnt/pmem0
CRASH_AFTER=${1:-20}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Manual Crash Recovery Test ==="
echo "  CRASH_AFTER=${CRASH_AFTER}s"
echo ""

# Clean
rm -f "${PMEM}/pmemBFPool" "${PMEM}/pmemVnodePool" "${PMEM}/pmemInodePool" "${PMEM}/ckpt_log"

echo "[Phase 1] Starting insert..."
./project c zipf "${THREADS}" "${PMEM}" --insert-only > /tmp/manual_crash_insert.log 2>&1 &
PID=$!
echo "  PID=${PID}"

# Wait for Populating
echo "  Waiting for pool creation..."
WAIT=0
while true; do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "  ERROR: Process exited during pool creation!"
        cat /tmp/manual_crash_insert.log
        exit 1
    fi
    if grep -q "Populating" /tmp/manual_crash_insert.log 2>/dev/null; then
        break
    fi
    sleep 2
    WAIT=$((WAIT + 2))
    if [ "$WAIT" -ge 300 ]; then
        echo "  ERROR: Timeout waiting for pool creation"
        kill -9 "$PID" 2>/dev/null
        exit 1
    fi
done
echo "  Pools created (~${WAIT}s). Insert phase running..."

# Wait for actual inserts
echo "  Sleeping ${CRASH_AFTER}s for inserts..."
sleep "${CRASH_AFTER}"

# Crash
if kill -0 "$PID" 2>/dev/null; then
    kill -9 "$PID"
    wait "$PID" 2>/dev/null
    echo "  Crashed (kill -9) at ${CRASH_AFTER}s after insert"
else
    echo "  WARNING: Process already exited (insert completed before crash?)"
    tail -5 /tmp/manual_crash_insert.log
fi

echo ""
echo "[Pool status after crash]"
ls -lh "${PMEM}/"

# Check pools exist
if [ ! -f "${PMEM}/pmemInodePool" ]; then
    echo "  ERROR: pmemInodePool missing!"
    exit 1
fi

echo ""
echo "[Phase 2] Recovery + YCSB_C lookup..."
./project c zipf "${THREADS}" "${PMEM}" > /tmp/manual_crash_lookup.log 2>&1
RET=$?
echo ""
echo "[Recovery output]"
grep -E "Recovery|superNode|inode_copy|total_recovery|YCSB_C throughput|Elapsed_time|Reclaim.*exec|MemoryUsage.*inode|Level [0-9]" /tmp/manual_crash_lookup.log || true
echo ""
echo "Exit code: ${RET}"
echo "Full insert log: /tmp/manual_crash_insert.log"
echo "Full lookup log: /tmp/manual_crash_lookup.log"
