<img width="1448" height="1086" alt="EDGE_primary_logo_mark" src="https://github.com/user-attachments/assets/d7be6d69-4959-41d1-8d3b-53bbd34b9be3" />

# EDGE (Eight-bit Damned! Game Engine)

> **Applies to EDGE v0.6.0** — see [CHANGELOG](./CHANGELOG.md) for version history.

EDGE is a C++20 game engine for constrained 6502-class systems, built around a small, deterministic, compile-time configured API instead of a heavy runtime.
The project is Atari-first today, but its architecture is intended to support additional 6502-family platforms over time. Game code is written against portable engine subsystems while hardware details live behind a platform HAL and compile-time capability profiles.

## What EDGE provides

- A header-only engine surface for game code under [`/engine`](./engine)
- Compile-time platform selection with capability-driven specialization
- Static memory management with fixed-size pools
- Frame-based input, rendering, sprite, sound, tile, scroll, and interrupt APIs
- A portable bitmap-drawing subsystem (`Game::gfx()`) that compiles to a hardware
  blitter where one exists and to a software path where it does not
- A real Atari 8-bit backend with ANTIC/GTIA/POKEY-oriented integration, plus an
  optional VBXE path (256-colour overlay, hardware 80-column text, blitter)
- Unit tests built for the llvm-mos `mos-sim` target under [`/tests`](./tests)
- Hardware validation demos that build to Atari `.xex` files under [`/demo`](./demo)

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
- sprite and missile management (P/M-style and full-colour pixel sprites)
- sound effect playback
- tileset and tile-map helpers
- hardware scrolling (`engine::ScrollRegion` + `Game::scroll_map`)
- portable bitmap drawing via `Game::gfx()` (`engine/gfx.h`), blitter-accelerated
  on capable platforms and software-rendered on baseline
- raster-hook and frame-hook interrupt management
- fixed-size slot and packed pools
- fixed-point math and lookup helpers
- Atari VBXE backend: 256-colour overlay, hardware 80-column text mode, blitter,
  and a sprites-over-bitmap compositor. The overlay is a first-class
  `engine::OverlayRegion` in a screen's `DisplayLayout`; a pure-overlay screen
  turns ANTIC DMA off automatically

Networking (`engine/net.h`):

- dual-lane API (`Game::net.realtime` and `Game::net.session`) implemented and frozen
- compile-time capability gating: `Game::net` is absent on `Network::None` platforms; individual
  lanes are absent when their lane capability is disabled
- **Session lane** (`Game::net.session`) optionally wired to real fujinet-lib TCP transport;
  enable at configure time with `-DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON` (OFF by default;
  no external library required for default builds)
- **Realtime lane** (`Game::net.realtime`) wired to the FujiNet Netstream path
  (EDGE-owned assembly, fixed 16-byte packets, no wire framing — the consumer
  reassembles units from the byte stream). The data path is validated against the
  fujinet-pc emulator stack (NetSIO + Altirra + Docker UDP peer, Mode B); it is
  **not yet validated on physical FujiNet hardware** (see `docs/DECISIONS.md` ADR-033)
- a build-time TX-clock override `EDGE_NETSTREAM_FLAGS` exists for hardware bring-up
  (default `0x26` external clock = emulator-validated; `0x22` = internal/local POKEY
  clock, **experimental, not yet validated**)
- the `net_dual_lane_demo` compiles and demonstrates the intended API shape; the
  `edge_net_realtime_meter` demo + host peer (`tools/net/edge_realtime_peer.py`)
  exercise and measure the realtime lane end to end

Planned or intentionally deferred:

- broader non-Atari backends
- physical FujiNet hardware validation of the realtime lane
- realtime-lane wire framing / resync / checksum / sequence (boundaries are
  currently implicit and cannot recover from lost bytes)
- a real gameplay demo over the realtime lane

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
- [`/demo`](./demo) — hardware validation demos: `atari_hw_test`, `atari_scroll_test`,
  the VBXE set (`atari_vbxe_bringup`, `atari_vbxe_gfx`, `atari_vbxe_text`,
  `atari_vbxe_sprites`), the Arena game (`arena_base`, plus `arena_vbxe` — the
  same game rendered entirely through the VBXE overlay), and the realtime-networking
  diagnostic `edge_net_realtime_meter` (public `Game::net.realtime` HUD + sparklines)
- [`/tools/net`](./tools/net) — host-side UDP peer (`edge_realtime_peer.py`) for the
  realtime networking demo
- [`/documents`](./documents) — end-user documentation
- [`/cmake`](./cmake) — toolchain files for simulator and Atari builds

## Requirements

### Core (required for any build or test)

