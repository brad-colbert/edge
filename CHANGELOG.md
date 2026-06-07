# Changelog

All notable changes to EDGE are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

The canonical version number lives in [`engine/version.h`](engine/version.h);
`CMakeLists.txt` parses it. See the "Releasing" section of the
[README](README.md) for the bump procedure.

## [Unreleased]

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
