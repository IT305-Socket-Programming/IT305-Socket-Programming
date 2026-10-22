#!/usr/bin/env bash
# ==============================================================================
# test_case2_enhanced.sh - Automated Validation Suite for Part 2 Case 2 Enhanced
# (Parallel TCP Streams with Disjoint Range Checkpointing)
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
TMP_DIR=$(mktemp -d /tmp/it305_c2e_test.XXXXXX)

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
echo -e "${CYAN}       IT305 PART 2 CASE 2 ENHANCED COMPREHENSIVE TEST SUITE          ${NC}"
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
# Test 2: CLI Argument Validation
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 2: CLI Argument Validation ---${NC}"
set +e
./server >/dev/null 2>&1
[ $? -eq 2 ] && pass "Server rejects 0 arguments" || fail "Server accepted 0 arguments"

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

./server 5000 "$TMP_DIR/nonexistent_root" 0.0 >/dev/null 2>&1
[ $? -ne 0 ] && pass "Server rejects non-existent topics directory" || fail "Server accepted non-existent directory"

./client >/dev/null 2>&1
[ $? -eq 2 ] && pass "Client rejects missing arguments" || fail "Client accepted missing arguments"

./client 127.0.0.1 5000 "../traversal" "$TMP_DIR" >/dev/null 2>&1
[ $? -eq 2 ] && pass "Client rejects path traversal topic name" || fail "Client accepted traversal topic"

./client 127.0.0.1 0 "os" "$TMP_DIR" >/dev/null 2>&1
[ $? -eq 2 ] && pass "Client rejects invalid port 0" || fail "Client accepted port 0"
set -e

# ------------------------------------------------------------------------------
# Test 3: Fixture Setup
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 3: Generating Fixtures ---${NC}"
TOPICS_DIR="$TMP_DIR/topics"
OUT_DIR="$TMP_DIR/client_out"
mkdir -p "$TOPICS_DIR/os" "$TOPICS_DIR/net" "$OUT_DIR"

# 'os' topic: 3 files (small, 512KB = 8 chunks, 256KB = 4 chunks)
echo "Operating Systems Kernel Notes" > "$TOPICS_DIR/os/intro.txt"
dd if=/dev/urandom of="$TOPICS_DIR/os/kernel.bin" bs=65536 count=8 status=none
dd if=/dev/urandom of="$TOPICS_DIR/os/modules.bin" bs=65536 count=4 status=none

# 'net' topic: 2 files (1MB = 16 chunks, 128KB = 2 chunks)
dd if=/dev/urandom of="$TOPICS_DIR/net/stream.bin" bs=65536 count=16 status=none
dd if=/dev/urandom of="$TOPICS_DIR/net/headers.bin" bs=65536 count=2 status=none

pass "Test fixtures created"

PORT=21101
./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_base.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

# ------------------------------------------------------------------------------
# Test 4: Baseline Multi-Stream Transfers (K=1, K=2, K=4)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 4: Baseline Parallel Transfers (K=1, 2, 4 streams) ---${NC}"

# K=1 stream
OUT_K1="$TMP_DIR/out_k1"
mkdir -p "$OUT_K1"
RES_K1=$(./client 127.0.0.1 $PORT os "$OUT_K1" 1 5)
if echo "$RES_K1" | grep -q "status=OK"; then
    pass "1-stream transfer completed with status=OK"
else
    fail "1-stream transfer failed: $RES_K1"
fi
diff -r "$TOPICS_DIR/os" "$OUT_K1/os" && pass "1-stream files match byte-for-byte" || fail "1-stream files corrupted"

# K=2 streams
OUT_K2="$TMP_DIR/out_k2"
mkdir -p "$OUT_K2"
RES_K2=$(./client 127.0.0.1 $PORT os "$OUT_K2" 2 5)
if echo "$RES_K2" | grep -q "status=OK"; then
    pass "2-stream transfer completed with status=OK"
else
    fail "2-stream transfer failed: $RES_K2"
