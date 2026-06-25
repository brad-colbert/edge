# Changelog

All notable changes to EDGE are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

The canonical version number lives in [`engine/version.h`](engine/version.h);
`CMakeLists.txt` parses it. See the "Releasing" section of the
[README](README.md) for the bump procedure.

## [Unreleased]

### Fixed
- **Arena demo — play-screen crash, colour-split flicker/flash, and a 1-row colour
  bleed.** Entering the play screen could freeze the machine (a wild jump into zero
  page → `KIL`), and the HUD/play colour split flickered or dropped out during
  motion. Root cause was the **C++ DLI dispatcher reading a stale chain index**: on
  arena's heavy frame the service overran past the early split scanline before
  `InterruptManager::prepare_chain` reset the walk index `current_`, so the
  dispatcher fetched a one-past-end handler pointer (`$00` high byte) and `JSR`ed
  into zero page. Three engine-wide robustness fixes plus one demo change:
  - **`dli_dispatch.h`** — the C++ dispatcher now bounds-checks `current_` against
    the live hook count; an out-of-range (stale) index re-syncs to the chain head
    (`current_ = 0`) and delivers slot 0 instead of jumping through garbage. Fixes
    the crash and the flicker.
  - **`InterruptManager::rearm_delivery()` (interrupt.h / core.h)** — `frame_service`
    now re-points the raster vector at the last-built chain head **in vertical
    blank**, before any visible scanline. `prepare_chain` rebuilds the chain late in
    a heavy service, by which point the beam may have passed an early hook's
    scanline; re-arming up front guarantees an early hook (e.g. a high colour split)
    fires every frame. Fixes the movement flash.
  - **Arena raw colour-split DLI** — the play-area palette swap is now a hand-written
    raw DLI (writes `COLBK`/`COLPF0-3` directly, no `$80-$9F` dispatcher save) that
    chains through the engine's raster tail like the multiplexer's zone DLIs. The
    shared C++ dispatcher's register-save overhead (~448 cycles) had delayed the
    colour latch ~1 row, tinting the play area's top wall with the HUD palette; the
    raw DLI latches inside the boundary's horizontal blank, with no bleed. The split
    scanline is also decoupled from the sprite/collision origin so it can be tuned
    without moving sprites.

  Verified on Altirra (no crash through sustained play, no flash across movement
  frames, clean colour boundary) with no regression to `atari_hw_test`, the
  multiplexer demo (9 sprites / 3 zones), or the tank demo; 33/33 host tests pass.

## [0.6.0] - 2026-06-21

### Added
- **Tank demo — Stage 5C real FujiNet validation + launch scripts.** The
  `LiveSession` build now links the real **fujinet-lib** backend
  (`-DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON -DEDGE_FUJINETLIB_ROOT=…`); a build
  marker (`EDGE-LIVE-BACKEND:RealFujinetLib` vs `…:Stub`), a configure print, and a
  loud warning make a stub build impossible to mistake for a real one. **Validated
  end-to-end on real FujiNet** (Altirra NetSIO → netsiohub → fujinet-pc → asset
  server): all 66 asset messages (5,619 framed wire bytes) transfer with the credit
  window of 2 honored, the loader completes (16/16 tiles, 96/96 rows), and gameplay
  begins with a playfield pixel-identical to Embedded — this **supersedes the Stage
  5B "unproven" note** below. The Atari session adapter now stages one bulk
  `network_read_nb` and serves the engine's byte-at-a-time drain from it (~30–90×
  fewer SIO transactions; the per-byte path desynced framing over emulated NetSIO),
  and the Python server gains `--linger` (holds the socket open after COMPLETE so
  the firmware delivers the final messages before FIN). New helper scripts
  `scripts/run_tank_embedded.sh` and `scripts/run_tank_networked.sh` build + launch
  the demo in Altirra (the networked one also starts the asset server). A focused
  CMake guard rejects `/mnt/old_ubuntu_22` in any fujinet-lib path variable.
