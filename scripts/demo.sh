#!/usr/bin/env bash
# End-to-end scenarios for the repair daemon.
#
#   ./scripts/demo.sh                      # port conflict (default)
#   ./scripts/demo.sh crash                # unexpected service exit
#   ./scripts/demo.sh oom                  # reported OOM / service down
#   ./scripts/demo.sh duplicate            # two identical error lines
#   ./scripts/demo.sh unfixable            # denied proposal / bounded cooldown
#   ./scripts/demo.sh all                  # every scenario
#
# Environment:
#   KEEP_MEMORY=1  leave the learned-fix journal for live inspection
#   WORKERS=N      Agent worker count (default 1 in the demo for readable output)
#   VICTIM_PORT=   override the demo port
#   LLM=cassette|deepseek|none  LLM transport (default cassette)
#   NO_SKILLS=1    force the LLM path instead of local deterministic skills
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/scripts/load-env.sh"
AGENTD="${AGENTD:-$ROOT/build/debug/bin/agentd}"
VICTIM="${VICTIM:-$ROOT/build/debug/bin/victim}"
LOG="${VICTIM_LOG:-$ROOT/data/logs/victim.log}"
RUN="${RUN_DIR:-$ROOT/run}"
PORT="${VICTIM_PORT:-18080}"
CASSETTE="${CASSETTE:-$ROOT/tests/data/port-conflict.reply.json}"
CONFIG="${CONFIG:-$ROOT/config/agent.json}"
KEEP_MEMORY="${KEEP_MEMORY:-0}"
WORKERS="${WORKERS:-1}"
LLM="${LLM:-cassette}"
NO_SKILLS="${NO_SKILLS:-0}"
SCENARIO="${1:-${SCENARIO:-port-conflict}}"
CONFIG_TMP=""

mkdir -p "$RUN" "$(dirname "$LOG")"

now_ms() {
    date +%s%3N
}

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

wait_health() {
    for _ in $(seq 1 50); do
        if curl -fsS --max-time 1 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_pid_gone() {
    local pid="$1"
    for _ in $(seq 1 20); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.05
    done
    return 1
}

start_victim_healthy() {
    : > "$LOG"
    "$VICTIM" --log "$LOG" --port "$PORT" >/dev/null 2>&1 &
    echo $! > "$RUN/victim.pid"
    if ! wait_health; then
        echo "[demo] FAIL: victim did not become healthy" >&2
        cat "$LOG" >&2 || true
        exit 1
    fi
    echo "[demo] victim healthy"
}

start_decoy() {
    python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 &
    echo $! > "$RUN/decoy.pid"
    sleep 0.3
    if ! kill -0 "$(cat "$RUN/decoy.pid")" 2>/dev/null; then
        echo "[demo] FAIL: decoy failed to bind port ${PORT}" >&2
        exit 1
    fi
    echo "[demo] decoy owns port ${PORT}"
}

run_agent() {
    local output_file="${1:-}"
    local cassette="${2:-$CASSETTE}"
    local llm_args=()
    case "$LLM" in
        none) llm_args=(--no-llm) ;;
        cassette) llm_args=(--llm=cassette "$cassette") ;;
        deepseek) llm_args=(--llm=deepseek) ;;
        *)
            echo "[demo] invalid LLM mode: $LLM" >&2
            return 2
            ;;
    esac
    if [ -z "$CONFIG_TMP" ] && [ "$PORT" != "18080" ]; then
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
    local command=(
        "$AGENTD" --config "$CONFIG" --replay "$LOG" --execute --yes
        "${llm_args[@]}" --workers "$WORKERS" --stats
    )
    if [ "$NO_SKILLS" = "1" ]; then
        command+=(--no-skills)
    fi
    if [ "$KEEP_MEMORY" = "1" ]; then
        command+=(--keep-memory-journal)
    fi
    if [ -n "$output_file" ]; then
        "${command[@]}" | tee "$output_file"
    else
        "${command[@]}"
    fi
}

require_recovered() {
    if ! wait_health; then
        echo "[demo] FAIL: service did not recover" >&2
        cat "$LOG" >&2 || true
        exit 1
    fi
    echo "[demo] PASS: service recovered"
}

scenario_port_conflict() {
    local cassette="${1:-$CASSETTE}"
    teardown
    start_victim_healthy

    kill "$(cat "$RUN/victim.pid")" 2>/dev/null || true
    rm -f "$RUN/victim.pid"
    sleep 0.2
    start_decoy

    set +e
    "$VICTIM" --log "$LOG" --port "$PORT" >/dev/null 2>&1
    local victim_rc=$?
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

    run_agent "" "$cassette"
    require_recovered
}

