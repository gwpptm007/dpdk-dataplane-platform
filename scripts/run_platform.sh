#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT_DIR/build/dppd"

if [[ ! -x "$BIN" ]]; then
    echo "[dppd] binary not found, building first"
    "$ROOT_DIR/scripts/build.sh"
fi

exec "$BIN" "$@"
