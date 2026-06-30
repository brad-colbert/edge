# Hardware validation demo (`atari_hw_test.xex`)

> **Applies to EDGE v0.8.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

A minimal Atari 8-bit program that drives every engine subsystem at once, so
the engine's live ANTIC path can be confirmed on real hardware / emulators
(Altirra, Fujisan).

## Build

From the existing (mos-sim) build directory — the demo is a separate target and
is **not** part of the default build or the test suite:

```sh
cmake --build build --target atari_hw_test     # -> build/atari_hw_test.xex
```

Or with a dedicated Atari toolchain configure:

```sh
cmake -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake \
      -DEDGE_BUILD_DEMO=ON
cmake --build build-atari --target atari_hw_test
```

Or manually:

```sh
cmake -DINPUT=$PWD/demo/TEST.FNT \
  -DOUTPUT=$PWD/demo/user_charset.h \
    -P cmake/generate_charset_header.cmake &&
mos-atari8-dos-clang++ -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra \
    -I. demo/atari_hw_test.cpp -o atari_hw_test.xex
```

To try a different 1K font file, replace `demo/TEST.FNT` in the command above,
or pass `-DEDGE_DEMO_CHARSET_BIN=/path/to/your.fnt` to the CMake build.

## What to look for

| Observation | Proves |
|---|---|
| Row 0: `EDGE ENGINE V0.1` + a counter ticking up | display list, screen memory, per-frame VBI timing |
| Row 1: `JOY:` / `FIRE:` / `COL:` / `SND:` track the stick | input capture |
| Red arrow moves with joystick 0 | P/M graphics + input + HPOSP |
| Green diamond fixed at screen centre | P/M shape data at a Y offset |
| Border colour splits (blue → green) around row 12 | DLI fires mid-frame |
| Pure tone on fire press | POKEY sound subsystem |
| Noise burst + `COL:Y` when the arrow overlaps the diamond | GTIA collision registers |

## Notes

This demo is written entirely against the public engine API (`engine::Core` /
`Platform::hal`) — no `poke()`s, no inline assembly, no magic addresses. The
engine owns the display program, sprite DMA, raster-hook delivery, and the
per-frame service; see the header comment in
[`atari_hw_test.cpp`](atari_hw_test.cpp) for the full breakdown.

The one backend seam worth calling out is the deferred-frame ISR install:
**`Hal::install_frame_isr`** (in `engine/platform/atari/hal.h`) installs a
trampoline that preserves the llvm-mos zero-page registers and exits via
`JMP XITVBV` — without it, `Game::run()` would crash on real hardware (it is never
exercised under `mos-sim`, which has no NMI).

# Scroll validation demo (`atari_scroll_test.xex`)

A second, independent demo that exercises the hardware scroll subsystem and the
Tile infrastructure: a fixed two-row HUD over a 22-row window onto a 64×32
`engine::TileMap`, scrolled with the joystick. The game loop only calls
`Game::scroll.move()`; the engine splits the position into fine (HSCROL/VSCROL)
and coarse (per-line LMS) scroll each frame. It uses the default OS font, so —
unlike `atari_hw_test` — it has no charset/asset dependency.

## Build

```sh
cmake --build build --target atari_scroll_test          # -> build/atari_scroll_test.xex
```

Or with the dedicated Atari toolchain configure (`EDGE_BUILD_DEMO=ON`, as above),
or manually:

```sh
mos-atari8-dos-clang++ -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra \
    -I. demo/atari_scroll_test.cpp -o atari_scroll_test.xex
```

## What to look for

| Observation | Proves |
|---|---|
| Push right/left → field scrolls right/left; up/down likewise | fine + coarse scroll, correct directions |
| `*` origin marker sits at the field's top-left at scroll (0,0) | map-to-screen alignment (HSCROLL margin handled) |
| Column ruler reads `00 01 02 …` across, row ruler `00 01 02 …` down | per-line LMS addressing, no tearing |
| Tile boundaries stay seamless through a fine→coarse step | fine/coarse handoff |
| Scrolling stops cleanly at every map edge | edge clamping (to the fetch width) |
| HUD (`X` / `Y` / frame) stays fixed while the field scrolls | mixed scroll + non-scroll layout |

