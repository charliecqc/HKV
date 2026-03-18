#!/bin/bash
# test_crash_recovery.sh — Measure DT impact on post-crash lookup performance
#
# Design:
#   Phase 1: Insert keys, then kill -9 after CRASH_AFTER seconds (simulate crash)
#   Phase 2: Restart WITHOUT log replay → YCSB_C lookup on recovered partial index
#
# Higher DT → fewer reclaims → fewer inodes in PMem at crash → worse lookup
#
# Usage: bash test_crash_recovery.sh <threads> <pmem_path> [crash_after_sec]

set -uo pipefail

THREADS=${1:?Usage: $0 <threads> <pmem_path> [crash_after_sec]}
PMEM=${2:?Usage: $0 <threads> <pmem_path> [crash_after_sec]}
CRASH_AFTER=${3:-30}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Kill any stale project processes
pkill -9 -f './project' 2>/dev/null || true
sleep 2

RESULT_DIR="$SCRIPT_DIR/threshold_results"
mkdir -p "$RESULT_DIR"
TS=$(date +%Y%m%d_%H%M%S)
CSV="$RESULT_DIR/crash_recovery_${TS}.csv"

echo "DT,CRASH_AFTER_SEC,INODE_COUNT,VNODE_COUNT,YCSB_C_THROUGHPUT,YCSB_C_ELAPSED,L0_INODES,L1_INODES,L2_INODES" > "$CSV"

DT_VALUES=(1.01 1.05 1.1 1.2 1.5 2.0 5.0 10.0)

clean_pmem() {
    rm -f "${PMEM}"/pmemBFPool "${PMEM}"/pmemVnodePool "${PMEM}"/pmemInodePool "${PMEM}"/ckpt_log 2>/dev/null || true
}

run_one() {
    local dt="$1"
    local insert_log="$RESULT_DIR/crash_insert_${dt}_${TS}.log"
    local lookup_log="$RESULT_DIR/crash_lookup_${dt}_${TS}.log"

    > "$insert_log"
    > "$lookup_log"

    echo "=== DT=${dt} ==="

    clean_pmem

    # Build with this DT value
    make clean -s 2>/dev/null
    if ! make -j"$(nproc)" -s \
        ENABLE_DT_ONLY_RECLAIM=1 \
        ENABLE_LOG_REPLAY=0 \
        CXXFLAGS_EXTRA="-DDIVERGENCE_THRESHOLD=${dt} -DPERSISTENT_THRESHOLD=8388608 -g"; then
        echo "  BUILD FAILED"
        echo "${dt},${CRASH_AFTER},BUILD_FAIL,,,,,," >> "$CSV"
        return
    fi

    # --- Phase 1: Insert + Crash ---
    echo "  Phase 1: Insert + crash after ${CRASH_AFTER}s..."
    ./project a zipf "${THREADS}" "${PMEM}" > "$insert_log" 2>&1 &
    local PID=$!

    # Wait for pool creation
    local pool_wait=0
    while ! grep -q "Populating" "$insert_log" 2>/dev/null; do
        if ! kill -0 "$PID" 2>/dev/null; then
            echo "  ERROR: process died during pool creation"
            cat "$insert_log"
            echo "${dt},${CRASH_AFTER},POOL_FAIL,,,,,," >> "$CSV"
            return
        fi
        sleep 2
        pool_wait=$((pool_wait + 2))
        if [ $pool_wait -ge 300 ]; then
            echo "  ERROR: pool creation timeout"
            kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null || true
            echo "${dt},${CRASH_AFTER},POOL_TIMEOUT,,,,,," >> "$CSV"
            return
        fi
    done
    echo "  Pools ready (~${pool_wait}s). Inserting for ${CRASH_AFTER}s..."

    sleep "${CRASH_AFTER}"

    if kill -0 "$PID" 2>/dev/null; then
        kill -9 "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null || true
        echo "  Crashed via kill -9"
    else
        echo "  WARNING: process exited before crash point"
    fi

    if [ ! -f "${PMEM}/pmemInodePool" ]; then
        echo "  ERROR: PMem pools missing"
        echo "${dt},${CRASH_AFTER},NO_POOLS,,,,,," >> "$CSV"
        return
    fi

    # --- Phase 2: Recovery + YCSB_C ---
    echo "  Phase 2: Recovery + YCSB_C..."
    ./project c zipf "${THREADS}" "${PMEM}" > "$lookup_log" 2>&1 &
    local LOOKUP_PID=$!

    # Wait up to 600s for lookup to finish
    local waited=0
    while kill -0 "$LOOKUP_PID" 2>/dev/null; do
        sleep 1
        waited=$((waited + 1))
        if [ $waited -ge 600 ]; then
            echo "  TIMEOUT: lookup didn't finish in 600s"
            kill -9 "$LOOKUP_PID" 2>/dev/null; wait "$LOOKUP_PID" 2>/dev/null || true
            break
        fi
    done
    wait "$LOOKUP_PID" 2>/dev/null || true

    # --- Parse results ---
    local inode_count vnode_count ycsb_c_tput ycsb_c_elapsed l0 l1 l2

    # [Recovery] Copied 19977 inodes from PMem to DRAM
    inode_count=$(grep -oP 'Copied \K\d+' "$lookup_log" | head -1)

    # [Init] pmemVnodePool currentIdx=1909376
    vnode_count=$(grep -oP 'currentIdx=\K\d+' "$lookup_log" | head -1)

    # YCSB_C throughput 1.67897e+07
    ycsb_c_tput=$(grep 'YCSB_C throughput' "$lookup_log" | awk '{print $NF}')

    # Elapsed_time 5.95604
    ycsb_c_elapsed=$(grep 'Elapsed_time' "$lookup_log" | awk '{print $NF}')

    # Level 0 has 19566 inodes.
    l0=$(grep 'Level 0 has' "$lookup_log" | grep -oP '\d+(?= inodes)')
    l1=$(grep 'Level 1 has' "$lookup_log" | grep -oP '\d+(?= inodes)')
    l2=$(grep 'Level 2 has' "$lookup_log" | grep -oP '\d+(?= inodes)')

    inode_count=${inode_count:-0}
    vnode_count=${vnode_count:-0}
    ycsb_c_tput=${ycsb_c_tput:-0}
    ycsb_c_elapsed=${ycsb_c_elapsed:-0}
    l0=${l0:-0}
    l1=${l1:-0}
    l2=${l2:-0}

    # Check if lookup actually produced results
    if [ "$ycsb_c_tput" = "0" ]; then
        echo "  WARNING: No YCSB_C throughput found. Lookup log:"
        tail -20 "$lookup_log"
    else
        echo "  Inodes=${inode_count}, Vnodes=${vnode_count}, YCSB_C=${ycsb_c_tput} ops/s (${ycsb_c_elapsed}s)"
        echo "  L0=${l0}, L1=${l1}, L2=${l2}"
    fi
    echo "${dt},${CRASH_AFTER},${inode_count},${vnode_count},${ycsb_c_tput},${ycsb_c_elapsed},${l0},${l1},${l2}" >> "$CSV"

    clean_pmem
}

# --- Run all experiments ---
total=${#DT_VALUES[@]}
current=0

for dt in "${DT_VALUES[@]}"; do
    current=$((current + 1))
    echo ""
    echo ">>> [${current}/${total}] DT=${dt}, crash after ${CRASH_AFTER}s"
    run_one "$dt"
done

echo ""
echo "========================================="
echo "All experiments complete. Results:"
echo "========================================="
column -t -s',' "$CSV"
echo ""
echo "CSV: $CSV"