- **Tank demo — Stage 5B live session-lane asset loading (`atari_tank_demo`).**
  Connects the Stage 5A loader to EDGE's reliable session lane (`Game::net.session`,
  never the realtime lane) and loads the tileset + four chunks from the Python
  server over TCP/FujiNet. Asset source is now a configure-time selector
  `EDGE_TANK_ASSET_SOURCE = Embedded (default) | SimulatedNetwork | LiveSession`.
  The session wire frame is `[kind][size_lo][size_hi][payload]` (engine framing —
  not a bare length prefix); new session kinds carry the asset payload
  (server→client) and the client's transfer **request** + **credit** grants
  (client→server). Because the engine's 256-byte RX ring drops bytes on overflow
  (no safe natural backpressure), a **credit window of 2** (≤184 B in flight; max
  frame 92 B) paces the transfer; the client tops up credit as it drains. A pure,
  templated `NetAssetClient<Session>` (`demo/tank/net_session_loader.h`) owns the
  connect→request→receive→install state machine, frame-based connect/inactivity
  timeouts, and compact transport+loader error codes — host-tested with a fake
  session (`test_net_session_loader`, 33/33 suite). **LiveSession links no embedded
  tileset/chunks** (linker-map verified: the 4,864 asset bytes are absent); it
  keeps one page-aligned 1,024-B network tileset buffer, the one 4,224-B physical
  map, loader/coverage state, and the session rings. The Python server
  (`tools/net/edge_tank_asset_server.py`) now speaks the real session wire format,
  parses the request, adopts the transfer id, obeys credit, and has a `--selftest`
  (validated end-to-end host↔host). Embedded + Simulated Altirra-validated;
  **live FujiNet hardware validation is unproven in this environment** (fujinet-lib
  is unavailable to the supported toolchain) — the live build boots and correctly
  halts on connect failure. No engine networking changes; demo-local protocol only.
- **Tank demo — Stage 5A network asset protocol + loader core (`atari_tank_demo`).**
  Adds a transport-neutral way to load the demo's tileset (1×1024 B) and four map
  chunks (4×960 B) from a server — **without** any FujiNet/session-lane/realtime
  integration (that is Stage 5B). A demo-local, little-endian, fixed-layout
  protocol (`demo/tank/asset_protocol.h`, spec in `demo/tank/ASSET_PROTOCOL.md`)
  defines MANIFEST / TILESET_BLOCK / CHUNK_ROWS / COMPLETE / ERROR messages, all
  ≤89 B (under the 128-B session payload). A fixed-memory loader
  (`demo/tank/asset_loader.h`) consumes already-framed payloads, validates every
  field, writes chunk rows **directly into the one 88×48 physical map** (no staging
  buffer, no second map) and the tileset into a single page-aligned 1024-B buffer
  (network mode's accepted duplicate), tracks coverage with fixed masks (u16 +
  4×u32), and exposes state/error/progress. The tileset is installed only on a
  valid COMPLETE, via the public charset API (`bind_charset_page`) — no access to
  private engine charset storage. New host test `test_asset_loader` (38 checks:
  manifest/version/geometry/transfer-id validation, tileset & chunk range checks,
  duplicate/out-of-order handling, coverage, premature-COMPLETE, failure+reset,
  full round-trip incl. shuffled order). A target-private simulated-loader build
  (`EDGE_TANK_ASSET_SOURCE=SimulatedNetwork`, see Stage 5B) drives a multi-frame
  load from the embedded assets through the loader, then enters the unchanged
  Stage 4 gameplay; `EDGE_TANK_NET_FAULT` selects deterministic fault modes. Adds
  `tools/net/edge_tank_asset_server.py`
  (stdlib-only: hex/capture/TCP). Altirra-validated: simulated load → install →
  gameplay is identical to embedded mode; fault modes halt without entering
  gameplay. No engine APIs changed; not a generic asset protocol.
- **Tank demo — Stage 4 centered + clamped following camera (`atari_tank_demo`).**
  Replaces the Stage 3 fixed camera with one that **follows the tank**: it keeps
  the tank centred while scrolling room remains and **clamps at all four logical-
  world edges**, after which the tank slides from screen centre toward the
  corresponding viewport edge (no camera-mode state machine — it falls out of
  clamping the desired origin). The horizontal camera and PMG X share **one
  color-clock quantization** (the player world X is quantized to color clocks once
  and drives both `Game::scroll` and the sprite), eliminating the one-pixel wobble
  at odd world X; the camera and sprite are derived from the **same frame's** tank
  state, so there is no camera/sprite lag. Pure helpers (`shifts/subtracts/clamps`
  only — no float/multiply/divide/trig) live in `demo/tank/tank_camera.h`, shared
  with the new `test_tank_camera` host test (30 checks incl. full legal-position
  sweeps for visibility, monotonic PMG X, and odd/even-X coherence). Altirra-
  validated (centre, four clamps, the four follow transitions, odd-X). The tank is
  fully visible at every legal world position. Still **no collision, terrain,
  bullets, networking, or map streaming**.
