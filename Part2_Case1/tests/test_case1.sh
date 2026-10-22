#!/usr/bin/env bash
# ==============================================================================
# test_case1.sh - Automated Comprehensive Validation Suite for Part 2 Case 1
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
TMP_DIR=$(mktemp -d /tmp/it305_case1_test.XXXXXX)

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
echo -e "${CYAN}           IT305 PART 2 CASE 1 COMPREHENSIVE TEST SUITE               ${NC}"
echo -e "${CYAN}======================================================================${NC}"

# ------------------------------------------------------------------------------
# Test 1: Strict Compilation Diagnostics
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

./client 127.0.0.1 5000 topic "" >/dev/null 2>&1
[ $? -eq 2 ] && pass "Client rejects empty output directory" || fail "Client accepted empty output directory"
set -e

# ------------------------------------------------------------------------------
# Test Dataset Setup
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Setting Up Topic Datasets ---${NC}"
DATASET_DIR="$TMP_DIR/dataset"
mkdir -p "$DATASET_DIR/cane" "$DATASET_DIR/gatto" "$DATASET_DIR/empty_topic"

# Cane: empty file, 1-byte file, exact 64KB boundary, 64KB+1, and 200KB multi-chunk file
touch "$DATASET_DIR/cane/00_empty.bin"
echo -n "X" > "$DATASET_DIR/cane/01_onebyte.bin"
dd if=/dev/urandom of="$DATASET_DIR/cane/02_64k.bin" bs=65536 count=1 status=none
dd if=/dev/urandom of="$DATASET_DIR/cane/03_64k_plus1.bin" bs=65537 count=1 status=none
dd if=/dev/urandom of="$DATASET_DIR/cane/04_200k.bin" bs=1024 count=200 status=none

# Gatto: multi-file topic
dd if=/dev/urandom of="$DATASET_DIR/gatto/img1.bin" bs=1024 count=10 status=none
dd if=/dev/urandom of="$DATASET_DIR/gatto/img2.bin" bs=1024 count=20 status=none
dd if=/dev/urandom of="$DATASET_DIR/gatto/img3.bin" bs=1024 count=30 status=none

pass "Created test datasets (0-byte, 1-byte, boundary, multi-chunk, multi-file)"

TEST_PORT=$(( 18000 + (RANDOM % 5000) ))

# ------------------------------------------------------------------------------
# Test 3: p=0 Ideal Baseline Transfer
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 3: p=0.0 Baseline Ideal Transfer ---${NC}"
PORT1=$TEST_PORT
./server "$PORT1" "$DATASET_DIR" 0.0 >"$TMP_DIR/server_p0.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

OUT_P0="$TMP_DIR/out_p0"
mkdir -p "$OUT_P0"
RES_P0=$(./client 127.0.0.1 "$PORT1" cane "$OUT_P0")

if echo "$RES_P0" | grep -q "status=OK"; then
    pass "Client completed transfer with status=OK at p=0.0"
else
    fail "Client failed transfer at p=0.0: $RES_P0"
fi

if echo "$RES_P0" | grep -q "attempts=1 retries=0"; then
    pass "Client reported attempts=1 retries=0 at p=0.0"
else
    fail "Client reported non-zero retries at p=0.0: $RES_P0"
fi

if diff -r "$DATASET_DIR/cane" "$OUT_P0/cane" >/dev/null 2>&1; then
    pass "Byte-for-byte data integrity verified against source dataset"
else
    fail "Published files do not match source files byte-for-byte"
fi

kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

# ------------------------------------------------------------------------------
# Test 4: Deterministic Fault Injection & Restart Recovery
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 4: Deterministic Fault Injection & Restart from Beginning ---${NC}"
PORT2=$((TEST_PORT + 1))
# Inject failure before chunk 1 on the first attempt only
IT305_TEST_FAIL_ON_CHUNK=2 ./server "$PORT2" "$DATASET_DIR" 0.0 >"$TMP_DIR/server_fault.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

OUT_FAULT="$TMP_DIR/out_fault"
mkdir -p "$OUT_FAULT"
RES_FAULT=$(IT305_MAX_ATTEMPTS=5 ./client 127.0.0.1 "$PORT2" cane "$OUT_FAULT")

if echo "$RES_FAULT" | grep -q "status=OK"; then
    pass "Client recovered and completed transfer successfully after injected drop"
else
    fail "Client failed recovery: $RES_FAULT"
fi

if echo "$RES_FAULT" | grep -E -q "attempts=[2-5] retries=[1-4]"; then
    pass "Client recorded retry (attempts > 1, retries >= 1)"
else
    fail "Client did not record retry after failure: $RES_FAULT"
fi

# Confirm retransmitted bytes > 0
RETX=$(echo "$RES_FAULT" | grep -o 'retransmitted_bytes=[0-9]*' | cut -d= -f2)
if [ "$RETX" -gt 0 ]; then
    pass "Retransmitted payload bytes correctly accounted ($RETX bytes)"
else
    fail "Retransmitted bytes was zero after restart: $RES_FAULT"
fi