The `#` left-margin column is intentionally **not** visible — it lives in the
left border (the HSCROLL fetch margin). See the ANTIC scroll quirks captured in
the engine memory / [`engine/scroll.h`](../engine/scroll.h) and ADR-027 in
[`docs/DECISIONS.md`](../docs/DECISIONS.md).

# Tank demo — Stage 4 following camera (`atari_tank_demo.xex`)

A polished public-API tank demo ([`tank/`](tank/)). The playfield (Stage 2) is a
full-screen **40×24 ANTIC Mode 4 viewport** onto an **80×48 logical tile map**
assembled from a **2×2 grid of 40×24 map chunks**, stored in one **88×48 physical
tile map** (4 padding cells on each side: `[4 pad][80 logical][4 pad]`, no vertical
padding). The chunk payloads and 1K tileset are embedded ATank-derived assets (see
[`tank/assets/PROVENANCE.md`](tank/assets/PROVENANCE.md)) copied directly into the
single physical map at startup.

The tank (Stage 3) is one normal-width **GTIA player** with tank-style steering:
**sixteen movement headings** (N, NNE, NE, … 22.5° apart) but **eight displayed
silhouettes** (N, NE, E, SE, S, SW, W, NW — adapted from ATank, doubled to 8×16 so
they display as square 16×16). The hull-centre position is tracked in **Q12.4**
nominal pixels and clamped to the logical world.

Stage 4 makes the **camera follow the tank**: it keeps the tank centred while
scrolling room remains and **clamps at all four logical-world edges**; once
clamped, the tank slides from screen centre toward the corresponding viewport
edge. The horizontal camera and sprite share **one color-clock quantization**
(1 cc = 2 nominal px), so there is no one-pixel wobble at odd world X, and the
camera + sprite are computed from the **same frame's** tank state (no lag). The
tank stays fully visible at every legal world position.

**No collision, terrain response, bullets, networking, or runtime map streaming.**
Note: eight silhouettes are not true 22.5° artwork — adjacent headings share art.

All geometry uses the Altirra-measured Stage 1.1 invariants (ADR-034 terminology).

## Build

```sh
cmake -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake -DEDGE_BUILD_DEMO=ON
cmake --build build-atari --target atari_tank_demo     # -> build-atari/atari_tank_demo.xex
```

The build turns `tank/assets/tank_tileset.fnt` and `tank/assets/chunk_*.scr` into
ROM-resident byte-array headers (in the build tree). For deterministic headless
screenshots, pin the boot state with `-DEDGE_TANK_HEADING=<0..15>` and/or
`-DEDGE_TANK_POSITION=<0..14>` (clamps, world edges, centre, follow transitions,
odd-X quantization — see `kValidationPositions`); unset = heading N at the centre.

## Controls (tank-style)

| Input | Effect |
|---|---|
| Joystick left / right | rotate counterclockwise / clockwise (one 22.5° step every ~7 NTSC frames) |
| Joystick up | move forward along the current heading |
| Joystick down | move backward (reverse) along the current heading |
| left + up / right + up | turn while moving forward |
| left + down / right + down | turn while reversing |
| left + right | cancel rotation |
| up + down | cancel movement |

The tank retains its heading while stationary. Fire is unused. Horizontal motion
is quantized to 2 nominal pixels (HPOSP is in color clocks).

## What to look for

| Observation | Proves |
|---|---|
| Driving through the interior scrolls the playfield while the tank stays centred | centered camera following |
| The camera stops at each world edge; the tank then slides toward that viewport edge | camera clamp + centre→edge slide |
| The tank returns to centre as it leaves an edge region | clamp is symmetric, no mode state |
| Eight distinct silhouettes as you rotate; equal-feeling speed in every direction | heading→silhouette mapping + Q12.4 table |
| No horizontal one-pixel wobble at odd world X; no one-frame camera/sprite lag | shared color-clock quantization, same-frame submit |
| No visible padding checker; clean seams/bottom row throughout | camera stays in the padding-safe scroll range |
| Playfield, palette, seams, and chunks unchanged from earlier stages | no scrolling/geometry regression |

## Optional: network asset loading (Stages 5A/5B)

The tileset and four map chunks can optionally be loaded from a server instead of
the embedded assets. The asset source is chosen at configure time:

