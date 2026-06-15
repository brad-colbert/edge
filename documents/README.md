# EDGE Engine Documentation

> **Applies to EDGE v0.5.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

EDGE is a C++ engine for building games and interactive applications on constrained 6502-class systems.

Its design goal is not to hide the machine behind a heavy runtime. The goal is to give the author a small, predictable, compile-time configured API for screens, input, sprites, sound, tiles, interrupts, and fixed-size storage, while keeping hardware details behind a platform backend.

The first concrete backend is the Atari 8-bit family. That means some types visible today, especially display mode names, come from the Atari implementation. The overall authoring model, however, is intentionally broader than a single machine.

## Goals

- Give game code a stable engine-facing API instead of direct register programming.
- Keep memory and timing explicit and deterministic.
- Make platform selection a compile-time choice through capabilities and templates.
- Avoid heap allocation, exceptions, RTTI, and virtual dispatch.
- Leave clean seams for low-level hooks and platform-specific escape hatches when needed.

## How to Read These Docs

Start with the general engine model, then move to the current backend details.

- [Quick Start](./QUICK_START.md): what an EDGE program looks like and how the main pieces fit together.
- [API Reference](./API_REFERENCE.md): portable engine-facing API, organized by subsystem.
- [Atari Platform Guide](./PLATFORM_ATARI.md): the current concrete backend, including Atari-specific types, modes, and hardware behavior.

## What Is Portable vs Backend-Specific Today

Mostly engine-level and intended to remain portable:

- `engine::Core<Platform, GameConfig>`
- `engine::SlotPool` and `engine::PackedPool`
- input snapshot model
- sound, sprite, tile, scroll, interrupt, and hook subsystems
- the bitmap-drawing subsystem `Game::gfx()` (`engine::BitmapOps`, `engine/gfx.h`)
- fixed-point math and lookup tables
- compile-time capability queries

The bitmap subsystem is the clearest example of the portability model in action:
`Game::gfx()` exposes one drawing API and dispatches at compile time on the
platform's `has_blitter` capability — to a hardware blitter where one exists and
to a software path otherwise — without the game code changing. The same pattern
(capability query → backend seam) is how additional backends will plug in.

Currently backend-specific in the public surface because Atari is the first implementation:

- `atari::Platform<...>` and Atari platform aliases
- the display-mode token: the engine's region/view templates take a backend mode type through
  `engine::display::traits<ModeT>`, spelled `atari::Mode` for the current backend
- Atari display-list / ANTIC / GTIA / POKEY terminology, confined to the Atari platform guide and the
  `demo/atari_hw_test.cpp` example (the generic API speaks of raster hooks, frame hooks, sprites, and
  `engine::audio::Waveform`)

## Scope of the Current Docs

These documents describe the API that is currently implemented in `engine/`, cross-checked against the demo in `demo/` and the unit tests in `tests/`, and informed by the design documents in `docs/`.

Subsystems with partial backend wiring today:

- `engine/net.h` — the dual-lane networking API is implemented. On the Atari
  platform the **session lane** is optionally wired to fujinet-lib (OFF by
  default) and the **realtime lane** is wired to an EDGE-owned FujiNet Netstream
  path, validated against the fujinet-pc emulator stack (NetSIO + Altirra +
  Docker UDP peer) but **not yet on physical FujiNet hardware**. The `Network` /
  Fujinet *capability axis* is real; non-Atari net backends remain unimplemented.
