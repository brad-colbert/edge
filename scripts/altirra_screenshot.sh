#!/usr/bin/env bash
# scripts/altirra_screenshot.sh — boot an EDGE Atari .xex in Altirra (under wine) and
# capture a PNG screenshot of the emulator window, fully headless/unattended. The
# companion of altirra_probe.sh: that one captures a page-6 H: dump for probes; this
# one captures the *visible display* for demos (no H: self-dump needed).
#
# Usage:  scripts/altirra_screenshot.sh <program.xex> [out.png] [seconds]
#   out.png  default /tmp/altirra_shot.png
#   seconds  emulator warm-up before the grab (default 8) — enough for the demo to
#            boot and render several frames.
#
# Why each piece is load-bearing:
#   * DISPLAY=:0 is a real X server here, and ImageMagick `import` + `wmctrl` are
#     installed, so no Xvfb is needed. We grab the Altirra top-level window by name.
#   * /debug /debugbrkrun /debugcmd:g is REQUIRED: Altirra breaks with a spurious
#     "program error" at the crt0 RUN entry (the documented false-crash); `g`
#     (space-free, the only token /debugcmd: accepts) resumes past it so the program
#     actually runs. Without /debug the program never gets past that trap and the
#     screen stays blank. The debugger panes are visible in the capture, but the
#     Atari display occupies the top-left and is clearly readable.
#   * NEVER `pkill -f Altirra` (it matches this script's own command line). Use
#     `wineserver -k`, which tears down the wine prefix without name-matching.
#
# Env overrides: ALTIRRA_DIR, DISPLAY, ALTIRRA_TV (/ntsc default), ALTIRRA_EXTRA
# (extra Altirra switches, word-split).

set -u
ALT_DIR="${ALTIRRA_DIR:-/home/brad/Dropbox/Projects/Atari/Altirra/Altirra-4.50-test4}"
export DISPLAY="${DISPLAY:-:0}"
TV="${ALTIRRA_TV:-/ntsc}"

XEX="${1:?usage: altirra_screenshot.sh <program.xex> [out.png] [seconds]}"
OUT="${2:-/tmp/altirra_shot.png}"
WARMUP="${3:-8}"
read -r -a EXTRA <<< "${ALTIRRA_EXTRA:-}"

[ -f "$XEX" ] || { echo "no such xex: $XEX" >&2; exit 2; }
to_wine() { printf 'Z:%s' "$(printf '%s' "$1" | tr '/' '\\')"; }
XEX_WIN="$(to_wine "$(readlink -f "$XEX")")"

rm -f "$ALT_DIR/AltirraCrash.mdmp"
( cd "$ALT_DIR" && wine Altirra64.exe \
    "$TV" /nobasic /tempprofile /w \
    /debug /debugbrkrun /debugcmd:g \
    "${EXTRA[@]}" /run "$XEX_WIN" >/tmp/altirra_screenshot.log 2>&1 ) &

sleep "$WARMUP"

# /debug opens the debugger panes (Registers/Disassembly right, Console/Memory
# bottom); the Atari display pane sits top-left. ALTIRRA_CROP crops the grab to just
# that pane for a clean display-only shot. Set ALTIRRA_CROP= (empty) for the full
# window incl. debugger panes. The default geometry matches the default debugger
# layout at this window size; widen it if your layout differs.
CROP="${ALTIRRA_CROP-1170x815+0+18}"

WID="$(wmctrl -l 2>/dev/null | grep -i 'Altirra' | awk '{print $1}' | head -1)"
rc=1
if [ -n "$WID" ]; then
    if import -window "$WID" "$OUT" 2>/tmp/altirra_screenshot.import.log; then
        rc=0
        [ -n "$CROP" ] && convert "$OUT" -crop "$CROP" +repage "$OUT" 2>/dev/null
    fi
fi

wineserver -k 2>/dev/null
wait 2>/dev/null

if [ "$rc" = 0 ] && [ -s "$OUT" ]; then
    echo "== captured -> $OUT ($(identify -format '%wx%h' "$OUT" 2>/dev/null)) =="
    exit 0
fi
echo "NO screenshot captured. See /tmp/altirra_screenshot.log; xex=$XEX_WIN; win=${WID:-none}" >&2
exit 1
