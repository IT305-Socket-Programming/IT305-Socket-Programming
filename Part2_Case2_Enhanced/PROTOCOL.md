# IT305 Part II Case 2 Enhanced: Parallel TCP Streams & Disjoint Range Recovery Protocol Specification

## 1. Overview & Architectural Principles

Case 2 Enhanced extends stateful checkpoint recovery to **parallel TCP streams**, enabling high-throughput topic file distribution over multiple concurrent TCP connections.

Key architectural concepts:
1. **Control vs Data Separation**:
   - A single **Control Connection** negotiates session parameters, exchanges catalog manifests, and synchronizes completion.
   - $K$ independent **Worker Streams** ($1 \le K \le 16$, default 4) attach to the active session over separate TCP connections.
2. **Dynamic Work-Stealing Block Scheduler**:
   - Files are partitioned into 64 KB disjoint blocks: $[O, O + \text{len})$.
   - Worker streams dynamically pull available unassigned blocks from the server's shared block pool.
   - Faster streams naturally pull and process more blocks, achieving optimal dynamic load balancing without straggler bottlenecks.
3. **Disjoint Byte Ranges & Concurrent `pwrite`**:
   - Each data block specifies an exact file index, byte offset, and length: `(file_idx, offset, length)`.
   - Client worker threads write blocks concurrently to disk using POSIX `pwrite()` without thread contention or file pointer collisions.
4. **Range-Level Checkpointing & Fault Tolerance**:
   - Each block requires client disk durability (`fdatasync`) followed by a range-level durability handshake (`MSG_CHECKPOINT_ACK` and `MSG_CHECKPOINT_COMMIT`).
   - If an individual worker stream fails or drops mid-block, the uncommitted block is automatically released back to the session pool for surviving (or reconnected) streams to fulfill.
   - **Zero payload bytes of committed blocks are ever retransmitted.**

---

## 2. Fixed Binary Header (16 Bytes)

All frames share an identical 16-byte fixed-width header encoded in network byte order:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Magic (0x5446)       |    Version    |  Message Type |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|             Flags             |         Status / Error        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Payload Length                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Sequence Number                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

---

## 3. Enhanced Protocol Message Types

| Value | Identifier | Direction | Purpose |
|---|---|---|---|
| `0x20` | `MSG_ENHANCED_SESSION_REQ` | Client $\to$ Server (Ctrl) | Initiate session with requested streams $K$ and topic |
| `0x21` | `MSG_ENHANCED_SESSION_RESP` | Server $\to$ Client (Ctrl) | Accept session with assigned streams $K$, session ID, and manifest digest |
| `0x22` | `MSG_STREAM_ATTACH` | Worker $\to$ Server (Data) | Attach parallel worker stream using Session ID and Stream ID |
| `0x23` | `MSG_STREAM_ATTACH_ACK` | Server $\to$ Worker (Data) | Acknowledge worker stream attachment |
| `0x24` | `MSG_PULL_WORK` | Worker $\to$ Server (Data) | Request next available unassigned data block |
| `0x25` | `MSG_NO_MORE_WORK` | Server $\to$ Worker (Data) | Inform worker that all blocks are completed |
| `0x07` | `MSG_DATA_CHUNK` | Server $\to$ Worker (Data) | Block payload with 16-byte range header |
| `0x13` | `MSG_CHECKPOINT_ACK` | Worker $\to$ Server (Data) | Client confirms block flushed to disk |
| `0x14` | `MSG_CHECKPOINT_COMMIT` | Server $\to$ Worker (Data) | Server confirms block persisted to `.sessions/` bitmask |
| `0x03` | `MSG_MANIFEST_START` | Server $\to$ Client (Ctrl) | Catalog file count and total size |
| `0x04` | `MSG_MANIFEST_ENTRY` | Server $\to$ Client (Ctrl) | Catalog entry (name, size, SHA-256) |
| `0x05` | `MSG_MANIFEST_END` | Server $\to$ Client (Ctrl) | Catalog transmission complete |
| `0x08` | `MSG_TRANSFER_DONE` | Server $\to$ Client (Ctrl) | All blocks completed and verified |
| `0xFF` | `MSG_ERROR` | Either $\to$ Other | Fatal error report |

