#!/bin/bash
###############################################################################
# run_sgp_table.sh  –  Benchmark 5 configurations × 2 workloads
#
# Configurations:
#   1. SPECTRUMKV                          (SGP=1, FLUSH=0, coeff=3)
#   2. SPECTRUMKV + NO SGP                 (SGP=0, FLUSH=0, coeff=3)
#   3. SPECTRUMKV + IMMEDIATE PERSIST      (SGP=1, FLUSH=1, coeff=3)
#   4. SPECTRUMKV + NO SGP + IMM PERSIST   (SGP=0, FLUSH=1, coeff=3)
#   5. SPECTRUMKV + COEFFICIENT=1          (SGP=1, FLUSH=0, coeff=1)
#
# Workloads: insert-only,  workload A (50% read / 50% update)
###############################################################################
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")" && pwd)"
PMEM_DIR="/mnt/pmem0"
THREADS=16
DIST="zipf"
RUNS=3                          # trials per (config, workload) pair

MAKEFILE="$PROJ_DIR/Makefile"
COMMON_H="$PROJ_DIR/include/common.h"
RESULT_FILE="$PROJ_DIR/sgp_table_results.txt"

# ── Backup originals ────────────────────────────────────────────────────────
cp "$MAKEFILE"  "$MAKEFILE.bak"
cp "$COMMON_H"  "$COMMON_H.bak"

restore_originals() {
    cp "$MAKEFILE.bak"  "$MAKEFILE"
    cp "$COMMON_H.bak"  "$COMMON_H"
}
trap restore_originals EXIT

# ── Helpers ──────────────────────────────────────────────────────────────────

set_sgp() {            # $1 = 0 or 1
    sed -i "s/-DENABLE_SGP=[01]/-DENABLE_SGP=$1/" "$MAKEFILE"
}

set_flush() {          # $1 = 0 or 1
    sed -i "s/-DENABLE_IMMEDIATE_FLUSH=[01]/-DENABLE_IMMEDIATE_FLUSH=$1/" "$MAKEFILE"
}

set_coefficient() {    # $1 = 1 to force all coefficients to 1, 0 for default (3,3,...)
    sed -i "s/-DENABLE_COEFF_ONE=[01]/-DENABLE_COEFF_ONE=$1/" "$MAKEFILE"
}

build() {
    cd "$PROJ_DIR"
    make clean >/dev/null 2>&1
    make -j"$(nproc)" 2>&1 | tail -1
}

