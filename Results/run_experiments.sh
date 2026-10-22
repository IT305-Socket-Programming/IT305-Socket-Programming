#!/usr/bin/env bash
# ==============================================================================
# run_experiments.sh - Automated Empirical Evaluation Suite for IT305 Part II
# Compares Case 1 (Restart), Case 2 (Resume), and Case 2 Enhanced (Multi-Stream)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RESULTS_DIR="$REPO_ROOT/Results"
RAW_DIR="$RESULTS_DIR/raw"
PROCESSED_DIR="$RESULTS_DIR/processed"
GRAPHS_DIR="$RESULTS_DIR/graphs"

mkdir -p "$RAW_DIR" "$PROCESSED_DIR" "$GRAPHS_DIR"

TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
RAW_CSV="$RAW_DIR/experiments_${TIMESTAMP}.csv"
LATEST_RAW_CSV="$RAW_DIR/experiments_latest.csv"
SUMMARY_CSV="$PROCESSED_DIR/summary.csv"

# Header for raw CSV
echo "timestamp,variant,streams,failure_prob,trial,topic,status,attempts,retries,useful_bytes,transmitted_bytes,retransmitted_bytes,protocol_bytes,elapsed_sec,mbps" > "$RAW_CSV"

WORK_DIR=$(mktemp -d /tmp/it305_bench.XXXXXX)
TOPICS_ROOT="$WORK_DIR/topics"
CLIENT_OUT="$WORK_DIR/out"
mkdir -p "$TOPICS_ROOT/dataset" "$CLIENT_OUT"

cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

# Build standard workload dataset (~3.1 MB total payload)
# 1 large file: 2 MB (32 chunks)
# 2 medium files: 512 KB each (8 chunks each)
# 1 small file: 64 KB (1 chunk)
# 1 tiny text file: 4 KB
dd if=/dev/urandom of="$TOPICS_ROOT/dataset/archive.bin" bs=65536 count=32 status=none
dd if=/dev/urandom of="$TOPICS_ROOT/dataset/audio_stream.raw" bs=65536 count=8 status=none
dd if=/dev/urandom of="$TOPICS_ROOT/dataset/video_frame.dat" bs=65536 count=8 status=none
dd if=/dev/urandom of="$TOPICS_ROOT/dataset/metadata.dat" bs=65536 count=1 status=none
head -c 4096 /dev/urandom | base64 > "$TOPICS_ROOT/dataset/readme.txt"

TOTAL_DATA_BYTES=$(du -sb "$TOPICS_ROOT/dataset" | awk '{print $1}')
echo "======================================================================"
echo "          IT305 PART II SYSTEMATIC EMPIRICAL BENCHMARK                "
echo "======================================================================"
echo "Workload: 'dataset' (5 files, $TOTAL_DATA_BYTES bytes, ~3.1 MB)"
echo "Raw Output: $RAW_CSV"
echo "Summary Output: $SUMMARY_CSV"
echo "======================================================================"

# Ensure all binaries are cleanly built
echo "Building all variants with -O2 -pthread..."
make -C "$REPO_ROOT/Part2_Case1" all >/dev/null
make -C "$REPO_ROOT/Part2_Case2" all >/dev/null
make -C "$REPO_ROOT/Part2_Case2_Enhanced" all >/dev/null

PORT_BASE=25000
PORT_OFFSET=0

PROBS=(0.00 0.02 0.05 0.10 0.20)
TRIALS=3

