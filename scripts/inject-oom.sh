#!/usr/bin/env bash
# Terminal 3: report a synthetic OOM while the victim is down.
# This does not allocate memory; it exercises the service-down skill path.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="${RUN_DIR:-$ROOT/run}"
LOG="${VICTIM_LOG:-$ROOT/data/logs/victim.log}"

if [ -f "$RUN/victim.pid" ]; then
    victim_pid="$(cat "$RUN/victim.pid")"
    if [ -n "$victim_pid" ]; then
        kill "$victim_pid" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$victim_pid" 2>/dev/null || break
            sleep 0.05
        done
    fi
fi

echo "fatal service=victim out of memory: killed by OOM" >> "$LOG"
echo "[inject] reported OOM / service-down line appended"