run_bench() {          # $1 = extra args (e.g. "--insert-only" or "")
    local extra="$1"
    rm -f "$PMEM_DIR"/* 2>/dev/null || true
    numactl --cpunodebind=0 --membind=0 \
        "$PROJ_DIR/project" a "$DIST" "$THREADS" "$PMEM_DIR" $extra 2>&1
}

run_bench_keep() {     # same but does NOT clear pmem (run on existing data)
    local extra="$1"
    numactl --cpunodebind=0 --membind=0 \
        "$PROJ_DIR/project" a "$DIST" "$THREADS" "$PMEM_DIR" $extra 2>&1
}

extract_throughput() { # stdin = full benchmark output, $1 = grep pattern
    grep "$1" | awk '{print $NF}'
}

avg() {                # $@ = list of numbers
    local sum=0 n=0
    for v in "$@"; do
        sum=$(echo "$sum + $v" | bc -l)
        n=$((n + 1))
    done
    echo "scale=2; $sum / $n" | bc -l
}

# ── Configuration table ─────────────────────────────────────────────────────
#           NAME                              SGP  FLUSH  COEFF_ONE
CONFIGS=(
    "SPECTRUMKV                             1    0      0"
    "SPECTRUMKV+NO_SGP                      0    0      0"
    "SPECTRUMKV+IMMEDIATE_PERSIST           1    1      0"
    "SPECTRUMKV+NO_SGP+IMMEDIATE_PERSIST    0    1      0"
    "SPECTRUMKV+COEFFICIENT=1               1    0      1"
)

# ── Header ───────────────────────────────────────────────────────────────────
header=$(printf "%-45s  %12s  %12s  %12s  %12s  %12s  %12s\n" \
    "SYS/LOAD" \
    "INSERT_R1" "INSERT_R2" "INSERT_R3" "INSERT_AVG" \
    "WKLDA_AVG" "  ")

# Re-format: two separate sub-tables
divider="==============================================================================================================="

{
echo "$divider"
echo "  SGP Table Benchmark   (threads=$THREADS, dist=$DIST, runs=$RUNS)"
echo "  $(date)"
echo "$divider"
echo ""
printf "%-45s | %12s %12s %12s | %12s\n" \
    "CONFIGURATION" "Run1" "Run2" "Run3" "AVG"
echo "----------------------------------------------+------------------------------------------+--------------"
} | tee "$RESULT_FILE"

# ── Main loop ────────────────────────────────────────────────────────────────
declare -A INSERT_AVGS
declare -A WKLDA_AVGS

for cfg_line in "${CONFIGS[@]}"; do
    read -r NAME SGP FLUSH COEFF_ONE <<< "$cfg_line"

    echo ""
    echo ">>> Building: $NAME  (SGP=$SGP  FLUSH=$FLUSH  COEFF_ONE=$COEFF_ONE)"

    # Apply configuration
    restore_originals          # always start from clean baseline
    set_sgp   "$SGP"
    set_flush "$FLUSH"
    set_coefficient "$COEFF_ONE"
    build

    # ── Insert-only ──────────────────────────────────────────────────────
    insert_vals=()
    for r in $(seq 1 $RUNS); do
        out=$(run_bench "--insert-only")
        tput=$(echo "$out" | extract_throughput "YCSB_INSERT throughput")
        insert_vals+=("$tput")
        echo "    insert-only  run$r: $tput"
    done
    ins_avg=$(avg "${insert_vals[@]}")
    INSERT_AVGS["$NAME"]="$ins_avg"

    printf "%-45s | %12s %12s %12s | %12s\n" \
        "$NAME (insert-only)" \
        "${insert_vals[0]}" "${insert_vals[1]}" "${insert_vals[2]}" \
        "$ins_avg" | tee -a "$RESULT_FILE"

    # ── Workload A (two-step: insert-only to populate, then run workload A) ──
    wklda_vals=()
    for r in $(seq 1 $RUNS); do
        # Step 1: populate data via insert-only (clear pmem first)
        echo "    workload-A   run$r: populating data (insert-only)..."
        run_bench "--insert-only" >/dev/null 2>&1
        # Step 2: run workload A on the populated data (no clear)
        out=$(run_bench_keep "")
        tput=$(echo "$out" | extract_throughput "YCSB_A throughput")
        wklda_vals+=("$tput")
        echo "    workload-A   run$r: $tput"
    done
    wa_avg=$(avg "${wklda_vals[@]}")
    WKLDA_AVGS["$NAME"]="$wa_avg"

    printf "%-45s | %12s %12s %12s | %12s\n" \
        "$NAME (workload-A)" \
        "${wklda_vals[0]}" "${wklda_vals[1]}" "${wklda_vals[2]}" \
        "$wa_avg" | tee -a "$RESULT_FILE"
done

# ── Summary table ────────────────────────────────────────────────────────────
{
echo ""
echo "$divider"
echo "  SUMMARY"
echo "$divider"
printf "%-45s | %14s | %14s\n" "CONFIGURATION" "INSERT-ONLY" "WORKLOAD-A"
echo "----------------------------------------------+----------------+----------------"
for cfg_line in "${CONFIGS[@]}"; do
    read -r NAME _ <<< "$cfg_line"
    printf "%-45s | %14s | %14s\n" \
        "$NAME" \
        "${INSERT_AVGS[$NAME]}" \
        "${WKLDA_AVGS[$NAME]}"
done
echo "$divider"
} | tee -a "$RESULT_FILE"

echo ""
echo "Results saved to: $RESULT_FILE"
