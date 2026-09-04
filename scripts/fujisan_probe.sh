#!/usr/bin/env bash
# scripts/fujisan_probe.sh — run an EDGE Atari probe .xex in Fujisan and read its
# page-6 result back. The Fujisan counterpart of scripts/altirra_probe.sh, for
# cross-checking a probe on a second emulator (documents/PLATFORM_ATARI.md).
#
# Usage:  scripts/fujisan_probe.sh <probe.xex> [bytes] [done-offset]
#   bytes        how many bytes of page 6 to read back (default 16)
#   done-offset  offset into page 6 the probe writes LAST and leaves non-zero;
#                the script polls it to know the run finished (default 8)
#
# How it works (each piece is load-bearing):
#   * Fujisan IGNORES atari800-style command-line options — it is a Qt front end that
#     boots from its saved profile. Its automation channel is instead a JSON control
#     port on localhost:6510 ("category.action" commands; capabilities: media, system,
#     input, debug, config, status, screen). This uses `media.load_xex` to run the
#     probe and `debug.read_memory` to read the result.
#   * That means NO H: self-dump is needed here: the probe's page-6 snapshot is read
#     straight out of emulated RAM. The same probe binary still works unmodified under
#     Altirra, which has to go through H: because its CLI can't drive the debugger.
#   * A cold boot precedes the load so page 6 starts clean and a previous probe's spin
#     loop is gone — otherwise a stale result reads as a fresh pass.
#   * Kill by PID, never `pkill -f fujisan`: that pattern matches this script's own
#     command line (the same lesson the Altirra runner encodes for "Altirra").
#
# If Fujisan is already running its port is reused and the instance is left running;
# an instance this script started is torn down at the end.

set -u

XEX="${1:?usage: fujisan_probe.sh <probe.xex> [bytes] [done-offset]}"
BYTES="${2:-16}"
DONE_OFF="${3:-8}"

PORT="${FUJISAN_PORT:-6510}"
PAGE6=1536                                  # $0600
STABLE="/tmp/fujisan_probe_last.bin"

jsonrpc() { printf '%s\n' "$1" | timeout 6 nc -q2 127.0.0.1 "$PORT" 2>/dev/null | tail -1; }
listening() { timeout 2 bash -c "exec 3<>/dev/tcp/127.0.0.1/$PORT" 2>/dev/null; }

STARTED=""
if ! listening; then
    FUJISAN="${FUJISAN_BIN:-$(command -v fujisan)}"
    [ -x "$FUJISAN" ] || { echo "fujisan not found; set FUJISAN_BIN=" >&2; exit 2; }
    DISPLAY="${DISPLAY:-:0}" "$FUJISAN" >/tmp/fujisan_probe.log 2>&1 &
    STARTED=$!
    for _ in $(seq 1 60); do listening && break; sleep 0.5; done
    listening || { echo "Fujisan control port $PORT never opened; see /tmp/fujisan_probe.log" >&2; exit 1; }
fi
cleanup() { [ -n "$STARTED" ] && { kill -TERM "$STARTED" 2>/dev/null; sleep 0.5;
                                   kill -KILL "$STARTED" 2>/dev/null; }; }
trap cleanup EXIT

jsonrpc '{"command":"system.cold_boot"}' >/dev/null
sleep 3
LOAD="$(jsonrpc "{\"command\":\"media.load_xex\",\"params\":{\"path\":\"$(readlink -f "$XEX")\"}}")"
case "$LOAD" in
    *xex_loaded*) ;;
    *) echo "load_xex failed: $LOAD" >&2; exit 1 ;;
esac

# Poll the probe's completion marker, then read the whole snapshot.
read_mem() {
    jsonrpc "{\"command\":\"debug.read_memory\",\"params\":{\"address\":$1,\"length\":$2}}" \
        | grep -oE '\$[0-9A-Fa-f]{2}"' | tr -d '$"' | tr 'A-F' 'a-f'
}
for _ in $(seq 1 60); do
    [ "$(read_mem $((PAGE6 + DONE_OFF)) 1)" != "00" ] && break
    sleep 0.5
done

read_mem "$PAGE6" "$BYTES" | tr '\n' ' ' | xxd -r -p > "$STABLE"
echo "== captured -> $STABLE =="
od -Ax -tx1 "$STABLE"