- **Tank demo — Stage 3 PMG tank + sixteen-heading steering (`atari_tank_demo`).**
  Adds one normal-width GTIA player tank to the Stage 2 playfield: **16 movement
  headings** (22.5° apart, with wrap) but **8 displayed silhouettes** (N, NE, E,
  SE, S, SW, W, NW — ATank's eight directional shapes doubled to 8×16 so they
  display as square 16×16; intermediate headings round clockwise to the nearest
  silhouette). The hull-centre is tracked in **Q12.4 nominal pixels** and moved by
  a 16-entry ROM motion table (|v|≈8 Q4, ~equal speed; reverse subtracts the same
  vector — no reverse table; no float/multiply/divide/trig). Tank-style joystick
  controls (left/right rotate, up/down forward/reverse, left+right and up+down
  cancel, turn-while-move), one 22.5° step every ~7 NTSC (~6 PAL) frames. The tank
  centre is clamped to the logical world; the **camera is fixed at the world
  centre** this stage (the Stage 2 free-camera is removed). World→screen→PMG
  conversion is signed and the player is hidden when fully off-screen (no `u8`
  wrap). Motion math is in `demo/tank/tank_motion.h` (shared with the
  `test_tank_motion` host test); silhouettes in `demo/tank/tank_shapes.h` (128 ROM
  bytes). Altirra-validated (all 8 silhouettes, fixed camera, viewport-edge
  placement, offscreen hide). Eight silhouettes are not true 22.5° artwork.
  (Camera following arrives in Stage 4, above; collision, bullets, and networking
  remain for later.)
- **Tank demo — Stage 2 static four-chunk playfield (`atari_tank_demo`).** A
  polished public-API demo ([`demo/tank/`](demo/tank/)): a full-screen 40×24 ANTIC
  Mode 4 viewport onto an 80×48 logical tile map assembled from a 2×2 grid of
  40×24 map chunks, stored in one 88×48 physical `engine::TileMap` (4 padding cells
  each side, no vertical padding) and bound via `Game::scroll_map()`. Embedded
  ATank-derived assets (1K tileset + four 960-byte chunks; see
  [`demo/tank/assets/PROVENANCE.md`](demo/tank/assets/PROVENANCE.md)) are turned
  into ROM-resident byte arrays at build time and copied **directly** into the
  single physical map at startup — no staging buffer, no per-frame copy. A
  temporary joystick free camera (nominal-pixel units → EDGE scroll units, clamped
  in the demo) scrolls the playfield. Uses the Altirra-measured Stage 1.1 geometry
  invariants; Altirra-validated (corners, seams, four-chunk centre, fine/coarse
  transitions, max camera — no padding intrusion or bottom-row tearing). Geometry,
  chunk placement, and camera math live in `demo/tank/playfield_geometry.h`, shared
  with the `test_tank_playfield` host test. (The movable tank is added in Stage 3,
  above.) Adds a generic `cmake/generate_bytes_header.cmake` (neutral companion to
  the charset generator) for raw binary assets.
- **Realtime networking diagnostic demo (Stage 9S.3)** — a two-ended user-facing
  diagnostic for the realtime lane, NOT a game and NOT a protocol layer.
  `demo/edge_net_realtime_meter.cpp` is an Atari client that uses **only** the public
  `Game::net.realtime` API (open/poll/send/recv/rx_count/rx_dropped/
  consume_rx_overflowed/last_error) — no session lane, no fujinet-lib, no HAL/internal
  access. It sends fixed 16-byte `MeterPacket16` units at a steady cadence, drains
  replies each frame, and renders a text-mode HUD plus custom-charset sparklines for
  TX/RX sequence, RX count/age, round-trip delay, clock offset, jitter, stale state,
  and the public RX drop/overflow indicators. Timing uses a four-timestamp
  (T1/T2/T3/T4) frame/jiffy model with signed 16-bit wrap helpers; milliseconds are
  shown only as approximate. The Netstream hardware settle is internal to
  `open_udp_seq()` (blocking), so the demo shows `OPENING / NETSTREAM SETTLE...` then
  `ACTIVE`/`OPEN FAILED` and never fakes a live settle countdown; an optional
  `POST-OPEN WARMUP` is labelled as demo-only. `MeterPacket16` is documented as demo
  diagnostic payload only — not an EDGE wire format. The host peer
  `tools/net/edge_realtime_peer.py` is generic stdlib-only UDP (no Atari/SIO/POKEY/
  FujiNet/Netstream transport knowledge): `--mode echo` preserves client fields and
  stamps T2/T3/peer_seq into exactly 16 bytes; `--mode ticker` streams to an explicit
  `--target-host/--target-port` or a learned sender; `--tv ntsc|pal` (+ `--hz`
  override) drives a virtual jiffy clock seeded from the client's first T1.
  **API gaps recorded:** TX queue depth and internal Netstream flags are not public,
  so TX health is inferred from `send()`/`poll()` status only and internal flags are
  not displayed. **Build note:** the demo target links the real Netstream adapter (the
  two `.S` handlers) only when `EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON`; otherwise it
  links the stub HAL (HUD shows `ACTIVE`/`LAST OK` but performs no SIO). Optional
  `-DEDGE_NET_PEER_HOST`/`-DEDGE_NET_PEER_PORT` CMake passthrough configures the peer
  endpoint without editing the source. Validated end-to-end over fujinet-pc + NetSIO +
  Altirra + a Docker UDP peer (Mode B) on 2026-06-15 — `TX`/`RX`/`GOT` advancing with
  computed delay (~4 frames/~66 ms), signed offset, RTT, and jitter, and realistic loss
  on the raw 16-byte path. Not validated on physical FujiNet hardware.