run_trial() {
    local variant="$1"
    local streams="$2"
    local prob="$3"
    local trial="$4"

    local port=$((PORT_BASE + PORT_OFFSET))
    PORT_OFFSET=$(( (PORT_OFFSET + 1) % 500 ))

    local var_dir=""
    local client_cmd=""
    local server_cmd=""

    rm -rf "$CLIENT_OUT"/* "$TOPICS_ROOT/.sessions"
    mkdir -p "$CLIENT_OUT"

    if [ "$variant" = "Case1" ]; then
        var_dir="$REPO_ROOT/Part2_Case1"
        "$var_dir/server" $port "$TOPICS_ROOT" $prob >/dev/null 2>&1 &
        local s_pid=$!
        sleep 0.2
        local out
        out=$("$var_dir/client" 127.0.0.1 $port dataset "$CLIENT_OUT" 20 2>/dev/null || true)
        kill -9 $s_pid 2>/dev/null || true
        wait $s_pid 2>/dev/null || true

    elif [ "$variant" = "Case2" ]; then
        var_dir="$REPO_ROOT/Part2_Case2"
        "$var_dir/server" $port "$TOPICS_ROOT" $prob >/dev/null 2>&1 &
        local s_pid=$!
        sleep 0.2
        local out
        out=$("$var_dir/client" 127.0.0.1 $port dataset "$CLIENT_OUT" 20 2>/dev/null || true)
        kill -9 $s_pid 2>/dev/null || true
        wait $s_pid 2>/dev/null || true

    elif [ "$variant" = "Case2_Enhanced" ]; then
        var_dir="$REPO_ROOT/Part2_Case2_Enhanced"
        "$var_dir/server" $port "$TOPICS_ROOT" $prob >/dev/null 2>&1 &
        local s_pid=$!
        sleep 0.2
        local out
        out=$("$var_dir/client" 127.0.0.1 $port dataset "$CLIENT_OUT" "$streams" 20 2>/dev/null || true)
        kill -9 $s_pid 2>/dev/null || true
        wait $s_pid 2>/dev/null || true
    fi

    # Parse RESULT line
    local res_line
    res_line=$(echo "$out" | grep "^RESULT " || true)
    if [ -n "$res_line" ]; then
        local status="FAIL" attempts="0" retries="0" useful_bytes="0"
        local transmitted_bytes="0" retransmitted_bytes="0" protocol_bytes="0"
        local elapsed_sec="0.0" mbps="0.0"
        for item in $res_line; do
            case "$item" in
                status=*) status="${item#*=}" ;;
                attempts=*) attempts="${item#*=}" ;;
                retries=*) retries="${item#*=}" ;;
                useful_bytes=*) useful_bytes="${item#*=}" ;;
                transmitted_bytes=*) transmitted_bytes="${item#*=}" ;;
                retransmitted_bytes=*) retransmitted_bytes="${item#*=}" ;;
                protocol_bytes=*) protocol_bytes="${item#*=}" ;;
                elapsed_sec=*) elapsed_sec="${item#*=}" ;;
                mbps=*) mbps="${item#*=}" ;;
            esac
        done

        local ts=$(date +"%Y-%m-%d %H:%M:%S")
        echo "$ts,$variant,$streams,$prob,$trial,dataset,$status,$attempts,$retries,$useful_bytes,$transmitted_bytes,$retransmitted_bytes,$protocol_bytes,$elapsed_sec,$mbps" >> "$RAW_CSV"
        printf "  [%s] prob=%-4s streams=%-2s trial=%d => status=%-4s time=%-7s mbps=%-7s retrans=%-8s\n" \
               "$variant" "$prob" "$streams" "$trial" "$status" "${elapsed_sec}s" "$mbps" "${retransmitted_bytes}B"
    else
        local ts=$(date +"%Y-%m-%d %H:%M:%S")
        echo "$ts,$variant,$streams,$prob,$trial,dataset,FAIL,0,0,0,0,0,0,0,0" >> "$RAW_CSV"
        printf "  [%s] prob=%-4s streams=%-2s trial=%d => FAILED\n" "$variant" "$prob" "$streams" "$trial"
    fi
}

echo -e "\n--- Benchmarking Case 1: Restart from Beginning ---"
for p in "${PROBS[@]}"; do
    for t in $(seq 1 $TRIALS); do
        run_trial "Case1" 1 "$p" "$t"
    done
done

echo -e "\n--- Benchmarking Case 2: Session Checkpoint Resume ---"
for p in "${PROBS[@]}"; do
    for t in $(seq 1 $TRIALS); do
        run_trial "Case2" 1 "$p" "$t"
    done
done

echo -e "\n--- Benchmarking Case 2 Enhanced: Parallel TCP Streams ---"
for s in 1 2 4 8; do
    echo "  [Streams K=$s]"
    for p in "${PROBS[@]}"; do
        for t in $(seq 1 $TRIALS); do
            run_trial "Case2_Enhanced" "$s" "$p" "$t"
        done
    done
done

cp "$RAW_CSV" "$LATEST_RAW_CSV"

echo -e "\n--- Processing Summary Metrics ---"
python3 "$RESULTS_DIR/aggregate_results.py" "$RAW_CSV" "$SUMMARY_CSV"

echo "Running plot_results.py..."
python3 "$RESULTS_DIR/plot_results.py" "$SUMMARY_CSV" "$GRAPHS_DIR"

echo -e "\n======================================================================"
echo "                 EXPERIMENTS COMPLETED SUCCESSFULLY                   "
echo "======================================================================"
