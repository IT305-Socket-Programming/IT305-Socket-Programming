# IT305 Socket Programming Project
## Fault-Tolerant Topic-based File Distribution Service

A TCP client/server system, written in C (POSIX sockets + pthreads), where a client asks a server
for a **topic** (e.g. `cane`) and the server sends every file in that topic's folder.

| Part | What it does | Status |
|---|---|---|
| **Part 1** | Basic file server, in single-threaded and multi-threaded versions, plus throughput measurement | Done and tested |
| **Part 2 – Case 1** | Server randomly drops connections; client restarts the topic from scratch | _to be added_ |
| **Part 2 – Case 2** | Session + checkpoint: client resumes from where it stopped | _to be added_ |
| **Part 2 – Case 2 Enhanced** | Case 2 plus parallel TCP connections | _to be added_ |

---

## 1. How it works (the idea in one minute)

```
 Client                                   Server
   |------ GET cane ---------------------->|   looks up topic -> folder
   |<----- manifest (file names + sizes) --|   lists the files in the folder
   |<----- FILE_HEADER + DATA chunks ------|   sends each file in 64 KB chunks
   |<----- ...repeat for every file... ----|
   |<----- TRANSFER_DONE -----------------|
```

- **Topics:** every sub-folder of the dataset root is a topic. The folder name is the key
  (`raw-img/cane` -> topic `cane`). The server scans the root at startup, so nothing is hard-coded
  and new topics work by just adding a folder.
- **Framing:** TCP is a byte stream, so every message has a 12-byte header
  (magic, type, flags, payload length, sequence number). The receiver validates each header.
- **No per-chunk ACK in Part 1:** TCP is already reliable, and an ACK per chunk would slow things down.
- **Single vs multi-threaded:** single-threaded serves one client at a time (the baseline);
  multi-threaded creates one thread per client so clients are served simultaneously.
- **Safety:** file names from the server are checked on the client, so a name like `../x` is rejected.
- **English names:** the server also accepts `dog`, `cat`, `horse`, ... as aliases for the Italian
  folders (used only if no folder matches the name exactly).

---

## 2. Requirements

- Linux (tested on Ubuntu). Windows users: use a Linux VM or WSL.
- `gcc`, `make`, `python3`

```bash
sudo apt update && sudo apt install -y build-essential python3
```

