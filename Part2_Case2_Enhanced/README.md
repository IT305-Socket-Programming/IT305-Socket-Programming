# Part II — Case 2 Enhanced: Fault-Tolerant Topic Distribution (Parallel TCP Streams)

## 1. Overview

Part II Case 2 Enhanced implements high-throughput, fault-tolerant topic-based file distribution over **multiple concurrent TCP streams** with **disjoint byte-range chunking** and **range-level checkpoint resumption**.

Key features:
- **Parallel TCP Worker Streams**: Client spawns $K$ parallel streams ($1 \le K \le 16$, default 4) connected to the server.
- **Dynamic Work-Stealing**: 64 KB blocks are pulled dynamically by available streams, maximizing utilization without stragglers.
- **Disjoint `pwrite` Concurrency**: Client worker threads write blocks concurrently to disk using POSIX `pwrite()` at non-overlapping byte offsets.
- **Range-Level Checkpoint Resilience**: Checkpoints track completion of individual 64 KB blocks via a compact persistent bitmask (`.sessions/`).
- **Zero Retransmission of Committed Blocks**: Resumption skips all previously committed blocks.
- **Worker Failure Tolerance**: If one worker stream fails or drops mid-transfer, surviving streams immediately pick up the uncommitted blocks and complete the session.
- **End-to-End Integrity**: Every file is cryptographically validated using SHA-256 before atomic directory publication.

---

## 2. Standard CLI Interface

### Server Invocation
```bash
./server <port> <topics_root_dir> <failure_probability> [max_streams]
```
- `port`: TCP listening port (1..65535).
- `topics_root_dir`: Directory containing topic folders (e.g. `os`, `net`, `raw-img`).
- `failure_probability`: Floating-point probability $p \in [0.0, 1.0]$ governing connection drops.
- `max_streams` (optional, default 16): Maximum allowed concurrent streams per session.
- Session metadata is persisted in `<topics_root_dir>/.sessions/`.

### Client Invocation
```bash
./client <server_ip> <server_port> <topic> <output_dir> [streams] [max_attempts]
```
- `server_ip`: IPv4 address of the server (e.g. `127.0.0.1`).
- `server_port`: TCP port of the server.
- `topic`: Topic name (e.g. `os`, `net`, `cane`, or alias `dog`).
- `output_dir`: Base output directory. Files are published to `<output_dir>/<topic>/`.
- `streams` (optional, default 4): Number of parallel TCP worker streams ($1 \le K \le 16$).
- `max_attempts` (optional, default 10): Maximum connection retry attempts.

---

## 3. Protocol Architecture (Binary Framing v2.0)

All messages share a 16-byte fixed binary header encoded in big-endian network byte order:

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
- `0x20` **`MSG_ENHANCED_SESSION_REQ`**: Control connection negotiates session & stream count.
- `0x21` **`MSG_ENHANCED_SESSION_RESP`**: Control connection confirms session ID & assigned streams.
- `0x22` **`MSG_STREAM_ATTACH`**: Worker connects and attaches to active session.
- `0x23` **`MSG_STREAM_ATTACH_ACK`**: Server acknowledges worker stream attachment.
- `0x24` **`MSG_PULL_WORK`**: Worker pulls next unassigned 64 KB block.
- `0x25` **`MSG_NO_MORE_WORK`**: Signals all blocks are committed.
- `0x07` **`MSG_DATA_CHUNK`**: Carries block range `(file_idx, offset, length)` + payload.
- `0x13` **`MSG_CHECKPOINT_ACK`**: Worker confirms chunk flushed to disk.
- `0x14` **`MSG_CHECKPOINT_COMMIT`**: Server confirms block committed in persistent bitmask.
- `0x08` **`MSG_TRANSFER_DONE`**: Control connection signals session completion.

For full wire format specifications, see [PROTOCOL.md](PROTOCOL.md). For formal invariant proofs, see [CORRECTNESS.md](CORRECTNESS.md).

---

## 4. Telemetry Output Format

Upon termination, the client outputs a single machine-readable `RESULT` line:

```text
RESULT topic=<topic> status=<OK|FAIL> attempts=<N> retries=<N-1> streams=<K> useful_bytes=<B> transmitted_bytes=<B> retransmitted_bytes=<B> protocol_bytes=<B> elapsed_sec=<S> mbps=<M>
```

---

## 5. Building and Testing

```bash
# Build server and client with strict flags (-Werror)
make

# Run automated comprehensive test suite
make test

# Clean artifacts
make clean
```
