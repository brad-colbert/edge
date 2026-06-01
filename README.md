<img width="1448" height="1086" alt="EDGE_primary_logo_mark" src="https://github.com/user-attachments/assets/d7be6d69-4959-41d1-8d3b-53bbd34b9be3" />

# EDGE (Eight-bit Damned! Game Engine)

> **Applies to EDGE v0.1.0** — see [CHANGELOG](./CHANGELOG.md) for version history.

EDGE is a C++20 game engine for constrained 6502-class systems, built around a small, deterministic, compile-time configured API instead of a heavy runtime.
The project is Atari-first today, but its architecture is intended to support additional 6502-family platforms over time. Game code is written against portable engine subsystems while hardware details live behind a platform HAL and compile-time capability profiles.

## What EDGE provides

- A header-only engine surface for game code under [`/engine`](./engine)
- Compile-time platform selection with capability-driven specialization
- Static memory management with fixed-size pools
- Frame-based input, rendering, sprite, sound, tile, scroll, and interrupt APIs
- A real Atari 8-bit backend with ANTIC/GTIA/POKEY-oriented integration
- Unit tests built for the llvm-mos `mos-sim` target under [`/tests`](./tests)
- A hardware validation demo that builds to an Atari `.xex` under [`/demo`](./demo)

## Design priorities

EDGE is designed for machines where memory, timing, and hardware behavior must stay explicit.

- no heap allocation
- no exceptions
- no RTTI
- no virtual dispatch
- compile-time resource budgets
- explicit seams for low-level hooks when needed

The result is an engine that aims to stay portable in authoring model while still generating efficient, hardware-aware binaries.

## Current status

Implemented today:

- core game façade via `engine::Core<Platform, GameConfig>`
- display and screen layout types
- input snapshot model
- sprite and missile management
- sound effect playback
- tile and charset helpers
- scrolling support
- raster-hook and frame-hook interrupt management
- fixed-size slot and packed pools
- fixed-point math and lookup helpers

Planned or intentionally deferred:

- broader non-Atari backends
- `engine/gfx.h`
- `engine/net.h`

## Example program shape

```cpp
#include <engine/engine.h>
#include <engine/platform/atari/platform.h>

namespace M = atari;
using Platform = atari::StockXL_NTSC;

struct MainScreen {
    using display = engine::DisplayLayout<
        engine::TextRegion<M::Mode::MODE_2, 24>
    >;
};

struct GameConfig {
    using screens = engine::ScreenSet<MainScreen>;
    static constexpr engine::u8 max_sprites = 2;
    static constexpr engine::u8 sound_channels = 2;
};

using Game = engine::Core<Platform, GameConfig>;
```

For a fuller walkthrough, start with [`/documents/QUICK_START.md`](./documents/QUICK_START.md).

## Repository layout

- [`/engine`](./engine) — public engine headers and the Atari backend
- [`/tests`](./tests) — `mos-sim` unit tests for engine subsystems
- [`/demo`](./demo) — hardware validation demo that builds as `atari_hw_test.xex`
- [`/documents`](./documents) — end-user documentation
- [`/docs`](./docs) — architecture notes, design constraints, and ADRs
- [`/cmake`](./cmake) — toolchain files for simulator and Atari builds

## Build and test

EDGE uses CMake and llvm-mos toolchains.

### Run the unit tests

The main test configuration targets the llvm-mos `mos-sim` simulator:

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mos-sim-toolchain.cmake
cmake --build build
ctest --test-dir build --output-on-failure
```

### Build the Atari hardware demo

From the simulator build directory:

```sh
cmake --build build --target atari_hw_test
```

Or with a dedicated Atari configure:

```sh
cmake -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake -DEDGE_BUILD_DEMO=ON
cmake --build build-atari --target atari_hw_test
```

See [`/demo/README.md`](./demo/README.md) for what the demo exercises on real hardware or emulators.

## Versioning and releases

EDGE follows [Semantic Versioning](https://semver.org/). The version number has a
single source of truth in [`engine/version.h`](./engine/version.h) (the
`EDGE_VERSION_*` macros); `CMakeLists.txt` parses that header so `PROJECT_VERSION`
never drifts, and game code can read `EDGE_VERSION_STRING` directly. Every document
carries an "Applies to EDGE vX.Y.Z" stamp under its title, and all notable changes are
recorded in [`CHANGELOG.md`](./CHANGELOG.md).

To cut a release:

```sh
scripts/bump-version.sh X.Y.Z   # updates engine/version.h + every doc stamp
# then edit CHANGELOG.md: move [Unreleased] notes into a new [X.Y.Z] section
git commit -am "Release X.Y.Z"
git tag vX.Y.Z
```

## Documentation guide

If you are new to the project, read these in order:

1. [`/documents/README.md`](./documents/README.md)
2. [`/documents/QUICK_START.md`](./documents/QUICK_START.md)
3. [`/documents/API_REFERENCE.md`](./documents/API_REFERENCE.md)
4. [`/documents/PLATFORM_ATARI.md`](./documents/PLATFORM_ATARI.md)

## Key architectural ideas

- **Portable engine API:** game code targets engine subsystems rather than hardware registers.
- **Compile-time capabilities:** platform feature checks are `constexpr`, so unused code paths disappear at compile time.
- **Platform HAL:** each backend supplies the hardware-specific implementation details.
- **Shared screen-buffer model:** multi-screen programs share one display buffer sized for the largest screen.
- **Frame-based execution:** input is captured once per frame, game logic runs on a stable snapshot, and hardware-facing state is committed at the correct time.

## Who this is for

EDGE is aimed at developers building games or interactive software for 6502-era systems who want:

- a higher-level engine-facing API
- explicit control over memory and timing
- modern C++ templates instead of runtime abstraction
- direct paths back to low-level hardware behavior when necessary

If that matches your goals, the quickest path into the codebase is the quick start, the API reference, then the hardware demo and tests.
