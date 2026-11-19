#!/usr/bin/env bash
# Four-case LLM/learning demo:
#   1. port-conflict with an empty store, LLM path forced
#   2. the same incident again, proving recall avoids the LLM
#   3. a new incident that requires an LLM decision
#   4. a hard port case that fails verification once and succeeds on replan
#
# Default is the deterministic cassette. For a real DeepSeek run:
#   LLM=deepseek DEEPSEEK_API_KEY=... ./scripts/demo-llm-cases.sh
# The hard case always uses a two-reply cassette so the first failed attempt is
# reproducible.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
. "$ROOT/scripts/load-env.sh"

AGENTD="${AGENTD:-$ROOT/build/debug-llm/bin/agentd}"
LLM="${LLM:-cassette}"
VICTIM_PORT="${VICTIM_PORT:-}"

if [ ! -x "$AGENTD" ]; then
    echo "[llm-demo] FAIL: missing $AGENTD (build debug-llm)" >&2
    exit 1
fi
if [ "$LLM" = "deepseek" ] && [ -z "${DEEPSEEK_API_KEY:-}" ]; then
    echo "[llm-demo] FAIL: LLM=deepseek requires DEEPSEEK_API_KEY" >&2
    exit 1
fi
if [ -z "$VICTIM_PORT" ]; then
    VICTIM_PORT="$(python3 - <<'PY'
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
)"
fi
export VICTIM_PORT
export AGENTD
export LLM
export NO_SKILLS=1
export KEEP_MEMORY=0

OUT="$(mktemp)"
cleanup() {
    set +e
    rm -f "$OUT" data/memory.bin data/memory.journal run/victim.pid run/decoy.pid
    set -e
}
trap cleanup EXIT INT TERM

now_ms() {
    date +%s%3N
}

run_demo() {
    local scenario="$1"
    local cassette="${2:-$ROOT/tests/data/port-conflict.reply.json}"
    local started
    started="$(now_ms)"
    CASSETTE="$cassette" "$ROOT/scripts/demo.sh" "$scenario" >"$OUT" 2>&1
    cat "$OUT"
    local finished
    finished="$(now_ms)"
    printf '[timing] case=%s total_ms=%s\n' "$scenario" "$((finished - started))"
}

require() {
    local pattern="$1"
    if ! grep -q "$pattern" "$OUT"; then
        echo "[llm-demo] FAIL: missing '$pattern'" >&2
        cat "$OUT" >&2 || true
        exit 1
    fi
}

reject() {
    local pattern="$1"
    if grep -q "$pattern" "$OUT"; then
        echo "[llm-demo] FAIL: unexpected '$pattern'" >&2
        cat "$OUT" >&2 || true
        exit 1
    fi
}

echo "[llm-demo] transport=$LLM port=$VICTIM_PORT"
rm -f data/memory.bin data/memory.journal

echo
echo "=== 1. base port conflict: empty store, forced LLM ==="
run_demo port-conflict "$ROOT/tests/data/port-conflict.reply.json"
require "\[DIAGNOSE\]"
require "\[PROPOSE\]"
require "\[DONE\]"

echo
echo "=== 2. learned port conflict: recall hit, no LLM decision ==="
run_demo port-conflict "$ROOT/tests/data/port-conflict.reply.json"
require "recall hit"
require "\[DONE\]"
reject "\[DIAGNOSE\]"

echo
echo "=== 3. new incident: LLM decision required ==="
run_demo new-llm "$ROOT/tests/data/restart-victim.reply.json"
require "\[DIAGNOSE\]"
require "\[PROPOSE\]"
require "\[DONE\]"

echo
echo "=== 4. hard port conflict: first attempt fails, replan succeeds ==="
rm -f data/memory.bin data/memory.journal
LLM=cassette CASSETTE="$ROOT/tests/data/hard-port.replies.json" \
    run_demo hard-port "$ROOT/tests/data/hard-port.replies.json"
require "restart-victim"
require "\[REPLAN\]"
require "kill-decoy"
require "after 2 attempt(s)"
require "\[DONE\]"

echo
echo "[llm-demo] PASS: all four cases"
