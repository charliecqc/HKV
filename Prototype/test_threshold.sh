#!/bin/bash
#
# test_threshold.sh — Validate RECLAIM_THRESHOLD and DIVERGENCE_THRESHOLD impact
#
# Usage: ./test_threshold.sh [threads] [pmem_path]
#   threads:   number of worker threads (default: 16)
#   pmem_path: PMem mount point (default: /mnt/pmem0)
#
# Output: results are written to threshold_results/ directory
#

set -euo pipefail

THREADS=${1:-16}
PMEM_PATH=${2:-/mnt/pmem0}
RESULT_DIR="threshold_results"

mkdir -p "$RESULT_DIR"

# Experiment configurations
# Format: "label RECLAIM_THRESHOLD_BYTES DIVERGENCE_THRESHOLD_VALUE"
CONFIGS=(
    # Vary DIVERGENCE_THRESHOLD with fixed RECLAIM_THRESHOLD=4MB
    "RT4M_DT0.9    4194304  0.9"
    "RT4M_DT1.0    4194304  1.0"
    "RT4M_DT1.1    4194304  1.1"
    "RT4M_DT1.2    4194304  1.2"
    "RT4M_DT1.5    4194304  1.5"
    "RT4M_DT2.0    4194304  2.0"

    # Vary RECLAIM_THRESHOLD with fixed DIVERGENCE_THRESHOLD=1.1
    "RT1M_DT1.1    1048576  1.1"
    "RT2M_DT1.1    2097152  1.1"
    "RT4M_DT1.1    4194304  1.1"
    "RT8M_DT1.1    8388608  1.1"
    "RT16M_DT1.1  16777216  1.1"
)

# Workloads to test: insert-only (write-heavy) + YCSB-A (50/50) + YCSB-C (read-only)
# Format: "workload_type key_dist extra_flags"
WL_TYPES=( "c"  "a"  "c" )
WL_DISTS=( "zipf" "zipf" "zipf" )
WL_EXTRA=( "--insert-only" "" "" )
WORKLOAD_LABELS=( "INSERT_ONLY" "YCSB_A" "YCSB_C" )
NUM_WORKLOADS=${#WL_TYPES[@]}

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SUMMARY="$RESULT_DIR/summary_${TIMESTAMP}.csv"

echo "label,workload,throughput,elapsed_time" > "$SUMMARY"

echo "============================================================"
echo " Threshold Validation Experiments"
echo " Threads: $THREADS | PMem: $PMEM_PATH"
echo " Configs: ${#CONFIGS[@]} | Workloads: $NUM_WORKLOADS"
echo " Total runs: $(( ${#CONFIGS[@]} * NUM_WORKLOADS ))"
echo " Results: $SUMMARY"
echo "============================================================"

run_count=0
total_runs=$(( ${#CONFIGS[@]} * NUM_WORKLOADS ))

for config in "${CONFIGS[@]}"; do
    read -r label rt dt <<< "$config"

    echo ""
    echo ">>> Building with RECLAIM_THRESHOLD=$rt DIVERGENCE_THRESHOLD=$dt ($label)"

    # Rebuild with overridden thresholds
    make clean -s
    make -j"$(nproc)" -s \
        CXXFLAGS_EXTRA="-DRECLAIM_THRESHOLD=$rt -DDIVERGENCE_THRESHOLD=$dt" \
        2>&1 | tail -1

    for i in $(seq 0 $((NUM_WORKLOADS - 1))); do
        wl_type="${WL_TYPES[$i]}"
        wl_dist="${WL_DISTS[$i]}"
        wl_extra="${WL_EXTRA[$i]}"
        wl_label="${WORKLOAD_LABELS[$i]}"
        run_count=$((run_count + 1))

        echo "  [$run_count/$total_runs] $label / $wl_label"

        # Clean PMem state — remove all pool directories
        rm -rf "$PMEM_PATH"/ckpt_log "$PMEM_PATH"/pmemBFPool "$PMEM_PATH"/pmemInodePool "$PMEM_PATH"/pmemVnodePool "$PMEM_PATH"/prism "$PMEM_PATH"/dl "$PMEM_PATH"/sl "$PMEM_PATH"/log 2>/dev/null

        # Run benchmark: ./project <workload> <key_dist> <threads> <pmem_path> [--insert-only]
        outfile="$RESULT_DIR/${label}_${wl_label}_${TIMESTAMP}.log"
        ./project "$wl_type" "$wl_dist" "$THREADS" "$PMEM_PATH" $wl_extra > "$outfile" 2>&1 || true

        # Extract throughput — for INSERT_ONLY, look for "YCSB_INSERT throughput"
        if [[ "$wl_label" == "INSERT_ONLY" ]]; then
            tput=$(grep -oP '(?<=YCSB_INSERT throughput )\S+' "$outfile" || echo "N/A")
        else
            tput=$(grep -oP '(?<=YCSB_[A-F] throughput )\S+' "$outfile" || echo "N/A")
        fi
        etime=$(grep -oP '(?<=Elapsed_time )\S+' "$outfile" | head -1 || echo "N/A")

        echo "    throughput=$tput  elapsed=${etime}s"
        echo "$label,$wl_label,$tput,$etime" >> "$SUMMARY"
    done
done

echo ""
echo "============================================================"
echo " All experiments complete. Summary:"
echo "============================================================"
column -t -s',' "$SUMMARY"
echo ""
echo "Raw logs: $RESULT_DIR/"
echo "CSV:      $SUMMARY"
