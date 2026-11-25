#!/bin/sh
set -eu
fake=$1 examples=$2 work=$3
rm -rf "$work" && mkdir -p "$work"

fifo="$work/stdin"
mkfifo "$fifo"
"$fake" < "$fifo" > "$work/port" &
pid=$!
exec 3> "$fifo"
trap 'exec 3>&-; wait $pid 2>/dev/null || true' EXIT

i=0
while [ ! -s "$work/port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
port=$(cat "$work/port")

out=$("$examples/identify" "$port")
echo "$out"
echo "$out" | grep -q "Operator SIMULATED0001, firmware 10.1.0"
echo "$out" | grep -q "Game Boy / Color, game 65536 B, save 8192 B"

"$examples/backup_save" "$work/game.sav" "$port"
[ "$(wc -c < "$work/game.sav" | tr -d ' ')" = 8192 ]
if "$examples/backup_save" "$work/game.sav" "$port" 2>/dev/null; then
  echo "backup_save overwrote an existing backup" >&2
  exit 1
fi
echo "examples: ok"
