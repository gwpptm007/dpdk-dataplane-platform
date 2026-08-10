#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${DPPD_BUILD_DIR:-"$ROOT_DIR/build"}

if [[ -f "$BUILD_DIR/build.ninja" ]]; then
    meson setup --reconfigure "$BUILD_DIR" "$ROOT_DIR" -Dtests=true
else
    meson setup "$BUILD_DIR" "$ROOT_DIR" -Dtests=true
fi

meson compile -C "$BUILD_DIR"
meson test -C "$BUILD_DIR" --print-errorlogs
printf '[dppd] build and unit tests completed: %s/dppd\n' "$BUILD_DIR"
