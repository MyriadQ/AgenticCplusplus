#!/usr/bin/env bash
# Full system smoke test: build, unit/integration suite, and every demo scenario.
#
#   ./scripts/test-system.sh              # debug suite + all demo scenarios
#   MATRIX=1 ./scripts/test-system.sh     # also run gcc-debug, debug-llm, tsan, release
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
system_started_ms="$(date +%s%3N)"

echo "=== build: debug ==="
cmake --preset debug >/dev/null
cmake --build --preset debug

echo "=== tests: debug ==="
ctest --preset debug --output-on-failure

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

echo "=== demo scenarios: all ==="
./scripts/demo.sh all

echo "=== live follow smoke test ==="
./scripts/test-live-scenarios.sh

if [ "${LLM_CASES:-0}" = "1" ]; then
    echo "=== LLM decision cases (cassette) ==="
    cmake --build --preset debug-llm
    LLM=cassette ./scripts/demo-llm-cases.sh
fi

if [ "${MATRIX:-0}" = "1" ]; then
    for preset in gcc-debug debug-llm tsan release; do
        echo "=== build: ${preset} ==="
        cmake --preset "$preset" >/dev/null
        cmake --build --preset "$preset"
        echo "=== tests: ${preset} ==="
        ctest --preset "$preset" --output-on-failure
    done
fi

system_finished_ms="$(date +%s%3N)"
printf '[timing] system total_ms=%s\n' "$((system_finished_ms - system_started_ms))"
echo "[system] PASS: full system test completed"