- **Realtime meter — comms reliability + true throughput measurement.** The meter now
  quantifies the lossy raw path at both ends, with **no `MeterPacket16` change** (it
  reuses the `peer_seq` reply counter already in the packet). The host peer reports
  per-interval (`--stats-interval`, default 1 s) forward-path **packets/s and bytes/s**,
  **forward loss** by `client_seq` gap analysis, plus **duplicate / reorder / bad**
  counts, and a cumulative `summary:` line on exit. The Atari HUD gains a
  `LINK QUALITY (1S)` block: measured `TXHZ`/`RXHZ`, `TXBPS`/`RXBPS`, and loss split
  three ways — round-trip, **forward** (Atari→peer, `1−Δpeer_seq/Δtx`), and **reverse**
  (peer→Atari, `1−Δgot/Δpeer_seq`) — over ~1 s RTCLOK windows using the existing `sub16`
  wrap helper, with a worst/best round-trip-loss range. The peer's forward loss is the
  authoritative figure; the HUD's directional split is a client-side estimate.
- **Realtime peer — byte-stream reassembly (correctness fix).** Netstream (UDP-seq) is an
  unframed byte stream: the firmware forwards serial bytes to UDP in arbitrary chunks, so a
  16-byte unit may split across datagrams. The peer previously assumed one datagram == one
  packet and miscounted split units as `bad`/lost (worse at low rates), which looked like a
  lossy/corrupt link. The peer now reassembles 16-byte units from the stream (matching the
  Atari adapter's serial-ring reassembly), resyncing on the `E7 01` marker after a
  lost/reordered datagram and reporting discarded bytes as `resync`. With this, the emulated
  Mode B forward path measures **0 % loss / 0 corruption**; the only limit is packet *rate*,
  which is bound by the emulated NetSIO external-clock serial stalling the CPU (a stack
  characteristic, not loss or EDGE code), not real FujiNet hardware behaviour.
- **FujiNet Netstream realtime lane (`Game::net.realtime`) wired** — the realtime
  lane is now backed by an EDGE-owned FujiNet Netstream assembly path (no
  fujinet-lib, no per-byte CIO/SIO), moving bytes through interrupt-driven POKEY
  serial rings. The engine `RealtimeLane` / `RealtimePacketQueues` hand the Atari
  adapter one **fixed 16-byte packet** at a time with **all-or-nothing** TX/RX;
  the adapter adds **no wire framing** (packet boundaries are implicit). Netstream
  policy: flags `0x26` (UDP + UDP-seq + TX-external-clock + register), nominal baud
  `31250`, external TX clock, 30 RTCLOK-frame settle after begin, host→swapped port
  byte order (low→DAUX1, high→DAUX2). The public `Game::net` API is unchanged.
  Validated **mos-sim/static** (lifecycle via FakeOps; CTests 19/19), **Altirra
  Mode A no-device clean-failure**, and **fujinet-pc + NetSIO + Altirra + Docker
  UDP peer (Mode B)**; **not** validated on physical FujiNet hardware. Production
  `.bss` unchanged at 359 bytes. See ADR-033.

### Changed
- **Tank demo — faster tank motion.** Translation speed is now a single
  compile-time `tank::kSpeedScale` (`demo/tank/tank_motion.h`, default **3** ≈ 1.5
  px/frame, up from ~0.5); the 16-heading motion table is the unit-speed vectors
  scaled by it **at compile time** (constexpr — no runtime multiply). Overridable
  via `-DEDGE_TANK_SPEED_SCALE=<n>` (≤18 to keep the scaled components in `int8_t`).
  `test_tank_motion` / `test_tank_camera` are now scale-aware (expectations × scale,
  equal-speed tolerance × scale²).
- **Tile subsystem terminology + naming cleanup — source-breaking API rename, no
  runtime change.** Source compatibility changed; runtime behaviour, ROM size, and
  RAM use did **not** (`atari_scroll_test.xex` and `atari_hw_test.xex` are
  byte-identical before/after; all 27 host tests pass). The following public names
  were renamed with **no compatibility aliases** — **breaking** for any downstream
  source that uses the old names. Recompilation alone is not sufficient; callers
  must update their source:
  - `engine::TileManager` → `engine::TileDisplay`
  - `engine::CharsetData` → `engine::TilesetData`
  - `engine::make_charset` → `engine::make_tileset`
  - `TileMap::tiles` → `TileMap::cells` — the tile-map's backing cell array is a
    **public** member, so code that indexes it directly (`map.tiles[i]`) must
    change to `map.cells[i]`.

  Unchanged: the `Game::tiles` façade, the `TileMap` type with its `tile_at` /
  `set_tile` / `make_map` API, the `Charset1K` / `Charset512` size aliases, and the
  `init_charset` / `bind_charset_page` operations (which keep the "charset" spelling
  because they act on character-set RAM / the character-base hardware).
  `TileDisplay` coordinates the displayed tileset/charset and viewport — it owns no
  map and performs no map-cell lookup. Established canonical terms (glyph, tileset,
  charset, tile, tile code, map cell, tile map, map chunk, chunk grid, viewport,
  playfield, screen) and reserved the
  `MapChunk`/`ChunkGrid`/`ChunkLoader`/`ChunkManager`/`ChunkCache` names for future
  map-streaming work — **no chunk management or new demo is introduced**. See
  ADR-034.

### Fixed
- **Scroll demos ran at half frame rate — `frame_service` overran one frame
  (engine-wide perf).** A scrolling, single-sprite demo (`atari_tank_demo`) was
  pinned to a steady **30 fps** (measured: OS `RTCLOK $14` advanced by exactly 2 per
  `frame_step`), which read as a motion stutter when scrolling — worst horizontally,
  where ANTIC HSCROLL steps a coarse 2 px (1 color clock) at an uneven sub-cell
  cadence. Root cause was two per-frame steps that ran every frame regardless of
  need: (1) `patch_scroll` rewrote **all** scroll-region LMS load addresses with a
  16-bit multiply per visible line, and (2) `InterruptManager::prepare_chain`
  walked the **entire** display list to re-arm raster delivery even with **zero**
  hooks. Together they pushed the service just past one 60 Hz frame. Fixes (all
  engine-wide — every scroll demo benefits): `patch_scroll` now walks the load
  address incrementally (`+= map_width`, one multiply total, byte-identical result);
  `ScreenManager::apply_scroll` skips the LMS rewrite while the **coarse** offset is
  unchanged (fine registers still update every frame; cache invalidated by
  `bind_scroll_map`); `prepare_chain` early-returns when the hook chain is empty and
  was empty last call. With these the tank demo also moves from `-Os` to `-O2` (the
  zero page now has room, so all three asset sources link). Verified **stable
  60 fps** (240/240 frames at one VBI), 33/33 host tests pass, and the tank,
  multiplexer (9 sprites/3 zones), and arena demos all render correctly.
- **Deferred-VBI re-entry guard now covers the `XITVBV` exit window (ADR-028).**
  The guard cleared `edge_vbi_busy` *inside the trampoline* before `JMP XITVBV`,
  leaving a hole: if the next VBI NMI arrived during `XITVBV` it ran a fresh service
  nested in the OS's (non-re-entrant) VBLANK exit → the OS `RTS` returned one byte
  off into ROM → garbage → `KIL`. Surfaced as a hard crash when scrolling to the
  far-left edge **and then up** with a sprite shown — the heaviest frame
  (`coarse_col=0`, where ANTIC's HSCROLL pre-fetch peaks DMA and starves the `-Os`
  service over one frame). Pre-existing (reproduced at the original tank speed).
  Fix: the trampoline keeps `edge_vbi_busy` set **through `XITVBV`**; the main loop
  releases it after consuming the frame (`loop.h` run/run_until → `Core::frame_consumed()`
  → `Hal::frame_consumed()`, `requires`-gated so HALs/mocks without the hook no-op).
  A VBI in the exit window now also sees the flag and skips. Verified: the
  far-left+up auto-drive repro no longer crashes; multiplexer (9 sprites/3 zones)
  and arena demos unaffected; 33/33 host tests pass.

## [0.5.0] - 2026-06-07

API-cleanup release: Atari-specific names are purged from the generic engine
surface, and a platform-specific escape hatch is relocated to the platform layer
(ADR-031). Breaking for Atari game code that called `Game::antic_playfield()` and for
any custom backend implementing the display-traits contract.

### Added
- `screen_buffer_alignment` capability field (`engine/config/capabilities.h`,
  default `1` = no constraint) — a platform-supplied screen-buffer alignment
  granularity. The Atari profile sets it to `4096`, replacing a hardcoded scan-
  boundary constant in `engine/screen.h`, so the portable screen manager asks for the
  alignment it needs without encoding ANTIC's 4K page behaviour.

### Changed
- Display-traits predicate `is_vbxe(ModeT)` renamed to `is_overlay_mode(ModeT)` — the
  `engine/display_traits.h` contract member and its Atari specialization. The name now
  describes what it tests (an overlay vs base mode) rather than the chip that
  introduced it. **Breaking** for custom backends that specialise the traits seam.
- Purged Atari chip/register names (ANTIC, VBXE, P/M, DLI, GTIA, …) from the generic
  `engine/*.h` headers — comments and identifiers now use portable
  capability/feature vocabulary. Engine-header rule: no hardware addresses, register
  names, or platform-specific types outside the `Platform` template parameter.

### Removed
- `Game::antic_playfield(bool)` removed from the engine facade. The Atari playfield-
  DMA escape hatch is now the free function `atari::set_playfield_dma(bool)` in the
  Atari platform layer, reached by including the Atari platform header. Per ADR-031,
  platform-specific escape hatches belong on the platform, not on the generic `Game::`
  facade. **Breaking** for Atari game code that called it.
- `DisplayLayout::antic_region_count` removed — an unused public layout query (no
  engine consumers; it degraded to `region_count` on any platform without an overlay
  plane). Its only consumers were unit tests, now expressed via `region_count` and the
  existing `has_overlay` / `is_pure_overlay` queries.

## [0.4.1] - 2026-06-06

### Added
- **Arena demo — VBXE "Tier 2" variant (`arena_vbxe`):** the same Berzerk-style
  game as the baseline `arena_base`, rendered entirely through the VBXE overlay
  (room, HUD, and text drawn into the bitmap; multi-colour pixel sprites for the
  player, enemies, and bullets composited over it; ANTIC playfield off). Built from
  the one `demo/arena/arena.cpp` source with `-DEDGE_VBXE=1`; all game logic, room
  layout, sound, collision, and difficulty are shared with the baseline. New
  `CMakeLists.txt` target `arena_vbxe`.
- `atari::vbxe::detect<Config>()` — runtime VBXE presence probe (MEMAC bank-select
  read-back signature, with floating-bus robustness: two distinct probe values + a
  bus-flush). `arena_vbxe` uses it to print "VBXE REQUIRED" on the OS text screen and
  halt on a machine without VBXE, rather than showing a black/garbage overlay. Pure
  decision logic split out as `detect_matches()` and unit-tested.

### Changed
- P/M **hardware missiles now render on the blitter (VBXE) backend** too. The missile
  half of `SpriteManager::commit_pm()` is factored into `commit_missiles()` and called
  from both `commit_pm()` (unchanged baseline behaviour) and `commit_blitter()`; the
  blitter init path now also arms P/M DMA. Projectiles stay direct P/M on both
  backends (ADR-025); see ADR-029.
- `OverlayHal::blit_rect()` / `blit_copy()` now wait for the blitter to be idle
  **before** submitting (not only after). A main-thread bitmap draw could otherwise
  overwrite the BCB queue while an async compositor blit was still in flight.

### Fixed
- **VBXE overlay corruption + crash (progressive on-screen noise, then a JAM, ~5 s
  in):** the default MEMAC-A window (`$B000-$BFFF`) overlapped the llvm-mos soft
  (call) stack (~`$BC01`). Enabling the window for any VRAM access aliased the call
  stack onto VRAM, so every overlay operation slowly corrupted both the displayed
  image and the program's own stack until it crashed. `arena_vbxe` places its window
  at `$A000` (free RAM between heap and stack). Hardware-diagnosed via the soft-stack
  pointer (`$80`/`$81`) sitting inside the window. See ADR-030 / CONSTRAINTS.md.

## [0.4.0] - 2026-06-05

### Added
- P/M sprite multiplexer now runs on real hardware. New lean raw zone-boundary
  DLI `edge_multiplex_dli` (platform/atari/dli_dispatch.h): it copies a zone's
  eight pre-baked bytes (HPOSP0-3 + COLPM0-3) from a flat table to GTIA and shares
  the C++ dispatcher's chain tail, staying under one mode line so closely-spaced
  boundaries don't re-enter. Backed by `SpriteManager::arm_multiplex_dli()` and the
  new HAL seam `hal::multiplex_dli()` / `hal::install_multiplex_dli()`.

### Changed
- **HAL contract (breaking for custom platforms):** the sprite commit now requires
  `hal::set_color_pm()` (zone 0's colour moved from a direct COLPM write to the OS
  PCOLR shadow), and a baseline HAL must now provide `hal::multiplex_dli()` and
  `hal::install_multiplex_dli()`. No game-author-facing API changed.
- P/M commit clears each sprite's **exact footprint** instead of a per-player
  min/max range, which degenerated to the whole strip under multiplexing
  (DECISIONS.md ADR-022 revision).
- Zone-boundary raster hooks are registered RAW again (dynamic hooks route through
  `edge_multiplex_dli`, not the C++ dispatcher).

### Fixed
- **Multiplexer crash on hardware (~15 s in, garbled screen + JAM):** the
  un-guarded deferred VBI could overrun a frame; the next VBI NMI re-entered the
  trampoline and corrupted the saved llvm-mos soft-stack pointer on unwind, drifting
  it down into `.text` until stack writes overwrote code. Added an `edge_vbi_busy`
  re-entry guard (DECISIONS.md ADR-028). The service also gates DLI NMIs off while
  it rebuilds the raster chain.
- **Top-zone sprites rendered black:** zone 0's colour was written straight to
  COLPM during the VBI, then zeroed by the OS PCOLR→COLPM copy that runs after it.
  Routed zone 0 through the PCOLR shadow; the boundary DLIs still write COLPM
  directly (they run after the copy).
- **VBI overrun / stutter with 9 sprites:** the wide per-player clear (above) was
  the cause; the per-sprite exact-extent clear pulls the commit back under one frame.
- **Ghost / split sprites at zone seams:** the boundary scanline is biased up by one
  mode line (`kBoundaryBias`) so the player switch lands in the inter-zone gap,
  since the DLI fires at the end of its mode line.

## [0.3.0] - 2026-06-04

### Added
- `engine::OverlayRegion<Mode, Height>` — a VRAM-backed VBXE overlay region usable
  inside a `DisplayLayout` alongside ANTIC `TextRegion`/`BitmapRegion`s. It costs
  zero screen-buffer RAM (its pixels live in VBXE VRAM), and region order sets the
  overlay's vertical position. New overlay-aware `DisplayLayout` queries:
  `has_overlay`, `is_pure_overlay`, `overlay_region_index()`.
- Automatic ANTIC DMA control in `set_screen<S>()`: a **pure-overlay** screen
  (every region is an `OverlayRegion`) keeps ANTIC DMA off — its display list
  collapses to a 3-byte JVB stub and the VBXE overlay drives the display via its
  XDL — which frees the VRAM bus for the blitter. ANTIC-only and mixed
  overlay+ANTIC screens enable DMA as before. This makes the previous manual
  `antic_playfield(false)` workaround unnecessary for the common opaque case.
- `Game::antic_playfield(bool)` (Atari, opt-in) — retained as a manual escape
  hatch for the cases the engine cannot infer: a transparent overlay that shows
  ANTIC through (leave it enabled), or a mixed overlay+ANTIC layout that wants the
  playfield fetch off. Toggles only the playfield DMA, preserving display-list and
  P/M DMA.
- Compile-time config-agreement check: `Core::set_screen` `static_assert`s that an
  `OverlayRegion`'s mode and height match the platform's VBXE `Config`
  (`overlay_mode`, `fb_height`) — e.g. `OverlayRegion<VBXE_HR>` under a
  `Config<VBXE_SR>` is a build error instead of silent corruption.

### Fixed
- VBXE `Background::Bitmap` overlay ran the game loop at ~8 Hz instead of 60 on
  real hardware: the hidden ANTIC text playfield's DMA contended the VRAM bus and
  stalled the blitter's per-frame VRAM→VRAM restore copies, overrunning the VBI.
  The `atari_vbxe_sprites` demo now uses a pure-overlay
  `DisplayLayout<OverlayRegion<VBXE_SR, 240>>`, so `set_screen` keeps ANTIC DMA off
  automatically, restoring full-rate motion. (Regression was masked because the
  dirty-rect restore was host-validated on `mos-sim`, which doesn't model
  VBXE/ANTIC bus timing.)
- `atari_vbxe_sprites`: sprite X position was computed as `u8` over a 40..259
  range, wrapping past 255 and briefly jumping the shape to the far left; the
  slide now stays within the valid u8 coordinate range.

## [0.2.0] - 2026-06-03

### Added
- Portable bitmap-drawing subsystem (`engine/gfx.h`): `engine::BitmapOps`, reached
  via `Game::gfx()`, with `clear` / `plot` / `hline` / `vline` / `fill_rect` /
  `line` / `blit`. It dispatches at compile time on the platform's `has_blitter`
  capability — the VBXE blitter + MEMAC window on overlay platforms, the ANTIC
  `BitmapRegionView` software path on baseline (selected by an optional
  `GameConfig::bitmap_region` typedef). The generic engine never names a VBXE type.
- VBXE graphics backend, exposed as a *type* axis: `atari::gfx::Baseline` (stock
  ANTIC/GTIA) and `atari::gfx::VBXE<Config>`. Configure the overlay with
  `atari::vbxe::Config<Mode, Buffers, RegBase, MEMAC, addr, Background>`
  (`Mode::{SR_320,HR_640,LR_160,Text_80}`, `Buffers::{Single,Double}`,
  `Background::{Flat,Bitmap}`) and upload palette entries with
  `atari::vbxe::set_color<Cfg>(...)`.
- VBXE 256-colour overlay plane composited over ANTIC at top priority, brought up
  automatically by `Game::init()` on a `gfx::VBXE<...>` platform.
- VBXE hardware text mode (`Mode::Text_80`) behind a portable overlay-text seam on
  `Game`: `overlay_text_font` (font→VRAM/CHBASE), `overlay_text_clear`,
  `overlay_put_char`, `overlay_print` (char + attribute pairs).
- Sprites-over-bitmap path: `Background::Bitmap` composes sprites over a drawn
  background. The game draws into a VRAM master canvas with `Game::gfx()`, seeds
  the display pages with `Game::overlay_publish_background()`, and the frame
  service restores each sprite's footprint from the master (flicker-free,
  double-buffered). `Game::set_overlay_background()` sets the flat-mode background.
