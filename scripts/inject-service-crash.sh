#!/usr/bin/env bash
# Terminal 3: kill the victim and append a fatal unexpected-exit line.
# The stale pidfile intentionally remains so the Probe skill sees a dead process.
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

echo "fatal service=victim process exited unexpectedly signal=TERM" >> "$LOG"
echo "[inject] victim process killed; unexpected-exit line appended"
