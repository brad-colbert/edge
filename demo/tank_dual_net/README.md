# tank_dual_net — dual-lane (TCP download → UDP stream) tank demo

Demonstrates **both** EDGE networking lanes in one program, used **sequentially**:

| Phase | Lane | Transport | Does |
|-------|------|-----------|------|
| 1 | session | reliable TCP (fujinet-lib) | downloads the playfield map + tileset, with a loading screen |
| handoff | — | — | closes TCP, installs the assets, opens the realtime lane |
| 2 | realtime | unframed UDP netstream | streams 3 adversary tanks at ~10 Hz while you drive |

This is the combination of [demo/tank](../tank) (LiveSession asset download) and
[demo/tank_net](../tank_net) (adversary streaming) in a single binary.

**Any keypress during Phase 2 quits cleanly:** the Atari sends a "bye" packet
(`TankPacket32` type 2) to the server over a few frames, then calls `Game::shutdown()`
and exits to DOS via `DOSVEC`. `Game::shutdown()` (new engine teardown —
`Core::shutdown` → `Platform::hal::shutdown`) quiesces **every** subsystem init
brought up: closes both net lanes, restores the OS VBI vectors and disables the DLI
(so no stale interrupt fires into reclaimed RAM), turns off P/M graphics DMA (so no
leftover sprite band paints over the OS screen), silences POKEY, and restores the
charset/scroll. On the bye, the **server stops streaming and returns to Phase 1** —
waiting for the next client's TCP asset request — so you can relaunch and go again.

The two lanes are used one-after-the-other, never at once: the realtime lane
reprograms POKEY serial for continuous streaming and cannot coexist with normal SIO
command/response fujinet-lib traffic. So: *download, then switch to streaming.*

Reuses `demo/tank` + `demo/tank_net` headers verbatim (added to the include path by
CMake); only the two-phase sequencing and the TCP→close→UDP handoff are new
([atari_tank_dual_net_demo.cpp](atari_tank_dual_net_demo.cpp)).

Gameplay matches `tank_net`: the player has GTIA wall collision, but only *pure-white*
(COLPF0) contact stops it. Fuel/ammo depots draw white letters inside a coloured
COLPF1/COLPF2 box, so their colour bit is masked out of the wall test and the tank
**drives over** depots while walls still block it.

> **Firmware:** the realtime lane requires **fujinet-firmware v1.6.2+** (whole-frame-
> aligned drop-oldest). Older firmware byte-drops the unframed stream and desyncs the
> adversaries. Same constraint as `demo/tank_net`.

## Phase-1 asset source (`EDGE_TANK_DUAL_ASSET_SOURCE`)

| Value | Phase 1 | fujinet-lib? | Server? |
|-------|---------|--------------|---------|
| `SimulatedNetwork` *(default)* | embedded assets fed through the real `AssetLoader` over several frames | no | no |
| `Embedded` | skipped — compiled-in assets, straight to the handoff (test Phase 2 alone) | no | no |
| `LiveSession` | real TCP download over `Game::net.session` | **yes** (llvm-mos `libfujinet.a`) | yes |

The default `SimulatedNetwork` build proves the whole download → handoff → stream
sequence with no library and no server (host / Altirra). Phase 2 needs
`-DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON` for real streaming; without it the
realtime lane is a stub, the adversaries stay hidden, and the border turns red
("NO NET") — Phase 1 still runs.

## Build

Default (SimulatedNetwork; builds with the rest of the demos):

```sh
scripts/build_demos.sh                       # → build-atari/atari_tank_dual_net_demo.xex
# or just this target:
cmake --build build-atari --target atari_tank_dual_net_demo
```

Real dual-lane build (the decisive hardware config — both lanes + real fujinet-lib):

```sh
FNL=~/Projects/Atari/fujinetlib-llvm
cmake -S . -B build-live \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/atari-toolchain.cmake" -DEDGE_BUILD_DEMO=ON \
  -DEDGE_TANK_DUAL_ASSET_SOURCE=LiveSession \
  -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON \
  -DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON \
  -DEDGE_FUJINETLIB_ROOT="$FNL" \
  -DEDGE_FUJINETLIB_INCLUDE_DIR="$FNL/src/include" \
  -DEDGE_FUJINETLIB_LIBRARY="$FNL/build/libfujinet.a"
cmake --build build-live --target atari_tank_dual_net_demo
```

Endpoint overrides (all optional): `-DEDGE_TANK_NET_HOST=` / `-DEDGE_TANK_NET_PORT=`
(Phase-1 TCP), `-DEDGE_NET_PEER_HOST=` / `-DEDGE_NET_PEER_PORT=` (Phase-2 UDP),
`-DEDGE_NETSTREAM_NOMINAL_BAUD=`.

## Server

One combined server serves the TCP asset transfer, then drops into the UDP adversary
loop — mirroring the binary's own sequence:

```sh
tools/net/edge_tank_dual_server.py --port 9000
```

TCP and UDP share the port (different protocols); split with `--asset-port` /
`--adv-port` if needed. It binds UDP before the TCP transfer so no early adversary
packet is lost during the TCP linger. Adversary options (`--modes`, `--starts`,
`--hz`, `--speed-scale`, …) match [edge_tank_adversary.py](../../tools/net/edge_tank_adversary.py).

It **loops**: serve assets (TCP) → stream adversaries (UDP) → on the client's "bye",
return to waiting for the next TCP request. Ctrl-C exits. The standalone
`edge_tank_adversary.py` loops the same way (a bye → wait for the next client).

Two-process alternative (same result):

```sh
tools/net/edge_tank_asset_server.py --oneshot     # then, once it completes:
tools/net/edge_tank_adversary.py --port 9000
```

## Verify

1. **Host / Altirra, no server** — build the default and screenshot it:
   `scripts/altirra_screenshot.sh build-atari/atari_tank_dual_net_demo.xex out.png 10`.
   The playfield (walls + map tiles) and the gold player tank confirm Phase-1
   download + handoff + gameplay. Press any key to confirm the loop unwinds and the
   lane closes (heads-up: Altirra shares the X display).
2. **Altirra + NetSIO + combined server** — LiveSession build, both gates ON: the
   loading screen completes, then 3 adversaries stream in; the server log shows the
   TCP transfer (66 messages) followed by UDP `learned target` + adversary tx.
3. **Real FujiNet hardware (decisive)** — same LiveSession build vs the combined
   server on the LAN. This is the only test that proves `session.close()` →
   `open_udp_seq()` releases/reprograms the SIO/POKEY cleanly for real. Build with
   `-DEDGE_TANK_NET_DIAG` to self-dump the final state to `H:TANKDBG.BIN` if the
   handoff stalls.

> The realtime open blocks for ~30 frames during clock renegotiation (Netstream
> "settle") right after the handoff — a brief screen freeze, not a hang.
