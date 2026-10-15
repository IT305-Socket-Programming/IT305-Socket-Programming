#!/usr/bin/env python3
"""aggregate.py - turn per-client rows from bench.sh into aggregate throughput.

usage: python3 aggregate.py results1.csv [results2.csv ...] [-o summary.csv]

Rows with the same (label, nclients) are one experiment (clients from several machines are
merged). Aggregate rate = total data bytes / (latest end - earliest start), in Mbps.
"""
import csv, sys, collections

args = sys.argv[1:]
out = None
if "-o" in args:
    i = args.index("-o"); out = args[i + 1]; del args[i:i + 2]
if not args:
    sys.exit(__doc__)

groups = collections.defaultdict(list)
for path in args:
    with open(path) as f:
        for r in csv.DictReader(f):
            groups[(r["label"], int(r["nclients"]))].append(r)

rows = []
for (label, n), rs in sorted(groups.items()):
    ok = [r for r in rs if r["status"] == "ok"]
    if not ok:
        continue
    total = sum(int(r["data_bytes"]) for r in ok)
    span = max(float(r["end"]) for r in ok) - min(float(r["start"]) for r in ok)
    avg_client = sum(float(r["mbps"]) for r in ok) / len(ok)
    rows.append([label, n, len(ok), len(rs) - len(ok), total, round(span, 3),
                 round(total * 8 / 1e6 / span, 2) if span > 0 else 0, round(avg_client, 2)])

hdr = ["label", "nclients", "ok", "failed", "total_data_bytes", "wall_secs", "aggregate_mbps", "avg_client_mbps"]
print("%-8s %8s %4s %6s %16s %10s %15s %16s" % tuple(hdr))
for r in rows:
    print("%-8s %8d %4d %6d %16d %10.3f %15.2f %16.2f" % tuple(r))
if out:
    with open(out, "w", newline="") as f:
        w = csv.writer(f); w.writerow(hdr); w.writerows(rows)
    print("wrote", out)