```sh
cmake -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake \
      -DEDGE_BUILD_DEMO=ON -DEDGE_TANK_ASSET_SOURCE=<mode>
cmake --build build-atari --target atari_tank_demo
```

| `EDGE_TANK_ASSET_SOURCE` | Behaviour |
|---|---|
| `Embedded` (default) | tileset + chunks compiled in; unchanged gameplay |
| `SimulatedNetwork` | feeds the protocol from the embedded assets through the loader over several frames (no connection); `-DEDGE_TANK_NET_FAULT=<1..3>` forces bad-manifest / missing-row / premature-complete failures |
| `LiveSession` | loads from a server over the reliable session lane; **no embedded tileset/chunks are linked** |

Protocol (asset payload **and** session-transport layers): [`tank/ASSET_PROTOCOL.md`](tank/ASSET_PROTOCOL.md).
The border shows coarse progress (amber loading → green complete → red failed); a
failed load halts before gameplay. Map chunks are written directly into the single
physical map; the tileset uses one page-aligned 1024-byte buffer installed via the
public charset API (`bind_charset_page`).

### LiveSession build + server

```sh
# Atari (requires the FujiNet session lane: build fujinet-lib, then configure with
# EDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON + EDGE_FUJINETLIB_ROOT=...):
cmake -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake \
      -DEDGE_BUILD_DEMO=ON -DEDGE_TANK_ASSET_SOURCE=LiveSession \
      -DEDGE_TANK_NET_HOST="192.168.1.10" -DEDGE_TANK_NET_PORT=9000 \
      -DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON -DEDGE_FUJINETLIB_ROOT=/path/to/fujinet-lib
cmake --build build-atari --target atari_tank_demo

# Host server (Python stdlib only):
python3 tools/net/edge_tank_asset_server.py --host 0.0.0.0 --port 9000
#   --selftest          verify framing/payloads     --hex   print asset payloads
#   --capture-session F  write the session-wire frames
```

The client connects, requests the transfer (its transfer id + an initial credit of
2), and grants credit as it drains so the 256-byte RX ring never overflows; the
server sends 66 messages (1 manifest + 16 tileset blocks + 48 chunk-row pairs + 1
complete), ~5.6 KB on the wire. `EDGE_TANK_NET_HOST`/`PORT` are baked into the ROM.
The live build still has no collision/terrain/bullets/realtime-lane/gameplay
networking; the session is only used to load assets before gameplay.

# Networked multi-tank demo (`atari_tank_net_demo.xex`)

A "bigger tank demo" ([`tank_net/`](tank_net/)): the same Stage-4 joystick tank
(local player) **plus three network-driven adversary tanks** whose authoritative
state (world position, heading, speed) is streamed from a Python UDP server at
**~10 Hz over the realtime lane** (`Game::net.realtime`). The
link is **bidirectional** — the Atari streams its own tank state back so the
server's adversary AI can **react (chase / avoid / patrol)**. It reuses the original
tank's motion/camera/shapes/assets verbatim (no fork); only the network glue and the
extra sprites are new.

All three adversaries are packed into **one 32-byte packet per tick** (status bits
mark which records are live), so the downstream packet rate is `--hz`, not `3 × --hz`
— the mitigation for a firmware pacing/backpressure issue that surfaced at higher
rates (the lane carries a per-demo 32-byte frame via `GameConfig::realtime_packet_bytes`;
see [`docs/HANDOFF_netstream_downstream_pacing.md`](../docs/HANDOFF_netstream_downstream_pacing.md)).

Between the 10 Hz snapshots the client **dead-reckons** every adversary forward with
its last-known heading + speed (the same Q12.4 motion table the player uses) and
**snaps** to each new authoritative position (ADR-015: the host sends state, not
input). The server mirrors that motion model, so the snaps are visually invisible.
The snap site is the marked seam for a future ease-in interpolation.