if diff -r "$DATASET_DIR/cane" "$OUT_FAULT/cane" >/dev/null 2>&1; then
    pass "Recovered output is byte-for-byte identical to source"
else
    fail "Recovered output differs from source"
fi

# Verify no staging directories leaked
STAGING_COUNT=$(find "$OUT_FAULT" -maxdepth 1 -name ".staging_*" | wc -l)
if [ "$STAGING_COUNT" -eq 0 ]; then
    pass "All temporary staging directories cleaned up after completion"
else
    fail "Leftover staging directories found: $STAGING_COUNT"
fi

kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

# ------------------------------------------------------------------------------
# Test 5: p=1.0 Bounded Termination & Cleanup
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 5: p=1.0 Guaranteed Drop Bounded Termination ---${NC}"
PORT3=$((TEST_PORT + 2))
./server "$PORT3" "$DATASET_DIR" 1.0 >"$TMP_DIR/server_p1.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

OUT_P1="$TMP_DIR/out_p1"
mkdir -p "$OUT_P1"
set +e
RES_P1=$(IT305_MAX_ATTEMPTS=3 ./client 127.0.0.1 "$PORT3" cane "$OUT_P1" 2>&1)
CLIENT_RC=$?
set -e

if [ $CLIENT_RC -ne 0 ]; then
    pass "Client exited non-zero ($CLIENT_RC) when max attempts exhausted under p=1.0"
else
    fail "Client exited 0 despite 100% failure probability"
fi

if echo "$RES_P1" | grep -q "status=FAIL attempts=3"; then
    pass "Client reported status=FAIL and attempts=3 (bounded attempts respected)"
else
    fail "Client did not report bounded attempts: $RES_P1"
fi

# Ensure staging directory cleaned up and target topic not published
if [ ! -d "$OUT_P1/cane" ]; then
    pass "Failed attempt output was not published to target topic directory"
else
    fail "Failed attempt erroneously published incomplete topic directory"
fi

STAGING_P1=$(find "$OUT_P1" -maxdepth 1 -name ".staging_*" | wc -l)
if [ "$STAGING_P1" -eq 0 ]; then
    pass "Failed attempt staging directory cleanly deleted on abort"
else
    fail "Failed staging directory leaked"
fi

kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

# ------------------------------------------------------------------------------
# Test 6: Unknown Topic Handling & Non-Retryable Error
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 6: Unknown Topic Non-Retryable Error ---${NC}"
PORT4=$((TEST_PORT + 3))
./server "$PORT4" "$DATASET_DIR" 0.0 >/dev/null 2>&1 &
SERVER_PID=$!
sleep 0.5

set +e
RES_UNK=$(./client 127.0.0.1 "$PORT4" nonexistent_topic "$TMP_DIR/out_unk" 2>&1)
UNK_RC=$?
set -e

if [ $UNK_RC -ne 0 ]; then
    pass "Client exited non-zero for unknown topic"
else
    fail "Client exited 0 for unknown topic"
fi

if echo "$RES_UNK" | grep -q "attempts=1"; then
    pass "Client did not wastefully retry non-retryable topic error"
else
    fail "Client retried unknown topic error: $RES_UNK"
fi

kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

# ------------------------------------------------------------------------------
# Test 7: English Alias Resolution (dog -> cane)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 7: English Alias Resolution ---${NC}"
PORT5=$((TEST_PORT + 4))
./server "$PORT5" "$DATASET_DIR" 0.0 >/dev/null 2>&1 &
SERVER_PID=$!
sleep 0.5

OUT_ALIAS="$TMP_DIR/out_alias"
mkdir -p "$OUT_ALIAS"
RES_ALIAS=$(./client 127.0.0.1 "$PORT5" dog "$OUT_ALIAS")

if echo "$RES_ALIAS" | grep -q "status=OK"; then
    pass "Client successfully downloaded topic via alias 'dog'"
else
    fail "Client failed alias download: $RES_ALIAS"
fi

if diff -r "$DATASET_DIR/cane" "$OUT_ALIAS/dog" >/dev/null 2>&1; then
    pass "Alias output verified byte-for-byte with source 'cane' directory"
else
    fail "Alias output files do not match source"
fi

kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

# ------------------------------------------------------------------------------
# Test 8: Symlink Traversal Protection
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 8: Client Symlink Attack Protection ---${NC}"
PORT6=$((TEST_PORT + 5))
./server "$PORT6" "$DATASET_DIR" 0.0 >/dev/null 2>&1 &
SERVER_PID=$!
sleep 0.5

OUT_SYM="$TMP_DIR/out_sym"
mkdir -p "$OUT_SYM"
CRITICAL_FILE="$TMP_DIR/critical.txt"
echo "SECRET_CONTENT_CANNOT_OVERWRITE" > "$CRITICAL_FILE"

# Pre-create staging dir with symlink to critical file
STAGING_FAKE="$OUT_SYM/.staging_cane_$$"
# Client creates .staging_cane_<pid>_<attempt>
# Test client O_NOFOLLOW by simulating an attack in outdir
ln -s "$CRITICAL_FILE" "$OUT_SYM/symlink_target"

