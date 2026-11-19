#!/usr/bin/env bash
# Terminal 3: stop the victim, occupy the demo port, and append a bind error.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="${RUN_DIR:-$ROOT/run}"
LOG="${VICTIM_LOG:-$ROOT/data/logs/victim.log}"
PORT="${VICTIM_PORT:-18080}"

if [ -f "$RUN/victim.pid" ]; then
    victim_pid="$(cat "$RUN/victim.pid")"
    if [ -n "$victim_pid" ]; then
        kill "$victim_pid" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$victim_pid" 2>/dev/null || break
            sleep 0.05
        done
    fi
    rm -f "$RUN/victim.pid"
fi

if [ -f "$RUN/decoy.pid" ]; then
    kill "$(cat "$RUN/decoy.pid")" 2>/dev/null || true
    rm -f "$RUN/decoy.pid"
fi

python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 &
echo $! > "$RUN/decoy.pid"
sleep 0.3
if ! kill -0 "$(cat "$RUN/decoy.pid")" 2>/dev/null; then
    echo "[inject] FAIL: decoy did not bind port ${PORT}" >&2
    exit 1
fi

echo "error service=victim bind: address already in use port=$PORT" >> "$LOG"
echo "[inject] decoy owns ${PORT}; bind error appended to ${LOG}"
