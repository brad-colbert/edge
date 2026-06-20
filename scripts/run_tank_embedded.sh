#!/usr/bin/env bash
# scripts/run_tank_embedded.sh — build (if needed) and launch the EDGE tank demo
# in Altirra (under wine), Embedded asset source. No network: the tileset + four
# map chunks are compiled into the .xex, so it boots straight into gameplay.
#
# Usage:  scripts/run_tank_embedded.sh
#   REBUILD=1   force a rebuild even if the .xex already exists
#   ALTIRRA_DIR=/path/to/Altirra-install   override Altirra location
#   ALTIRRA_TV=/ntsc|/pal                   override TV standard (default /ntsc)
#   ALTIRRA_EXTRA="..."                     extra Altirra switches (word-split)
#
# Controls: click the Atari display pane to focus it, then steer with the joystick
# (Altirra's default port-1 mapping is the arrow keys): left/right rotate the tank
# through its 16 headings, up/down drive. The camera follows + clamps at the edges.
#
# Load-bearing details (see documents/PLATFORM_ATARI.md):
#   * /debug /debugbrkrun /debugcmd:g resumes past Altirra's spurious "program
#     error" trap at the crt0 RUN entry; without it the program never runs.
#   * Closing the Altirra window ends this script. NEVER `pkill -f Altirra`
#     (it matches this script's own command line); the window's close button or
#     `wineserver -k` is the clean teardown.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO/build-atari-emb"
XEX="$BUILD_DIR/atari_tank_demo.xex"

# ── Build if needed (atari8-dos llvm-mos toolchain under /usr/local/bin) ──────
if [ ! -f "$XEX" ] || [ "${REBUILD:-0}" = "1" ]; then
    echo "== building Embedded tank demo =="
    cmake -S "$REPO" -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/atari-toolchain.cmake" \
        -DEDGE_BUILD_DEMO=ON \
        -DEDGE_TANK_ASSET_SOURCE=Embedded
    cmake --build "$BUILD_DIR" --target atari_tank_demo
fi
[ -f "$XEX" ] || { echo "build produced no .xex: $XEX" >&2; exit 1; }

# ── Locate Altirra ───────────────────────────────────────────────────────────
ALT_DIR="${ALTIRRA_DIR:-}"
if [ -z "$ALT_DIR" ]; then
    ALT_DIR="$(ls -d "$HOME"/Dropbox/Projects/Atari/Altirra/Altirra-*/ 2>/dev/null | sort -V | tail -n 1)"
    ALT_DIR="${ALT_DIR%/}"
fi
[ -n "$ALT_DIR" ] && [ -d "$ALT_DIR" ] || {
    echo "Altirra dir not found; set ALTIRRA_DIR=/path/to/Altirra-install" >&2; exit 2; }

export DISPLAY="${DISPLAY:-:0}"
TV="${ALTIRRA_TV:-/ntsc}"
read -r -a EXTRA <<< "${ALTIRRA_EXTRA:-}"

to_wine() { printf 'Z:%s' "$(printf '%s' "$1" | tr '/' '\\')"; }
XEX_WIN="$(to_wine "$(readlink -f "$XEX")")"

echo "== launching Embedded tank demo in Altirra (close the window to quit) =="
echo "   xex: $XEX"
rm -f "$ALT_DIR/AltirraCrash.mdmp"
cd "$ALT_DIR"
exec wine Altirra64.exe "$TV" /nobasic /tempprofile /w \
    /debug /debugbrkrun /debugcmd:g "${EXTRA[@]}" /run "$XEX_WIN"
