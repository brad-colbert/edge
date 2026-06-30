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

## Firing & adversary respawn

Press the **joystick trigger** to fire a missile in the tank's current heading. Up to
**4 missiles** can be airborne at once — each is a hardware GTIA missile (pool index ==
missile slot), advanced in world space with the shared Q12.4 motion table. A missile
expires when it leaves the world, after a short lifetime, or on a *pure-white* wall
(same COLPF0 rule as the tank — missiles fly over depots too). Firing works even in the
"NO NET" fallback (it is purely local).

When a missile strikes an adversary, the **server respawns that adversary at its start
corner**. The mechanism rides the existing 10 Hz client→server packet — no new packet
type, no extra round-trip:

- The client keeps a **monotonic per-adversary hit counter** (`TankPacket32.hit_count[i]`,
  bytes 27..29) and bumps `hit_count[i]` each time one of its missiles hits adversary `i`
  (collision is a software AABB in world space, so it never depends on GTIA logical↔
  hardware player remapping). Hit detection releases the missile immediately, so one
  missile == exactly one increment.
- The server **edge-detects** each counter changing (a u8-ring delta, wrap-safe) and
  respawns that adversary. Because the counter rides *every* C→S packet, a dropped
  packet just delays the respawn one tick; because the server acts only on the change,
  a duplicated/re-sent value respawns **once** (idempotent). The respawn logic lives in
  [edge_tank_adversary.py](../../tools/net/edge_tank_adversary.py) (`hit_delta` +
  `last_hit_count`), so both the standalone and combined servers get it.

> Hardware missiles take the colour of their corresponding player, so the first shot
> (missile 0) is player-coloured and rapid follow-ups borrow the adversary colours —
> cosmetic only.

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
`-DEDGE_NETSTREAM_NOMINAL_BAUD=`. For **real hardware**, set *both* `EDGE_TANK_NET_HOST`
(Phase-1 TCP) and `EDGE_NET_PEER_HOST` (Phase-2 UDP) to the server's LAN IP — the
Phase-1 default `127.0.0.1` is unreachable from the Atari.

> **LiveSession is built `-Os`, the other variants `-O2`.** LiveSession links the real
> fujinet-lib, which nearly fills the `0x2000`–`0x8000` code region; the firing code's
> ~1.7 KB overflows it at `-O2`, so that variant compiles `-Os` (the per-frame
> `frame_service` overrun that once required `-O2` was fixed engine-wide, so `-Os` still
> holds 60 fps). SimulatedNetwork/Embedded don't link fujinet-lib and stay `-O2`.

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
   TCP transfer (66 messages) followed by UDP `learned target` + adversary tx. Fire at
   an adversary and confirm the server logs `adv i HIT — respawning to (x,y)` and the
   adversary jumps back to its start corner. (The respawn path also has a host-side
   loopback test — see the firing section — that drives the real server over UDP.)
3. **Real FujiNet hardware (decisive)** — same LiveSession build vs the combined
   server on the LAN. This is the only test that proves `session.close()` →
   `open_udp_seq()` releases/reprograms the SIO/POKEY cleanly for real. Build with
   `-DEDGE_TANK_NET_DIAG` to self-dump the final state to `H:TANKDBG.BIN` if the
   handoff stalls.

> The realtime open blocks for ~30 frames during clock renegotiation (Netstream
> "settle") right after the handoff — a brief screen freeze, not a hang.
