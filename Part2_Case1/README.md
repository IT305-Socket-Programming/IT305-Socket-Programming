# Part II — Case 1: Fault-Tolerant Topic Distribution (Restart from Beginning)

## 1. Overview

Part II Case 1 implements fault-tolerant topic-based file distribution over TCP with probabilistic server-side connection drop injection and client-side un-sessioned restart recovery.

When the server interrupts an active TCP connection during file streaming, the client discards any partial staging data received during that attempt, reconnects over a fresh TCP socket, and requests the entire topic from byte offset zero. There is **no session management and no checkpointing** in Case 1.

---

## 2. Standard CLI Interface

### Server Invocation
```bash
./server <port> <topics_root_dir> <failure_probability>
```
- `port`: TCP listening port (1..65535).
- `topics_root_dir`: Directory directly containing topic sub-folders (e.g. `raw-img`).
- `failure_probability`: Floating-point probability $p \in [0.0, 1.0]$ governing connection drops.

### Client Invocation
```bash
./client <server_ip> <server_port> <topic> <output_dir> [max_attempts]
```
- `server_ip`: IPv4 address of the server (e.g. `127.0.0.1`).
- `server_port`: TCP port of the server.
- `topic`: Topic name (e.g. `cane`, `gatto`, or alias `dog`).
- `output_dir`: Base output directory. The client writes files to `<output_dir>/<topic>/`.
- `max_attempts` (optional, default 5 or `IT305_MAX_ATTEMPTS`): Maximum total attempts.

---

## 3. Protocol Specification (v2.0 Binary Framing)

All frames share a fixed 16-byte binary header encoded strictly in network byte order:

```text
+-----------------------+-----------------------+
|  magic (uint16_t)     | version (uint8) | type|
|  0x5446 ('T','F')     |      0x02       |(u8) |
+-----------------------+-----------------------+
|  flags (uint16_t)     | status / err (uint16) |
+-----------------------+-----------------------+
|              payload_len (uint32_t)           |
+-----------------------------------------------+
|              seq_number (uint32_t)            |
+-----------------------------------------------+
```

### Message Types
1. **`MSG_GET_REQ` (0x01)**: Client -> Server. Request for a topic name (payload: null-terminated topic string, up to 64 bytes).
2. **`MSG_MANIFEST_START` (0x02)**: Server -> Client. Payload: `total_files` (uint32), `total_payload_bytes` (uint64), `manifest_hash` (32 bytes SHA-256).
3. **`MSG_MANIFEST_ENTRY` (0x03)**: Server -> Client. Payload: `file_index` (uint32), `file_size` (uint64), `file_hash` (32 bytes SHA-256), `name_len` (uint16), `file_name` (ASCII bytes).
4. **`MSG_MANIFEST_END` (0x04)**: Server -> Client. Payload: `entries_sent` (uint32), `status` (uint32 = ERR_OK).
5. **`MSG_FILE_HEADER` (0x05)**: Server -> Client. Payload: `file_index` (uint32), `file_size` (uint64), `start_offset` (uint64 = 0), `file_hash` (32 bytes), `name_len` (uint16), `file_name` (ASCII bytes).
6. **`MSG_DATA_CHUNK` (0x06)**: Server -> Client. Flags: `FLAG_EOF_FILE` (0x0001) on terminal chunk. Payload: raw file slice (up to 64 KB).
7. **`MSG_TRANSFER_DONE` (0x07)**: Server -> Client. Payload: `total_files` (uint32), `total_payload_bytes` (uint64), `manifest_hash` (32 bytes SHA-256).
8. **`MSG_ERROR` (0x0F)**: Bidirectional. Payload: `error_code` (uint16), `msg_len` (uint16), `error_message` (ASCII bytes).

---

## 4. Failure Injection Model

1. **Eligible Decision Point:** Evaluated immediately before sending each non-empty data chunk (`MSG_DATA_CHUNK`). Empty files (0 bytes) generate no data chunks and thus do not trigger failure decisions.
2. **Bernoulli Trial:** For each eligible chunk, the server samples a pseudo-random float $r \in [0.0, 1.0)$ using thread-safe `rand_r(&seed)`. If $r < p$, failure is injected.
3. **Connection Interruption:** When a fault is injected, the server immediately calls `shutdown(fd, SHUT_RDWR)` and terminates the worker without sending `MSG_TRANSFER_DONE` or `MSG_ERROR`.
4. **Deterministic Testing Hooks:** Controlled via environment variables:
   - `IT305_TEST_FAIL_AFTER_CHUNKS=<N>`: Injects failure immediately before chunk $N+1$.
   - `IT305_TEST_FAIL_ON_CHUNK=<K>`: Injects failure immediately before chunk $K$.
   When unset, behavior is strictly probabilistic according to $p$.

---

## 5. Case 1 Retry and Staging Semantics

1. **Attempt Staging Isolation:** Each attempt $k \in [1, A]$ streams into an isolated staging directory:
   `<output_dir>/.staging_<topic>_<pid>_<attempt>/`
2. **Detection & Cleanup:** If the socket drops or an error occurs during attempt $k$:
   - The attempt is recorded as failed.
   - The entire staging directory `.staging_<topic>_<pid>_<attempt>` is deleted recursively (`rm_rf`).
   - If the error is retryable and $k < A$, the client executes backoff and initiates attempt $k+1$.
3. **Restart from Beginning:** Every retry establishes a new TCP connection and requests the topic starting from File 0, Byte Offset 0.
4. **Atomic Publication:** Only after `MSG_TRANSFER_DONE` is received and all file hashes match the manifest does the client atomically rename the staging directory to `<output_dir>/<topic>/`.

---

## 6. Integrity and Metrics

- **Integrity:** Every file is verified using full SHA-256 digests against the manifest snapshot. In addition, the aggregate manifest hash is validated prior to streaming file data.
- **Metrics Accounting:**
  - `useful_bytes`: Total valid file content bytes published in the final successful download.
  - `transmitted_bytes`: Sum of all file payload bytes received across all attempts (useful + wasted).
  - `retransmitted_bytes`: Transmitted payload bytes minus useful payload bytes ($B_{tx} - B_u$).
  - `protocol_bytes`: All framing header and control payload bytes.
- **Result Line:**
  ```text
  RESULT topic=<topic> status=<OK|FAIL> attempts=<A> retries=<R> useful_bytes=<b> transmitted_bytes=<b> retransmitted_bytes=<b> protocol_bytes=<b> elapsed_sec=<s> mbps=<m>
  ```

---

## 7. Build and Test Instructions

```bash
cd Part2_Case1
make clean
make
make test
```
