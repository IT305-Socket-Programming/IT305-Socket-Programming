#!/usr/bin/env python3
import csv
from collections import defaultdict
import math
import sys

def aggregate(raw_file, summary_file):
    data = defaultdict(list)

    with open(raw_file, 'r', newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            key = (row['variant'], int(row['streams']), float(row['failure_prob']))
            data[key].append(row)

    with open(summary_file, 'w', newline='') as f:
        writer = csv.writer(f, lineterminator='\n')
        writer.writerow([
            "variant", "streams", "failure_prob", "trials", "success_rate",
            "avg_elapsed_sec", "std_elapsed_sec", "avg_useful_bytes",
            "avg_transmitted_bytes", "avg_retransmitted_bytes", "avg_mbps"
        ])

        for key in sorted(data.keys()):
            var, streams, prob = key
            rows = data[key]
            n = len(rows)
            successes = sum(1 for r in rows if r['status'] == 'OK')
            ok_rows = [r for r in rows if r['status'] == 'OK']

            if ok_rows:
                times = [float(r['elapsed_sec']) for r in ok_rows]
                useful = [int(r['useful_bytes']) for r in ok_rows]

                avg_time = sum(times) / len(times)
                std_time = math.sqrt(sum((x - avg_time)**2 for x in times) / len(times)) if len(times) > 1 else 0.0
                avg_useful = sum(useful) / len(useful)
                # True aggregate goodput (Mbps): useful bits delivered divided by total transfer time
                avg_mbps = (avg_useful * 8.0) / (avg_time * 1e6) if avg_time > 0 else 0.0
            else:
                avg_time = std_time = avg_useful = avg_mbps = 0.0

            # Network traffic expended across all attempted trials (including aborted attempts)
            avg_retrans = sum(int(r['retransmitted_bytes']) for r in rows) / n
            avg_trans = sum(int(r['transmitted_bytes']) for r in rows) / n

            succ_rate = (successes / n) * 100.0 if n > 0 else 0.0

            writer.writerow([
                var, streams, f"{prob:.2f}", n, f"{succ_rate:.1f}%",
                f"{avg_time:.4f}", f"{std_time:.4f}", int(avg_useful),
                int(avg_trans), int(avg_retrans), f"{avg_mbps:.2f}"
            ])

    print(f"Aggregated summary written to: {summary_file}")

if __name__ == '__main__':
    raw = sys.argv[1] if len(sys.argv) > 1 else "Results/raw/experiments_latest.csv"
    summary = sys.argv[2] if len(sys.argv) > 2 else "Results/processed/summary.csv"
    aggregate(raw, summary)
