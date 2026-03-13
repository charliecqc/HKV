#!/bin/bash
###############################################################################
# run_full_bench_scan.sh  –  YCSB-E (scan) benchmark:
#   1. SPECTRUMKV                             (SGP=1, FLUSH=0, coeff=3)
#   2. SPECTRUMKV+COEFF=1+NO_SGP+IMM_PERSIST  (SGP=0, FLUSH=1, coeff=1)
#
# Execution pattern per (config, dist, threads):
#   1 run of:  Load (insert-only) → YCSB-E → clear pmem
#
# Threads:       1, 4, 8, 16, 32
# Distributions: zipf, unif
###############################################################################
set -uo pipefail

PROJ_DIR="$(cd "$(dirname "$0")" && pwd)"
PMEM_DIR="/mnt/pmem0"

THREAD_COUNTS=(1 4 8 16 32)
DISTS=("zipf" "unif")

RESULT_FILE="$PROJ_DIR/full_bench_scan_results.txt"
CSV_FILE="$PROJ_DIR/full_bench_scan_results.csv"

# ── Helpers ──────────────────────────────────────────────────────────────────

build() {              # $1=SGP $2=FLUSH $3=COEFF_ONE $4=RECLAIM
    cd "$PROJ_DIR"
    make clean >/dev/null 2>&1 || true
    if ! make -j"$(nproc)" \
            ENABLE_SGP="$1" \
            ENABLE_IMMEDIATE_FLUSH="$2" \
            ENABLE_COEFF_ONE="$3" \
            ENABLE_IMMEDIATE_RECLAIM="${4:-0}" \
            2>&1 | tail -5; then
        echo "*** BUILD FAILED ***"
        return 1
    fi
}

# run_load: clears pmem, then runs insert-only
#   $1 = dist, $2 = threads
run_load() {
    local dist="$1"
    local threads="$2"
    rm -rf "$PMEM_DIR"/* 2>/dev/null || true
    numactl --cpunodebind=0 --membind=0 \
        "$PROJ_DIR/project" a "$dist" "$threads" "$PMEM_DIR" --insert-only 2>&1
}

# run_workload: runs on existing populated data (does NOT clear pmem)
#   $1 = workload letter, $2 = dist, $3 = threads
run_workload() {
    local wl="$1"
    local dist="$2"
    local threads="$3"
    numactl --cpunodebind=0 --membind=0 \
        "$PROJ_DIR/project" "$wl" "$dist" "$threads" "$PMEM_DIR" 2>&1
}

# clear_pmem: remove all files in pmem directory
clear_pmem() {
    rm -rf "$PMEM_DIR"/* 2>/dev/null || true
}

extract_throughput() { # stdin = full benchmark output, $1 = grep pattern
    grep "$1" | awk '{print $NF}' || echo "N/A"
}

# ── Configuration table ─────────────────────────────────────────────────────
#           NAME                              SGP  FLUSH  COEFF_ONE  RECLAIM
CONFIGS=(
    "SPECTRUMKV                             1    0      0          0"
    "SPECTRUMKV+COEFF=1+NO_SGP+IMM_PERSIST  0    1      1          1"
)

# ── Header ───────────────────────────────────────────────────────────────────
divider="================================================================================================================================="

{
echo "$divider"
echo "  Scan Benchmark (YCSB-E): SPECTRUMKV & SPECTRUMKV+COEFF=1+NO_SGP+IMM_PERSIST"
echo "  threads=${THREAD_COUNTS[*]}, dists=${DISTS[*]}, 1 run each"
echo "  Pattern: Load → YCSB-E → clear pmem"
echo "  $(date)"
echo "$divider"
} | tee "$RESULT_FILE"

# Write CSV header
echo "CONFIG,DIST,WORKLOAD,THREADS,THROUGHPUT" > "$CSV_FILE"

# ── Main loop ────────────────────────────────────────────────────────────────

for cfg_line in "${CONFIGS[@]}"; do
    read -r NAME SGP FLUSH COEFF_ONE RECLAIM <<< "$cfg_line"

    echo "" | tee -a "$RESULT_FILE"
    echo "================================================================================" | tee -a "$RESULT_FILE"
    echo ">>> Building: $NAME  (SGP=$SGP  FLUSH=$FLUSH  COEFF_ONE=$COEFF_ONE  RECLAIM=$RECLAIM)" | tee -a "$RESULT_FILE"
    echo "================================================================================" | tee -a "$RESULT_FILE"

    build "$SGP" "$FLUSH" "$COEFF_ONE" "$RECLAIM"

    for DIST in "${DISTS[@]}"; do
        for THR in "${THREAD_COUNTS[@]}"; do

            echo "" | tee -a "$RESULT_FILE"
            echo "--- $NAME | dist=$DIST | threads=$THR | Load → YCSB-E → clear ---" | tee -a "$RESULT_FILE"

            # Step 1: Load (insert-only)
            echo "    [load] clearing pmem and running load..." | tee -a "$RESULT_FILE"
            out_load=$(run_load "$DIST" "$THR")
            tput_load=$(echo "$out_load" | extract_throughput "YCSB_INSERT throughput")
            echo "    [load] throughput: $tput_load" | tee -a "$RESULT_FILE"
            echo "$NAME,$DIST,load,$THR,$tput_load" >> "$CSV_FILE"

            # Step 2: YCSB-E (scan workload on populated data)
            echo "    [e] running YCSB-E on populated data..." | tee -a "$RESULT_FILE"
            out_e=$(run_workload "e" "$DIST" "$THR")
            tput_e=$(echo "$out_e" | extract_throughput "YCSB_E throughput")
            echo "    [e] throughput: $tput_e" | tee -a "$RESULT_FILE"
            echo "$NAME,$DIST,e,$THR,$tput_e" >> "$CSV_FILE"

            # Step 3: Clear pmem
            echo "    [cleanup] clearing pmem..." | tee -a "$RESULT_FILE"
            clear_pmem

        done  # end THR
    done  # end DIST
done  # end CONFIG

# ── Summary table ────────────────────────────────────────────────────────────
{
echo ""
echo "$divider"
echo "  SUMMARY"
echo "$divider"
echo ""
printf "%-45s | %6s | %5s | %8s | %14s\n" \
    "CONFIGURATION" "DIST" "THR" "WORKLOAD" "THROUGHPUT"
echo "----------------------------------------------+--------+-------+----------+----------------"
} | tee -a "$RESULT_FILE"

tail -n +2 "$CSV_FILE" | while IFS=',' read -r cfg dist wl thr tp; do
    printf "%-45s | %6s | %5s | %8s | %14s\n" \
        "$cfg" "$dist" "$thr" "$wl" "$tp"
done | tee -a "$RESULT_FILE"

{
echo ""
echo "$divider"
echo "  Finished: $(date)"
echo "$divider"
} | tee -a "$RESULT_FILE"

echo ""
echo "Results saved to: $RESULT_FILE"
echo "CSV saved to:     $CSV_FILE"