fi
diff -r "$TOPICS_DIR/os" "$OUT_K2/os" && pass "2-stream files match byte-for-byte" || fail "2-stream files corrupted"

# K=4 streams
OUT_K4="$TMP_DIR/out_k4"
mkdir -p "$OUT_K4"
RES_K4=$(./client 127.0.0.1 $PORT os "$OUT_K4" 4 5)
if echo "$RES_K4" | grep -q "status=OK"; then
    pass "4-stream transfer completed with status=OK"
else
    fail "4-stream transfer failed: $RES_K2"
fi
diff -r "$TOPICS_DIR/os" "$OUT_K4/os" && pass "4-stream files match byte-for-byte" || fail "4-stream files corrupted"

# ------------------------------------------------------------------------------
# Test 5: Deterministic Checkpoint Resumption (Parallel Stream Drop)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 5: Deterministic Parallel Stream Drop & Resumption ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=21102
# Inject failure on chunk 4
IT305_TEST_FAIL_ON_CHUNK=4 IT305_TEST_FAIL_COUNT=4 ./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_resume.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

CLIENT_RES=$(./client 127.0.0.1 $PORT net "$OUT_DIR" 4 5)

if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Multi-stream client resumed and completed successfully (status=OK)"
else
    fail "Multi-stream client failed resumption: $CLIENT_RES"
fi

if echo "$CLIENT_RES" | grep -q "attempts=2" && echo "$CLIENT_RES" | grep -q "retries=1"; then
    pass "Exact multi-stream attempt accounting: attempts=2, retries=1"
else
    fail "Incorrect attempt accounting: $CLIENT_RES"
fi

# Zero retransmission of committed blocks
if echo "$CLIENT_RES" | grep -q "retransmitted_bytes=0"; then
    pass "Zero redundant payload retransmission for committed blocks in multi-stream transfer"
else
    fail "Committed blocks retransmitted: $CLIENT_RES"
fi

diff -r "$TOPICS_DIR/net" "$OUT_DIR/net"
pass "Multi-stream resumed files match server topic byte-for-byte"

# ------------------------------------------------------------------------------
# Test 6: Server Crash / Reboot Multi-Stream Recovery
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 6: Server Crash & Reboot Multi-Stream Recovery ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=21103
IT305_TEST_FAIL_ON_CHUNK=3 IT305_TEST_FAIL_COUNT=1 ./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_s1.log" 2>&1 &
S1_PID=$!
sleep 0.5

./client 127.0.0.1 $PORT net "$OUT_DIR" 4 10 > "$TMP_DIR/client_reboot.log" 2>&1 &
C_PID=$!

sleep 0.2
kill -9 "$S1_PID" 2>/dev/null || true
wait "$S1_PID" 2>/dev/null || true
sleep 0.2

# Start Server 2
./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_s2.log" 2>&1 &
SERVER_PID=$!

wait "$C_PID" || true

CLIENT_RES=$(cat "$TMP_DIR/client_reboot.log" | grep "RESULT topic=net" || true)
if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Multi-stream client recovered across server reboot"
else
    fail "Multi-stream client failed reboot recovery: $CLIENT_RES"
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

PORT=21104
./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_stale.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

cat << 'EOF' > "$OUT_DIR/.session_os.chk"
DEADBEEF000000000000000000000000
os
0000000000000000000000000000000000000000000000000000000000000000
1
intro.txt 30 0000000000000000000000000000000000000000000000000000000000000000
EOF

CLIENT_RES=$(./client 127.0.0.1 $PORT os "$OUT_DIR" 4 5 2>"$TMP_DIR/stale_err.log")
if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Multi-stream client gracefully recovered from non-existent session ID"
else
    fail "Multi-stream client failed corrupted session fallback: $CLIENT_RES"
fi

diff -r "$TOPICS_DIR/os" "$OUT_DIR/os"
pass "Output matches byte-for-byte after recovering from stale session"

# ------------------------------------------------------------------------------
# Test 8: Topic Alteration Detection
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 8: Topic Content Modification Detection ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=21105
IT305_TEST_FAIL_ON_CHUNK=2 IT305_TEST_FAIL_COUNT=1 ./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_mod.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

