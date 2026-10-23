#!/usr/bin/env python3
"""aggregate.py - turn per-client rows from bench.sh into aggregate throughput.

usage: python3 aggregate.py results1.csv [results2.csv ...] [-o summary.csv]

Groups rows by (trial, label, total_clients). Clients from distributed machines
belonging to the same trial are combined. Independent trials are never merged.
Aggregate rate = total useful data bytes / (latest end - earliest start), in Mbps.
"""
import csv, sys, collections

args = sys.argv[1:]
out = None
if "-o" in args:
    i = args.index("-o")
    out = args[i + 1]
    del args[i:i + 2]
if not args:
    sys.exit(__doc__)

groups = collections.defaultdict(list)
for path in args:
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            if not r or not any(r.values()):
                continue
            trial = r.get("trial", "1").strip() if r.get("trial") else "1"
            label = r.get("label", "unknown").strip()
            if "total_clients" in r and r["total_clients"]:
                total_clients = int(r["total_clients"])
            elif "nclients" in r and r["nclients"]:
                total_clients = int(r["nclients"])
            else:
                total_clients = 1
            groups[(trial, label, total_clients)].append(r)

rows = []
# Sort by trial (numeric if possible), label, total_clients
def sort_key(item):
    k = item[0]
    try:
        trial_key = (0, int(k[0]), "")
    except ValueError:
        trial_key = (1, 0, str(k[0]))
    return (k[1], k[2], trial_key)

for (trial, label, total_clients), rs in sorted(groups.items(), key=sort_key):
    ok = [r for r in rs if r.get("status") == "ok"]
    n_ok = len(ok)
    n_failed = len(rs) - n_ok
    if n_ok > 0:
        total_bytes = sum(int(r["data_bytes"]) for r in ok)
        starts = [float(r["start"]) for r in ok if float(r.get("start", 0)) > 0]
        ends = [float(r["end"]) for r in ok if float(r.get("end", 0)) > 0]
        if starts and ends:
            span = max(ends) - min(starts)
        else:
            span = sum(float(r["secs"]) for r in ok) / n_ok
        agg_mbps = round(total_bytes * 8.0 / 1e6 / span, 2) if span > 0 else 0.0
        avg_client = round(sum(float(r["mbps"]) for r in ok) / n_ok, 2)
    else:
        total_bytes = 0
        span = 0.0
        agg_mbps = 0.0
        avg_client = 0.0

    rows.append([trial, label, total_clients, n_ok, n_failed, total_bytes, round(span, 3), agg_mbps, avg_client])

hdr = ["trial", "label", "total_clients", "ok", "failed", "total_data_bytes", "wall_secs", "aggregate_mbps", "avg_client_mbps"]
fmt = "%-8s %-8s %13s %4s %6s %16s %10s %15s %16s"
print(fmt % tuple(hdr))
for r in rows:
    print("%-8s %-8s %13d %4d %6d %16d %10.3f %15.2f %16.2f" % tuple(r))
if out:
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(hdr)
        w.writerows(rows)
    print("wrote", out)
