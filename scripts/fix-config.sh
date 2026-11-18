#!/usr/bin/env bash
# Trusted remediation script: placeholder for the config-corruption demo mode.
# The port-conflict demo deliberately does not use this path.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT/scripts/restart-victim.sh"
