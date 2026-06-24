#!/usr/bin/env bash
# scripts/run_demo.sh — launch any built EDGE demo .xex interactively in Altirra
# (under wine). The generic companion of run_tank_embedded.sh: that one is
# tank-specific and builds first; this one boots an already-built .xex from
# build-atari/ (run scripts/build_demos.sh first).
#
# Usage:  scripts/run_demo.sh <demo-name>
#   <demo-name>   e.g. atari_scroll_test, arena_native, atari_vbxe_gfx
#                 (the .xex stem; ".xex" optional). Run with no name to list.
#   BUILD_DIR=<dir>   where the .xex live (default: build-atari)
#   ALTIRRA_DIR / ALTIRRA_TV / ALTIRRA_EXTRA   as in run_tank_embedded.sh
#
# VBXE demos (atari_vbxe_*, arena_vbxe) are launched with /vbxe; you must also
# select the **FX core** in Altirra's VBXE device settings (the core choice is
# not a reliable CLI switch — see demo/README.md "Common setup").
#
# Controls + the /debug false-crash rationale + teardown caveat: see
# run_tank_embedded.sh. Close the Altirra window to quit; never `pkill Altirra`.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$REPO/scripts/lib/atari_emu.sh"
BUILD_DIR="${BUILD_DIR:-$REPO/build-atari}"

# ── Resolve the demo name ────────────────────────────────────────────────────
list_demos() {
    echo "Available demos in $BUILD_DIR:" >&2
    if compgen -G "$BUILD_DIR/*.xex" >/dev/null; then
        for f in "$BUILD_DIR"/*.xex; do echo "  $(basename "${f%.xex}")" >&2; done
    else
        echo "  (none — run scripts/build_demos.sh first)" >&2
    fi
}

NAME="${1:-}"
if [ -z "$NAME" ]; then
    echo "usage: scripts/run_demo.sh <demo-name>" >&2
    list_demos
    exit 2
fi
NAME="${NAME%.xex}"
XEX="$BUILD_DIR/$NAME.xex"
[ -f "$XEX" ] || {
    echo "no such demo: $XEX" >&2
    list_demos
    exit 2; }

# ── VBXE demos need the overlay enabled (and the FX core, set in the UI) ──────
VBXE_ARGS=()
case "$NAME" in
    atari_vbxe_*|arena_vbxe)
        VBXE_ARGS=(/vbxe)
        echo "NOTE: $NAME needs VBXE — launching with /vbxe. In Altirra, make sure the" >&2
        echo "      VBXE device uses the FX core (System > Configure > Video/Devices)." >&2
        ;;
esac

# ── Locate Altirra (ALTIRRA_DIR override, else common locations) ─────────────
ALT_DIR="$(edge_find_altirra)" || exit 2

export DISPLAY="${DISPLAY:-:0}"
TV="${ALTIRRA_TV:-/ntsc}"
read -r -a EXTRA <<< "${ALTIRRA_EXTRA:-}"
XEX_WIN="$(edge_to_wine "$(readlink -f "$XEX")")"

echo "== launching $NAME in Altirra (close the window to quit) =="
echo "   xex: $XEX"
rm -f "$ALT_DIR/AltirraCrash.mdmp"
cd "$ALT_DIR"
exec wine Altirra64.exe "$TV" /nobasic /tempprofile /w \
    "${VBXE_ARGS[@]}" /debug /debugbrkrun /debugcmd:g "${EXTRA[@]}" /run "$XEX_WIN"
