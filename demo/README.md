# Hardware validation demo (`hw_test.xex`)

A minimal Atari 8-bit program that drives every engine subsystem at once, so
the engine's live ANTIC path can be confirmed on real hardware / emulators
(Altirra, Fujisan).

## Build

From the existing (mos-sim) build directory — the demo is a separate target and
is **not** part of the default build or the test suite:

```sh
cmake --build build --target hw_test     # -> build/hw_test.xex
```

Or with a dedicated Atari toolchain configure:

```sh
cmake -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake \
      -DEDGE_BUILD_DEMO=ON
cmake --build build-atari --target hw_test
```

Or manually:

```sh
mos-atari8-dos-clang++ -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra \
    -I. demo/hw_test.cpp -o hw_test.xex
```

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

## Notes on engine gaps this demo fills in

The engine's portable subsystems are used through the public API. Two
live-hardware seams were still stubs; see the header comment in
[`hw_test.cpp`](hw_test.cpp) for details:

- **`Hal::install_vbi`** was fixed (in `engine/platform/atari/hal.h`) to install
  a deferred-VBI trampoline that preserves the llvm-mos zero-page registers and
  exits via `JMP XITVBV` — without this, `Game::run()` crashes on hardware.
- **DLI chain delivery** and **P/M DMA / OS shadow-register upkeep** are not yet
  implemented in the engine, so the demo installs the single colour-split DLI
  directly and writes the OS shadow registers (`SDLSTL`, `SDMCTL`, `COLOR*`,
  `PCOLR*`) itself.
