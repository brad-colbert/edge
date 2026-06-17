#!/usr/bin/env bash
# scripts/altirra_probe.sh — run an EDGE Atari probe .xex in Altirra (under wine) and capture
# its page-6 snapshot automatically. See documents/PLATFORM_ATARI.md ("Altirra headless probe
# runner") for the full rationale and the gotchas this encodes.
#
# Usage:  scripts/altirra_probe.sh <probe.xex> [A|B] [extra Altirra flags...]
#   A (default) = Mode A, no FujiNet (/cleardevices, then re-mount H:).
#   B           = Mode B, default profile (FujiNet device present).
#
# How it works (each piece is load-bearing):
#   * The probe self-dumps its result to H1:NSDUMP.BIN via CIO (tests/.../atari_hostdump.h);
#     we mount H: to a temp host dir with /hdpathrw and read the file back. Altirra's CLI
#     /debugcmd: only takes a single space-free token, so the debugger memory-dump commands
#     CANNOT be driven from the command line — the H: self-dump is the capture channel.
#   * /debug /debugbrkrun /debugcmd:g cleanly resumes past Altirra's spurious "program error"
#     trap at the crt0 RUN entry (the documented false-crash); `g` is space-free so it passes.
#   * NEVER `pkill -f Altirra` from here — it would match this script's own command line. Use
#     `wineserver -k` (kills the wine prefix without matching "Altirra").

set -u

# Altirra install dir: explicit ALTIRRA_DIR wins; otherwise auto-discover the
# newest Altirra-* under the conventional location (relative to $HOME, no
# hardcoded username or version). Override with ALTIRRA_DIR=/path/to/Altirra-dir.
ALT_DIR="${ALTIRRA_DIR:-}"
if [ -z "$ALT_DIR" ]; then
    ALT_DIR="$(ls -d "$HOME"/Dropbox/Projects/Atari/Altirra/Altirra-*/ 2>/dev/null \
               | sort -V | tail -n 1)"
    ALT_DIR="${ALT_DIR%/}"   # trailing slash from the dirs-only glob
fi
if [ -z "$ALT_DIR" ] || [ ! -d "$ALT_DIR" ]; then
    echo "Altirra dir not found; set ALTIRRA_DIR=/path/to/Altirra-install" >&2
    exit 2
fi

XEX="${1:?usage: altirra_probe.sh <probe.xex> [A|B] [extra Altirra flags...]}"
MODE="${2:-A}"
shift || true; [ $# -gt 0 ] && shift || true
EXTRA=("$@")

# Map a Linux absolute path to wine's Z: drive (/ -> Z:\, '/' -> '\').
to_wine() { printf 'Z:%s' "$(printf '%s' "$1" | tr '/' '\\')"; }
XEX_WIN="$(to_wine "$(readlink -f "$XEX")")"

HOSTDIR="$(mktemp -d /tmp/altirra_probe.XXXXXX)"
trap 'rm -rf "$HOSTDIR"' EXIT           # don't accumulate temp host dirs
OUT="$HOSTDIR/nsdump.bin"               # probe writes H1:NSDUMP.BIN -> Altirra lowercases it
STABLE="/tmp/altirra_probe_last.bin"    # last capture, kept for re-reading after cleanup

# Mode A drops ONLY the FujiNet (NetSIO) device so SIO gets no responder (init -> timeout),
# while keeping H: (and other devices) intact. Do NOT use /cleardevices: it also removes the
# H: host device even when /hdpathrw follows it. The FujiNet device tag is config-specific
# (here the netsio.atdevice is registered as tag "custom"); override via $ALTIRRA_FUJINET_TAG.
FUJINET_TAG="${ALTIRRA_FUJINET_TAG:-custom}"
DEV=()
[ "$MODE" = "A" ] && DEV+=("/removedevice" "$FUJINET_TAG")

rm -f "$ALT_DIR/AltirraCrash.mdmp"
( cd "$ALT_DIR" && timeout 40 wine Altirra64.exe \
    /ntsc /nobasic /tempprofile "${DEV[@]}" \
    /hdpathrw "$(to_wine "$HOSTDIR")" \
    /debug /debugbrkrun /debugcmd:g \
    "${EXTRA[@]}" /run "$XEX_WIN" >/tmp/altirra_probe.log 2>&1 ) &

# The probe writes the dump in its first frame; poll, then stop the emulator.
for _ in $(seq 1 120); do [ -s "$OUT" ] && break; sleep 0.25; done
sleep 0.3
wineserver -k 2>/dev/null
wait 2>/dev/null

if [ -s "$OUT" ]; then
    cp -f "$OUT" "$STABLE"
    echo "== captured (mode $MODE) -> $STABLE =="
    od -Ax -tx1 "$STABLE"
    exit 0
fi
echo "NO DUMP captured (mode $MODE). See /tmp/altirra_probe.log; xex=$XEX_WIN" >&2
exit 1
