#!/usr/bin/env bash
# Source local secrets without adding them to the shell history.
#
# The default file is env/deepseek.env. Override it with DEEPSEEK_ENV_FILE.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${DEEPSEEK_ENV_FILE:-$ROOT/env/deepseek.env}"
if [ -f "$ENV_FILE" ]; then
    set -a
    # shellcheck disable=SC1090
    . "$ENV_FILE"
    set +a
fi
