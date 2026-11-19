#!/usr/bin/env bash
# Terminal 3: create a port conflict and append two identical bind errors.
# The first line is repaired; the second should be suppressed by CooldownTable.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="${VICTIM_LOG:-$ROOT/data/logs/victim.log}"
PORT="${VICTIM_PORT:-18080}"

"$ROOT/scripts/inject-port-conflict.sh"
echo "error service=victim bind: address already in use port=$PORT" >> "$LOG"
echo "[inject] duplicate bind error appended; expect one repair and one Cooldown"
