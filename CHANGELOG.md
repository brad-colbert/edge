# Changelog

All notable changes to EDGE are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

The canonical version number lives in [`engine/version.h`](engine/version.h);
`CMakeLists.txt` parses it. See the "Releasing" section of the
[README](README.md) for the bump procedure.

## [Unreleased]

### Added
- `Game::antic_playfield(bool)` (Atari, opt-in): enable/disable the ANTIC
  playfield (character/bitmap) DMA, preserving display-list and P/M DMA. Under an
  opaque VBXE overlay the ANTIC playfield is invisible, but its per-scanline VRAM
  DMA starves the blitter's restore copies; calling `antic_playfield(false)` after
  init frees the bus.

### Fixed
- VBXE `Background::Bitmap` overlay ran the game loop at ~8 Hz instead of 60 on
  real hardware: the hidden ANTIC text playfield's DMA contended the VRAM bus and
  stalled the blitter's per-frame VRAM→VRAM restore copies, overrunning the VBI.
  The `atari_vbxe_sprites` demo now calls `Game::antic_playfield(false)`, restoring
  full-rate motion. (Regression was masked because the dirty-rect restore was
  host-validated on `mos-sim`, which doesn't model VBXE/ANTIC bus timing.)
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
