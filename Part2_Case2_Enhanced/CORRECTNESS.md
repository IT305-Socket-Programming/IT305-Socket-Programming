# Formal Correctness & Invariant Proofs: IT305 Part II Case 2 Enhanced
## Parallel TCP Streams & Disjoint Byte-Range Checkpoint Resumption

---

## 1. System Model & Multi-Stream Concurrency

1. **Concurrency Model**:
   - The client operates $K$ concurrent POSIX worker threads ($1 \le K \le 16$, default 4).
   - Each worker maintains its own independent TCP stream to the server.
   - The server maintains a shared `enhanced_session_t` object protected by `session->lock` and `session->cond`.
2. **File I/O Model**:
   - File data blocks are fixed slices of size up to `CHUNK_SIZE` (64 KB).
   - Parallel worker threads write received slices concurrently into shared open file descriptors using `pwrite(fd, buf, len, offset)`.
   - `pwrite()` is reentrant, atomic with respect to write offset, and does not alter or depend on the file descriptor's seek position (`lseek`).
3. **Storage Durability Model**:
   - After writing a block, the client calls `fdatasync(fd)` before acknowledging.
   - The server updates a persistent session file in `.sessions/<session_id>.session` via atomic write-sync-rename.
   - *Durability Scope & Testing Boundary*: Tested recovery is against ungraceful worker/server process termination (`SIGKILL`, abnormal exits, connection aborts) and process restart. While `fdatasync()` flushes dirty pages before commit, physical host power outages or kernel panics would additionally require filesystem-level directory entry barriers and non-volatile write caches.

---

## 2. Invariants & Proofs

### Invariant 1: Disjoint Byte-Range Mutual Exclusion
$$\forall \text{ blocks } i \ne j \text{ in file } F: \quad [O_i, O_i + L_i) \cap [O_j, O_j + L_j) = \emptyset$$

*Proof by Construction*:
1. For each file $F$, blocks are generated sequentially at offsets:
   $$O_b = b \times \text{CHUNK\_SIZE}, \quad L_b = \min(\text{CHUNK\_SIZE}, \text{FileSize} - O_b)$$
2. Since $O_{b+1} = O_b + L_b$, every byte in $[0, \text{FileSize})$ belongs to exactly one block $b$.
3. Two worker threads concurrently writing block $i$ and block $j$ write to non-overlapping byte regions of the file.
4. Therefore, concurrent execution of `pwrite` across parallel streams exhibits no race conditions, overwrites, or data corruption. $\blacksquare$

---

### Invariant 2: Dynamic Work Allocation Without Duplication
$$\text{At any instant } t, \text{ each block } b \text{ is in exactly one state } S \in \{\text{UNASSIGNED}, \text{IN\_FLIGHT}, \text{COMMITTED}\}$$

*Proof*:
1. State transitions are governed under `pthread_mutex_lock(&session->lock)`.
2. A block enters `IN_FLIGHT` only when selected by a worker stream while in `UNASSIGNED` state:
   $$\text{UNASSIGNED} \xrightarrow{\text{worker pull}} \text{IN\_FLIGHT}$$
3. No block in `IN_FLIGHT` or `COMMITTED` state can be selected by another worker stream.
4. When `MSG_CHECKPOINT_ACK` is validated, the state transitions to `COMMITTED`:
   $$\text{IN\_FLIGHT} \xrightarrow{\text{valid ACK}} \text{COMMITTED}$$
5. If a worker stream disconnects before acknowledging, the cleanup handler resets the block:
   $$\text{IN\_FLIGHT} \xrightarrow{\text{disconnect/error}} \text{UNASSIGNED}$$
6. Thus, no two worker streams ever concurrently process the same block. $\blacksquare$

---

### Invariant 3: Zero Retransmission of Committed Blocks
$$\forall b \text{ where } \text{State}(b) = \text{COMMITTED}: \quad \text{Block } b \text{ is never transmitted again}$$

*Proof*:
1. The server session marks committed blocks in `session->bitmask[b / 8] |= (1 << (b % 8))` and persists the bitmask to disk.
2. The block selection loop:
   ```c
   if (session->blocks[b].status == 0) { ... }
   ```
   only inspects blocks with status 0 (`UNASSIGNED`).
3. Blocks with status 2 (`COMMITTED`) are permanently skipped.
4. If a connection interruption occurs and the client reconnects, the server reloads the committed bitmask from disk. Committed blocks remain status 2 and are never transmitted again.
5. Therefore, redundant payload retransmission for committed blocks is identically 0 bytes. $\blacksquare$

---

### Invariant 4: Fault Resiliency Under Stream Failure
$$\text{Failure of } M < K \text{ worker streams does not prevent session convergence}$$

*Proof*:
1. If $M$ worker streams experience socket resets or timeouts, their in-flight blocks are reset to `UNASSIGNED` in `handle_worker_stream` cleanup.
2. `pthread_cond_broadcast(&session->cond)` notifies all surviving worker streams.
3. The remaining $K - M$ surviving worker streams pull the released blocks and continue streaming.
4. If all streams drop, the client reconnects on the next attempt, reloads the session, launches new worker streams, and completes the remaining uncommitted blocks. $\blacksquare$

---

### Invariant 5: End-to-End Cryptographic Verification
$$\text{Atomic Rename} \implies \forall i \in [0, N-1]: \quad \text{SHA256}(\text{File}_i) = \text{ManifestHash}_i$$

*Proof*:
1. Files are pre-allocated and written in staging directory `.staging_<topic>`.
2. Control connection triggers completion only when `committed_blocks_count == total_blocks`.
3. Client verifies SHA-256 hash of each file from byte 0 to `size`.
4. Staging files are moved to `<outdir>/<topic>/` via `rename()` if and only if all SHA-256 hashes match.
5. Partial or corrupted downloads are never exposed. $\blacksquare$
