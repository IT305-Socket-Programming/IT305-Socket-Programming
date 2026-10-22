# Part II — Case 2: Fault-Tolerant Topic Distribution (Session & Checkpoint Resume)

## 1. Overview

Part II Case 2 implements fault-tolerant topic-based file distribution over TCP with **persistent session management** and **chunk-level checkpoint resumption**.

Unlike Case 1 (which restarts the entire transfer from byte 0 whenever a connection failure occurs), Case 2 maintains authoritative transfer progress on both client and server:
- When a chunk is received by the client, it is flushed to disk (`fdatasync`) and acknowledged (`MSG_CHECKPOINT_ACK`).
- The server updates the session's committed offset and durably writes it to disk (`.sessions/session_<id>.meta`).
- When a connection drops and reconnects, the server resumes streaming directly from the committed checkpoint offset.
- **Payload bytes committed prior to interruption are never retransmitted.**
- Uncommitted trailing bytes written by the client before a drop are cleanly reconciled via truncation to match the server's authoritative checkpoint.

---

## 2. Standard CLI Interface

### Server Invocation
```bash
./server <port> <topics_root_dir> <failure_probability>
```
- `port`: TCP listening port (1..65535).
- `topics_root_dir`: Directory containing topic sub-folders (e.g. `os`, `net`, `raw-img`).
- `failure_probability`: Floating-point probability $p \in [0.0, 1.0]$ governing connection drops.
- Durable session state is automatically stored in `<topics_root_dir>/.sessions/` (or directory configured via `IT305_SESSION_DIR`).

### Client Invocation
```bash
./client <server_ip> <server_port> <topic> <output_dir> [max_attempts]
```
- `server_ip`: IPv4 address of the server (e.g. `127.0.0.1`).
- `server_port`: TCP port of the server.
- `topic`: Topic name (e.g. `os`, `net`, `cane`, or alias `dog`).
- `output_dir`: Base output directory. The client writes files to `<output_dir>/<topic>/`.
- `max_attempts` (optional, default 10 or `IT305_MAX_ATTEMPTS`): Maximum total attempts.
- Local checkpoint is automatically stored in `<output_dir>/.session_<topic>.chk` during in-flight transfers and cleaned up upon successful atomic publication.

---

## 3. Protocol Architecture (Binary Framing v2.0)

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

### Core Message Types
- `0x10` **`MSG_SESSION_REQ`**: Client requests new session or resumes existing 32-hex Session ID.
- `0x11` **`MSG_SESSION_RESP`**: Server returns Session ID and topic manifest digest.
- `0x12` **`MSG_RESUME_RESP`**: Server returns authoritative committed offsets for all files.
- `0x03` **`MSG_MANIFEST_START`**, `0x04` **`MSG_MANIFEST_ENTRY`**, `0x05` **`MSG_MANIFEST_END`**: Topic catalog.
- `0x06` **`MSG_FILE_HEADER`**: Signals start of file streaming with authoritative start offset.
- `0x07` **`MSG_DATA_CHUNK`**: Payload block (up to 64 KB).
- `0x13` **`MSG_CHECKPOINT_ACK`**: Client confirms chunk durability to server.
- `0x14` **`MSG_CHECKPOINT_COMMIT`**: Server confirms persistent commit to client.
- `0x08` **`MSG_TRANSFER_DONE`**: Signals transfer completion.
- `0xFF` **`MSG_ERROR`**: Bidirectional error reporting.

For full wire format specifications, see [PROTOCOL.md](PROTOCOL.md). For formal invariant proofs, see [CORRECTNESS.md](CORRECTNESS.md).

---

## 4. Telemetry Output Format

Upon termination, the client outputs a single machine-readable `RESULT` line:

```text
RESULT topic=<topic> status=<OK|FAIL> attempts=<N> retries=<N-1> useful_bytes=<B> transmitted_bytes=<B> retransmitted_bytes=<B> protocol_bytes=<B> elapsed_sec=<S> mbps=<M>
```

- `useful_bytes`: Exact payload bytes of files in the topic catalog.
- `transmitted_bytes`: Total file payload bytes received across all connection attempts.
- `retransmitted_bytes`: Retransmitted payload bytes ($\max(0, \text{transmitted\_bytes} - \text{useful\_bytes})$).
- `protocol_bytes`: Total network volume (fixed headers + payload + framing).
- `elapsed_sec`: Total wall-clock time including connection backoff and retries.
- `mbps`: Goodput in megabits per second ($(\text{useful\_bytes} \times 8) / (\text{elapsed\_sec} \times 10^6)$).

---

## 5. Building and Testing

```bash
# Build server and client with strict flags (-Werror)
make

# Run automated comprehensive test suite (38 automated tests)
make test

# Clean artifacts
make clean
```