- Full-colour sprite shapes: `engine::make_pixel_sprite<W,H>` (`Pixel8bpp`, one
  byte/pixel palette indices) alongside the P/M-style `engine::make_sprite<W,H>`
  (`Packed1bpp`). Pixel sprites require a blitter platform.
- Overlay collision snapshots latched at the frame service:
  `Game::overlay_collision()` and `Game::overlay_blit_collision()`.
- Four VBXE validation demos building to independent `.xex` targets:
  `atari_vbxe_bringup`, `atari_vbxe_gfx`, `atari_vbxe_text`, `atari_vbxe_sprites`.
- Hardware scrolling, wired live end-to-end. Declare a scrolling region by wrapping
  any region in `engine::ScrollRegion<Inner, MapW, MapH>` inside a `DisplayLayout`,
  bind a game-held `engine::TileMap` as its source with `Game::scroll_map(map)`, and
  drive it with `Game::scroll.move()` / `Game::scroll.set()`. The frame service splits
  the position into fine (HSCROL/VSCROL) and coarse (per-line LMS) scroll, clamps at the
  map edges, and keeps the tile viewport (`Game::tiles`) in sync automatically.
- Scroll validation demo building to `atari_scroll_test.xex` — a 64×32 tilemap scrolled
  under a fixed HUD. Builds independently of `atari_hw_test`.
