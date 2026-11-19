#!/usr/bin/env bash
# Reproduce the report's retrieval benchmark.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

cmake --build --preset release

echo "=== C++ retrieval ==="
for n in 1000 10000 100000; do
    ./build/release/bin/bench_search --n "$n" --iters 10
done

echo "=== C++ peak RSS ==="
/usr/bin/time -v ./build/release/bin/bench_search --n 10000 --iters 1 \
    >/tmp/agenticpp-bench-10k.out 2>/tmp/agenticpp-bench-10k.time
grep 'Maximum resident set size' /tmp/agenticpp-bench-10k.time
/usr/bin/time -v ./build/release/bin/bench_search --n 100000 --iters 1 \
    >/tmp/agenticpp-bench-100k.out 2>/tmp/agenticpp-bench-100k.time
grep 'Maximum resident set size' /tmp/agenticpp-bench-100k.time

echo "=== Python baseline: list of floats ==="
for n in 1000 10000; do
    python3 tools/bench_python_baseline.py --n "$n" --dim 512 --iters 3 --mode list
done

echo "=== Python baseline: flat array('f') ==="
for n in 1000 10000 100000; do
    python3 tools/bench_python_baseline.py --n "$n" --dim 512 --iters 3 --mode array
done

echo "=== idle RSS ==="
: > /tmp/agenticpp-empty.log
./build/release/bin/agentd --follow /tmp/agenticpp-empty.log --dry-run --no-llm --workers 1 \
    >/tmp/agenticpp-agent-idle.out 2>&1 &
agent_pid=$!
sleep 1
grep VmRSS "/proc/${agent_pid}/status"
kill -INT "$agent_pid" 2>/dev/null || true
wait "$agent_pid" 2>/dev/null || true

python3 -c 'import time; time.sleep(30)' &
python_pid=$!
sleep 1
grep VmRSS "/proc/${python_pid}/status"
kill "$python_pid" 2>/dev/null || true
wait "$python_pid" 2>/dev/null || true
