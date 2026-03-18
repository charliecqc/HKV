#!/bin/bash
#
# test_dt_only.sh — Test DT-only reclaim control vs always-reclaim
#
# Scenario A: RT=64 (极小) → q < RT always false → 无条件 reclaim，DT 无效
# Scenario B: RT=1GB (极大) → q < RT always true  → 仅 DT 控制 reclaim
# Baseline:   RT=4MB (默认) → 正常三条件门控
#
# Usage: ./test_dt_only.sh [threads] [pmem_path]
#

set -euo pipefail

THREADS=${1:-16}
PMEM_PATH=${2:-/mnt/pmem0}
RESULT_DIR="threshold_results"

mkdir -p "$RESULT_DIR"

# RT=1GB (effectively infinite — WAL is 64MB, so q < RT always true)
# This makes the gate: skip reclaim iff threshold <= DT
# DT alone decides when to reclaim.
#
# E_pmem >= E_dram always (PMem is a lagging snapshot of DRAM),
# so threshold = E_pmem/E_dram >= 1.0 always.
# DT < 1.0 degenerates to "always reclaim" — no point testing.
RT_HUGE=1073741824

CONFIGS=(
    # DT >= 1.05: values too close to 1.0 cause WAL starvation (reclaim never runs)
    "DTonly_1.05   $RT_HUGE  1.05"
    "DTonly_1.1    $RT_HUGE  1.1"
    "DTonly_1.2    $RT_HUGE  1.2"
    "DTonly_1.5    $RT_HUGE  1.5"
    "DTonly_2.0    $RT_HUGE  2.0"
    "DTonly_5.0    $RT_HUGE  5.0"
    "DTonly_100.0  $RT_HUGE  100.0"

    # Baseline: normal RT=4MB DT=1.2 for comparison
    "baseline      4194304  1.2"
)

WL_TYPES=( "c"  "a"  "c" )
WL_DISTS=( "zipf" "zipf" "zipf" )
WL_EXTRA=( "--insert-only" "" "" )
WORKLOAD_LABELS=( "INSERT_ONLY" "YCSB_A" "YCSB_C" )
NUM_WORKLOADS=${#WL_TYPES[@]}

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SUMMARY="$RESULT_DIR/dt_only_${TIMESTAMP}.csv"

echo "label,workload,throughput,elapsed_time" > "$SUMMARY"

echo "============================================================"
echo " DT-Only Reclaim Control (RT disabled, DT sweeps)"
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

    make clean -s
    if ! make -j"$(nproc)" -s \
        CXXFLAGS_EXTRA="-DRECLAIM_THRESHOLD=$rt -DDIVERGENCE_THRESHOLD=$dt" \
        2>&1 | tail -1; then
        echo "  *** Build failed for $label, skipping"
        for i in $(seq 0 $((NUM_WORKLOADS - 1))); do
            run_count=$((run_count + 1))
            echo "$label,${WORKLOAD_LABELS[$i]},BUILD_FAIL,0" >> "$SUMMARY"
        done
        continue
    fi

    if [[ ! -x ./project ]]; then
        echo "  *** ./project not found after build, skipping $label"
        for i in $(seq 0 $((NUM_WORKLOADS - 1))); do
            run_count=$((run_count + 1))
            echo "$label,${WORKLOAD_LABELS[$i]},BUILD_FAIL,0" >> "$SUMMARY"
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

        rm -f "$PMEM_PATH"/ckpt_log "$PMEM_PATH"/pmemBFPool "$PMEM_PATH"/pmemInodePool "$PMEM_PATH"/pmemVnodePool "$PMEM_PATH"/prism "$PMEM_PATH"/dl "$PMEM_PATH"/sl "$PMEM_PATH"/log 2>/dev/null
        # Also remove any directory variants
        rm -rf "$PMEM_PATH"/ckpt_log "$PMEM_PATH"/pmemBFPool "$PMEM_PATH"/pmemInodePool "$PMEM_PATH"/pmemVnodePool 2>/dev/null
        sync

        outfile="$RESULT_DIR/${label}_${wl_label}_${TIMESTAMP}.log"
        ./project "$wl_type" "$wl_dist" "$THREADS" "$PMEM_PATH" $wl_extra > "$outfile" 2>&1 || true

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
