#!/bin/bash
# test_dt_sweep.sh — Measure DT impact on throughput + forceReclaim time
# Usage: bash test_dt_sweep.sh <threads> <pmem_path>
# Example: bash test_dt_sweep.sh 16 /mnt/pmem0

set -euo pipefail

THREADS=${1:?Usage: $0 <threads> <pmem_path>}
PMEM=${2:?Usage: $0 <threads> <pmem_path>}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

RESULT_DIR="$SCRIPT_DIR/threshold_results"
mkdir -p "$RESULT_DIR"
TS=$(date +%Y%m%d_%H%M%S)
CSV="$RESULT_DIR/dt_sweep_${TS}.csv"

echo "DT,WORKLOAD,THROUGHPUT,RECLAIM_COUNT,FORCE_RECLAIM_MS_MERGE,FORCE_RECLAIM_MS_TANDEM,ELAPSED_TIME" > "$CSV"

# DT values to sweep
DT_VALUES=(1.01 1.05 1.1 1.2 1.5 2.0 5.0 10.0)

# Workloads: insert-only and YCSB-A
WORKLOADS=("INSERT_ONLY" "YCSB_A")

clean_pmem() {
    rm -rf "${PMEM}"/prism/* "${PMEM}"/dl "${PMEM}"/sl "${PMEM}"/log 2>/dev/null || true
    rm -f "${PMEM}"/pmemBFPool "${PMEM}"/pmemVnodePool "${PMEM}"/pmemInodePool "${PMEM}"/ckpt_log 2>/dev/null || true
}

run_one() {
    local dt="$1"
    local wl="$2"
    local logfile="$RESULT_DIR/dt_sweep_${dt}_${wl}_${TS}.log"

    echo "=== DT=${dt} WORKLOAD=${wl} ==="
    clean_pmem

    # Build
    make clean -s
    if ! make -j"$(nproc)" -s \
        ENABLE_DT_ONLY_RECLAIM=1 \
        CXXFLAGS_EXTRA="-DDIVERGENCE_THRESHOLD=${dt}"; then
        echo "  BUILD FAILED"
        echo "${dt},${wl},BUILD_FAIL,,,," >> "$CSV"
        return
    fi

    # Run
    local cmd
    if [ "$wl" = "INSERT_ONLY" ]; then
        cmd="./project c zipf ${THREADS} ${PMEM} --insert-only"
    else
        cmd="./project a zipf ${THREADS} ${PMEM}"
    fi

    echo "  Running: $cmd"
    if ! $cmd > "$logfile" 2>&1; then
        echo "  RUN FAILED (exit $?)"
        echo "${dt},${wl},RUN_FAIL,,,," >> "$CSV"
        return
    fi

    # Parse results
    local tput reclaim_count fr_merge fr_tandem elapsed

    if [ "$wl" = "INSERT_ONLY" ]; then
        tput=$(grep "YCSB_INSERT throughput" "$logfile" | awk '{print $NF}' | tail -1 || true)
    else
        tput=$(grep "YCSB_A throughput" "$logfile" | awk '{print $NF}' | tail -1 || true)
    fi

    reclaim_count=$(grep '\[Reclaim\] exec count:' "$logfile" | awk '{print $NF}' | tail -1 || true)
    fr_merge=$(grep '\[~LogMergeThread\] forceReclaim time:' "$logfile" | awk '{print $(NF-1)}' | tail -1 || true)
    fr_tandem=$(grep '\[~TandemIndex\] forceReclaim time:' "$logfile" | awk '{print $(NF-1)}' | tail -1 || true)
    elapsed=$(grep 'Elapsed_time' "$logfile" | awk '{print $NF}' | tail -1 || true)

    # Default to 0 if not found
    tput=${tput:-0}
    reclaim_count=${reclaim_count:-0}
    fr_merge=${fr_merge:-0}
    fr_tandem=${fr_tandem:-0}
    elapsed=${elapsed:-0}

    echo "  Throughput=${tput}, ReclaimCount=${reclaim_count}, FR_merge=${fr_merge}ms, FR_tandem=${fr_tandem}ms"
    echo "${dt},${wl},${tput},${reclaim_count},${fr_merge},${fr_tandem},${elapsed}" >> "$CSV"
}

# Run all experiments
total=${#DT_VALUES[@]}
total=$((total * ${#WORKLOADS[@]}))
current=0

for dt in "${DT_VALUES[@]}"; do
    for wl in "${WORKLOADS[@]}"; do
        current=$((current + 1))
        echo ""
        echo ">>> [${current}/${total}] DT=${dt} WL=${wl}"
        run_one "$dt" "$wl"
    done
done

echo ""
echo "========================================="
echo "All experiments complete. Results:"
echo "========================================="
column -t -s',' "$CSV"
echo ""
echo "CSV: $CSV"
