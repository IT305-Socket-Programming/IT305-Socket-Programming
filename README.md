# IT305 Socket Programming Project
## Fault-Tolerant Topic-Based File Distribution Service

A high-performance POSIX C99 topic file distribution architecture over TCP featuring single-threaded and multi-threaded server execution models, probabilistic fault injection, un-sessioned restart recovery, checkpoint-based session resume with explicit acknowledgement commitments, and multi-stream non-blocking range streaming.

---

### Submission & Group Information
* **Course:** IT305: Computer Networks Laboratory
* **Institute:** Dhirubhai Ambani Institute of Information and Communication Technology (DA-IICT)
* **Merged Lab Groups:** Group 109 & Group 104 (`Group_109_104`)
* **Team Members:**
  * **Group 109:** Darshan Talati (202401046), Kavya Bhojwani (202401090), Aaryan Modi (202401053)
  * **Group 104:** Siddh Shah (202401088), Tanish Sanghvi (202401471), Khushi Shah (202401465)
* **Project Directory Structure:**
  ```
  FinalSubmission_Group_109_104/
  ├── FinalReport_Group_109_104.pdf
  ├── README.md
  ├── Part1/
  │   ├── server.c, client.c, common.c, common.h, Makefile
  │   └── bench.sh, aggregate.py, test_part1.sh
  ├── Part2_Case1/
  │   ├── server.c, client.c, common.c, common.h, Makefile
  │   └── CORRECTNESS.md, tests/
  ├── Part2_Case2/
  │   ├── server.c, client.c, common.c, common.h, Makefile
  │   └── tests/
  ├── Part2_Case2_Enhanced/
  │   ├── server.c, client.c, common.c, common.h, Makefile
  │   └── tests/
  └── Results/
      ├── raw/                  (raw performance CSV logs)
      ├── processed/            (aggregated summary CSVs)
      ├── graphs/               (publication-grade PNG, PDF & SVG charts)
      ├── plot_results.py       (automated chart generator)
      └── run_experiments.sh    (automated benchmark harness)
  ```

---

### Implementation Summary

| Part | Architecture & Mechanism | Key Primitives |
| :--- | :--- | :--- |
| **Part 1** | Baseline Topic Distribution: Single-Threaded (sequential) vs Multi-Threaded (worker-per-client) | `pthread_create()`, 12-byte binary framing, non-blocking topic scan |
| **Part 2 Case 1** | Fault-Tolerant Un-sessioned Restart: Restarts from File 0, Byte 0 upon connection termination | Bernoulli drop decision, atomic staging directory, redundant retransmission |
| **Part 2 Case 2** | Fault-Tolerant Session Resume: Durable disk checkpointing (`.session_<topic>.chk`) and server session table | Monotonic ACK commitment, zero redundant byte retransmission, SHA-256 integrity |
| **Part 2 Case 2 Enhanced** | Multi-Stream Parallel TCP Connections: $K$ simultaneous TCP connections over disjoint byte ranges | Work-stealing range reactor, link saturation, parallel speedup |

---

### Build Instructions

Each case directory contains a standalone `Makefile`. Running `make` inside any case directory builds exactly two executables: `server` and `client`.

```bash
# Build all implementations at once:
make -C Part1/ clean all
make -C Part2_Case1/ clean all
make -C Part2_Case2/ clean all
make -C Part2_Case2_Enhanced/ clean all

# Or use top-level Makefile:
make all
```

---

### Standard CLI Interface (Auto-Test Compliant)

All executables strictly comply with the auto-testing interface specified in the assignment:

#### 1. Server Invocations

* **Part 1:**
  ```bash
  ./Part1/server <port> <topics_root_dir> [--mode single|multi]
  ```
  * `<port>`: TCP port to bind (e.g. `9001`)
  * `<topics_root_dir>`: Absolute or relative path to the directory containing topic sub-folders (e.g. `/home/user/raw-img` containing `cane/`, `gatto/`, etc.)
  * `--mode`: Optional flag to toggle between `single` (sequential) and `multi` (concurrent) execution models (default: `multi`).

* **Part 2 (All Cases — Case 1, Case 2, Case 2 Enhanced):**
  ```bash
  ./Part2_Case1/server <port> <topics_root_dir> <failure_probability>
  ./Part2_Case2/server <port> <topics_root_dir> <failure_probability>
  ./Part2_Case2_Enhanced/server <port> <topics_root_dir> <failure_probability>
  ```
  * `<failure_probability>`: A floating-point number $p \in [0.0, 1.0]$ specifying the Bernoulli drop probability per transmitted chunk.

#### 2. Client Invocation (Universal across all parts)

```bash
./<case_dir>/client <server_ip> <server_port> <topic> <output_dir>
```
* `<server_ip>`: Target server IP (e.g. `127.0.0.1`)
* `<server_port>`: Target server TCP port (e.g. `9001`)
* `<topic>`: Topic name key matching a sub-folder in topics root (e.g. `cane`)
* `<output_dir>`: Destination folder. The client automatically creates `output_dir/topic/` and writes all received topic files into that directory.

---

### Fault Injection Mechanism

The probabilistic fault injector is integrated into each Part 2 server:
1. **Decision Point:** Immediately prior to calling `write()` / `send()` on the TCP socket for any 64 KB `MSG_DATA_CHUNK`, the server generates a pseudo-random sample:
   ```c
   double r = (double)rand_r(&seed) / (double)RAND_MAX;
   if (r < failure_probability) {
       LOG("Fault injected on socket %d! Terminating connection...", client_fd);
       close(client_fd);
       pthread_exit(NULL);
   }
   ```
2. **Failure Simulation:** The socket descriptor is closed immediately without sending error frames or trailing bytes. This induces a clean TCP FIN / RST from the client's perspective, identical to physical network disconnection.
3. **Recovery Execution:**
   * **Case 1:** Client detects EOF / `ECONNRESET`, cleans staging files, backs off, reconnects to the server, and requests the entire topic from Byte 0.
   * **Case 2:** Client flushes received bytes to disk, writes committed byte offset to `.session_<topic>.chk`, and reconnects. The client sends `MSG_ACK` with its committed offset, and the server validates session state and resumes streaming directly from that offset.
   * **Case 2 Enhanced:** $K$ worker threads multiplex ranges across $K$ parallel TCP sockets. If one connection is dropped, only its active range is re-queued and recovered, allowing other parallel connections to continue streaming without interruption.

---

### Running Performance Benchmarks & Testing

#### 1. Automated Universal Evaluator
The preliminary evaluation suite verifies compliance, build artifacts, edge cases, and layout:
```bash
# Fast sanity check (synthetic dataset, binary boundary sizes, edge filenames):
python3 test_universal.py . --quick

# Full layout, report, and group submission validation:
python3 test_universal.py . --submission --group 109 104
```

#### 2. Part 1 Concurrency Benchmarking
```bash
cd Part1
# Start server in multi-threaded mode on port 9001
./server 9001 /path/to/raw-img &
# Run benchmark across 1, 2, 4, 8, 12 concurrent clients
bash bench.sh 127.0.0.1 9001 multi 12 results_multi.csv "cane,gatto,cavallo"
python3 aggregate.py results_multi.csv -o summary_part1.csv
```

#### 3. Part 2 Fault-Tolerance Benchmarking
```bash
cd Results
# Executes 3 trials across all failure probabilities (p in 0.00 to 0.20):
bash run_experiments.sh
# Regenerates SVG, 300 DPI PNG, and PDF charts in Results/graphs/:
python3 plot_results.py processed/summary.csv graphs/
```