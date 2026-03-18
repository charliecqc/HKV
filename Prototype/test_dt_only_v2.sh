#!/bin/bash
#
# test_dt_only_v2.sh — DT-only reclaim mode experiments
#
# Tests ENABLE_DT_ONLY_RECLAIM=1 with various DT values.
# DT controls the trade-off between write performance and recovery time:
#   - Low DT (1.01): frequent reclaim → fast recovery, more overhead
#   - High DT (1.1+): rare reclaim → slow recovery, less overhead
#
# Also includes baseline (original mode) for comparison.
#
# Usage: ./test_dt_only_v2.sh [threads] [pmem_path]
#

set -euo pipefail

THREADS=${1:-16}
PMEM_PATH=${2:-/mnt/pmem0}
RESULT_DIR="threshold_results"

mkdir -p "$RESULT_DIR"

# Experiment groups:
#   Group DT: DT-only mode (ENABLE_DT_ONLY_RECLAIM=1) with DT sweep
#   Group BL: Baseline (original mode, ENABLE_DT_ONLY_RECLAIM=0)
#
# Format: "label  DT_ONLY_flag  DT_value  extra_cxxflags"
CONFIGS=(
    # === Group DT: DT-only mode ===
    "DTonly_1.005  1  1.005  "
    "DTonly_1.01   1  1.01   "
    "DTonly_1.02   1  1.02   "
    "DTonly_1.05   1  1.05   "
    "DTonly_1.10   1  1.10   "
    "DTonly_1.20   1  1.20   "
    "DTonly_1.50   1  1.50   "
    "DTonly_2.00   1  2.00   "

    # === Group BL: Baseline (original 3-condition gate) ===
    "BL_default    0  1.01   "
    "BL_RT4M       0  1.01   -DRECLAIM_THRESHOLD=4194304"
)

WL_TYPES=(  "c"             "a"    )
WL_DISTS=(  "zipf"          "zipf" )
WL_EXTRA=(  "--insert-only"  ""    )
WORKLOAD_LABELS=( "INSERT_ONLY" "YCSB_A" )
NUM_WORKLOADS=${#WL_TYPES[@]}

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SUMMARY="$RESULT_DIR/dt_only_v2_${TIMESTAMP}.csv"

echo "label,group,workload,DT_ONLY,DT,throughput,elapsed_time" > "$SUMMARY"

total_runs=$(( ${#CONFIGS[@]} * NUM_WORKLOADS ))
echo "============================================================"
echo " DT-Only Reclaim Mode Experiments"
echo " Threads: $THREADS | PMem: $PMEM_PATH"
echo " Configs: ${#CONFIGS[@]} | Workloads: $NUM_WORKLOADS"
echo " Total runs: $total_runs"
echo " Results: $SUMMARY"
echo "============================================================"

run_count=0

for config in "${CONFIGS[@]}"; do
    read -r label dt_only dt extra <<< "$config"
    group="${label%%_*}"

    echo ""
    echo ">>> Building: $label (DT_ONLY=$dt_only, DT=$dt)"

    make clean -s 2>/dev/null || true
    if ! make -j"$(nproc)" \
        ENABLE_DT_ONLY_RECLAIM="$dt_only" \
        CXXFLAGS_EXTRA="-DDIVERGENCE_THRESHOLD=$dt $extra" \
        2>&1; then
        echo "  [BUILD FAILED] Skipping $label"
        for i in $(seq 0 $((NUM_WORKLOADS - 1))); do
            echo "$label,$group,${WORKLOAD_LABELS[$i]},$dt_only,$dt,BUILD_FAIL,BUILD_FAIL" >> "$SUMMARY"
        done
        continue
    fi

    if [[ ! -x ./project ]]; then
        echo "  [BINARY NOT FOUND] Skipping $label"
        for i in $(seq 0 $((NUM_WORKLOADS - 1))); do
            echo "$label,$group,${WORKLOAD_LABELS[$i]},$dt_only,$dt,NO_BINARY,NO_BINARY" >> "$SUMMARY"
        done
        continue
    fi

    for i in $(seq 0 $((NUM_WORKLOADS - 1))); do
        wl_type="${WL_TYPES[$i]}"
        wl_dist="${WL_DISTS[$i]}"
        wl_extra="${WL_EXTRA[$i]}"
        wl_label="${WORKLOAD_LABELS[$i]}"
        run_count=$((run_count + 1))

        echo "  [$run_count/$total_runs] $label / $wl_label"

        # Clean PMem state
        rm -f "$PMEM_PATH"/*.pool 2>/dev/null || true
        rm -rf "$PMEM_PATH"/ckpt_log "$PMEM_PATH"/pmemBFPool \
               "$PMEM_PATH"/pmemInodePool "$PMEM_PATH"/pmemVnodePool \
               "$PMEM_PATH"/prism "$PMEM_PATH"/dl "$PMEM_PATH"/sl \
               "$PMEM_PATH"/log 2>/dev/null || true
        sync

        outfile="$RESULT_DIR/${label}_${wl_label}_${TIMESTAMP}.log"
        timeout 600 ./project "$wl_type" "$wl_dist" "$THREADS" "$PMEM_PATH" $wl_extra \
            > "$outfile" 2>&1 || true

        # Extract throughput
        if [[ "$wl_label" == "INSERT_ONLY" ]]; then
            tput=$(grep -oP '(?<=YCSB_INSERT throughput )\S+' "$outfile" 2>/dev/null || echo "N/A")
        else
            tput=$(grep -oP '(?<=YCSB_[A-F] throughput )\S+' "$outfile" 2>/dev/null || echo "N/A")
        fi
        etime=$(grep -oP '(?<=Elapsed_time )\S+' "$outfile" 2>/dev/null | head -1 || echo "N/A")

        echo "    throughput=$tput  elapsed=${etime}s"
        echo "$label,$group,$wl_label,$dt_only,$dt,$tput,$etime" >> "$SUMMARY"
    done
done

echo ""
echo "============================================================"
echo " All experiments complete."
echo "============================================================"
echo ""

for wl in "${WORKLOAD_LABELS[@]}"; do
    echo "--- $wl ---"
    head -1 "$SUMMARY"
    grep ",$wl," "$SUMMARY" | sort
    echo ""
done

echo "CSV: $SUMMARY"
echo "Logs: $RESULT_DIR/"