- Backend ANTIC scroll geometry: `DL_HSCROLL` / `DL_VSCROLL`, `scroll_margin`,
  `scroll_fetch_width`, and `fine_scroll_range` (`platform/atari/antic.h`), plus a
  per-line-LMS scroll-aware display-program builder with `patch_scroll`
  (`platform/atari/display_list.h`).

### Changed
- `ScrollManager` reworked into a portable fine/coarse split: it owns the position and
  the fine-register writes (via `Platform::hal`) and exposes `coarse_col()` /
  `coarse_row()`; the backend display program owns all load-address patching. The
  per-axis fine-scroll inversion and the fetch width are supplied by the backend through
  display traits, so the generic scroll layer names no ANTIC specifics (Dependency Rule 2).

## [0.1.0] - 2026-05-31

### Added
- Portable engine API: core integration layer (`engine::Core<Platform, GameConfig>`),
  display, screen, sprites, sound, scroll, tiles, interrupts, hooks, the game loop,
  fixed-size pools (`SlotPool`/`PackedPool`), and fixed-point math.
- Atari 8-bit backend: ANTIC/GTIA/POKEY HAL, display-list builder, Player/Missile
  graphics, VBI install, DLI dispatch, and OS shadow integration.
- Hardware validation demo building to `atari_hw_test.xex`.
- Unit test suite for the llvm-mos `mos-sim` target, run via CTest.
- Versioning mechanism: `engine/version.h` as the single source of truth, parsed by
  CMake, surfaced in the demo HUD, and stamped across the documentation.

[Unreleased]: https://github.com/
[0.2.0]: https://github.com/
[0.1.0]: https://github.com/
