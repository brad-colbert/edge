#!/usr/bin/env bash
# scripts/run_tank_networked.sh — build (if needed) and launch the EDGE tank demo
# in Altirra (under wine), LiveSession asset source linked against the REAL
# fujinet-lib backend. The tileset + four map chunks are NOT compiled in; they are
# streamed over real FujiNet (N:TCP) from the bundled Python asset server, then the
# demo drops into the identical tank gameplay.
#
# This script starts the asset server itself and stops it when you close Altirra.
#
# Usage:  scripts/run_tank_networked.sh
#   REBUILD=1            force a rebuild even if the .xex already exists
#   NET_HOST=<ip>        server address the FujiNet connects to (default LAN IP)
#   NET_PORT=<n>         server TCP port (default 9000)
#   FUJINETLIB_ROOT=...  fujinet-lib checkout (default validated local path)
#   LINGER=<sec>         server post-COMPLETE linger (default 10)
#   ALTIRRA_DIR / ALTIRRA_TV / ALTIRRA_EXTRA   as in run_tank_embedded.sh
#
# Prerequisite — the FujiNet emulation stack must be running (this script only
# checks and warns; it does not start them):
#   * netsiohub bridge (UDP 9997)      — the NetSIO endpoint Altirra talks to
#   * fujinet-pc firmware (TCP :8000)  — does the actual N:TCP to the server
#     start it from your fujinet-pc build's dist dir (its run-fujinet helper).
# On real Atari hardware with a physical FujiNet, neither is needed.
#
# Controls + the /debug rationale + teardown caveat: see run_tank_embedded.sh.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$REPO/scripts/lib/atari_emu.sh"
BUILD_DIR="$REPO/build-atari-live"
XEX="$BUILD_DIR/atari_tank_demo.xex"

NET_HOST="${NET_HOST:-$(hostname -I | awk '{print $1}')}"
NET_PORT="${NET_PORT:-9000}"
FUJINETLIB_ROOT="${FUJINETLIB_ROOT:-$REPO/third_party/fujinetlib-llvm}"
LINGER="${LINGER:-10}"

[ -f "$FUJINETLIB_ROOT/build/libfujinet.a" ] || {
    echo "fujinet-lib not found at: $FUJINETLIB_ROOT (set FUJINETLIB_ROOT)" >&2; exit 2; }

# ── Build if needed (real fujinet-lib backend; /usr/local/bin llvm-mos) ───────
if [ ! -f "$XEX" ] || [ "${REBUILD:-0}" = "1" ]; then
    echo "== building LiveSession tank demo (RealFujinetLib) for $NET_HOST:$NET_PORT =="
    cmake -S "$REPO" -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/atari-toolchain.cmake" \
        -DEDGE_BUILD_DEMO=ON \
        -DEDGE_TANK_ASSET_SOURCE=LiveSession \
        -DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON \
        -DEDGE_FUJINETLIB_ROOT="$FUJINETLIB_ROOT" \
        -DEDGE_TANK_NET_HOST="$NET_HOST" \
        -DEDGE_TANK_NET_PORT="$NET_PORT"
    cmake --build "$BUILD_DIR" --target atari_tank_demo
fi
[ -f "$XEX" ] || { echo "build produced no .xex: $XEX" >&2; exit 1; }

# Confirm this really is the real backend, not the stub.
if ! strings "$XEX" | grep -q "EDGE-LIVE-BACKEND:RealFujinetLib"; then
    echo "WARNING: $XEX is not a RealFujinetLib build (stub?). Try REBUILD=1." >&2
fi

# ── Pre-flight: the FujiNet emulation stack ──────────────────────────────────
ss -lun 2>/dev/null | grep -q ':9997' || \
    echo "WARNING: netsiohub (UDP 9997) not detected — the live load will stall." >&2
ss -lnt 2>/dev/null | grep -q ':8000' || \
    echo "WARNING: fujinet-pc firmware (:8000) not detected — start your fujinet-pc
         build (its run-fujinet helper) before the live load, or the transfer stalls." >&2

# ── Locate Altirra (ALTIRRA_DIR override, else common locations) ─────────────
ALT_DIR="$(edge_find_altirra)" || exit 2

export DISPLAY="${DISPLAY:-:0}"
TV="${ALTIRRA_TV:-/ntsc}"
read -r -a EXTRA <<< "${ALTIRRA_EXTRA:-}"
XEX_WIN="$(edge_to_wine "$(readlink -f "$XEX")")"

# ── Start the asset server; stop it (and Altirra) on exit ────────────────────
SRV_LOG="$(mktemp /tmp/edge_tank_server.XXXXXX.log)"
echo "== starting asset server on 0.0.0.0:$NET_PORT (log: $SRV_LOG) =="
python3 "$REPO/tools/net/edge_tank_asset_server.py" \
    --host 0.0.0.0 --port "$NET_PORT" --linger "$LINGER" >"$SRV_LOG" 2>&1 &
SRV_PID=$!
cleanup() { kill "$SRV_PID" 2>/dev/null || true; }
trap cleanup EXIT

echo "== launching LiveSession tank demo in Altirra (close the window to quit) =="
echo "   xex:  $XEX"
echo "   load: ~18 s over FujiNet (boot + wifi + 66-message transfer), then gameplay"
rm -f "$ALT_DIR/AltirraCrash.mdmp"
( cd "$ALT_DIR" && wine Altirra64.exe "$TV" /nobasic /tempprofile /w \
    "${EXTRA[@]}" /run "$XEX_WIN" )
#    /debug /debugbrkrun /debugcmd:g "${EXTRA[@]}" /run "$XEX_WIN" )

echo "== server transfer log =="
grep -E "connection from|request:|transfer complete|disconnected" "$SRV_LOG" || cat "$SRV_LOG"