---

## 4. Message Payloads

### 4.1 `MSG_ENHANCED_SESSION_REQ`
```
+--------------------------------+--------------------+--------------------+----------------------+
| Session ID (32 bytes ASCII)   | Streams (2 bytes)  | Topic Len (2 bytes)| Topic Name (variable)|
+--------------------------------+--------------------+--------------------+----------------------+
```

### 4.2 `MSG_ENHANCED_SESSION_RESP`
```
+--------------------+------------------+------------------+-------------------+-------------------+----------------------+
| Session ID (32 B)  | Streams (2 bytes)| Resumed (1 byte) | Total Files (4 B) | Total Bytes (8 B) | Manifest Hash (32 B) |
+--------------------+------------------+------------------+-------------------+-------------------+----------------------+
```

### 4.3 `MSG_STREAM_ATTACH`
```
+--------------------------------+--------------------+
| Session ID (32 bytes ASCII)   | Stream ID (2 bytes)|
+--------------------------------+--------------------+
```

### 4.4 `MSG_DATA_CHUNK` (with Range Header)
```
+----------------------+--------------------+--------------------+--------------------------------+
| File Index (4 bytes) | Byte Offset (8 B)  | Range Length (4 B) | Raw Chunk Payload (variable)   |
+----------------------+--------------------+--------------------+--------------------------------+
```

### 4.5 `MSG_CHECKPOINT_ACK`
```
+--------------------------------+----------------------+-------------------+--------------------+
| Session ID (32 bytes ASCII)   | File Index (4 bytes) | Byte Offset (8 B) | Range Length (4 B) |
+--------------------------------+----------------------+-------------------+--------------------+
```

### 4.6 `MSG_CHECKPOINT_COMMIT`
```
+----------------------+--------------------+--------------------+
| File Index (4 bytes) | Byte Offset (8 B)  | Range Length (4 B) |
+----------------------+--------------------+--------------------+
```

---

## 5. Sequence Diagram

```mermaid
sequenceDiagram
    participant C as Client (Control)
    participant W1 as Worker Stream 1
    participant W2 as Worker Stream 2
    participant S as Server Scheduler

    Note over C,S: Control Negotiation
    C->>S: MSG_ENHANCED_SESSION_REQ (Streams=2, Topic="os")
    S->>C: MSG_ENHANCED_SESSION_RESP (SessionID="abc", Streams=2)
    S->>C: MSG_MANIFEST (Catalog)

    Note over W1,W2,S: Parallel Stream Attachment
    par Stream 1 Attach
        W1->>S: MSG_STREAM_ATTACH (SessionID="abc", Stream=0)
        S->>W1: MSG_STREAM_ATTACH_ACK
    and Stream 2 Attach
        W2->>S: MSG_STREAM_ATTACH (SessionID="abc", Stream=1)
        S->>W2: MSG_STREAM_ATTACH_ACK
    end

    Note over W1,W2,S: Concurrent Work Pull & pwrite
    par Worker 1 Pulls Block 0
        W1->>S: MSG_PULL_WORK
        S->>W1: MSG_DATA_CHUNK (File=0, Offset=0, Len=65536)
        Note over W1: pwrite(fd, buf, 65536, 0)
        W1->>S: MSG_CHECKPOINT_ACK (File=0, Offset=0, Len=65536)
        S->>W1: MSG_CHECKPOINT_COMMIT
    and Worker 2 Pulls Block 1
        W2->>S: MSG_PULL_WORK
        S->>W2: MSG_DATA_CHUNK (File=0, Offset=65536, Len=65536)
        Note over W2: pwrite(fd, buf, 65536, 65536)
        W2->>S: MSG_CHECKPOINT_ACK (File=0, Offset=65536, Len=65536)
        S->>W2: MSG_CHECKPOINT_COMMIT
    end

    Note over W1,W2: All blocks committed
    W1->>S: MSG_PULL_WORK
    S->>W1: MSG_NO_MORE_WORK
    W2->>S: MSG_PULL_WORK
    S->>W2: MSG_NO_MORE_WORK

    S->>C: MSG_TRANSFER_DONE
    Note over C: Whole-topic SHA-256 validation & atomic publication
```
