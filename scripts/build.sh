#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
MODE="${DPPD_BUILD_MODE:-auto}"

printf '[dppd] build mode request: %s\n' "$MODE"
make -C "$ROOT_DIR" MODE="$MODE" clean all
printf '[dppd] build complete: %s/build/dppd\n' "$ROOT_DIR"
