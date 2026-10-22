#!/usr/bin/env bash
# ==============================================================================
# test_case2.sh - Automated Comprehensive Validation Suite for Part 2 Case 2
# (Fault-Tolerant Session Management & Checkpoint Resumption)
# ==============================================================================
set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

PASSED_COUNT=0
FAILED_COUNT=0

pass() {
    echo -e "  ${GREEN}[PASS]${NC} $1"
    PASSED_COUNT=$((PASSED_COUNT + 1))
}

fail() {
    echo -e "  ${RED}[FAIL]${NC} $1"
    FAILED_COUNT=$((FAILED_COUNT + 1))
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VARIANT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TMP_DIR=$(mktemp -d /tmp/it305_case2_test.XXXXXX)

cleanup() {
    if [ -n "${SERVER_PID:-}" ]; then
        kill -9 "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

cd "$VARIANT_DIR"

echo -e "\n${CYAN}======================================================================${NC}"
echo -e "${CYAN}           IT305 PART 2 CASE 2 COMPREHENSIVE TEST SUITE               ${NC}"
echo -e "${CYAN}======================================================================${NC}"

# ------------------------------------------------------------------------------
# Test 1: Strict Compilation Diagnostics (-Werror)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 1: Strict Build Diagnostics (-Werror) ---${NC}"
make clean >/dev/null 2>&1
if make CFLAGS="-std=gnu11 -O2 -Wall -Wextra -Wpedantic -Werror -pthread" >/dev/null 2>&1; then
    pass "Clean build with -std=gnu11 -O2 -Wall -Wextra -Wpedantic -Werror succeeded"
else
    fail "Build produced compiler warnings or errors"
fi

if [ -x ./server ] && [ -x ./client ]; then
    pass "server and client binaries generated and executable"
else
    fail "server or client executable missing"
fi

# ------------------------------------------------------------------------------
# Test 2: Server & Client Argument Validation
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 2: CLI Argument & Probability Validation ---${NC}"

set +e
./server >/dev/null 2>&1
[ $? -eq 2 ] && pass "Server rejects 0 arguments (exit code 2)" || fail "Server did not reject 0 arguments"

./server 0 /tmp 0.0 >/dev/null 2>&1
[ $? -eq 2 ] && pass "Server rejects port 0" || fail "Server accepted port 0"

./server 70000 /tmp 0.0 >/dev/null 2>&1
[ $? -eq 2 ] && pass "Server rejects port > 65535" || fail "Server accepted port > 65535"

./server 5000 /tmp nan >/dev/null 2>&1
[ $? -eq 2 ] && pass "Server rejects probability 'nan'" || fail "Server accepted probability 'nan'"

./server 5000 /tmp inf >/dev/null 2>&1
[ $? -eq 2 ] && pass "Server rejects probability 'inf'" || fail "Server accepted probability 'inf'"

./server 5000 /tmp -0.1 >/dev/null 2>&1
[ $? -eq 2 ] && pass "Server rejects negative probability (-0.1)" || fail "Server accepted negative probability"

./server 5000 /tmp 1.1 >/dev/null 2>&1
[ $? -eq 2 ] && pass "Server rejects probability > 1.0 (1.1)" || fail "Server accepted probability > 1.0"

./server 5000 /tmp "invalid_prob" >/dev/null 2>&1
[ $? -eq 2 ] && pass "Server rejects non-numeric probability" || fail "Server accepted non-numeric probability"

./server 5000 "$TMP_DIR/nonexistent_root" 0.0 >/dev/null 2>&1
[ $? -ne 0 ] && pass "Server rejects non-existent topics directory" || fail "Server accepted non-existent directory"

./client >/dev/null 2>&1
[ $? -eq 2 ] && pass "Client rejects missing arguments" || fail "Client accepted missing arguments"

./client 127.0.0.1 5000 "../traversal" "$TMP_DIR" >/dev/null 2>&1
[ $? -eq 2 ] && pass "Client rejects path traversal in topic name" || fail "Client accepted traversal topic"

./client 127.0.0.1 0 "os" "$TMP_DIR" >/dev/null 2>&1
[ $? -eq 2 ] && pass "Client rejects invalid port 0" || fail "Client accepted port 0"
set -e

# ------------------------------------------------------------------------------
# Test 3: Fixture Setup (Multi-File Topics, Large Files, Empty Files)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 3: Generating Test Fixtures ---${NC}"
TOPICS_DIR="$TMP_DIR/topics"
OUT_DIR="$TMP_DIR/client_out"
mkdir -p "$TOPICS_DIR/os" "$TOPICS_DIR/net" "$TOPICS_DIR/empty_topic" "$OUT_DIR"

# 'os' topic: 3 files (small, medium 256KB = 4 chunks, empty 0B)
echo "Operating Systems Kernel Notes" > "$TOPICS_DIR/os/intro.txt"
dd if=/dev/urandom of="$TOPICS_DIR/os/blob.dat" bs=65536 count=4 status=none
touch "$TOPICS_DIR/os/empty.txt"

# 'net' topic: 2 files (512KB = 8 chunks, 128KB = 2 chunks)
dd if=/dev/urandom of="$TOPICS_DIR/net/chunk_stream.bin" bs=65536 count=8 status=none
dd if=/dev/urandom of="$TOPICS_DIR/net/headers.bin" bs=65536 count=2 status=none

pass "Test fixtures created with varied chunk counts and empty files"

PORT=20101
./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_base.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

# ------------------------------------------------------------------------------
# Test 4: Baseline Transfer (p = 0.0) — Integrity & Zero Retransmission
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 4: Baseline Zero-Fault Transfer (p = 0.0) ---${NC}"
CLIENT_RES=$(./client 127.0.0.1 $PORT os "$OUT_DIR" 5)

if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Client completed transfer with status=OK"
else
    fail "Client failed transfer: $CLIENT_RES"
fi

if echo "$CLIENT_RES" | grep -q "attempts=1"; then
    pass "Completed in exactly 1 attempt without retries"
else
    fail "Unexpected attempt count: $CLIENT_RES"
fi

if echo "$CLIENT_RES" | grep -q "retransmitted_bytes=0"; then
    pass "Retransmitted payload bytes is 0"
else
    fail "Retransmitted bytes is non-zero: $CLIENT_RES"
fi

# Compare file hashes
diff -r "$TOPICS_DIR/os" "$OUT_DIR/os"
pass "Output directory matches server topic exactly byte-for-byte"

# Verify .staging and .session files cleaned up after success
if [ -d "$OUT_DIR/.staging_os" ] || [ -f "$OUT_DIR/.session_os.chk" ]; then
    fail "Staging dir or checkpoint file left behind after successful transfer"
else
    pass "Staging dir and checkpoint file removed upon atomic publication"
fi

# ------------------------------------------------------------------------------
# Test 5: Deterministic Checkpoint Resumption (Single Interruption)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 5: Deterministic Checkpoint Resumption (Mid-Stream Drop) ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=20102
# Inject failure on chunk 3 (after chunks 0, 1, 2 have been committed)
IT305_TEST_FAIL_ON_CHUNK=3 IT305_TEST_FAIL_COUNT=1 ./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_resume.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

CLIENT_RES=$(./client 127.0.0.1 $PORT net "$OUT_DIR" 5)

if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Client resumed and completed successfully (status=OK)"
else
    fail "Client failed resumption: $CLIENT_RES"
fi

if echo "$CLIENT_RES" | grep -q "attempts=2" && echo "$CLIENT_RES" | grep -q "retries=1"; then
    pass "Exact attempt accounting: attempts=2, retries=1"
else
    fail "Incorrect attempt accounting: $CLIENT_RES"
fi

# Critical Case 2 Invariant: zero retransmitted payload bytes for committed checkpoints!
if echo "$CLIENT_RES" | grep -q "retransmitted_bytes=0"; then
    pass "Zero redundant payload retransmission for committed chunks"
else
    fail "Committed chunks were retransmitted unnecessarily: $CLIENT_RES"
fi

diff -r "$TOPICS_DIR/net" "$OUT_DIR/net"
pass "Resumed files match server topic byte-for-byte"

# ------------------------------------------------------------------------------
# Test 6: Server Crash / Reboot Session Recovery
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 6: Server Crash / Kill -9 Reboot Recovery ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=20103
# Start Server 1: will fail on chunk 2
IT305_TEST_FAIL_ON_CHUNK=2 IT305_TEST_FAIL_COUNT=1 ./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_s1.log" 2>&1 &
S1_PID=$!
sleep 0.5

# Start client in background
./client 127.0.0.1 $PORT net "$OUT_DIR" 10 > "$TMP_DIR/client_reboot.log" 2>&1 &
C_PID=$!

# Wait for Server 1 to inject failure and kill it hard
sleep 0.2
kill -9 "$S1_PID" 2>/dev/null || true
wait "$S1_PID" 2>/dev/null || true
sleep 0.2

# Start fresh Server 2 on the SAME port and topic root (loading persistent .sessions/)
./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_s2.log" 2>&1 &
SERVER_PID=$!

# Wait for client to reconnect to Server 2 and complete
wait "$C_PID" || true

CLIENT_RES=$(cat "$TMP_DIR/client_reboot.log" | grep "RESULT topic=net" || true)
if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Client recovered across server crash and reboot"
else
    fail "Client failed across server reboot: $CLIENT_RES"
fi

diff -r "$TOPICS_DIR/net" "$OUT_DIR/net"
pass "Reboot-recovered files match byte-for-byte"

# ------------------------------------------------------------------------------
# Test 7: Stale / Corrupted Checkpoint Fallback
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 7: Stale / Corrupted Checkpoint Fallback ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=20104
./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_stale.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

# Manually create a bogus checkpoint file with a non-existent session ID
cat << 'EOF' > "$OUT_DIR/.session_os.chk"
DEADBEEF000000000000000000000000
os
0000000000000000000000000000000000000000000000000000000000000000
1
intro.txt
EOF

# Client should attempt resume, get ERR_SESSION_NOT_FOUND, discard checkpoint, and fetch fresh
CLIENT_RES=$(./client 127.0.0.1 $PORT os "$OUT_DIR" 5 2>"$TMP_DIR/stale_err.log")

if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Client gracefully recovered from non-existent session ID"
else
    fail "Client failed with corrupted session ID: $CLIENT_RES"
fi

diff -r "$TOPICS_DIR/os" "$OUT_DIR/os"
pass "Output matches byte-for-byte after recovering from stale session"

# ------------------------------------------------------------------------------
# Test 8: Modified Topic Manifest Checksum Mismatch Detection
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 8: Topic Content Modification Detection ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=20105
# Fail at chunk 2 to leave a valid checkpoint
IT305_TEST_FAIL_ON_CHUNK=2 IT305_TEST_FAIL_COUNT=1 ./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_mod.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

# Run client with max_attempts=1 so it exits after failure, leaving checkpoint intact
set +e
./client 127.0.0.1 $PORT net "$OUT_DIR" 1 >/dev/null 2>&1
set -e

[ -f "$OUT_DIR/.session_net.chk" ] && pass "Checkpoint file exists after interrupted transfer" || fail "Checkpoint missing"

# Now alter the server topic (change content of headers.bin)
echo "tampered content" >> "$TOPICS_DIR/net/headers.bin"

# Restart server so it scans altered topic
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_mod2.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

# Client resumes: server must reject resume due to ERR_CHECKSUM_MISMATCH, client restarts fresh
CLIENT_RES=$(./client 127.0.0.1 $PORT net "$OUT_DIR" 5 2>"$TMP_DIR/mod_err.log")
if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Client detected server content change, reset session, and completed transfer"
else
    fail "Client failed after topic alteration: $CLIENT_RES"
fi

diff -r "$TOPICS_DIR/net" "$OUT_DIR/net"
pass "Topic content matches the updated server version"

# Revert tampered content
dd if=/dev/urandom of="$TOPICS_DIR/net/headers.bin" bs=65536 count=2 status=none

# ------------------------------------------------------------------------------
# Test 9: Probabilistic Fault Injection (p = 0.20)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 9: Bernoulli Fault Injection (p = 0.20) ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=20106
./server $PORT "$TOPICS_DIR" 0.20 > "$TMP_DIR/server_p20.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

CLIENT_RES=$(./client 127.0.0.1 $PORT net "$OUT_DIR" 20)
if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Client converged under p=0.20 failure probability"
else
    fail "Client failed to converge under p=0.20: $CLIENT_RES"
fi

diff -r "$TOPICS_DIR/net" "$OUT_DIR/net"
pass "Files intact under random drop conditions"

# ------------------------------------------------------------------------------
# Test 10: Deterministic Failure Ceiling (p = 1.0)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 10: 100% Failure Ceiling (p = 1.0) ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=20107
./server $PORT "$TOPICS_DIR" 1.0 > "$TMP_DIR/server_p100.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

set +e
CLIENT_RES=$(./client 127.0.0.1 $PORT os "$OUT_DIR" 3 2>/dev/null)
CLIENT_EXIT=$?
set -e

if [ $CLIENT_EXIT -ne 0 ] && echo "$CLIENT_RES" | grep -q "status=FAIL"; then
    pass "Client terminates cleanly with status=FAIL on p=1.0 without hanging"
else
    fail "Client did not terminate cleanly on p=1.0: exit=$CLIENT_EXIT, res=$CLIENT_RES"
fi

if echo "$CLIENT_RES" | grep -q "attempts=3"; then
    pass "Client respected max_attempts=3 bound"
else
    fail "Client did not respect max_attempts: $CLIENT_RES"
fi

# ------------------------------------------------------------------------------
# Test 11: Concurrent Clients
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 11: Concurrent Client Resumptions ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=20108
./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_conc.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

OUT1="$TMP_DIR/c1"
OUT2="$TMP_DIR/c2"
mkdir -p "$OUT1" "$OUT2"

./client 127.0.0.1 $PORT os "$OUT1" 5 > "$TMP_DIR/c1.log" 2>&1 &
PID_C1=$!
./client 127.0.0.1 $PORT net "$OUT2" 5 > "$TMP_DIR/c2.log" 2>&1 &
PID_C2=$!

wait $PID_C1
wait $PID_C2

diff -r "$TOPICS_DIR/os" "$OUT1/os" && pass "Concurrent client 1 verified" || fail "Concurrent client 1 corrupted"
diff -r "$TOPICS_DIR/net" "$OUT2/net" && pass "Concurrent client 2 verified" || fail "Concurrent client 2 corrupted"

# ------------------------------------------------------------------------------
# Test 12: AddressSanitizer and UndefinedBehaviorSanitizer (ASan + UBSan)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 12: Memory Safety (ASan + UBSan) ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

make clean >/dev/null 2>&1
make CFLAGS="-std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread" >/dev/null 2>&1

PORT=20109
IT305_TEST_FAIL_ON_CHUNK=2 IT305_TEST_FAIL_COUNT=1 ./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_asan.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

./client 127.0.0.1 $PORT os "$OUT_DIR" 5 > "$TMP_DIR/client_asan.log" 2>&1 || true

kill -INT "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

if grep -qi "AddressSanitizer" "$TMP_DIR/server_asan.log" "$TMP_DIR/client_asan.log" || \
   grep -qi "runtime error:" "$TMP_DIR/server_asan.log" "$TMP_DIR/client_asan.log"; then
    fail "Sanitizer detected memory safety or undefined behavior defect"
else
    pass "Clean run under AddressSanitizer & UndefinedBehaviorSanitizer (no leaks/UB)"
fi

# Rebuild release binary
make clean >/dev/null 2>&1
make >/dev/null 2>&1

# ------------------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------------------
echo -e "\n${CYAN}======================================================================${NC}"
echo -e "${CYAN}                      CASE 2 TEST RESULTS SUMMARY                      ${NC}"
echo -e "${CYAN}======================================================================${NC}"
echo -e "  TOTAL PASSED: ${GREEN}${PASSED_COUNT}${NC}"
echo -e "  TOTAL FAILED: ${RED}${FAILED_COUNT}${NC}"

if [ "$FAILED_COUNT" -eq 0 ]; then
    echo -e "\n${GREEN}ALL CASE 2 TESTS PASSED PERFECTLY!${NC}\n"
    exit 0
else
    echo -e "\n${RED}SOME CASE 2 TESTS FAILED.${NC}\n"
    exit 1
fi
