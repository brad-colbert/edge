# Hardware validation demo (`atari_hw_test.xex`)

A minimal Atari 8-bit program that drives every engine subsystem at once, so
the engine's live ANTIC path can be confirmed on real hardware / emulators
(Altirra, Fujisan).

## Build

From the existing (mos-sim) build directory — the demo is a separate target and
is **not** part of the default build or the test suite:

```sh
cmake --build build --target atari_hw_test     # -> build/atari_hw_test.xex
```

Or with a dedicated Atari toolchain configure:

```sh
cmake -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake \
      -DEDGE_BUILD_DEMO=ON
cmake --build build-atari --target atari_hw_test
```

Or manually:

```sh
cmake -DINPUT=$PWD/demo/TEST.FNT \
  -DOUTPUT=$PWD/demo/user_charset.h \
    -P cmake/generate_charset_header.cmake &&
mos-atari8-dos-clang++ -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra \
    -I. demo/atari_hw_test.cpp -o atari_hw_test.xex
```

To try a different 1K font file, replace `demo/TEST.FNT` in the command above,
or pass `-DEDGE_DEMO_CHARSET_BIN=/path/to/your.fnt` to the CMake build.

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

## Notes

This demo is written entirely against the public engine API (`engine::Core` /
`Platform::hal`) — no `poke()`s, no inline assembly, no magic addresses. The
engine owns the display program, sprite DMA, raster-hook delivery, and the
per-frame service; see the header comment in
[`atari_hw_test.cpp`](atari_hw_test.cpp) for the full breakdown.

The one backend seam worth calling out is the deferred-frame ISR install:
**`Hal::install_frame_isr`** (in `engine/platform/atari/hal.h`) installs a
trampoline that preserves the llvm-mos zero-page registers and exits via
`JMP XITVBV` — without it, `Game::run()` would crash on real hardware (it is never
exercised under `mos-sim`, which has no NMI).
