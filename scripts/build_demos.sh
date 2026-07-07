#!/usr/bin/env bash
# scripts/build_demos.sh — one command to build every Atari demo .xex.
#
# Configures a dedicated Atari build directory with the atari8-dos llvm-mos
# toolchain and builds the aggregate `demos` target (every always-available
# demo). The network-only demos (fujinet_net_test, fujinet_session_validate)
# are not included — they need an external fujinet-lib checkout; see the README.
#
# Usage:  scripts/build_demos.sh
#   BUILD_DIR=<dir>   build directory (default: build-atari)
#   RECONF=1          force a fresh CMake configure even if the cache exists
#   HOT_OPT=<flag>    optimization for the realtime demos: -O2 (default) | -Os | -Oz
#                     (only applied at configure time; use RECONF=1 to change it)
#
# Run scripts/setup.sh first if you are unsure the toolchain is installed.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO/build-atari}"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] || [ "${RECONF:-0}" = "1" ]; then
    echo "== configuring Atari demo build in $BUILD_DIR =="
    cmake -S "$REPO" -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/atari-toolchain.cmake" \
        -DEDGE_BUILD_DEMO=ON \
        ${HOT_OPT:+-DEDGE_HOT_OPT="$HOT_OPT"}
fi

echo "== building all demos (target: demos) =="
cmake --build "$BUILD_DIR" --target demos

echo "== built .xex files in $BUILD_DIR =="
ls -1 "$BUILD_DIR"/*.xex 2>/dev/null || {
    echo "no .xex produced — check the build output above" >&2; exit 1; }
