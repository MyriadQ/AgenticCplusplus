#!/usr/bin/env bash
# Foreground live demo:
#   terminal 1: ./scripts/live-demo.sh
#   terminal 2: tail -F data/memory.journal
#   terminal 3: ./scripts/inject-port-conflict.sh
#               ./scripts/inject-service-crash.sh
#               ./scripts/inject-oom.sh
#               ./scripts/inject-duplicate.sh
#               ./scripts/inject-unfixable.sh
#
# Terminal 1 starts the victim and runs agentd --follow in the foreground.
# Ctrl-C stops the follower and tears down the demo processes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/scripts/load-env.sh"
AGENTD="${AGENTD:-$ROOT/build/debug/bin/agentd}"
VICTIM="${VICTIM:-$ROOT/build/debug/bin/victim}"
LOG="${VICTIM_LOG:-$ROOT/data/logs/victim.log}"
RUN="${RUN_DIR:-$ROOT/run}"
PORT="${VICTIM_PORT:-18080}"
CONFIG="${CONFIG:-$ROOT/config/agent.json}"
CASSETTE="${CASSETTE:-$ROOT/tests/data/port-conflict.reply.json}"
WORKERS="${WORKERS:-1}"
FRESH="${FRESH:-1}"
KEEP_MEMORY="${KEEP_MEMORY:-1}"
LLM="${LLM:-none}"
NO_SKILLS="${NO_SKILLS:-0}"
CONFIG_TMP=""

mkdir -p "$RUN" "$(dirname "$LOG")"

export VICTIM_PORT="$PORT"
export VICTIM_LOG="$LOG"
export RUN_DIR="$RUN"

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
    if [ -n "$CONFIG_TMP" ]; then
        rm -f "$CONFIG_TMP"
    fi
    set -e
}
trap teardown EXIT INT TERM
teardown

if [ "$PORT" != "18080" ]; then
    CONFIG_TMP="$(mktemp)"
    python3 - "$CONFIG" "$CONFIG_TMP" "$PORT" <<'PY'
import json
import sys
with open(sys.argv[1]) as source:
    config = json.load(source)
config.setdefault("verifier", {})["health_url"] = f"http://127.0.0.1:{sys.argv[3]}/health"
with open(sys.argv[2], "w") as target:
    json.dump(config, target)
PY
    CONFIG="$CONFIG_TMP"
fi


if [ "$FRESH" = "1" ]; then
    rm -f "$ROOT/data/memory.bin" "$ROOT/data/memory.journal"
fi

: > "$LOG"
"$VICTIM" --log "$LOG" --port "$PORT" >/dev/null 2>&1 &
echo $! > "$RUN/victim.pid"

for _ in $(seq 1 50); do
    if curl -fsS --max-time 1 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
if ! curl -fsS --max-time 1 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
    echo "[live] FAIL: victim did not become healthy" >&2
    exit 1
fi
echo "[live] victim healthy on port ${PORT}"
echo "[live] follow agent is starting; choose an injector:"
echo "[live]   ./scripts/inject-port-conflict.sh"
echo "[live]   ./scripts/inject-service-crash.sh"
echo "[live]   ./scripts/inject-oom.sh"
echo "[live]   ./scripts/inject-duplicate.sh"
echo "[live]   ./scripts/inject-unfixable.sh"
echo "[live] tail the learned-fix journal with: tail -F data/memory.journal"

AGENTD_EXTRA=()
if [ "$KEEP_MEMORY" = "1" ]; then
    AGENTD_EXTRA+=(--keep-memory-journal)
fi
case "$LLM" in
    none) AGENTD_EXTRA+=(--no-llm) ;;
    deepseek) AGENTD_EXTRA+=(--llm=deepseek) ;;
    cassette) AGENTD_EXTRA+=(--llm=cassette "$CASSETTE") ;;
    *)
        echo "[live] FAIL: invalid LLM mode '$LLM'" >&2
        exit 2
        ;;
esac
if [ "$NO_SKILLS" = "1" ]; then
    AGENTD_EXTRA+=(--no-skills)
fi

"$AGENTD" --config "$CONFIG" --follow "$LOG" --execute --yes \
    --workers "$WORKERS" --stats "${AGENTD_EXTRA[@]}"
