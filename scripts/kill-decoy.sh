#!/usr/bin/env bash
# Trusted remediation script: free port 18080 by stopping the decoy and restarting victim.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="${RUN_DIR:-$ROOT/run}"
LOG="${VICTIM_LOG:-$ROOT/data/logs/victim.log}"
PORT="${VICTIM_PORT:-18080}"

mkdir -p "$RUN" "$(dirname "$LOG")"

if [ -f "$RUN/decoy.pid" ]; then
    decoy_pid="$(cat "$RUN/decoy.pid")"
    if [ -n "$decoy_pid" ]; then
        kill "$decoy_pid" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$decoy_pid" 2>/dev/null || break
            sleep 0.05
        done
        kill -9 "$decoy_pid" 2>/dev/null || true
    fi
    rm -f "$RUN/decoy.pid"
fi

if [ -f "$RUN/victim.pid" ]; then
    victim_pid="$(cat "$RUN/victim.pid")"
    if [ -n "$victim_pid" ]; then
        kill "$victim_pid" 2>/dev/null || true
    fi
    rm -f "$RUN/victim.pid"
fi

# Give the kernel a moment to release the old listener before rebinding.
sleep 0.2

VICTIM_BIN="${VICTIM:-}"
if [ -z "$VICTIM_BIN" ]; then
    for candidate in "$ROOT/build/debug/bin/victim" "$ROOT/build/gcc-debug/bin/victim" \
                     "$ROOT/build/release/bin/victim"; do
        if [ -x "$candidate" ]; then
            VICTIM_BIN="$candidate"
            break
        fi
    done
fi
if [ -z "$VICTIM_BIN" ]; then
    echo "kill-decoy: victim binary not found" >&2
    exit 1
fi

nohup "$VICTIM_BIN" --log "$LOG" --port "$PORT" >/dev/null 2>&1 &
echo $! > "$RUN/victim.pid"
exit 0