The dataset is [Animals10](https://www.kaggle.com/datasets/alessiocorrado99/animals10): a folder
`raw-img/` containing `cane`, `cavallo`, `elefante`, `farfalla`, `gallina`, `gatto`, `mucca`,
`pecora`, `ragno`, `scoiattolo`.
(The `translate.py` file that comes with the dataset is not used by our code.)

---

## 3. Files

```
Part1/
├── Makefile       builds ./server and ./client
├── common.h/.c    shared protocol: header format, message types, send_all/recv_all, helpers
├── server.c       topic table, manifest + file sending, single/multi-threaded loops, statistics
├── client.c       sends GET, checks manifest, saves files, prints a RESULT line
├── bench.sh       runs N clients in parallel on one machine -> CSV
├── aggregate.py   merges CSVs -> aggregate throughput per (mode, N)
└── README.md
```

---

## 4. Build

```bash
cd Part1
chmod +x bench.sh aggregate.py        # only needed if the files lost their permissions
make                                  # creates ./server and ./client (no warnings expected)
make clean                            # removes them
```

If you copied the files from Windows and see `^M` or `bad interpreter` errors:

```bash
sed -i 's/\r$//' *.sh *.c *.h *.py Makefile
```

---

## 5. Run

### Server

```bash
./server <port> <topics_root_dir> [--mode single|multi]
```

| Argument | Meaning |
|---|---|
| `port` | TCP port to listen on, e.g. `5000` |
| `topics_root_dir` | Folder that **directly contains** the topic folders, e.g. `/home/you/raw-img` |
| `--mode` | `multi` (default, one thread per client) or `single` (one client at a time) |

Example:

```bash
./server 5000 /home/you/raw-img                 # multi-threaded
./server 5000 /home/you/raw-img --mode single   # single-threaded
```

At startup the server prints each topic it found. Stop it with **Ctrl+C** (it prints totals and exits cleanly).

### Client

```bash
./client <server_ip> <server_port> <topic> <output_dir>
```

Files are saved to `<output_dir>/<topic>/`.

```bash
./client 127.0.0.1 5000 cane /tmp/out        # files land in /tmp/out/cane/
./client 192.168.1.10 5000 gatto ~/received  # server on another machine
./client 127.0.0.1 5000 dog /tmp/out         # English alias for cane
```

On success the client prints a line like:
`RESULT topic=cane files=4863 data_bytes=60686716 wire_bytes=61377391 start=... end=... secs=1.5 mbps=304.7`

The server also logs one line per finished client (bytes, time, rate).

---

## 6. Quick test (everything on one machine)

```bash
cd Part1 && make
DATA=/home/you/raw-img            # <-- use the real path to your raw-img folder
rm -rf /tmp/out
./server 5000 "$DATA" 2>/tmp/server.log &
sleep 1
for t in cane cavallo elefante farfalla gallina gatto mucca pecora ragno scoiattolo; do
  ./client 127.0.0.1 5000 $t /tmp/out >/dev/null 2>&1
  diff -r "$DATA/$t" /tmp/out/$t >/dev/null && echo "$t OK" || echo "$t MISMATCH"
done
pkill -x server
```

All ten lines should say `OK` (files are byte-for-byte identical).

More checks:

| What | Command | Expected |
|---|---|---|
| Unknown topic | `./client 127.0.0.1 5000 nope /tmp/out` | `server error 2: unknown topic`, exit code 1 |
| Case-insensitive | `./client 127.0.0.1 5000 FARFALLA /tmp/out` | works |
| Server not running | `./client 127.0.0.1 5999 cane /tmp/out` | `cannot connect ...` |
| Wrong arguments | `./server` or `./client` | usage message |
| Graceful stop | Ctrl+C in the server terminal | prints totals and exits |

---

## 7. Performance experiment (single vs multi-threaded)

Goal: aggregate transfer rate vs number of clients, for both server modes.
The recommended setup is **3 machines**: machine A runs the server, B and C each run half of the clients.

**On machine A (server)** – run once per mode:

```bash
./server 5000 /path/to/raw-img --mode single      # first experiment set
./server 5000 /path/to/raw-img --mode multi       # second experiment set
```

**On machines B and C** – start them at about the same moment (clocks should be NTP-synced):

```bash
# usage: ./bench.sh <server_ip> <port> <label> <N_on_this_machine> <results.csv> <topic1,topic2,...> [trial_id] [total_clients]
# Machine B (8 clients, trial 1, intended total 16 clients):
./bench.sh <A_ip> 5000 single 8 B.csv cane,cavallo,elefante,farfalla 1 16
# Machine C (8 clients, trial 1, intended total 16 clients):
./bench.sh <A_ip> 5000 single 8 C.csv gallina,gatto,mucca,pecora 1 16
```

Here 8 + 8 = **16 clients total**. Specifying the shared `trial_id` (e.g. `1`, `2`, `3`) and `total_clients` (`16`) ensures `aggregate.py` properly merges distributed client groups belonging to the same trial while keeping independent trial repeats separate.

**Combine the results** (copy B.csv and C.csv to one place):

```bash
python3 aggregate.py B.csv C.csv -o summary_single.csv
```

Output columns: `trial, label, total_clients, ok, failed, total_data_bytes, wall_secs, aggregate_mbps, avg_client_mbps`.
Aggregate rate = total data bytes of all clients in the trial / (latest end - earliest start).

**Recommended sweep:** N = 2, 4, 8, 16, 32 (split evenly between B and C), for both `single` and `multi`,
3–5 repeats per point (average them), and one warm-up run first so the server's disk cache is the same
for every measurement. Plot aggregate Mbps vs N for both modes.

> Single-machine runs (`127.0.0.1`) are only for checking that the scripts work. Their numbers are not
> meaningful because server and clients compete for the same CPU and disk.

---

## 8. Troubleshooting

| Problem | Fix |
|---|---|
| `bind: Address already in use` | An old server is still running: `pkill -x server`, wait a second, retry (or use another port) |
| `cannot open topics dir` | Wrong path. Give the folder that directly contains `cane`, `gatto`, ...; use a full path (`$PWD/raw-img`) |
| `./bench.sh: Permission denied` | `chmod +x bench.sh aggregate.py` |
| `^M` / `bad interpreter` | `sed -i 's/\r$//' *.sh *.c *.h *.py Makefile` |
| Client can't connect from another machine | Check the IP, that the server is running, the firewall (`sudo ufw allow 5000`), and that the VMs share a network |
| `make: command not found` | `sudo apt install build-essential` (this code needs Linux, not plain Windows) |
| `Permission denied` running `./server` | The folder is on a mount that doesn't allow executables; copy it to your home directory and run `make` there |

---

## 9. Part 2 (fault tolerance) – to be added

This section will describe, for each case, the build/run commands and the fault-injection mechanism:

- **Interface (fixed by the assignment):**
  `./server <port> <topics_root_dir> <failure_probability>` and the same client command as Part 1.
- **Fault injection:** the server will make a random decision before each data chunk is sent and close the
  connection with the given probability.
- **Case 1:** client detects the drop and restarts the topic from the beginning.
- **Case 2:** server keeps a session table and the client a checkpoint file; after a drop the client reconnects
  and the transfer resumes from the last committed byte offset.
- **Case 2 Enhanced:** several parallel TCP connections per transfer, with per-range recovery.

---

## 10. Team

_Group numbers, member names, roll numbers and individual contributions go here._