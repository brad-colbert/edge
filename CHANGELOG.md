# Changelog

All notable changes to EDGE are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

The canonical version number lives in [`engine/version.h`](engine/version.h);
`CMakeLists.txt` parses it. See the "Releasing" section of the
[README](README.md) for the bump procedure.

## [Unreleased]

### Added
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
[0.1.0]: https://github.com/