scenario_new_llm() {
    teardown
    start_victim_healthy

    echo "fatal service=victim unknown failure code=42" >> "$LOG"
    echo "[demo] injected unknown failure for an LLM decision"

    run_agent
    require_recovered
}

scenario_hard_port() {
    scenario_port_conflict "$ROOT/tests/data/hard-port.replies.json"
}

scenario_crash() {
    teardown
    start_victim_healthy

    local victim_pid
    victim_pid="$(cat "$RUN/victim.pid")"
    kill "$victim_pid" 2>/dev/null || true
    wait_pid_gone "$victim_pid" || true
    # Keep the stale pidfile: this is the "process disappeared" case.
    echo "fatal service=victim process exited unexpectedly signal=TERM" >> "$LOG"
    echo "[demo] injected unexpected service exit"

    run_agent
    require_recovered
}

scenario_oom() {
    teardown
    start_victim_healthy

    local victim_pid
    victim_pid="$(cat "$RUN/victim.pid")"
    kill "$victim_pid" 2>/dev/null || true
    wait_pid_gone "$victim_pid" || true
    # This is a synthetic memory-pressure report: the service is down, so the
    # service-state skill can restart it. It does not allocate host memory.
    echo "fatal service=victim out of memory: killed by OOM" >> "$LOG"
    echo "[demo] injected reported OOM / service down"

    run_agent
    require_recovered
}

scenario_unfixable() {
    teardown
    start_victim_healthy

    echo "fatal service=victim unknown failure code=42" >> "$LOG"
    echo "[demo] injected unknown failure with a denied cassette proposal"

    local output
    output="$(mktemp)"
    run_agent "$output" "$ROOT/tests/data/denied.reply.json"
    if ! grep -q "\[COOLDOWN\]" "$output"; then
        echo "[demo] FAIL: denied proposal did not end in Cooldown" >&2
        cat "$output" >&2 || true
        rm -f "$output"
        exit 1
    fi
    if grep -q "\[DONE\]" "$output"; then
        echo "[demo] FAIL: denied proposal was reported as learned" >&2
        cat "$output" >&2 || true
        rm -f "$output"
        exit 1
    fi
    rm -f "$output"

    if ! wait_health; then
        echo "[demo] FAIL: service was damaged by the denied proposal" >&2
        cat "$LOG" >&2 || true
        exit 1
    fi
    echo "[demo] PASS: denied proposal cooled down without a repair"
}

scenario_duplicate() {
    teardown
    start_victim_healthy

    kill "$(cat "$RUN/victim.pid")" 2>/dev/null || true
    rm -f "$RUN/victim.pid"
    sleep 0.2
    start_decoy

    echo "error service=victim bind: address already in use port=$PORT" >> "$LOG"
    echo "error service=victim bind: address already in use port=$PORT" >> "$LOG"
    echo "[demo] injected duplicate port conflict"

    local output
    output="$(mktemp)"
    run_agent "$output"
    if ! grep -q "deduplicated" "$output"; then
        echo "[demo] FAIL: duplicate error was not suppressed" >&2
        cat "$output" >&2 || true
        rm -f "$output"
        exit 1
    fi
    rm -f "$output"
    echo "[demo] duplicate suppressed by CooldownTable"
    require_recovered
}

scenario_started_ms="$(now_ms)"
case "$SCENARIO" in
    port-conflict|port)
        scenario_port_conflict
        ;;
    crash|unexpected-exit)
        scenario_crash
        ;;
    oom|memory)
        scenario_oom
        ;;
    duplicate|dedup)
        scenario_duplicate
        ;;
    new-llm|llm-new)
        scenario_new_llm
        ;;
    hard-port|hard)
        scenario_hard_port
        ;;
    unfixable|denied|unknown)
        scenario_unfixable
        ;;
    all)
        for scenario in port-conflict crash oom duplicate unfixable; do
            echo
            echo "=== scenario: $scenario ==="
            "$0" "$scenario"
        done
        ;;
    *)
        echo "usage: $0 [port-conflict|crash|oom|duplicate|unfixable|new-llm|hard-port|all]" >&2
        exit 2
        ;;
esac

scenario_finished_ms="$(now_ms)"
printf '[timing] scenario=%s total_ms=%s\n' "$SCENARIO" \
    "$((scenario_finished_ms - scenario_started_ms))"
