# Formal Correctness Arguments — Part II Case 1

## 1. Scope, System Model, and Assumptions

### 1.1 Network and Failure Model
- **Underlying Transport:** Point-to-point stream over TCP/IPv4. TCP guarantees in-order, lossless byte delivery within an active connection.
- **Injected Failure Model:** At each eligible non-empty data chunk boundary ($64\text{ KB}$ slice), the server executes an independent Bernoulli trial with parameter $p \in [0.0, 1.0]$ using a thread-safe PRNG (`rand_r` with per-worker seed).
  - If $r < p$, the server abruptly interrupts the connection via `shutdown(fd, SHUT_RDWR)` before writing that chunk.
  - No completion message (`MSG_TRANSFER_DONE`) is ever transmitted for an interrupted attempt.
- **Natural Failures:** Transient socket reset, connection timeout, process interruption (`SIGINT`/`SIGTERM`), or peer disconnect.
- **Independence Property:** If a transfer comprises $k$ eligible chunk decisions, the probability of at least one failure during the transfer is:
  $$\mathbb{P}(\text{failure}) = 1 - (1 - p)^k$$

---

## 2. Formal Safety Invariants

### Invariant 1 (Path Traversal and Symlink Safety)
*No untrusted input can cause file creation, truncation, or deletion outside the application-owned destination directory.*
- **Enforcement:**
  - `safe_name()` rejects all strings containing `/`, `\`, NUL, DEL ($127$), empty strings, `.`, `..`, or lengths $> 255$ (valid POSIX filename characters including spaces, newlines, and UTF-8 are supported).
  - Destination file descriptors are opened strictly with flags:
    $$\text{flags} = O\_WRONLY \mid O\_CREAT \mid O\_TRUNC \mid O\_NOFOLLOW, \quad \text{mode} = 0644$$
  - If an existing symbolic link is encountered in the destination path, `open()` fails immediately with `ELOOP`, aborting the operation before truncating or modifying the link target.
  - `fstat()` explicitly validates `S_ISREG(st.st_mode)`. If the opened descriptor refers to a FIFO, directory, device, or socket, it is closed immediately and rejected.

### Invariant 2 (Framing and Memory Safety)
*Malformed, truncated, or oversized network frames cannot cause out-of-bounds memory accesses or undefined behavior.*
- **Enforcement:**
  - Every protocol message begins with a rigid 16-byte binary header.
  - Multi-byte fields (`magic`, `flags`, `status`, `payload_len`, `seq`) are serialized and deserialized using explicit network byte order (`htons`/`ntohs`, `htonl`/`ntohl`). Native C struct bit-casting is prohibited.
  - Length bounding: If `payload_len > MAX_PAYLOAD` ($65536$), `conn_recv()` returns `-2` (`ERR_PROTOCOL`), terminating the connection before allocating or reading memory.
  - Monotonic sequence numbers: `seq` is checked against expected receiver sequence `rx_seq`. Any sequence mismatch triggers immediate rejection.

### Invariant 3 (Content Snapshot and Manifest Integrity)
*The final published dataset matches the exact server snapshot captured at manifest creation time, even if source files are modified during streaming.*
- **Enforcement:**
  - The server scans topic files, sorts them deterministically in alphabetical order, and computes individual 32-byte SHA-256 digests.
  - The aggregate manifest hash $H_M = \text{SHA256}(f_0.name, f_0.size, f_0.hash, \dots)$ is sent in `MSG_MANIFEST_START` and `MSG_TRANSFER_DONE`.
  - While streaming each file's chunks, the server computes SHA-256 on the fly from the bytes actually read from disk and verifies it against the manifest digest before completing the file. If file contents mutated on disk, the server aborts with `ERR_INTERNAL`.
  - The client independently verifies every received file's SHA-256 and validates the entire manifest identity.

### Invariant 4 (Transactional Atomic Publication)
*No incomplete, truncated, or failed transfer is ever published to the target destination.*
- **Enforcement:**
  - All downloads during attempt $k$ are staged into an attempt-isolated temporary directory:
    $$D_{\text{staging}} = \langle\text{output\_dir}\rangle/\text{.staging\_}\langle\text{topic}\rangle\_\langle\text{pid}\rangle\_k$$
  - If attempt $k$ fails at any byte offset, `rm_rf(D_{\text{staging}})` recursively purges the staging directory.
  - Atomic rename: Only upon receiving `MSG_TRANSFER_DONE` and verifying all file sizes and SHA-256 digests does the client replace the target topic directory via `rename()`.

---

## 3. Inductive Proof Sketch for Case 1

**Theorem:** *Under Case 1 semantics, every attempt restarts from byte offset zero, the maximum number of attempts is bounded by $A$, and the final published output is either completely valid or absent.*

- **Base Case ($k = 1$):**
  The client initializes `attempt = 1`, creates `.staging_<topic>_<pid>_1`, connects via TCP, sends `MSG_GET_REQ`, and requests the topic starting from File 0, Byte Offset 0.
  - *Branch A (Zero failures):* All files are received, validated against SHA-256 hashes, and verified against `MSG_TRANSFER_DONE`. Staging is atomically renamed to target directory. Success is published. Invariant holds.
  - *Branch B (Failure occurs):* Connection drops or integrity check fails. `.staging_<topic>_<pid>_1` is completely deleted. No target directory is published.

- **Inductive Step ($k \to k + 1$):**
  Assume attempts $1 \dots k$ failed.
  By the induction hypothesis, all staging directories $.staging\_*\_1 \dots .staging\_*\_k$ have been completely destroyed, and the target destination remains untouched.
  If $k < A$, the client increments to attempt $k+1$. It establishes a brand new TCP connection and transmits `MSG_GET_REQ` with `FLAG_RETRY`.
  The server responds with the complete manifest from file 0, and the client receives file data starting at File 0, Byte Offset 0 into `.staging_<topic>_<pid>_{k+1}`.
  There is no checkpoint state, and no byte offset from prior attempts is reused.
  - If attempt $k+1$ succeeds, it is verified and atomically published.
  - If attempt $k+1$ fails, staging is destroyed.

- **Termination Condition:**
  The loop condition is bounded by `attempt < max_attempts` ($A$). If $k = A$ and attempt $A$ fails, the client halts, logs `status=FAIL`, exits with code $1$, and leaves zero staging files.

$$\tag*{$\blacksquare$}$$

---

## 4. Liveness vs. Safety Clarification

- **Safety Guarantee:** The system will never publish corrupted, partial, or out-of-order data.
- **Liveness Limitation:** Under persistent failure injection with $p = 1.0$, every attempt will encounter an injected failure. Bounded retries guarantee that execution terminates cleanly in finite time, but does not guarantee successful data delivery if network conditions persistently prevent completion.
