# Formal Correctness & Invariant Proofs: IT305 Part II Case 2
## Stateful Session Management & Durable Checkpoint Resumption

---

## 1. System Model & Assumptions

1. **Network Model**: The network medium is TCP/IP over an unreliable channel. Sockets may experience sudden termination (`ECONNRESET`, `EPIPE`, EOF, or timeout) at arbitrary byte boundaries. TCP provides in-order, error-free delivery for bytes successfully read before connection failure.
2. **Storage Model**: The underlying filesystem supports POSIX primitives: `open`, `read`, `pwrite`/`write`, `lseek`, `truncate`, `fdatasync`, and `rename`. Storage hardware flushes writes to persistent medium upon `fdatasync()`. `rename()` within the same filesystem is atomic.
3. **Crash-Recovery Model**:
   - Client process may fail or restart; checkpoint is durably retained in `.session_<topic>.chk`.
   - Server process may be terminated (`SIGKILL`, abnormal exit) and restarted; sessions are durably preserved on disk in `.sessions/<session_id>.session`.
   - *Durability Scope & Testing Boundary*: Tested recovery is against ungraceful process termination (`SIGKILL`, abnormal exits, connection aborts) and process restart. The implementation issues `fdatasync()` on session state files and payload staging files. However, bare-metal hardware power loss or operating system kernel panics may additionally require filesystem directory entry syncing and non-volatile write caches, which is outside the scope of POSIX process-level crash testing.

---

## 2. Formal Invariants & Inductive Proofs

### Invariant 1: Checkpoint Atomicity & Agreement
$$\forall \text{ file } i, \quad \text{Committed}(i, O) \iff (\text{ClientFlushed}(i, O) \land \text{ServerPersisted}(i, O))$$

*Proof by Construction*:
1. The server transmits a chunk covering bytes $[O, O + \Delta)$.
2. The client writes bytes $[O, O + \Delta)$ into staging file `.staging_<topic>/filename` and calls `fdatasync(ofd)`.
3. Only after `fdatasync()` returns 0 does the client emit `MSG_CHECKPOINT_ACK` with offset $O + \Delta$.
4. The server receives `MSG_CHECKPOINT_ACK`, validates that the acknowledged offset matches the current transmission window, acquires `session->lock`, records `session->committed_offsets[i] = O + \Delta`, and writes the session state file `.sessions/session_<id>.meta` followed by `fdatasync()`.
5. Only after server persistence succeeds does the server send `MSG_CHECKPOINT_COMMIT`.
6. Upon receiving `MSG_CHECKPOINT_COMMIT`, the client writes the new offset to `.session_<topic>.chk` via atomic write-sync-rename.
7. If any failure occurs before step 5, the server's committed offset remains $O$. On resume, the client queries the server, receives $O$, and truncates any uncommitted bytes $> O$. Thus, both client and server strictly agree on the committed boundary. $\blacksquare$

---

### Invariant 2: Monotonic Forward Progress & Zero Retransmission
$$\text{For any attempt } k+1 \text{ resuming from attempt } k: \quad O_{k+1} \ge O_k$$
$$\text{Bytes transmitted in attempt } k+1 \text{ lie strictly in } [O_k, \text{FileSize})$$

*Proof*:
1. Let $O_k$ be the authoritative offset stored in `session->committed_offsets[i]` at the end of attempt $k$.
2. Upon reconnecting, the client issues `MSG_SESSION_REQ` with `FLAG_RESUME` and session ID.
3. The server locates the session and sends `MSG_RESUME_RESP` containing $O_k$.
4. In `server.c`, when streaming file $i$:
   - If $O_k \ge \text{FileSize}$, the server skips the file entirely (`continue`).
   - If $O_k < \text{FileSize}$, the server opens the file and invokes `lseek(ffd, O_k, SEEK_SET)`.
5. The server transmits starting at byte $O_k$. No byte in $[0, O_k)$ is ever read from disk or put onto the socket during attempt $k+1$.
6. Retransmission of payload bytes committed prior to interruption is identically zero:
   $$\text{Payload Retransmitted} = 0 \text{ bytes}$$
   $\blacksquare$

---

### Invariant 3: Clean Reconciliation of Uncommitted Tail
$$\text{Staging File Size upon Resumption} = O_{\text{authoritative}}$$

*Proof*:
1. Suppose a failure interrupts transmission after the client has written $\delta$ bytes ($0 < \delta \le \Delta$) to disk, but before the server persisted `MSG_CHECKPOINT_ACK`.
2. The client's staging file has length $O_k + \delta$, but the server's authoritative state is $O_k$.
3. Upon reconnection, the client receives `MSG_RESUME_RESP` specifying authoritative offset $O_k$.
4. The client immediately executes:
   ```c
   truncate(fpath, (off_t)auth_off);
   ```
5. Any uncommitted bytes $\delta$ are discarded. The staging file length is exactly $O_k$. Subsequent writes from the resumed server at offset $O_k$ append cleanly without offset drift or file corruption. $\blacksquare$

---

### Invariant 4: End-to-End Cryptographic Manifest Integrity
$$\text{Published Topic Digest} = \text{Server Manifest SHA-256}$$

*Proof*:
1. The server computes a SHA-256 digest over the concatenation of `(filename, size, file_sha256)` for all sorted files in the topic catalog.
2. The client verifies every individual file digest using its on-the-fly streaming SHA-256 computation.
3. Only when all files are 100% committed and all SHA-256 digests match does the client execute atomic rename:
   ```c
   rename(staging_dir, target_topic_dir);
   unlink(chk_path);
   ```
4. No partially transferred or corrupted file is ever exposed in the target topic directory. $\blacksquare$
