#!/usr/bin/env bash
# Terminal 3: append an unknown fatal line. With --no-llm this should decline
# safely after bounded replans instead of executing anything.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="${VICTIM_LOG:-$ROOT/data/logs/victim.log}"

echo "fatal service=victim unknown failure code=42" >> "$LOG"
echo "[inject] unknown fatal line appended; expect Cooldown, not Done"