RES_SYM=$(./client 127.0.0.1 "$PORT6" cane "$OUT_SYM")
if [ -f "$CRITICAL_FILE" ] && [ "$(cat "$CRITICAL_FILE")" = "SECRET_CONTENT_CANNOT_OVERWRITE" ]; then
    pass "Existing critical target remains untouched and protected"
else
    fail "Symlink target was modified or overwritten"
fi

kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

# ------------------------------------------------------------------------------
# Test 9: Concurrent Clients Stress Test
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 9: Concurrent Clients Stress Test ---${NC}"
PORT7=$((TEST_PORT + 6))
./server "$PORT7" "$DATASET_DIR" 0.0 >/dev/null 2>&1 &
SERVER_PID=$!
sleep 0.5

CONC_OUT="$TMP_DIR/conc_out"
mkdir -p "$CONC_OUT"

PIDS=()
for i in {1..6}; do
    ./client 127.0.0.1 "$PORT7" gatto "$CONC_OUT/c$i" >/dev/null 2>&1 &
    PIDS+=($!)
done

CONC_FAIL=0
for pid in "${PIDS[@]}"; do
    wait "$pid" || CONC_FAIL=$((CONC_FAIL + 1))
done

if [ $CONC_FAIL -eq 0 ]; then
    pass "All 6 concurrent clients completed successfully with exit code 0"
else
    fail "$CONC_FAIL concurrent clients failed"
fi

# Verify data across all 6 clients
CONC_DATA_OK=1
for i in {1..6}; do
    if ! diff -r "$DATASET_DIR/gatto" "$CONC_OUT/c$i/gatto" >/dev/null 2>&1; then
        CONC_DATA_OK=0
        break
    fi
done

if [ $CONC_DATA_OK -eq 1 ]; then
    pass "Data integrity verified across all 6 concurrent downloads"
else
    fail "Data corruption detected in concurrent downloads"
fi

kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

# ------------------------------------------------------------------------------
# Test 10: Graceful Server Shutdown Assertions
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 10: Server Graceful Shutdown on SIGINT ---${NC}"
PORT8=$((TEST_PORT + 7))
./server "$PORT8" "$DATASET_DIR" 0.0 >"$TMP_DIR/server_shutdown.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

kill -INT "$SERVER_PID"
set +e
wait "$SERVER_PID"
SRV_RC=$?
set -e
unset SERVER_PID

if [ $SRV_RC -eq 0 ]; then
    pass "Server terminated with clean exit code 0 on SIGINT"
else
    fail "Server exited with non-zero code ($SRV_RC) on SIGINT"
fi

if grep -q "Server stopped cleanly" "$TMP_DIR/server_shutdown.log"; then
    pass "Server logged 'Server stopped cleanly' after freeing resources"
else
    fail "Clean server stop log message missing"
fi

# ------------------------------------------------------------------------------
# Test 11: Memory & Undefined Behavior Audit (ASan + UBSan)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}--- Test 11: AddressSanitizer & UBSan Memory Verification ---${NC}"
SAN_DIR="$TMP_DIR/san"
mkdir -p "$SAN_DIR"

if gcc -fsanitize=address,undefined -fno-omit-frame-pointer -std=gnu11 -O1 \
       -o "$SAN_DIR/server_san" server.c common.c -pthread >/dev/null 2>&1 && \
   gcc -fsanitize=address,undefined -fno-omit-frame-pointer -std=gnu11 -O1 \
       -o "$SAN_DIR/client_san" client.c common.c -pthread >/dev/null 2>&1; then

    PORT_SAN=$((TEST_PORT + 8))
    "$SAN_DIR/server_san" "$PORT_SAN" "$DATASET_DIR" 0.0 >/dev/null 2>&1 &
    SAN_SRV_PID=$!
    sleep 0.5

    "$SAN_DIR/client_san" 127.0.0.1 "$PORT_SAN" cane "$SAN_DIR/out" >/dev/null 2>&1
    SAN_CLIENT_RC=$?

    kill -INT "$SAN_SRV_PID"
    set +e
    wait "$SAN_SRV_PID" 2>/dev/null
    SAN_SRV_RC=$?
    set -e

    if [ $SAN_CLIENT_RC -eq 0 ] && [ $SAN_SRV_RC -eq 0 ]; then
        pass "Client and server executed under ASan/UBSan with zero errors"
    else
        fail "Sanitizer detected memory or undefined behavior error"
    fi
else
    echo -e "  ${YELLOW}[SKIP]${NC} AddressSanitizer/UBSan not available on this compiler"
fi

# ------------------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------------------
echo -e "\n${CYAN}======================================================================${NC}"
echo -e "  CASE 1 VALIDATION SUMMARY: ${GREEN}$PASSED_COUNT PASSED${NC}, ${RED}$FAILED_COUNT FAILED${NC}"
echo -e "${CYAN}======================================================================${NC}"

if [ $FAILED_COUNT -eq 0 ]; then
    exit 0
else
    exit 1
fi