**Four tanks, no multiplexer.** The player (slot 0) and the three adversaries (slots
1–3) are pinned to dedicated hardware players via the engine **direct-bind** mode
(`GameConfig::sprite_binding = engine::SpriteBinding::Direct`; see
[`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) "Sprites"). 1 player + 3 adversaries
== the 4 hardware players, so direct-bind stays a single zone. Chasing adversaries
cross the player in Y constantly; the multiplexer's per-frame Y-sort would swap their
players/colours for a frame, so direct binding (fixed slot→player) is required here,
not the multiplexer.

The local player starts in the **upper-right** of the world; the adversaries start in
the other corners (the server's `--starts`). The player has **wall collision**: GTIA
player→playfield collision (P0PF) is read each frame, and a *pure-white* hit (COLPF0
with no colour bit) snaps the player back to its last wall-free position. Depot icons
(fuel/ammo) draw white letters inside a coloured COLPF1/COLPF2 box, so they set a
colour bit too — those are masked out of the wall test and the tank **drives over**
them; only plain white walls stop it. The whole test is derived from the live P0PF
register, with no depot tile codes hardcoded. This is reliable specifically because of
direct binding — logical slot 0 is always hardware player 0, so the P0PF mask is never
reassigned by a multiplexer. (The adversaries are not wall-collided; their motion is
the server's authority.)

> The realtime lane is validated on the emulator stack (Altirra + NetSIO + Docker peer)
> **and on physical FujiNet hardware** (2026-06-27, this demo).
>
> **Requires fujinet-firmware v1.6.2 or greater** — the downstream path depends on the
> firmware's whole-frame-aligned drop-oldest (added in 1.6.2). Older firmware drops
> individual bytes from the unframed stream and permanently desyncs the fixed-size
> deframer under load.

## Build

```sh
cmake -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake -DEDGE_BUILD_DEMO=ON \
      -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON \
      -DEDGE_NET_PEER_HOST=172.30.0.2 -DEDGE_NET_PEER_PORT=9000
cmake --build build-atari --target atari_tank_net_demo   # -> build-atari/atari_tank_net_demo.xex
```

Without `EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON` the realtime HAL is the **stub**
(`open_udp_seq` → `Unsupported`): the demo still runs player-only and turns the
**border red ("NO NET")** so the missing transport is obvious, not mistaken for a bug.

## Server (Python stdlib only)

```sh
python3 tools/net/edge_tank_adversary.py --hz 10 --decode
#   --count <N>                 number of adversaries (default 3 = the demo's kAdvCount)
#   --modes chase,avoid,patrol  per-adversary AI (falls back to --mode for extras)
#   --starts 8,376;624,376;624,16   per-adversary start x,y (falls back to --start)
#   --speed <steps/frame>   --speed-scale <EDGE_TANK_SPEED_SCALE>
```

By default the three adversaries run **mixed AI** (chase / avoid / patrol) from three
corners. The server learns the Atari's address from its first inbound packet (or pass
`--target-host/--target-port`). Keep `--speed-scale` equal to the demo's
`EDGE_TANK_SPEED_SCALE` (default 3) so the server's motion matches the client's
dead reckoning. See [`tools/net/README.md`](../tools/net/README.md) and the Mode B
recipe there (the demo replaces the echo peer with this server on the same Docker
network at `172.30.0.2:9000`).

## What to look for

| Observation | Proves |
|---|---|
| Four distinctly-coloured tanks (brassy player; pink/purple/cyan adversaries) | direct-bind: slot i→player i for slots 0–3 |
| Each adversary moves under server control, smooth between the 10 Hz updates | per-adversary dead-reckon fills the ~6 inter-packet frames |
| Their silhouettes match their headings as they turn | heading→silhouette on each received state |
| Driving the player makes the adversaries chase/avoid/patrol | bidirectional link + per-adversary AI (server consumes the Atari's TX) |
| Adversaries cross the player (and each other) in Y with **no colour flip** | direct binding, not the Y-sort multiplexer |
| Driving the player into a wall stops it; it never passes through | GTIA P0PF wall collision (pure white/COLPF0) + revert |
| Driving over a fuel/ammo depot does **not** stop the tank | depot letters set a COLPF1/COLPF2 colour bit, masked out of the wall test |
| An adversary leaves the viewport → hidden; re-enters at the right spot | each drawn in the player's camera frame |
| Red border when built without the netstream flag / with no peer | stub-HAL / no-transport indication |

# Dual-lane tank demo (`atari_tank_dual_net_demo.xex`)

Uses **both** networking lanes **sequentially** in one program
([`tank_dual_net/`](tank_dual_net/)): Phase 1 downloads the playfield map + tileset
over the **reliable TCP/session lane** (like the `tank` LiveSession mode), then closes
it and Phase 2 streams the three adversaries over the **UDP/realtime lane** (like
`tank_net`). The two lanes are never used at once — the realtime lane reprograms POKEY
serial for continuous streaming and can't coexist with fujinet-lib SIO traffic.

Phase-1 source is a target-private `EDGE_TANK_DUAL_ASSET_SOURCE` switch
(`SimulatedNetwork` default — builds with no fujinet-lib and no server; `Embedded`;
`LiveSession` — real TCP, needs the llvm-mos fujinet-lib). One combined server
(`tools/net/edge_tank_dual_server.py`) serves the assets over TCP then streams
adversaries over UDP, and **loops** back to waiting on the client's "bye".

**Any keypress quits cleanly** — the new engine teardown `Game::shutdown()`
(`Core::shutdown` → `Platform::hal::shutdown`) closes both net lanes, restores the OS
VBI vectors, disables the DLI, fully blanks P/M graphics (DMA **and** the GRAFP/GRAFM
registers), silences POKEY, and restores the charset/scroll, then exits to DOS via
`DOSVEC`. Full build/server/verify details in [`tank_dual_net/README.md`](tank_dual_net/README.md).

# Native bitmap primitive-drawing demo (`native_gfx.xex`)

The baseline (stock ANTIC/GTIA, no VBXE) counterpart to `atari_vbxe_gfx`. It draws a
picture with the **same** portable bitmap primitives the VBXE demo uses, but the
canvas is an ANTIC `BitmapRegion` drawn by its software `BitmapRegionView` — the
`gfx::Baseline` path the blitter demo doesn't exercise. **Press FIRE to cycle three
native bitmap modes that all share ONE screen buffer:**

- **ANTIC D** (GR.7) — 160×96 4-colour, 2 scanlines/line → full height, **3840 B**
- **ANTIC E** — 160×192 4-colour, 1 scanline/line → **7680 B**
- **ANTIC F** (GR.8) — 320×192 hi-res 2-colour, 1 scanline/line → **7680 B**

The shared buffer is sized for the largest screen (E/F, 7680 B); ANTIC D uses only
the first half. A row of 1/2/3 squares at the top tags the current mode. Each mode
packs pixels differently, so the demo keeps one `engine::BitmapOps` per mode pointed
at each screen's front-aligned canvas (`Game::gfx()` binds a single packing).

```sh
cmake --build build-atari --target native_gfx        # -> native_gfx.xex
```

Run with BASIC disabled. Needs **no VBXE**. All three modes draw to their full
width (the software `BitmapRegionView` uses `u16` x, so ANTIC F reaches x = 319).

| Observation | Proves |
|---|---|
| Framed safe-area border | `hline` / `vline` (software, packed write) |
| Colour-bar rectangles down the left (3 colours; 1 in hi-res F) | `fill_rect` |
| Two crossing diagonals on the right | `line` (software Bresenham via `plot`) |
| Dotted accent line across the lower band | `plot` |
| Small chequered 16×16 image | `blit` of a source packed at the region bpp |
| FIRE swaps mode/resolution; 1/2/3 tag squares track it | runtime `set_screen` between bitmap modes over a shared buffer |
| Steady picture, no corrupt row across the 192-line E/F field | the screen manager front-aligns the >4KB canvas so no mode line straddles the 4K scan boundary |

# VBXE demos

Four additional demos exercise the VBXE (`atari::gfx::VBXE<...>`) graphics path —
the 256-colour overlay, the blitter-backed bitmap subsystem (`Game::gfx()`), the
hardware 80-column text overlay, and the sprites-over-bitmap compositor. They
build to independent `.xex` targets and need a VBXE-capable target.

## Common setup (all four)

- **Run with BASIC disabled.** The default MEMAC-A window is `$B000–$BFFF`; with
  BASIC enabled that range is ROM and the CPU's framebuffer writes won't reach
  VRAM. On Altirra: disable the BASIC ROM (or hold OPTION at boot).
- **Altirra:** enable the VBXE device with the **FX core** (not the GTIA-emu core).
- The engine default register base is `$D640`; a `$D740` board needs a `Config`
  override (`gfx::VBXE<vbxe::Config<...RegBase::D740...>>`).

## Build

```sh
cmake --build build --target atari_vbxe_bringup    # -> build/atari_vbxe_bringup.xex
cmake --build build --target atari_vbxe_gfx        # -> build/atari_vbxe_gfx.xex
cmake --build build --target atari_vbxe_text       # -> build/atari_vbxe_text.xex
cmake --build build --target atari_vbxe_sprites    # -> build/atari_vbxe_sprites.xex
```

(Or configure with `-DEDGE_BUILD_DEMO=ON` against the Atari toolchain, as above.)

## `atari_vbxe_bringup` (`vbxe_bringup.cpp`)

Phase-4a overlay bring-up smoke test: brings the VBXE overlay up through the
engine and paints a diagnostic pattern with the `atari::vbxe` power-user header.

| Observation | Proves |
|---|---|
| Eight horizontal colour bars fill the screen | overlay compositing over ANTIC at top priority |
| A normal ANTIC text screen instead | overlay isn't compositing (check setup) |
| A black screen instead | framebuffer writes aren't reaching VRAM (BASIC/MEMAC) |

## `atari_vbxe_gfx` (`vbxe_gfx.cpp`)

Draws a static picture into the overlay framebuffer entirely through the portable
`Game::gfx()` API (single-buffered; the picture is drawn once and persists).

| Observation | Proves |
|---|---|
| Dark-blue field covering the screen | `gfx().clear()` (blitter fill) |
| Eight colour-bar rectangles down the left | `fill_rect` via the blitter |
| White frame around the inset edge | `hline` / `vline` |
| Two crossing diagonals on the right | `line` (Bresenham via the MEMAC window) |
| Small chequered 16×16 image | `blit` of an 8bpp source |

## `atari_vbxe_text` (`vbxe_text.cpp`)

Brings up the overlay in `Mode::Text_80`, uploads the PREPPIE font to VRAM, and
prints through the portable `Game::overlay_*` text seam.

| Observation | Proves |
|---|---|
| Dark-blue field with white/yellow text | hardware 80-column text overlay |
| Title + body lines render in the PREPPIE font | `overlay_text_font` (font→CHBASE) + `overlay_print` |
| A 16×16 block showing all 256 glyphs | `overlay_put_char` across the full font |

## `atari_vbxe_sprites` (`vbxe_sprites_over_bitmap.cpp`)

Double-buffered `Background::Bitmap`: the background is drawn once with
`Game::gfx()` into the VRAM master, published, and two sprites slide over it.
The screen is a pure-overlay `DisplayLayout<OverlayRegion<VBXE_SR, 240>>`, so
`set_screen` keeps ANTIC DMA off automatically — without that, the hidden ANTIC
playfield's DMA would contend the VRAM bus and stall the blitter's per-frame
restore copies, throttling motion to ~8 Hz. (Earlier versions of this demo
achieved the same with a manual `atari::set_playfield_dma(false)` call.)

| Observation | Proves |
|---|---|
| The `vbxe_gfx` picture as a steady background | `gfx()` master canvas + `overlay_publish_background()` |
| A red ball and a multi-colour pixel sprite slide across | `make_sprite` (Packed1bpp) + `make_pixel_sprite` (Pixel8bpp) |
| The background stays intact under the moving sprites | per-frame footprint restore from the master (flicker-free) |
| The sprites move at full speed (~60 px/s) | pure-overlay layout keeps ANTIC DMA off, freeing the VRAM bus for the blitter |

# Dual-lane network API example (`net_dual_lane_demo.xex`)

A small engine-style sample showing intended use of the dual-lane EDGE network
surface from game code:

- `Game::net.realtime` for fixed-size realtime packets.
- `Game::net.session` for reliable framed session/control messages.

It uses a FujiNet-enabled Atari `Platform<..., Network::Fujinet>` and runs the
frame-loop pattern (`poll` -> drain `recv` -> `send`) for both lanes. The
current Atari FujiNet HAL seam is intentionally still stubbed; real transport
wiring is deferred, so this example has no server requirement.

## Build

```sh
cmake --build build --target net_dual_lane_demo   # -> build/net_dual_lane_demo.xex
```

# Session lane runtime validation (`fujinet_session_validate.xex`)

A minimal Stage 8H validation tool for the optional fujinet-lib-backed session
lane. This is intentionally not a gameplay demo.

What it does:

- `Game::net.session.connect_tcp(host, port)`
- one small send (`"ping"`)
- bounded `poll()`/`recv()` loop
- on-screen status codes:
  - connect result
  - send result
  - read result
  - `last_error.status`
  - `last_error.detail`

It uses the session lane only (no realtime lane calls).

## Build

```sh
cmake --build build --target fujinet_session_validate   # -> build/fujinet_session_validate.xex
```

Built only when all are true:

- `EDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON`
- valid `EDGE_FUJINETLIB_INCLUDE_DIR` and `EDGE_FUJINETLIB_LIBRARY`
- Atari/llvm-mos demo path is active (same gating style as other FujiNet demo rules)

## Host / port config

Defaults:

- host: `192.168.1.100`
- port: `9000`

Override at configure time:

```sh
cmake -S . -B build \
  -DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON \
  -DEDGE_FUJINET_VALIDATE_HOST=192.168.1.50 \
  -DEDGE_FUJINET_VALIDATE_PORT=9000
```

## Timing note

Session send currently uses fujinet-lib `network_write` under the hood.
`network_write` may block for a FujiNet/CIO transaction. Keep writes small and
avoid using this path for timing-critical frame data.

# Arena game demo (`arena_base.xex` / `arena_vbxe.xex`)

A small complete Berzerk-style game (`demo/arena/arena.cpp`): a three-screen flow
(title → play → game-over), a bordered brick room, a HUD (score / level / lives),
homing enemies, shooting, explosions, a difficulty ramp, and a high score. It is
the worked example of one game source targeting two rendering backends from the
**same game logic**, selected at compile time:

- **`arena_base`** — baseline Atari: ANTIC Mode 4 (4-colour) text screens for the
  room/HUD/text, 1bpp Player/Missile sprites (player + up to 3 enemies, single
  multiplex zone), P/M hardware missiles, and a DLI HUD/play colour split.
- **`arena_vbxe`** — VBXE "Tier 2": the **entire** scene renders through the VBXE
  overlay with the ANTIC playfield off (no bus contention). The room, HUD, and text
  are drawn into the overlay bitmap (reusing the same charset glyphs), and the
  player, enemies, and bullets are multi-colour `Pixel8bpp` blitter sprites
  composited over it. Everything outside a few `#ifdef EDGE_VBXE` blocks (platform
  typedef, sprite shapes, palette, and the overlay render helpers) is shared
  verbatim with the baseline.

```sh
cmake --build build --target arena_base     # -> build/arena_base.xex
cmake --build build --target arena_vbxe     # -> build/arena_vbxe.xex  (-DEDGE_VBXE=1)
```

## Setup (`arena_vbxe`)

Same as the other VBXE demos (FX core, BASIC disabled — see "Common setup" above),
with one extra requirement baked into the demo: it runs `atari::vbxe::detect()`
before bringing up the overlay and prints **"VBXE REQUIRED"** on the OS text screen
(then halts) if no VBXE is found, instead of showing a black/garbage overlay.

> **MEMAC window note:** `arena_vbxe` places its MEMAC-A window at **`$A000`**, not
> the engine default `$B000`. On this binary the llvm-mos soft (call) stack lives
> at ~`$BC01`, inside the default `$B000-$BFFF` window — and enabling the window for
> VRAM access then aliases the call stack onto VRAM, corrupting both the display and
> the program. Any overlay program whose stack reaches into the window must move it.
> See [`docs/CONSTRAINTS.md`](../docs/CONSTRAINTS.md) and ADR-030.

## What to look for (`arena_vbxe`)

| Observation | Proves |
|---|---|
| Title / game-over text legible, room + HUD on the play screen | overlay bitmap text + tile rendering (`gfx()` into the master) |
| Blue multi-colour player, red multi-colour enemies, white bullets | `Pixel8bpp` blitter sprites composited over the bitmap |
| Score/level/lives update, enemies explode, death → game-over → title | shared game logic driving the overlay render path |
| Full-speed play, no progressive screen corruption | pure-overlay (ANTIC off) + MEMAC window clear of the stack |
