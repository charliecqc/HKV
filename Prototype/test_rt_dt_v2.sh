#!/bin/bash
#
# test_rt_dt_v2.sh — Corrected RT & DT performance experiments
#
# Based on corrected WAL analysis:
#   - Per inode split lifecycle: ~16.2 KB WAL (97 DELTA + 18 FULL)
#   - Per 0.01 DT ≈ 375 inode splits ≈ 6 MB WAL
#   - RT=4MB ≈ 253 inode splits ≈ DT=1.007
#   - WAL ring buffer = 64 MB
#
# Experiment groups:
#   Group A: RT sweep with DT disabled (DT=100.0) → isolate pure RT effect
#   Group B: DT sweep with large RT (RT=32MB) → let DT actually control reclaim
#   Group C: DT sweep with small RT (RT=4MB) → verify DT irrelevance
#
# Usage: ./test_rt_dt_v2.sh [threads] [pmem_path]
#

set -euo pipefail

THREADS=${1:-16}
PMEM_PATH=${2:-/mnt/pmem0}
RESULT_DIR="threshold_results"

mkdir -p "$RESULT_DIR"

# RT equivalents (corrected, ~16.2 KB WAL per inode split):
#   RT= 1MB → ~62  inode splits → DT≈1.002
#   RT= 2MB → ~123 inode splits → DT≈1.003
#   RT= 4MB → ~253 inode splits → DT≈1.007
#   RT= 8MB → ~506 inode splits → DT≈1.014
#   RT=16MB → ~1011 inode splits → DT≈1.028
#   RT=32MB → ~2021 inode splits → DT≈1.055
#
# DT equivalents (corrected):
#   DT=1.01 → ~371 splits → ~6.0 MB WAL
#   DT=1.02 → ~744 splits → ~11.8 MB WAL
#   DT=1.05 → ~1864 splits → ~29.5 MB WAL
#   DT=1.10 → ~3745 splits → ~59.2 MB WAL (risky, near 64MB WAL limit!)

CONFIGS=(
    # === Group A: RT sweep, DT disabled (100.0) ===
    # Pure RT effect — DT never triggers
    "A_RT1M      1048576   100.0"    # 1 MB
    "A_RT2M      2097152   100.0"    # 2 MB
    "A_RT4M      4194304   100.0"    # 4 MB
    "A_RT8M      8388608   100.0"    # 8 MB
    "A_RT16M    16777216   100.0"    # 16 MB
    "A_RT32M    33554432   100.0"    # 32 MB

    # === Group B: DT sweep, RT=32MB (large, so DT triggers first) ===
    # RT=32MB ≈ DT=1.055, so DT<1.055 will trigger before RT
    "B_DT1.01   33554432   1.01"    # DT triggers at ~6MB, well before RT=32MB
    "B_DT1.02   33554432   1.02"    # DT triggers at ~12MB
    "B_DT1.05   33554432   1.05"    # DT triggers at ~30MB, just before RT=32MB
    # B_DT100.0 is same as A_RT32M, skip

    # === Group C: DT sweep, RT=4MB (small, should dominate) ===
    # RT=4MB ≈ DT=1.007, so DT≥1.01 should NOT trigger before RT
    # All should give identical results → proves DT is irrelevant at small RT
    "C_DT1.01    4194304   1.01"
    "C_DT1.1     4194304   1.1"
    "C_DT1.2     4194304   1.2"
    # C_DT100.0 is same as A_RT4M, skip
)

# Workloads: insert-heavy and mixed (skip read-only, unaffected by reclaim)
WL_TYPES=(  "c"            "a"    )
WL_DISTS=(  "zipf"         "zipf" )
WL_EXTRA=(  "--insert-only" ""    )
WORKLOAD_LABELS=( "INSERT_ONLY" "YCSB_A" )
NUM_WORKLOADS=${#WL_TYPES[@]}

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SUMMARY="$RESULT_DIR/rt_dt_v2_${TIMESTAMP}.csv"

echo "label,group,workload,RT_bytes,DT,throughput,elapsed_time" > "$SUMMARY"

total_runs=$(( ${#CONFIGS[@]} * NUM_WORKLOADS ))
echo "============================================================"
echo " RT & DT Performance Experiments (v2 — corrected WAL model)"
echo " Threads: $THREADS | PMem: $PMEM_PATH"
echo " Configs: ${#CONFIGS[@]} | Workloads: $NUM_WORKLOADS"
echo " Total runs: $total_runs"
echo " Results: $SUMMARY"
echo "============================================================"

run_count=0

for config in "${CONFIGS[@]}"; do
    read -r label rt dt <<< "$config"
    group="${label%%_*}"    # Extract A, B, or C

    echo ""
    echo ">>> Building: $label (RT=$rt, DT=$dt)"

    make clean -s 2>/dev/null || true
    if ! make -j"$(nproc)" \
        CXXFLAGS_EXTRA="-DRECLAIM_THRESHOLD=$rt -DDIVERGENCE_THRESHOLD=$dt" \
        2>&1; then
        echo "  [BUILD FAILED] Skipping $label"
        for i in $(seq 0 $((NUM_WORKLOADS - 1))); do
            echo "$label,$group,${WORKLOAD_LABELS[$i]},$rt,$dt,BUILD_FAIL,BUILD_FAIL" >> "$SUMMARY"
        done
        continue
    fi

    if [[ ! -x ./project ]]; then
        echo "  [BINARY NOT FOUND] Skipping $label"
        for i in $(seq 0 $((NUM_WORKLOADS - 1))); do
            echo "$label,$group,${WORKLOAD_LABELS[$i]},$rt,$dt,NO_BINARY,NO_BINARY" >> "$SUMMARY"
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
        echo "$label,$group,$wl_label,$rt,$dt,$tput,$etime" >> "$SUMMARY"
    done
done

echo ""
echo "============================================================"
echo " All experiments complete."
echo "============================================================"
echo ""

# Print results grouped by workload
for wl in "${WORKLOAD_LABELS[@]}"; do
    echo "--- $wl ---"
    head -1 "$SUMMARY"
    grep ",$wl," "$SUMMARY" | sort
    echo ""
done

echo "CSV: $SUMMARY"
echo "Logs: $RESULT_DIR/"
