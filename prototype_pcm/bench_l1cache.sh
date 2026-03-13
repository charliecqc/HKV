#!/bin/bash
# bench_l1cache.sh
# Benchmark ENABLE_L1_TLS_CACHE=0 vs =1
# Usage: ./bench_l1cache.sh [threads] [pmem_path]
# Example: ./bench_l1cache.sh 16 /mnt/pmem0

set -euo pipefail

THREADS=${1:-16}
PMEM_PATH=${2:-/mnt/pmem0}
WORKLOADS=("a" "c")
KEY_TYPES=("zipf" "unif")
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULT_FILE="$SCRIPT_DIR/l1cache_bench_result.txt"

BASE_FLAGS="-Wall -Iinclude --std=c++17 -O3 -flto=auto -mssse3 -mbmi -mlzcnt -mbmi2 \
-DENABLE_CACHE_STATS=0 \
-DENABLE_THREAD_KEY_CACHE=1 -DENABLE_L2_SHARD_CACHE=0 -DENABLE_PMEM_STATS=0"

NUMACTL="numactl --cpunodebind=0 --membind=0"
BINARY="$SCRIPT_DIR/project"

# ──────────────────────────────────────────────
# helpers
# ──────────────────────────────────────────────
clear_pmem() {
    rm -rf "$PMEM_PATH"/prism "$PMEM_PATH"/dl \
           "$PMEM_PATH"/sl    "$PMEM_PATH"/log \
           "$PMEM_PATH"/ckpt_log "$PMEM_PATH"/pmemBFPool \
           "$PMEM_PATH"/pmemInodePool "$PMEM_PATH"/pmemVnodePool \
           2>/dev/null || true
}

build_with_flag() {
    local flag_val=$1
    echo "[BUILD] ENABLE_L1_TLS_CACHE=$flag_val ..."
    sed -i "s/^CXXFLAGS = .*/CXXFLAGS = $BASE_FLAGS -DENABLE_L1_TLS_CACHE=$flag_val/" \
        "$SCRIPT_DIR/Makefile"
    make -C "$SCRIPT_DIR" clean -s
    make -C "$SCRIPT_DIR" -j"$(nproc)" 2>/dev/null
    echo "[BUILD] done"
}

run_bench() {
    local wl=$1 kt=$2
    local out
    # clear pmem before each run
    clear_pmem
    out=$($NUMACTL "$BINARY" "$wl" "$kt" "$THREADS" "$PMEM_PATH" 2>/dev/null)
    # extract the throughput line matching YCSB_<WL> throughput <num>
    local wl_upper
    wl_upper=$(echo "$wl" | tr '[:lower:]' '[:upper:]')
    echo "$out" | grep -i "YCSB_${wl_upper} throughput\|YCSB_INSERT throughput" | \
        awk '{print $NF}' | head -1
}

# ──────────────────────────────────────────────
# main
# ──────────────────────────────────────────────
echo "==========================================" | tee "$RESULT_FILE"
echo " ENABLE_L1_TLS_CACHE Performance Benchmark" | tee -a "$RESULT_FILE"
echo " Threads: $THREADS   PMem: $PMEM_PATH" | tee -a "$RESULT_FILE"
echo "==========================================" | tee -a "$RESULT_FILE"
printf "%-20s %-10s %-15s %-15s %-10s\n" \
    "Workload" "KeyType" "L1_CACHE=0" "L1_CACHE=1" "Speedup" | tee -a "$RESULT_FILE"
printf "%-20s %-10s %-15s %-15s %-10s\n" \
    "--------" "-------" "----------" "----------" "-------" | tee -a "$RESULT_FILE"

declare -A results_0
declare -A results_1

for flag in 0 1; do
    build_with_flag "$flag"
    for wl in "${WORKLOADS[@]}"; do
        for kt in "${KEY_TYPES[@]}"; do
            key="${wl}_${kt}"
            echo -n "[RUN] wl=$wl kt=$kt L1=$flag ... "
            tput=$(run_bench "$wl" "$kt") || tput="ERROR"
            echo "$tput"
            if [ "$flag" -eq 0 ]; then
                results_0["$key"]="$tput"
            else
                results_1["$key"]="$tput"
            fi
        done
    done
done

echo "" | tee -a "$RESULT_FILE"
echo "--- Results ---" | tee -a "$RESULT_FILE"
printf "%-20s %-10s %-15s %-15s %-10s\n" \
    "Workload" "KeyType" "L1_CACHE=0" "L1_CACHE=1" "Speedup" | tee -a "$RESULT_FILE"
printf "%-20s %-10s %-15s %-15s %-10s\n" \
    "--------" "-------" "----------" "----------" "-------" | tee -a "$RESULT_FILE"

for wl in "${WORKLOADS[@]}"; do
    for kt in "${KEY_TYPES[@]}"; do
        key="${wl}_${kt}"
        v0="${results_0[$key]:-N/A}"
        v1="${results_1[$key]:-N/A}"
        speedup="N/A"
        if [[ -n "$v0" && -n "$v1" && "$v0" != "N/A" && "$v1" != "N/A" ]]; then
            speedup=$(awk "BEGIN{printf \"%.3fx\", $v1 / $v0}")
        fi
        printf "%-20s %-10s %-15s %-15s %-10s\n" \
            "YCSB_$(echo "$wl" | tr '[:lower:]' '[:upper:]')" "$kt" "$v0" "$v1" "$speedup" \
            | tee -a "$RESULT_FILE"
    done
done

echo "" | tee -a "$RESULT_FILE"
echo "Results saved to: $RESULT_FILE"

# Restore Makefile to L1_CACHE=1 (user's current setting)
build_with_flag 1
echo "[INFO] Makefile restored to ENABLE_L1_TLS_CACHE=1"
