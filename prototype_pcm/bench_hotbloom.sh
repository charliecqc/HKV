#!/bin/bash
# ============================================================================
# bench_hotbloom.sh — Compare DRAM_BLOOM_FULL vs DRAM_BLOOM_HOT
#
# Tests YCSB-C (100% read) and YCSB-D (95% read + 5% insert) with Zipfian
# distribution to measure search performance impact of selective bloom caching.
#
# Usage:
#   ./bench_hotbloom.sh [threads] [pmem_path]
#   Example: ./bench_hotbloom.sh 16 /mnt/pmem0
# ============================================================================

set -euo pipefail

THREADS=${1:-16}
PMEM_PATH=${2:-/mnt/pmem0}
RESULT_DIR="results/hotbloom_bench"
PROTO_DIR="$(cd "$(dirname "$0")" && pwd)"

# Common flags (everything except the bloom switches)
BASE_FLAGS="-Wall -Iinclude --std=c++17 -O3 -flto=auto -mssse3 -mbmi -mlzcnt -mbmi2"
BASE_FLAGS+=" -DENABLE_CACHE_STATS=0 -DENABLE_THREAD_KEY_CACHE=1"
BASE_FLAGS+=" -DENABLE_L1_TLS_CACHE=0 -DENABLE_PMEM_STATS=0"
BASE_FLAGS+=" -DENABLE_HOTPATH_DEBUG_LOG=0"

# --- helpers ---
clean_pmem() {
    echo "  Cleaning PMEM at ${PMEM_PATH} ..."
    rm -rf "${PMEM_PATH}/ckpt_log" "${PMEM_PATH}/pmemBFPool" \
           "${PMEM_PATH}/pmemInodePool" "${PMEM_PATH}/pmemVnodePool"
}

build_variant() {
    local label="$1"
    local bloom_full="$2"
    local bloom_hot="$3"
    local binary="project_${label}"

    echo "=== Building ${label} ==="
    cd "$PROTO_DIR"

    # Override CXXFLAGS for this variant
    local flags="${BASE_FLAGS} -DENABLE_DRAM_BLOOM_FULL=${bloom_full} -DENABLE_DRAM_BLOOM_HOT=${bloom_hot}"

    make clean -s
    make -j"$(nproc)" -s CXXFLAGS="${flags}" 2>&1
    cp project "${binary}"
    echo "  -> ${binary} built OK"
}

run_bench() {
    local label="$1"
    local workload="$2"
    local dist="$3"
    local binary="project_${label}"
    local outfile="${RESULT_DIR}/${label}_${workload}_${dist}.txt"

    echo "--- Running ${label} / YCSB-${workload} / ${dist} / ${THREADS}T ---"
    clean_pmem

    cd "$PROTO_DIR"
    # Single invocation: insert + search in one process.
    # This ensures the DRAM hot bloom cache built during inserts remains
    # available during the search phase.
    numactl --cpunodebind=0 --membind=0 \
        ./"${binary}" "${workload}" "${dist}" "${THREADS}" "${PMEM_PATH}" \
        > "${outfile}" 2>&1

    echo "  -> Results saved to ${outfile}"
}

extract_metric() {
    local file="$1"
    local pattern="$2"
    grep "${pattern}" "${file}" 2>/dev/null | tail -1 | awk '{print $NF}'
}

# ============================================================================
# Main
# ============================================================================
echo "================================================================"
echo " HotBloomCache Benchmark"
echo " Threads: ${THREADS}    PMEM: ${PMEM_PATH}"
echo "================================================================"

mkdir -p "${RESULT_DIR}"

# --- Step 1: Build both variants ---
build_variant "bloom_full"  1 0
build_variant "bloom_hot"   0 1
# Also build a no-DRAM-bloom variant (direct PMEM access) for reference
build_variant "bloom_pmem"  0 0

echo ""

# --- Step 2: Run benchmarks ---
# YCSB-C (100% read) with Zipfian — pure search performance
for variant in bloom_full bloom_hot bloom_pmem; do
    run_bench "${variant}" c zipf
done

# YCSB-D (95% read + 5% insert) with Zipfian — mixed with hot path detection
for variant in bloom_full bloom_hot bloom_pmem; do
    run_bench "${variant}" d zipf
done

echo ""

# --- Step 3: Summary ---
echo "================================================================"
echo " RESULTS SUMMARY (${THREADS} threads)"
echo "================================================================"
printf "%-14s %-8s %-6s %12s %12s %18s %18s %12s\n" \
    "Variant" "Workload" "Dist" "Insert(ops/s)" "Search(ops/s)" "BloomDRAM" "HotCacheCount" "Evictions"
echo "--------------------------------------------------------------------------------------------"

for dist in zipf; do
    for wl in c d; do
        for variant in bloom_full bloom_hot bloom_pmem; do
            f="${RESULT_DIR}/${variant}_${wl}_${dist}.txt"
            if [[ ! -f "$f" ]]; then continue; fi

            ins_tput=$(extract_metric "$f" "YCSB_INSERT throughput")
            case "${wl}" in
                c) search_tput=$(extract_metric "$f" "YCSB_C throughput") ;;
                d) search_tput=$(extract_metric "$f" "YCSB_D throughput") ;;
                *) search_tput="N/A" ;;
            esac

            bloom_dram=$(grep -o 'bloom\.dram\.alloc=[^ ]*' "$f" 2>/dev/null | tail -1 | cut -d= -f2 || echo "N/A")
            hot_count=$(grep -oP 'cached=\K[0-9]+' "$f" 2>/dev/null | tail -1 || echo "N/A")
            evict_count=$(grep -oP 'evictions=\K[0-9]+' "$f" 2>/dev/null | tail -1 || echo "N/A")

            printf "%-14s %-8s %-6s %12s %12s %18s %18s %12s\n" \
                "${variant}" "YCSB-${wl^^}" "${dist}" \
                "${ins_tput:-N/A}" "${search_tput:-N/A}" \
                "${bloom_dram:-N/A}" "${hot_count:-N/A}" \
                "${evict_count:-N/A}"
        done
    done
done

echo ""
echo "Detailed logs in: ${RESULT_DIR}/"
echo "================================================================"
