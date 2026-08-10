#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${DPPD_BUILD_DIR:-"$ROOT_DIR/build"}
BIN="$BUILD_DIR/dppd"

if [[ ! -x "$BIN" ]]; then
    printf '[dppd] executable is missing; run scripts/build.sh first\n' >&2
    exit 1
fi

exec "$BIN" "$@"