- **[LLVM-MOS](https://github.com/llvm-mos/llvm-mos)** — the LLVM/Clang C++
  cross compiler for the MOS 6502 and related processors. EDGE builds with
  `mos-sim-clang++` (unit tests) and `mos-atari8-dos-clang++` (Atari `.xex`), and the
  bundled `mos-sim` simulator runs the test suite. Install the
  **[llvm-mos-sdk](https://github.com/llvm-mos/llvm-mos-sdk)** into `/usr/local`
  (the toolchain files expect it on `/usr/local/bin`).
- **[CMake](https://cmake.org/) ≥ 3.20** — the build system for both the test suite
  and the Atari demos.
- **[Python 3](https://www.python.org/)** — host-side tooling (`tools/net`,
  `tools/mode4`). Standard library only; no extra packages.

### Atari hardware & VBXE validation

- **[Altirra](https://www.virtualdub.org/altirra.html)** — the reference Atari 8-bit
  emulator and the only one with VBXE (FX core) support, used to validate the VBXE
  demos and capture screenshots. On Linux it is driven headless through Wine by
  `scripts/altirra_screenshot.sh` and `scripts/altirra_probe.sh`.
- **[Wine](https://www.winehq.org/)** *(Linux only)* — runs the Windows Altirra build
  (and the Mode4 charset tool) under Linux.
- **[Fujisan](https://github.com/pedgarcia/fujisan)** — a cross-platform Atari 8-bit
  emulator built on atari800 with built-in FujiNet/NetSIO support; the cross-check
  emulator when Altirra misbehaves at startup.

### FujiNet networking (optional — only for the networking lanes/demos)

- **[fujinet-firmware](https://github.com/FujiNetWIFI/fujinet-firmware)** — the ESP32
  FujiNet multifunction firmware that provides the `N:` network device. The
  [fujinet-pc](https://github.com/FujiNetWIFI/fujinet-pc) port runs it on the desktop
  as part of the emulator stack.
- **[fujinet-emulator-bridge](https://github.com/FujiNetWIFI/fujinet-emulator-bridge)**
  — bridges an emulator (Altirra) to FujiNet over the NetSIO UDP protocol (`netsiohub`
  + the Altirra `netsio.atdevice`). EDGE's realtime (Netstream) lane is validated
  against this stack.
- **fujinet-lib (llvm-mos build)** — a pre-built C library (`libfujinet.a` + headers)
  for the session lane's real TCP transport, only required when configuring with
  `-DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON` (OFF by default; see
  [`documents/PLATFORM_ATARI.md`](./documents/PLATFORM_ATARI.md)). Note that the
  upstream [fujinet-lib](https://github.com/FujiNetWIFI/fujinet-lib) is built for CC65
  and is **not** compatible with EDGE's llvm-mos toolchain; an llvm-mos-compatible
  build is required and is **not yet published**.
- **[Docker](https://www.docker.com/)** *(optional)* — runs the isolated UDP peer used
  in the Mode-B networking validation stack.

## Getting started

After cloning, three scripts take you from a bare checkout to a running demo:

```sh
scripts/setup.sh        # verify the toolchain is installed (read-only doctor)
scripts/build_demos.sh  # build every Atari demo .xex into build-atari/
scripts/run_tank_embedded.sh   # build + launch the tank demo in Altirra
```

`setup.sh` checks for the core tools (llvm-mos toolchains, CMake, Python) and
prints the exact remedy for anything missing. `build_demos.sh` configures the
Atari build and builds the aggregate `demos` target. The detailed
[Requirements](#requirements) and [Build and test](#build-and-test) sections
below cover the same ground manually.

Useful environment knobs (all optional; sensible defaults):

| Variable | Used by | Meaning |
| --- | --- | --- |
| `ALTIRRA_DIR` | `run_tank_*.sh`, `setup.sh` | Path to your Altirra install (else common locations are searched) |
| `FUJINETLIB_ROOT` | `run_tank_networked.sh` | fujinet-lib (llvm-mos) checkout for the live networking demo |
| `BUILD_DIR` | `build_demos.sh` | Build directory (default `build-atari`) |
| `RECONF=1` | `build_demos.sh` | Force a fresh CMake configure |

## Build and test

EDGE uses CMake and llvm-mos toolchains.

### Run the unit tests

The main test configuration targets the llvm-mos `mos-sim` simulator:

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mos-sim-toolchain.cmake
cmake --build build
ctest --test-dir build --output-on-failure
```

### Build the Atari hardware demos

From the simulator build directory:

```sh
cmake --build build --target atari_hw_test
```

Or with a dedicated Atari configure:

```sh
cmake -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake -DEDGE_BUILD_DEMO=ON
cmake --build build-atari --target atari_hw_test
```

Other demo targets follow the same pattern: `atari_scroll_test` (hardware scroll),
the VBXE set `atari_vbxe_bringup`, `atari_vbxe_gfx`, `atari_vbxe_text`, and
`atari_vbxe_sprites`, and the Arena game `arena_base` / `arena_vbxe` (one source,
ANTIC vs full-VBXE-overlay backends via `-DEDGE_VBXE=1`). See
[`/demo/README.md`](./demo/README.md) for what each one exercises on real hardware
or emulators.

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
- **Compile-time capabilities:** platform feature checks are `constexpr`, so unused code paths disappear at compile time. The bitmap subsystem is the worked example — `Game::gfx()` dispatches on `has_blitter` to a VBXE blitter or a software path with the *same* game-facing code.
- **Platform HAL:** each backend supplies the hardware-specific implementation details. Atari is the first backend; the same engine surface is intended to host others.
- **Shared screen-buffer model:** multi-screen programs share one display buffer sized for the largest screen.
- **Frame-based execution:** input is captured once per frame, game logic runs on a stable snapshot, and hardware-facing state is committed at the correct time.

## Who this is for

EDGE is aimed at developers building games or interactive software for 6502-era systems who want:

- a higher-level engine-facing API
- explicit control over memory and timing
- modern C++ templates instead of runtime abstraction
- direct paths back to low-level hardware behavior when necessary

If that matches your goals, the quickest path into the codebase is the quick start, the API reference, then the hardware demo and tests.
