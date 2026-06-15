#!/usr/bin/env bash
# scripts/netstream_modeb_peer.sh — stand up / tear down the Netstream Mode B UDP echo peer in
# a Docker container with a fixed, distinct IP (172.30.0.2), so the FujiNet firmware's
# 0.0.0.0:<port> bind + send-to-host:<port> does not self-loop. See documents/PLATFORM_ATARI.md
# ("Netstream Mode B emulator validation"). The container runs tests/backends/atari/netstream_peer_echo.py.
#
# Usage:
#   scripts/netstream_modeb_peer.sh up     # create network + start the echo container
#   scripts/netstream_modeb_peer.sh down   # stop + remove container and network
#   scripts/netstream_modeb_peer.sh status # show container + last recv.bin
#
# The probe must be built with NS_PEER_HOST matching PEER_IP (default 172.30.0.2), e.g.
#   cmake --build <dir> --target netstream_datapath_altirra_probe   # uses NS_PEER_HOST cache var
#   scripts/altirra_probe.sh <dir>/netstream_datapath_altirra_probe.xex B

set -u
NET="${NSPEER_NET:-nspeer}"
SUBNET="${NSPEER_SUBNET:-172.30.0.0/24}"
PEER_IP="${NSPEER_IP:-172.30.0.2}"
NAME="${NSPEER_NAME:-nspeer-echo}"
OUTDIR="${NSPEER_OUTDIR:-/tmp/nspeer}"
IMAGE="${NSPEER_IMAGE:-python:3-slim}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ECHO_PY="$HERE/tests/backends/atari/netstream_peer_echo.py"

case "${1:-}" in
  up)
    mkdir -p "$OUTDIR"
    cp -f "$ECHO_PY" "$OUTDIR/echo.py"
    docker network inspect "$NET" >/dev/null 2>&1 || \
      docker network create --subnet "$SUBNET" "$NET" >/dev/null
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    docker run -d --name "$NAME" --network "$NET" --ip "$PEER_IP" \
      -v "$OUTDIR:/out" "$IMAGE" python /out/echo.py >/dev/null
    sleep 1
    echo "peer up: $(docker inspect -f '{{.State.Running}} {{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$NAME") (out=$OUTDIR)"
    echo "build the probe with NS_PEER_HOST=$PEER_IP"
    ;;
  down)
    docker rm -f "$NAME" >/dev/null 2>&1 && echo "removed $NAME" || echo "($NAME already gone)"
    docker network rm "$NET" >/dev/null 2>&1 && echo "removed $NET" || echo "($NET already gone)"
    ;;
  status)
    docker ps --filter "name=$NAME" --format '{{.Names}} {{.Status}} {{.Networks}}' || true
    echo "recv.bin:"; od -An -tx1 "$OUTDIR/recv.bin" 2>/dev/null || echo "  (none)"
    ;;
  *)
    echo "usage: $0 {up|down|status}" >&2; exit 2 ;;
esac
