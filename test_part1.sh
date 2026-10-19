#!/usr/bin/env bash
# ==============================================================================
# test_part1.sh - Comprehensive Validation Test Suite for Part 1
# ==============================================================================
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

PASS_COUNT=0
FAIL_COUNT=0
TEST_PORT=19580

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    echo -e "  [${GREEN}PASS${NC}] $1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo -e "  [${RED}FAIL${NC}] $1"
    if [ -n "${2:-}" ]; then
        echo -e "         ${YELLOW}Detail:${NC} $2"
    fi
}

cleanup() {
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -INT "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [ -n "${STALL_PID:-}" ] && kill -0 "$STALL_PID" 2>/dev/null; then
        kill -9 "$STALL_PID" 2>/dev/null || true
    fi
    if [ -n "${STALL1_PID:-}" ] && kill -0 "$STALL1_PID" 2>/dev/null; then
        kill -9 "$STALL1_PID" 2>/dev/null || true
    fi
    if [ -n "${STALL2_PID:-}" ] && kill -0 "$STALL2_PID" 2>/dev/null; then
        kill -9 "$STALL2_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}

TMP_DIR=$(mktemp -d /tmp/it305_test.XXXXXX)
trap cleanup EXIT INT TERM

echo -e "${BLUE}======================================================================${NC}"
echo -e "${BLUE}           IT305 PART 1 COMPREHENSIVE VALIDATION SUITE                ${NC}"
echo -e "${BLUE}======================================================================${NC}"

# ------------------------------------------------------------------------------
# TEST GROUP 1: Clean Build & Zero Warnings
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 1: Build & Static Compiler Diagnostics ---${NC}"
make clean >/dev/null 2>&1
if BUILD_OUT=$(make CFLAGS="-std=gnu11 -O2 -Wall -Wextra -Wpedantic -Werror" 2>&1); then
    pass "Build with -Wall -Wextra -Wpedantic -Werror succeeded with zero warnings"
else
    fail "Build failed or generated compiler warnings" "$BUILD_OUT"
fi

[ -x "./server" ] && pass "server binary created and executable" || fail "server binary missing"
[ -x "./client" ] && pass "client binary created and executable" || fail "client binary missing"

# ------------------------------------------------------------------------------
# TEST GROUP 2: Server CLI & Input Validation
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 2: Server Command-Line & Argument Validation ---${NC}"

./server >/dev/null 2>&1 && fail "Server should reject 0 arguments" || pass "Server rejects 0 arguments (exit code != 0)"
./server 0 /tmp >/dev/null 2>&1 && fail "Server should reject port 0" || pass "Server rejects port 0"
./server 70000 /tmp >/dev/null 2>&1 && fail "Server should reject port > 65535" || pass "Server rejects port > 65535"
./server -1 /tmp >/dev/null 2>&1 && fail "Server should reject negative port" || pass "Server rejects negative port"
./server not_a_port /tmp >/dev/null 2>&1 && fail "Server should reject non-numeric port" || pass "Server rejects non-numeric port"
./server 5000 /non_existent_dir_99999 >/dev/null 2>&1 && fail "Server should reject non-existent directory" || pass "Server rejects non-existent topics directory"
./server 5000 /tmp --mode invalid_mode >/dev/null 2>&1 && fail "Server should reject invalid --mode" || pass "Server rejects invalid --mode argument"

# ------------------------------------------------------------------------------
# TEST GROUP 3: Client CLI & Argument Validation
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 3: Client Command-Line & Argument Validation ---${NC}"

./client >/dev/null 2>&1 && fail "Client should reject 0 arguments" || pass "Client rejects 0 arguments (exit code != 0)"
./client 127.0.0.1 not_a_port cane /tmp >/dev/null 2>&1 && fail "Client should reject non-numeric port" || pass "Client rejects non-numeric port"
./client 127.0.0.1 70000 cane /tmp >/dev/null 2>&1 && fail "Client should reject port > 65535" || pass "Client rejects port > 65535"
./client 127.0.0.1 5000 "../escape" /tmp >/dev/null 2>&1 && fail "Client should reject path traversal in topic" || pass "Client rejects path traversal in topic"
./client 127.0.0.1 5000 "topic/slash" /tmp >/dev/null 2>&1 && fail "Client should reject slashes in topic" || pass "Client rejects slash in topic"
./client 127.0.0.1 5000 cane "" >/dev/null 2>&1 && fail "Client should reject empty output directory" || pass "Client rejects empty output directory"
./client 127.0.0.1 59999 cane /tmp >/dev/null 2>&1 && fail "Client should fail when server is not running" || pass "Client cleanly fails when connection refused"

# ------------------------------------------------------------------------------
# TEST GROUP 4: Test Dataset Preparation
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 4: Preparing Test Dataset & Topics ---${NC}"

DATASET_DIR="$TMP_DIR/raw-img"
mkdir -p "$DATASET_DIR/cane" "$DATASET_DIR/gatto" "$DATASET_DIR/cavallo"

# File edge cases:
touch "$DATASET_DIR/cane/empty.dat"
printf "A" > "$DATASET_DIR/cane/single_byte.dat"
dd if=/dev/urandom of="$DATASET_DIR/cane/small_1k.dat" bs=1024 count=1 status=none
dd if=/dev/urandom of="$DATASET_DIR/cane/exact_64k.dat" bs=65536 count=1 status=none
dd if=/dev/urandom of="$DATASET_DIR/cane/boundary_64k_plus1.dat" bs=1 count=65537 status=none
dd if=/dev/urandom of="$DATASET_DIR/cane/medium_512k.dat" bs=65536 count=8 status=none
dd if=/dev/urandom of="$DATASET_DIR/gatto/large_2m.dat" bs=65536 count=32 status=none

for i in {1..10}; do
    echo "Horse file content number $i" > "$DATASET_DIR/cavallo/file_$i.txt"
done

pass "Created test dataset with 0-byte, 1-byte, 64KB boundary, 64KB+1, and multi-chunk files"

# ------------------------------------------------------------------------------
# TEST GROUP 5: Single-Threaded Server Validation & Reliable Shutdown
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 5: Single-Threaded Server Validation & Shutdown Assertions ---${NC}"

PORT=$TEST_PORT
./server "$PORT" "$DATASET_DIR" --mode single >"$TMP_DIR/server_single.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

CLIENT_OUT="$TMP_DIR/client_single_out"
mkdir -p "$CLIENT_OUT"

RES=$(./client 127.0.0.1 "$PORT" cane "$CLIENT_OUT" 2>"$TMP_DIR/client_err.log" || true)
if [ -n "$RES" ] && echo "$RES" | grep -q "RESULT topic=cane"; then
    pass "Single-threaded client transfer finished with RESULT line"
else
    fail "Single-threaded client failed to complete transfer" "$(cat "$TMP_DIR/client_err.log")"
fi

if diff -rq "$DATASET_DIR/cane" "$CLIENT_OUT/cane" >/dev/null 2>&1; then
    pass "Single-threaded data integrity: diff -r matches byte-for-byte"
else
    fail "Single-threaded data mismatch" "$(diff -rq "$DATASET_DIR/cane" "$CLIENT_OUT/cane" 2>&1)"
fi

# Reliable Shutdown Assertion: verify exact exit code and shutdown log
kill -INT "$SERVER_PID"
set +e
wait "$SERVER_PID"
SRV_RC=$?
set -e
unset SERVER_PID

if [ $SRV_RC -eq 0 ]; then
    pass "Single-threaded server stopped cleanly on SIGINT with exit code 0"
else
    fail "Single-threaded server exited with unexpected code on SIGINT: $SRV_RC"
fi

if grep -q "shutting down..." "$TMP_DIR/server_single.log" && grep -q "served " "$TMP_DIR/server_single.log"; then
    pass "Single-threaded server logged clean shutdown sequence and final totals"
else
    fail "Single-threaded server missing shutdown logging"
fi

# ------------------------------------------------------------------------------
# TEST GROUP 6: Multi-Threaded Server, Aliases, and Case-Insensitivity
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 6: Multi-Threaded Server, Aliases, and Case-Insensitivity ---${NC}"

PORT=$((TEST_PORT + 1))
./server "$PORT" "$DATASET_DIR" --mode multi >"$TMP_DIR/server_multi.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

CLIENT_OUT="$TMP_DIR/client_multi_out"
mkdir -p "$CLIENT_OUT"

RES1=$(./client 127.0.0.1 "$PORT" gatto "$CLIENT_OUT" 2>/dev/null || true)
if echo "$RES1" | grep -q "RESULT topic=gatto"; then
    pass "Exact topic match (gatto) transfer succeeded"
else
    fail "Exact topic match (gatto) failed"
fi
diff -rq "$DATASET_DIR/gatto" "$CLIENT_OUT/gatto" >/dev/null 2>&1 && \
    pass "Large 2MB file diff verified byte-for-byte" || fail "Large file diff mismatch"

RES2=$(./client 127.0.0.1 "$PORT" CAVALLO "$CLIENT_OUT" 2>/dev/null || true)
if echo "$RES2" | grep -q "RESULT topic=CAVALLO"; then
    pass "Case-insensitive topic match (CAVALLO) succeeded"
else
    fail "Case-insensitive topic match failed"
fi
diff -rq "$DATASET_DIR/cavallo" "$CLIENT_OUT/CAVALLO" >/dev/null 2>&1 && \
    pass "Case-insensitive topic files verified byte-for-byte" || fail "Case-insensitive diff mismatch"

RES3=$(./client 127.0.0.1 "$PORT" dog "$CLIENT_OUT" 2>/dev/null || true)
if echo "$RES3" | grep -q "RESULT topic=dog"; then
    pass "English alias lookup (dog -> cane) succeeded"
else
    fail "English alias lookup failed"
fi
diff -rq "$DATASET_DIR/cane" "$CLIENT_OUT/dog" >/dev/null 2>&1 && \
    pass "English alias files verified byte-for-byte with source topic folder" || fail "Alias diff mismatch"

UNKNOWN_ERR=$(./client 127.0.0.1 "$PORT" nonexistent_topic "$CLIENT_OUT" 2>&1 || true)
if echo "$UNKNOWN_ERR" | grep -qi "server error 2"; then
    pass "Server returns MSG_ERROR code 2 (ERR_TOPIC_NOT_FOUND) for unknown topic"
else
    fail "Unknown topic did not produce expected error code 2" "$UNKNOWN_ERR"
fi

kill -INT "$SERVER_PID"
set +e
wait "$SERVER_PID"
SRV_RC=$?
set -e
unset SERVER_PID

if [ $SRV_RC -eq 0 ]; then
    pass "Multi-threaded server stopped cleanly on SIGINT with exit code 0"
else
    fail "Multi-threaded server exited with unexpected code: $SRV_RC"
fi

# ------------------------------------------------------------------------------
# TEST GROUP 7: High Concurrency Multi-Threaded Stress Test
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 7: High Concurrency Multi-Threaded Stress Test ---${NC}"

PORT=$((TEST_PORT + 2))
./server "$PORT" "$DATASET_DIR" --mode multi >"$TMP_DIR/server_stress.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

CONCURRENT_CLIENTS=8
PIDS=()
for i in $(seq 1 $CONCURRENT_CLIENTS); do
    OUT_DIR="$TMP_DIR/stress_out_$i"
    mkdir -p "$OUT_DIR"
    TOPIC="cane"
    if [ $((i % 3)) -eq 1 ]; then TOPIC="gatto"; fi
    if [ $((i % 3)) -eq 2 ]; then TOPIC="cavallo"; fi
    ./client 127.0.0.1 "$PORT" "$TOPIC" "$OUT_DIR" >"$TMP_DIR/res_$i.txt" 2>"$TMP_DIR/err_$i.txt" &
    PIDS+=($!)
done

ALL_OK=1
for pid in "${PIDS[@]}"; do
    if ! wait "$pid"; then
        ALL_OK=0
    fi
done

if [ $ALL_OK -eq 1 ]; then
    pass "All $CONCURRENT_CLIENTS concurrent clients completed successfully"
else
    fail "One or more concurrent clients failed"
fi

CONCURRENCY_INTEGRITY=1
for i in $(seq 1 $CONCURRENT_CLIENTS); do
    OUT_DIR="$TMP_DIR/stress_out_$i"
    TOPIC="cane"
    if [ $((i % 3)) -eq 1 ]; then TOPIC="gatto"; fi
    if [ $((i % 3)) -eq 2 ]; then TOPIC="cavallo"; fi
    if ! diff -rq "$DATASET_DIR/$TOPIC" "$OUT_DIR/$TOPIC" >/dev/null 2>&1; then
        CONCURRENCY_INTEGRITY=0
    fi
done

if [ $CONCURRENCY_INTEGRITY -eq 1 ]; then
    pass "Data integrity verified across all $CONCURRENT_CLIENTS concurrent client outputs"
else
    fail "Data corruption detected under concurrent transfers"
fi

kill -INT "$SERVER_PID"
set +e
wait "$SERVER_PID"
set -e
unset SERVER_PID

# ------------------------------------------------------------------------------
# TEST GROUP 8: Active Worker & Stalled Client Shutdown Interruption (Fix 4)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 8: Active Worker & Stalled Client Interruption on Shutdown ---${NC}"

PORT=$((TEST_PORT + 3))
./server "$PORT" "$DATASET_DIR" --mode multi >"$TMP_DIR/server_stalled.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

# Connect a client that connects and goes silent (stalled connection)
python3 -c "import socket, time; s = socket.create_connection(('127.0.0.1', $PORT)); time.sleep(10)" >/dev/null 2>&1 &
STALL_PID=$!
sleep 0.3

# Now send SIGINT to server while stalled client connection is active
T_START=$(date +%s%N)
kill -INT "$SERVER_PID"
set +e
wait "$SERVER_PID"
SRV_RC=$?
set -e
T_END=$(date +%s%N)
DURATION_MS=$(( (T_END - T_START) / 1000000 ))
unset SERVER_PID

# Wait for stalled client process to be unblocked by server's shutdown()
set +e
wait "$STALL_PID" 2>/dev/null
set -e
unset STALL_PID

if [ $SRV_RC -eq 0 ]; then
    pass "Server with active/stalled connection stopped cleanly on SIGINT with exit code 0"
else
    fail "Server with active connection failed shutdown: exit code $SRV_RC"
fi

if grep -q "Server stopped cleanly" "$TMP_DIR/server_stalled.log"; then
    pass "Server log confirms all workers finished cleanly before shared resources freed"
else
    fail "Server log did not confirm clean worker shutdown"
fi

# The shutdown should have completed rapidly (well under the 5s timeout)
if [ "$DURATION_MS" -lt 3000 ]; then
    pass "Active client socket interrupted immediately (shutdown took ${DURATION_MS}ms < 3000ms)"
else
    fail "Shutdown did not interrupt active sockets promptly (took ${DURATION_MS}ms)"
fi

# Test Registry Capacity Saturation & Rejection (Fix 2)
gcc -std=gnu11 -O2 -Wall -Wextra -Wpedantic -pthread -DMAX_ACTIVE_CLIENTS=2 -o "$TMP_DIR/server_cap" server.c common.c -pthread

CAP_PORT=$((TEST_PORT + 10))
"$TMP_DIR/server_cap" "$CAP_PORT" "$DATASET_DIR" --mode multi >"$TMP_DIR/server_cap.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

# Connect 2 clients to saturate capacity (MAX_ACTIVE_CLIENTS=2)
python3 -c "import socket, time; s1 = socket.create_connection(('127.0.0.1', $CAP_PORT)); time.sleep(5)" >/dev/null 2>&1 &
STALL1_PID=$!
python3 -c "import socket, time; s2 = socket.create_connection(('127.0.0.1', $CAP_PORT)); time.sleep(5)" >/dev/null 2>&1 &
STALL2_PID=$!
sleep 0.3

# Attempt 3rd connection (exceeds MAX_ACTIVE_CLIENTS=2)
set +e
./client 127.0.0.1 "$CAP_PORT" cane "$TMP_DIR/rej_out" >/dev/null 2>"$TMP_DIR/client_rej.log"
REJ_RC=$?
set -e

if [ $REJ_RC -ne 0 ]; then
    pass "Server rejected 3rd connection when registry reached capacity (2/2)"
else
    fail "Server accepted connection beyond registry capacity"
fi

if grep -q "client registry full" "$TMP_DIR/server_cap.log"; then
    pass "Server logged explicit registry full condition and rejection"
else
    fail "Server did not log registry full rejection"
fi

kill -INT "$SERVER_PID"
set +e
wait "$SERVER_PID"
SRV_RC=$?
wait "$STALL1_PID" 2>/dev/null
wait "$STALL2_PID" 2>/dev/null
set -e
unset SERVER_PID
unset STALL1_PID
unset STALL2_PID

if [ $SRV_RC -eq 0 ] && grep -q "Server stopped cleanly" "$TMP_DIR/server_cap.log"; then
    pass "Server with saturated registry interrupted all active workers and stopped cleanly"
else
    fail "Server with saturated registry failed clean shutdown"
fi

# ------------------------------------------------------------------------------
# TEST GROUP 9: Client Symlink Protection (Fix 1)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 9: Client Destination Symlink Traversal Protection ---${NC}"

PORT=$((TEST_PORT + 4))
./server "$PORT" "$DATASET_DIR" --mode multi >/dev/null 2>&1 &
SERVER_PID=$!
sleep 0.5

SYMLINK_OUT="$TMP_DIR/symlink_out"
mkdir -p "$SYMLINK_OUT/cane"
TARGET_FILE="$TMP_DIR/safe_sensitive_file.txt"
echo "SECRET_ORIGINAL_CONTENT_PRESERVED" > "$TARGET_FILE"

# Create a symlink in the destination directory pointing to our target file
ln -s "$TARGET_FILE" "$SYMLINK_OUT/cane/empty.dat"

# Run client attempting to download topic cane
set +e
./client 127.0.0.1 "$PORT" cane "$SYMLINK_OUT" >"$TMP_DIR/symlink_client.log" 2>&1
CLIENT_RC=$?
set -e

kill -INT "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

if [ $CLIENT_RC -ne 0 ]; then
    pass "Client rejected overwriting through existing symbolic link (exit code $CLIENT_RC != 0)"
else
    fail "Client followed or silently accepted destination symbolic link!"
fi

# Verify the target file content was NOT overwritten or truncated
TARGET_CONTENT=$(cat "$TARGET_FILE")
if [ "$TARGET_CONTENT" = "SECRET_ORIGINAL_CONTENT_PRESERVED" ]; then
    pass "Target file verified completely untouched and not truncated"
else
    fail "Security violation: target file was modified or truncated!" "$TARGET_CONTENT"
fi

# ------------------------------------------------------------------------------
# TEST GROUP 10: Benchmark & Aggregation Pipeline Fixtures (Fix 2)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 10: Benchmark & Aggregation Pipeline Fixtures ---${NC}"

FIXTURE_CSV="$TMP_DIR/fixture_trials.csv"
# Write header
echo "trial,label,host,total_clients,nclients,client_id,topic,status,files,data_bytes,wire_bytes,start,end,secs,mbps" > "$FIXTURE_CSV"
# Trial 1: Machine B (local 2, total 4) + Machine C (local 2, total 4) -> ok
echo "1,multi,hostB,4,2,0,cane,ok,10,1000000,1050000,100.000000,101.000000,1.000000,8.00" >> "$FIXTURE_CSV"
echo "1,multi,hostB,4,2,1,gatto,ok,10,1000000,1050000,100.000000,101.500000,1.500000,5.33" >> "$FIXTURE_CSV"
echo "1,multi,hostC,4,2,2,cavallo,ok,10,1000000,1050000,100.000000,101.200000,1.200000,6.67" >> "$FIXTURE_CSV"
echo "1,multi,hostC,4,2,3,cane,ok,10,1000000,1050000,100.000000,101.800000,1.800000,4.44" >> "$FIXTURE_CSV"

# Trial 2: Run 1 hour later (timestamps at 3700.000000) -> ok
echo "2,multi,hostB,4,2,0,cane,ok,10,1000000,1050000,3700.000000,3701.000000,1.000000,8.00" >> "$FIXTURE_CSV"
echo "2,multi,hostB,4,2,1,gatto,ok,10,1000000,1050000,3700.000000,3701.200000,1.200000,6.67" >> "$FIXTURE_CSV"
echo "2,multi,hostC,4,2,2,cavallo,ok,10,1000000,1050000,3700.000000,3701.100000,1.100000,7.27" >> "$FIXTURE_CSV"
echo "2,multi,hostC,4,2,3,cane,ok,10,1000000,1050000,3700.000000,3701.500000,1.500000,5.33" >> "$FIXTURE_CSV"

# Trial 3: All-failed trial
echo "3,multi,hostB,2,2,0,cane,FAIL,0,0,0,0,0,0,0" >> "$FIXTURE_CSV"
echo "3,multi,hostB,2,2,1,cane,FAIL,0,0,0,0,0,0,0" >> "$FIXTURE_CSV"

# Trial 4: Non-numeric text trial ID (mixed with numeric IDs 1, 2, 3)
echo "trial_a,multi,hostB,2,2,0,cane,ok,10,1000000,1050000,5000.000000,5001.000000,1.000000,8.00" >> "$FIXTURE_CSV"
echo "trial_a,multi,hostB,2,2,1,gatto,ok,10,1000000,1050000,5000.000000,5001.200000,1.200000,6.67" >> "$FIXTURE_CSV"

AGG_SUMMARY="$TMP_DIR/agg_summary.csv"
python3 aggregate.py "$FIXTURE_CSV" -o "$AGG_SUMMARY" >"$TMP_DIR/agg.out"

# Verification 1: Four distinct trial rows must be generated
TRIAL_ROWS=$(grep -v "^trial," "$AGG_SUMMARY" | wc -l)
if [ "$TRIAL_ROWS" -eq 4 ]; then
    pass "aggregate.py preserved 4 distinct trials independently without combining wall times"
else
    fail "aggregate.py produced $TRIAL_ROWS rows instead of 4" "$(cat "$TMP_DIR/agg.out")"
fi

# Verification 2: Trial 1 wall time must be 1.800s (max 101.8 - min 100.0), NOT 3601s!
TRIAL1_LINE=$(grep "^1," "$AGG_SUMMARY")
TRIAL1_SPAN=$(echo "$TRIAL1_LINE" | cut -d',' -f7)
if [ "$TRIAL1_SPAN" = "1.8" ] || [ "$TRIAL1_SPAN" = "1.800" ]; then
    pass "Trial 1 wall time accurately calculated as ${TRIAL1_SPAN}s (isolated from Trial 2)"
else
    fail "Trial 1 wall time corrupted: expected 1.8s, got $TRIAL1_SPAN"
fi

# Verification 3: Distributed client total concurrency must be 4
TRIAL1_TOTAL=$(echo "$TRIAL1_LINE" | cut -d',' -f3)
if [ "$TRIAL1_TOTAL" = "4" ]; then
    pass "Distributed concurrency correctly aggregated as total_clients=4"
else
    fail "Distributed concurrency mismatch: expected 4, got $TRIAL1_TOTAL"
fi

# Verification 4: Trial 3 with 100% failure must be reported
TRIAL3_LINE=$(grep "^3," "$AGG_SUMMARY")
TRIAL3_OK=$(echo "$TRIAL3_LINE" | cut -d',' -f4)
TRIAL3_FAILED=$(echo "$TRIAL3_LINE" | cut -d',' -f5)
if [ "$TRIAL3_OK" = "0" ] && [ "$TRIAL3_FAILED" = "2" ]; then
    pass "All-failure trial correctly reported in summary (ok=0, failed=2, rate=0.00)"
else
    fail "All-failure trial reporting error: ok=$TRIAL3_OK, failed=$TRIAL3_FAILED"
fi

# Verification 5: Mixed numeric and text trial IDs successfully aggregated
TRIAL4_LINE=$(grep "^trial_a," "$AGG_SUMMARY")
if [ -n "$TRIAL4_LINE" ] && grep -q "^1," "$AGG_SUMMARY"; then
    pass "Mixed numeric and text trial IDs (1 and trial_a) aggregated cleanly without TypeError"
else
    fail "Mixed numeric and text trial IDs failed to aggregate" "$(cat "$TMP_DIR/agg.out")"
fi

# Live bench.sh test with trial_id and total_clients arguments
PORT=$((TEST_PORT + 5))
./server "$PORT" "$DATASET_DIR" --mode multi >/dev/null 2>&1 &
SERVER_PID=$!
sleep 0.5

LIVE_CSV="$TMP_DIR/live_bench.csv"
./bench.sh 127.0.0.1 "$PORT" multi 2 "$LIVE_CSV" cane,gatto 42 4 >/dev/null 2>&1

kill -INT "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

if grep -q "^42,multi," "$LIVE_CSV"; then
    pass "bench.sh successfully wrote trial=42 and total_clients=4 into live CSV"
else
    fail "bench.sh failed to pass trial_id or total_clients to CSV"
fi

# ------------------------------------------------------------------------------
# TEST GROUP 11: Sanitizer Memory & Undefined Behavior Audit (ASan + UBSan)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Group 11: Memory & Undefined Behavior Audit (ASan + UBSan) ---${NC}"

make clean >/dev/null 2>&1
make CFLAGS="-std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" >/dev/null 2>&1

PORT=$((TEST_PORT + 6))
./server "$PORT" "$DATASET_DIR" --mode multi >"$TMP_DIR/asan_server.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

ASAN_OUT="$TMP_DIR/asan_client_out"
mkdir -p "$ASAN_OUT"
./client 127.0.0.1 "$PORT" cane "$ASAN_OUT" >/dev/null 2>"$TMP_DIR/asan_client.log"
CLIENT_RC=$?

kill -INT "$SERVER_PID"
set +e
wait "$SERVER_PID"
SRV_RC=$?
set -e
unset SERVER_PID

if [ $CLIENT_RC -eq 0 ] && ! grep -qi "ERROR: AddressSanitizer" "$TMP_DIR/asan_client.log" && \
   ! grep -qi "runtime error:" "$TMP_DIR/asan_client.log"; then
    pass "Client completed under ASan/UBSan with zero memory errors or undefined behavior"
else
    fail "Client encountered sanitizer violation" "$(cat "$TMP_DIR/asan_client.log")"
fi

if [ $SRV_RC -eq 0 ] && ! grep -qi "ERROR: AddressSanitizer" "$TMP_DIR/asan_server.log" && \
   ! grep -qi "runtime error:" "$TMP_DIR/asan_server.log"; then
    pass "Server completed under ASan/UBSan with zero memory errors or undefined behavior"
else
    fail "Server encountered sanitizer violation" "$(cat "$TMP_DIR/asan_server.log")"
fi

# Restore standard release build
make clean >/dev/null 2>&1
make >/dev/null 2>&1

# ------------------------------------------------------------------------------
# SUMMARY
# ------------------------------------------------------------------------------
echo -e "\n${BLUE}======================================================================${NC}"
echo -e "  VALIDATION COMPLETE: ${GREEN}${PASS_COUNT} PASSED${NC}, ${RED}${FAIL_COUNT} FAILED${NC}"
echo -e "${BLUE}======================================================================${NC}"

if [ $FAIL_COUNT -eq 0 ]; then
    exit 0
else
    exit 1
fi
