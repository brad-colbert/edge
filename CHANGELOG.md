# Changelog

All notable changes to EDGE are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

The canonical version number lives in [`engine/version.h`](engine/version.h);
`CMakeLists.txt` parses it. See the "Releasing" section of the
[README](README.md) for the bump procedure.

## [Unreleased]

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
