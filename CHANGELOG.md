# Changelog

All notable changes to EDGE are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

The canonical version number lives in [`engine/version.h`](engine/version.h);
`CMakeLists.txt` parses it. See the "Releasing" section of the
[README](README.md) for the bump procedure.

## [Unreleased]

### Added
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
