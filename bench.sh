#!/usr/bin/env bash
# bench.sh - launch N clients in parallel on THIS machine and append one CSV row per client.
#
# usage: ./bench.sh <server_ip> <port> <label> <N> <results.csv> <topic1,topic2,...> [trial_id] [total_clients]
#   label         : e.g. single or multi (what the server was started as)
#   topics        : clients take them round-robin (client i asks for topic i mod count)
#   trial_id      : optional trial/run identifier (default: 1)
#   total_clients : optional intended total concurrency across all machines (default: N)
#
# Run it on each client machine (with N/2 clients each), into the same or different CSVs,
# then use aggregate.py. Client machines should have NTP-synced clocks (timestamps are epoch).
set -u
if [ $# -lt 6 ] || [ $# -gt 8 ]; then
  sed -n '2,11p' "$0"; exit 2
fi
IP=$1; PORT=$2; LABEL=$3; N=$4; CSV=$5; IFS=',' read -r -a TOPICS <<< "$6"
TRIAL=${7:-1}
TOTAL_CLIENTS=${8:-$N}

if ! [[ "$N" =~ ^[0-9]+$ ]] || [ "$N" -le 0 ]; then
  echo "error: N must be a positive integer" >&2
  exit 2
fi

if ! [[ "$TOTAL_CLIENTS" =~ ^[0-9]+$ ]] || [ "$TOTAL_CLIENTS" -le 0 ]; then
  echo "error: total_clients must be a positive integer" >&2
  exit 2
fi

if [ ${#TOPICS[@]} -eq 0 ] || [ -z "${TOPICS[0]}" ]; then
  echo "error: at least one topic must be specified" >&2
  exit 2
fi

HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d /tmp/bench.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

[ -f "$CSV" ] || echo "trial,label,host,total_clients,nclients,client_id,topic,status,files,data_bytes,wire_bytes,start,end,secs,mbps" > "$CSV"

for i in $(seq 0 $((N-1))); do
  topic=${TOPICS[$((i % ${#TOPICS[@]}))]}
  ( "$HERE/client" "$IP" "$PORT" "$topic" "$TMP/out_$i" > "$TMP/res_$i" 2> "$TMP/err_$i"; echo $? > "$TMP/rc_$i" ) &
done
wait

HOST=$(hostname)
get() { echo "$line" | tr ' ' '\n' | sed -n "s/^$1=//p"; }

for i in $(seq 0 $((N-1))); do
  topic=${TOPICS[$((i % ${#TOPICS[@]}))]}
  rc=$(cat "$TMP/rc_$i")
  line=$(grep '^RESULT' "$TMP/res_$i" || true)
  if [ "$rc" = 0 ] && [ -n "$line" ]; then
    echo "$TRIAL,$LABEL,$HOST,$TOTAL_CLIENTS,$N,$i,$topic,ok,$(get files),$(get data_bytes),$(get wire_bytes),$(get start),$(get end),$(get secs),$(get mbps)" >> "$CSV"
  else
    echo "$TRIAL,$LABEL,$HOST,$TOTAL_CLIENTS,$N,$i,$topic,FAIL,0,0,0,0,0,0,0" >> "$CSV"
    echo "client $i failed: $(tail -1 "$TMP/err_$i")" >&2
  fi
done
echo "done: $N clients (trial=$TRIAL, total=$TOTAL_CLIENTS), label=$LABEL -> $CSV"
