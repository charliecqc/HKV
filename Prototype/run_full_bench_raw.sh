#!/bin/bash
###############################################################################
# run_full_bench_raw.sh  –  Two-config benchmark:
#   1. SPECTRUMKV                             (SGP=1, FLUSH=0, coeff=3)
#   2. SPECTRUMKV+COEFF=1+NO_SGP+IMM_PERSIST  (SGP=0, FLUSH=1, coeff=1)
#
# Execution pattern per (config, dist, threads):
#   Repeat 3 rounds of:
#     1. Clear pmem  +  Load (insert-only)   → record load throughput
#     2. Workload C  (on populated data)     → record C throughput
#     3. Workload B  (on populated data)     → record B throughput
#     4. Workload D  (on populated data)     → record D throughput
#     5. Workload A  (on populated data)     → record A throughput
#     6. Clear pmem
#   Then average the 3 per-round values for each workload.
#
# Threads:       1, 4, 8, 16, 32
# Distributions: zipf, unif
###############################################################################
set -uo pipefail

PROJ_DIR="$(cd "$(dirname "$0")" && pwd)"
PMEM_DIR="/mnt/pmem0"
RUNS=3                                  # number of rounds

THREAD_COUNTS=(32)
DISTS=("zipf")
# Workload sequence within each round (load first, then C B D A)
ROUND_SEQ=("load" "c" "b" "d" "a")

RESULT_FILE="$PROJ_DIR/full_bench_raw_results.txt"
CSV_FILE="$PROJ_DIR/full_bench_raw_results.csv"

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
#   $1 = workload letter (a/b/c/d), $2 = dist, $3 = threads
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

avg() {                # $@ = list of numbers
    local sum=0 n=0
    for v in "$@"; do
        [[ "$v" == "N/A" ]] && continue
        sum=$(echo "$sum + $v" | bc -l)
        n=$((n + 1))
    done
    if [[ $n -eq 0 ]]; then
        echo "N/A"
    else
        echo "scale=2; $sum / $n" | bc -l
    fi
}

# Map workload name → throughput grep pattern
throughput_pattern() {  # $1 = workload name
    case "$1" in
        load) echo "YCSB_INSERT throughput" ;;
        a)    echo "YCSB_A throughput" ;;
        b)    echo "YCSB_B throughput" ;;
        c)    echo "YCSB_C throughput" ;;
        d)    echo "YCSB_D throughput" ;;
        *)    echo "UNKNOWN" ;;
    esac
}

# ── Configuration table ──────────────────────────────────────────────────────
#           NAME                              SGP  FLUSH  COEFF_ONE  RECLAIM
CONFIGS=(
    #"SPECTRUMKV                             1    0      0          0"
    "SPECTRUMKV+COEFF=1+NO_SGP+IMM_PERSIST  0    1      1          1"
)

# ── Header ───────────────────────────────────────────────────────────────────
divider="================================================================================================================================="

{
echo "$divider"
echo "  Raw Benchmark: SPECTRUMKV & SPECTRUMKV+COEFF=1+NO_SGP+IMM_PERSIST"
echo "  threads=${THREAD_COUNTS[*]}, dists=${DISTS[*]}, rounds=$RUNS"
echo "  Round pattern: Load → C → B → D → A → clear pmem"
echo "  $(date)"
echo "$divider"
} | tee "$RESULT_FILE"

# Write CSV header
echo "CONFIG,DIST,WORKLOAD,THREADS,RUN1,RUN2,RUN3,AVG" > "$CSV_FILE"

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
            echo "--- $NAME | dist=$DIST | threads=$THR | $RUNS rounds of Load→C→B→D→A→clear ---" | tee -a "$RESULT_FILE"

        # Associative arrays: WL → space-separated list of per-round values
        declare -A round_vals
        for WL in "${ROUND_SEQ[@]}"; do
            round_vals["$WL"]=""
        done

        for r in $(seq 1 $RUNS); do
            echo "  == Round $r / $RUNS ==" | tee -a "$RESULT_FILE"

            for WL in "${ROUND_SEQ[@]}"; do
                pattern=$(throughput_pattern "$WL")

                if [[ "$WL" == "load" ]]; then
                    # Step 1: clear pmem + insert-only
                    echo "    [$WL] clearing pmem and running load..." | tee -a "$RESULT_FILE"
                    out=$(run_load "$DIST" "$THR")
                else
                    # Steps 2-5: run workload on populated data
                    echo "    [$WL] running workload $WL on populated data..." | tee -a "$RESULT_FILE"
                    out=$(run_workload "$WL" "$DIST" "$THR")
                fi

                tput=$(echo "$out" | extract_throughput "$pattern")
                round_vals["$WL"]="${round_vals[$WL]} $tput"
                echo "    [$WL] round$r: $tput" | tee -a "$RESULT_FILE"
            done

            # End of round: clear pmem
            echo "    [cleanup] clearing pmem..." | tee -a "$RESULT_FILE"
            clear_pmem
        done

        # Compute averages and write CSV for each workload
        echo "" | tee -a "$RESULT_FILE"
        for WL in "${ROUND_SEQ[@]}"; do
            # Convert space-separated string to array
            read -ra vals <<< "${round_vals[$WL]}"
            wl_avg=$(avg "${vals[@]}")

            # Pad vals to exactly RUNS entries for CSV
            v1="${vals[0]:-N/A}"; v2="${vals[1]:-N/A}"; v3="${vals[2]:-N/A}"

            printf "    %-55s => R1=%-12s R2=%-12s R3=%-12s AVG=%s\n" \
                "$NAME | $DIST | T=$THR | $WL" \
                "$v1" "$v2" "$v3" "$wl_avg" | tee -a "$RESULT_FILE"

            echo "$NAME,$DIST,$WL,$THR,$v1,$v2,$v3,$wl_avg" >> "$CSV_FILE"
        done

        unset round_vals

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
    "CONFIGURATION" "DIST" "THR" "WORKLOAD" "AVG THROUGHPUT"
echo "----------------------------------------------+--------+-------+----------+----------------"
} | tee -a "$RESULT_FILE"

# Re-read from CSV for summary (skip header)
tail -n +2 "$CSV_FILE" | while IFS=',' read -r cfg dist wl thr r1 r2 r3 avg_val; do
    printf "%-45s | %6s | %5s | %8s | %14s\n" \
        "$cfg" "$dist" "$thr" "$wl" "$avg_val"
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
