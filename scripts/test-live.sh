#!/usr/bin/env bash
# Non-interactive live-tail smoke test.
# Starts live-demo.sh, injects a port conflict, waits for the repair, then
# sends SIGINT to the follower and checks the shutdown path.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUTPUT="$(mktemp)"
LIVE_SHELL=""

cleanup() {
    set +e
    local agent_pid
    agent_pid="$(pgrep -x agentd | head -1 || true)"
    if [ -n "$agent_pid" ]; then
        kill -INT "$agent_pid" 2>/dev/null
    fi
    if [ -n "$LIVE_SHELL" ]; then
        wait "$LIVE_SHELL" 2>/dev/null
    fi
    rm -f run/victim.pid run/decoy.pid
    set -e
}
trap cleanup EXIT INT TERM

if [ -z "${VICTIM_PORT:-}" ]; then
    VICTIM_PORT="$(python3 - <<'PY'
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
)"
    export VICTIM_PORT
fi

rm -f data/memory.bin data/memory.journal
./scripts/live-demo.sh >"$OUTPUT" 2>&1 &
LIVE_SHELL=$!

for _ in $(seq 1 50); do
    if grep -q "agentd: following" "$OUTPUT"; then
        break
    fi
    sleep 0.1
done
if ! grep -q "agentd: following" "$OUTPUT"; then
    echo "[live-test] FAIL: follower did not start" >&2
    cat "$OUTPUT" >&2 || true
    exit 1
fi

./scripts/inject-port-conflict.sh

for _ in $(seq 1 100); do
    if [ -f data/memory.journal ] && grep -q "kill-decoy.sh" data/memory.journal; then
        break
    fi
    sleep 0.1
done
if ! grep -q "kill-decoy.sh" data/memory.journal; then
    echo "[live-test] FAIL: repair was not learned" >&2
    cat "$OUTPUT" >&2 || true
    cat data/memory.journal >&2 2>/dev/null || true
    exit 1
fi

AGENT_PID="$(pgrep -x agentd | head -1 || true)"
if [ -z "$AGENT_PID" ]; then
    echo "[live-test] FAIL: follower disappeared" >&2
    cat "$OUTPUT" >&2 || true
    exit 1
fi
kill -INT "$AGENT_PID"
wait "$LIVE_SHELL" || true
LIVE_SHELL=""

if ! grep -q "\[DONE\]" "$OUTPUT"; then
    echo "[live-test] FAIL: repair did not reach Done" >&2
    cat "$OUTPUT" >&2 || true
    exit 1
fi
if ! grep -q "\[PROBE\].*recall miss" "$OUTPUT"; then
    echo "[live-test] FAIL: deterministic Probe path missing" >&2
    cat "$OUTPUT" >&2 || true
    exit 1
fi
if ! grep -q "^follow:" "$OUTPUT"; then
    echo "[live-test] FAIL: follow shutdown stats missing" >&2
    cat "$OUTPUT" >&2 || true
    exit 1
fi

rm -f data/memory.bin data/memory.journal
echo "[live-test] PASS: follow, inject, repair, and shutdown"
