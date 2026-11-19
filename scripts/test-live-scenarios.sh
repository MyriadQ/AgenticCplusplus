#!/usr/bin/env bash
# End-to-end live-tail scenario matrix. Each scenario gets a fresh follower
# and service so the CooldownTable does not carry state across scenarios.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

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

OUTPUT="$(mktemp)"
LIVE_SHELL=""
SCENARIO_STARTED_MS=0
now_ms() {
    date +%s%3N
}

stop_agent() {
    local pid
    pid="$(pgrep -x agentd | head -1 || true)"
    if [ -n "$pid" ]; then
        kill -INT "$pid" 2>/dev/null || true
    fi
    if [ -n "$LIVE_SHELL" ]; then
        wait "$LIVE_SHELL" 2>/dev/null || true
        LIVE_SHELL=""
    fi
}

cleanup() {
    set +e
    stop_agent
    rm -f run/victim.pid run/decoy.pid
    set -e
}
trap cleanup EXIT INT TERM

wait_for_output() {
    local pattern="$1"
    for _ in $(seq 1 100); do
        if grep -q "$pattern" "$OUTPUT"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_for_journal() {
    local pattern="$1"
    for _ in $(seq 1 100); do
        if [ -f data/memory.journal ] && grep -q "$pattern" data/memory.journal; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_for_audit_since() {
    local start="$1"
    local pattern="$2"
    for _ in $(seq 1 100); do
        if tail -n "+$((start + 1))" data/audit.log 2>/dev/null | grep -q "$pattern"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_for_health() {
    for _ in $(seq 1 50); do
        if curl -fsS --max-time 1 "http://127.0.0.1:${VICTIM_PORT}/health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

start_live() {
    : > "$OUTPUT"
    rm -f data/memory.bin data/memory.journal run/victim.pid run/decoy.pid
    ./scripts/live-demo.sh >"$OUTPUT" 2>&1 &
    LIVE_SHELL=$!
    if ! wait_for_output "agentd: following"; then
        echo "[live-test] FAIL: follower did not start" >&2
        cat "$OUTPUT" >&2 || true
        exit 1
    fi
}

stop_live() {
    stop_agent
    if ! grep -q "^follow:" "$OUTPUT"; then
        echo "[live-test] FAIL: follow shutdown stats missing" >&2
        cat "$OUTPUT" >&2 || true
        exit 1
    fi
}

run_scenario() {
    local name="$1"
    local injector="$2"
    local journal_pattern="$3"
    local audit_start
    audit_start="$(wc -l < data/audit.log 2>/dev/null || echo 0)"

    echo "[live-test] scenario: $name"
    SCENARIO_STARTED_MS="$(now_ms)"
    start_live
    local repair_started
    repair_started="$(now_ms)"
    "./scripts/${injector}"

    if [ -n "$journal_pattern" ]; then
        if ! wait_for_journal "$journal_pattern"; then
            echo "[live-test] FAIL: journal pattern '$journal_pattern' missing for $name" >&2
            cat "$OUTPUT" >&2 || true
            exit 1
        fi
    fi
    if [ "$name" = "duplicate" ]; then
        if ! wait_for_audit_since "$audit_start" "deduplicated"; then
            echo "[live-test] FAIL: duplicate suppression not observed" >&2
            cat "$OUTPUT" >&2 || true
            exit 1
        fi
    fi
    if [ "$name" = "unfixable" ]; then
        if ! wait_for_audit_since "$audit_start" "attempts exhausted"; then
            echo "[live-test] FAIL: unfixable scenario did not reach Cooldown" >&2
            cat "$OUTPUT" >&2 || true
            exit 1
        fi
    fi
    if ! wait_for_health; then
        echo "[live-test] FAIL: service unhealthy after $name" >&2
        cat "$OUTPUT" >&2 || true
        exit 1
    fi
    local repair_finished
    repair_finished="$(now_ms)"

    stop_live
    if [ "$name" = "unfixable" ]; then
        if ! grep -q "\\[COOLDOWN\\]" "$OUTPUT" || grep -q "\\[DONE\\]" "$OUTPUT"; then
            echo "[live-test] FAIL: unfixable output was not a clean Cooldown" >&2
            cat "$OUTPUT" >&2 || true
            exit 1
        fi
    else
        if ! grep -q "\\[DONE\\]" "$OUTPUT"; then
            echo "[live-test] FAIL: $name did not reach Done" >&2
            cat "$OUTPUT" >&2 || true
            exit 1
        fi
    fi
    local total_finished
    total_finished="$(now_ms)"
    printf '[timing] live=%s repair_ms=%s total_ms=%s\n' "$name" \
        "$((repair_finished - repair_started))" "$((total_finished - SCENARIO_STARTED_MS))"
    echo "[live-test] PASS: $name"
}

run_scenario port-conflict inject-port-conflict.sh "kill-decoy.sh"
run_scenario crash inject-service-crash.sh "process exited unexpectedly"
run_scenario oom inject-oom.sh "out of memory"
run_scenario duplicate inject-duplicate.sh "kill-decoy.sh"
run_scenario unfixable inject-unfixable.sh ""

rm -f data/memory.bin data/memory.journal
echo "[live-test] PASS: all live scenarios"
