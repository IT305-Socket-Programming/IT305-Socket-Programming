#!/usr/bin/env bash
# bench.sh - launch N clients in parallel on THIS machine and append one CSV row per client.
#
# usage: ./bench.sh <server_ip> <port> <label> <N> <results.csv> <topic1,topic2,...>
#   label  : e.g. single or multi (what the server was started as)
#   topics : clients take them round-robin (client i asks for topic i mod count)
#
# Run it on each client machine (with N/2 clients each), into the same or different CSVs,
# then use aggregate.py. Client machines should have NTP-synced clocks (timestamps are epoch).
set -u
if [ $# -ne 6 ]; then
  sed -n '2,9p' "$0"; exit 2
fi
IP=$1; PORT=$2; LABEL=$3; N=$4; CSV=$5; IFS=',' read -r -a TOPICS <<< "$6"
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d /tmp/bench.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

[ -f "$CSV" ] || echo "label,host,nclients,client_id,topic,status,files,data_bytes,wire_bytes,start,end,secs,mbps" > "$CSV"

for i in $(seq 0 $((N-1))); do
  topic=${TOPICS[$((i % ${#TOPICS[@]}))]}
  ( "$HERE/client" "$IP" "$PORT" "$topic" "$TMP/out_$i" > "$TMP/res_$i" 2> "$TMP/err_$i"; echo $? > "$TMP/rc_$i" ) &
done
wait

HOST=$(hostname)
for i in $(seq 0 $((N-1))); do
  topic=${TOPICS[$((i % ${#TOPICS[@]}))]}
  rc=$(cat "$TMP/rc_$i")
  line=$(grep '^RESULT' "$TMP/res_$i" || true)
  if [ "$rc" = 0 ] && [ -n "$line" ]; then
    get() { echo "$line" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
    echo "$LABEL,$HOST,$N,$i,$topic,ok,$(get files),$(get data_bytes),$(get wire_bytes),$(get start),$(get end),$(get secs),$(get mbps)" >> "$CSV"
  else
    echo "$LABEL,$HOST,$N,$i,$topic,FAIL,0,0,0,0,0,0,0" >> "$CSV"
    echo "client $i failed: $(tail -1 "$TMP/err_$i")" >&2
  fi
done
echo "done: $N clients, label=$LABEL -> $CSV"