set +e
./client 127.0.0.1 $PORT net "$OUT_DIR" 1 1 >/dev/null 2>&1
set -e

[ -f "$OUT_DIR/.session_net.chk" ] && pass "Checkpoint file exists after interrupted transfer" || fail "Checkpoint missing"

# Alter file content on server
echo "tampered multi-stream content" >> "$TOPICS_DIR/net/headers.bin"

kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_mod2.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

CLIENT_RES=$(./client 127.0.0.1 $PORT net "$OUT_DIR" 4 5 2>"$TMP_DIR/mod_err.log")
if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Detected server content change, reset session, and completed transfer"
else
    fail "Failed after topic alteration: $CLIENT_RES"
fi

diff -r "$TOPICS_DIR/net" "$OUT_DIR/net"
pass "Topic content matches the updated server version"

# Revert tampered content
dd if=/dev/urandom of="$TOPICS_DIR/net/headers.bin" bs=65536 count=2 status=none

# ------------------------------------------------------------------------------
# Test 9: Bernoulli Fault Injection (p = 0.20)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 9: Bernoulli Fault Injection (p = 0.20) ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=21106
./server $PORT "$TOPICS_DIR" 0.20 > "$TMP_DIR/server_p20.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

CLIENT_RES=$(./client 127.0.0.1 $PORT net "$OUT_DIR" 4 20)
if echo "$CLIENT_RES" | grep -q "status=OK"; then
    pass "Multi-stream client converged under p=0.20 failure probability"
else
    fail "Multi-stream client failed to converge under p=0.20: $CLIENT_RES"
fi

diff -r "$TOPICS_DIR/net" "$OUT_DIR/net"
pass "Files intact under random drop conditions"

# ------------------------------------------------------------------------------
# Test 10: 100% Failure Ceiling (p = 1.0)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 10: 100% Failure Ceiling (p = 1.0) ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=21107
./server $PORT "$TOPICS_DIR" 1.0 > "$TMP_DIR/server_p100.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

set +e
CLIENT_RES=$(./client 127.0.0.1 $PORT os "$OUT_DIR" 4 3 2>/dev/null)
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
# Test 11: Concurrent Multi-Stream Clients
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 11: Concurrent Multi-Stream Clients ---${NC}"
kill -9 "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$OUT_DIR" "$TOPICS_DIR/.sessions"
mkdir -p "$OUT_DIR"

PORT=21108
./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_conc.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

OUT1="$TMP_DIR/c1"
OUT2="$TMP_DIR/c2"
mkdir -p "$OUT1" "$OUT2"

./client 127.0.0.1 $PORT os "$OUT1" 4 5 > "$TMP_DIR/c1.log" 2>&1 &
PID_C1=$!
./client 127.0.0.1 $PORT net "$OUT2" 4 5 > "$TMP_DIR/c2.log" 2>&1 &
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

PORT=21109
IT305_TEST_FAIL_ON_CHUNK=2 IT305_TEST_FAIL_COUNT=1 ./server $PORT "$TOPICS_DIR" 0.0 > "$TMP_DIR/server_asan.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

./client 127.0.0.1 $PORT os "$OUT_DIR" 4 5 > "$TMP_DIR/client_asan.log" 2>&1 || true

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
echo -e "${CYAN}                 CASE 2 ENHANCED TEST RESULTS SUMMARY                 ${NC}"
echo -e "${CYAN}======================================================================${NC}"
echo -e "  TOTAL PASSED: ${GREEN}${PASSED_COUNT}${NC}"
echo -e "  TOTAL FAILED: ${RED}${FAILED_COUNT}${NC}"

if [ "$FAILED_COUNT" -eq 0 ]; then
    echo -e "\n${GREEN}ALL CASE 2 ENHANCED TESTS PASSED PERFECTLY!${NC}\n"
    exit 0
else
    echo -e "\n${RED}SOME CASE 2 ENHANCED TESTS FAILED.${NC}\n"
    exit 1
fi
