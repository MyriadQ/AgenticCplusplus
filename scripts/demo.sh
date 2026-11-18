#!/usr/bin/env bash
# One failure mode: port conflict. Re-runnable; teardown by pidfile only.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AGENTD="${AGENTD:-$ROOT/build/debug/bin/agentd}"
VICTIM="${VICTIM:-$ROOT/build/debug/bin/victim}"
LOG="${VICTIM_LOG:-$ROOT/data/logs/victim.log}"
RUN="${RUN_DIR:-$ROOT/run}"
PORT="${VICTIM_PORT:-18080}"
CASSETTE="${CASSETTE:-$ROOT/tests/data/port-conflict.reply.json}"
CONFIG="${CONFIG:-$ROOT/config/agent.json}"

mkdir -p "$RUN" "$(dirname "$LOG")"

teardown() {
    set +e
    if [ -f "$RUN/victim.pid" ]; then
        kill "$(cat "$RUN/victim.pid")" 2>/dev/null
        rm -f "$RUN/victim.pid"
    fi
    if [ -f "$RUN/decoy.pid" ]; then
        kill "$(cat "$RUN/decoy.pid")" 2>/dev/null
        rm -f "$RUN/decoy.pid"
    fi
    set -e
}
trap teardown EXIT INT TERM
teardown

wait_health() {
    for _ in $(seq 1 50); do
        if curl -fsS --max-time 1 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

: > "$LOG"

"$VICTIM" --log "$LOG" --port "$PORT" >/dev/null 2>&1 &
echo $! > "$RUN/victim.pid"

if ! wait_health; then
    echo "[demo] FAIL: victim did not become healthy" >&2
    cat "$LOG" >&2 || true
    exit 1
fi
echo "[demo] victim healthy"

kill "$(cat "$RUN/victim.pid")" 2>/dev/null || true
rm -f "$RUN/victim.pid"
sleep 0.2

python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 &
echo $! > "$RUN/decoy.pid"
sleep 0.3
if ! kill -0 "$(cat "$RUN/decoy.pid")" 2>/dev/null; then
    echo "[demo] FAIL: decoy failed to bind port ${PORT}" >&2
    exit 1
fi
echo "[demo] decoy owns port ${PORT}"

set +e
"$VICTIM" --log "$LOG" --port "$PORT" >/dev/null 2>&1
victim_rc=$?
set -e
if [ "$victim_rc" -eq 0 ]; then
    echo "[demo] FAIL: victim unexpectedly bound the occupied port" >&2
    exit 1
fi
if ! grep -q "address already in use" "$LOG"; then
    echo "[demo] FAIL: expected bind error missing from victim log" >&2
    cat "$LOG" >&2 || true
    exit 1
fi
echo "[demo] injected port conflict"

"$AGENTD" --config "$CONFIG" --replay "$LOG" --execute --yes \
    --llm=cassette "$CASSETTE" --stats

if ! wait_health; then
    echo "[demo] FAIL: service did not recover" >&2
    cat "$LOG" >&2 || true
    exit 1
fi
echo "[demo] PASS: service recovered"
