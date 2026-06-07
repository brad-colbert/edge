# Hardware validation demo (`atari_hw_test.xex`)

> **Applies to EDGE v0.4.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

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

# Scroll validation demo (`atari_scroll_test.xex`)

A second, independent demo that exercises the hardware scroll subsystem and the
Tile infrastructure: a fixed two-row HUD over a 22-row window onto a 64×32
`engine::TileMap`, scrolled with the joystick. The game loop only calls
`Game::scroll.move()`; the engine splits the position into fine (HSCROL/VSCROL)
and coarse (per-line LMS) scroll each frame. It uses the default OS font, so —
unlike `atari_hw_test` — it has no charset/asset dependency.

## Build

```sh
cmake --build build --target atari_scroll_test          # -> build/atari_scroll_test.xex
```

Or with the dedicated Atari toolchain configure (`EDGE_BUILD_DEMO=ON`, as above),
or manually:

```sh
mos-atari8-dos-clang++ -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra \
    -I. demo/atari_scroll_test.cpp -o atari_scroll_test.xex
```

## What to look for

| Observation | Proves |
|---|---|
| Push right/left → field scrolls right/left; up/down likewise | fine + coarse scroll, correct directions |
| `*` origin marker sits at the field's top-left at scroll (0,0) | map-to-screen alignment (HSCROLL margin handled) |
| Column ruler reads `00 01 02 …` across, row ruler `00 01 02 …` down | per-line LMS addressing, no tearing |
| Tile boundaries stay seamless through a fine→coarse step | fine/coarse handoff |
| Scrolling stops cleanly at every map edge | edge clamping (to the fetch width) |
| HUD (`X` / `Y` / frame) stays fixed while the field scrolls | mixed scroll + non-scroll layout |

The `#` left-margin column is intentionally **not** visible — it lives in the
left border (the HSCROLL fetch margin). See the ANTIC scroll quirks captured in
the engine memory / [`engine/scroll.h`](../engine/scroll.h) and ADR-027 in
[`docs/DECISIONS.md`](../docs/DECISIONS.md).

# VBXE demos

Four additional demos exercise the VBXE (`atari::gfx::VBXE<...>`) graphics path —
the 256-colour overlay, the blitter-backed bitmap subsystem (`Game::gfx()`), the
hardware 80-column text overlay, and the sprites-over-bitmap compositor. They
build to independent `.xex` targets and need a VBXE-capable target.

## Common setup (all four)

- **Run with BASIC disabled.** The default MEMAC-A window is `$B000–$BFFF`; with
  BASIC enabled that range is ROM and the CPU's framebuffer writes won't reach
  VRAM. On Altirra: disable the BASIC ROM (or hold OPTION at boot).
- **Altirra:** enable the VBXE device with the **FX core** (not the GTIA-emu core).
- The engine default register base is `$D640`; a `$D740` board needs a `Config`
  override (`gfx::VBXE<vbxe::Config<...RegBase::D740...>>`).

## Build

```sh
cmake --build build --target atari_vbxe_bringup    # -> build/atari_vbxe_bringup.xex
cmake --build build --target atari_vbxe_gfx        # -> build/atari_vbxe_gfx.xex
cmake --build build --target atari_vbxe_text       # -> build/atari_vbxe_text.xex
cmake --build build --target atari_vbxe_sprites    # -> build/atari_vbxe_sprites.xex
```

(Or configure with `-DEDGE_BUILD_DEMO=ON` against the Atari toolchain, as above.)

## `atari_vbxe_bringup` (`vbxe_bringup.cpp`)

Phase-4a overlay bring-up smoke test: brings the VBXE overlay up through the
engine and paints a diagnostic pattern with the `atari::vbxe` power-user header.

| Observation | Proves |
|---|---|
| Eight horizontal colour bars fill the screen | overlay compositing over ANTIC at top priority |
| A normal ANTIC text screen instead | overlay isn't compositing (check setup) |
| A black screen instead | framebuffer writes aren't reaching VRAM (BASIC/MEMAC) |

## `atari_vbxe_gfx` (`vbxe_gfx.cpp`)

Draws a static picture into the overlay framebuffer entirely through the portable
`Game::gfx()` API (single-buffered; the picture is drawn once and persists).

| Observation | Proves |
|---|---|
| Dark-blue field covering the screen | `gfx().clear()` (blitter fill) |
| Eight colour-bar rectangles down the left | `fill_rect` via the blitter |
| White frame around the inset edge | `hline` / `vline` |
| Two crossing diagonals on the right | `line` (Bresenham via the MEMAC window) |
| Small chequered 16×16 image | `blit` of an 8bpp source |

## `atari_vbxe_text` (`vbxe_text.cpp`)

Brings up the overlay in `Mode::Text_80`, uploads the PREPPIE font to VRAM, and
prints through the portable `Game::overlay_*` text seam.

| Observation | Proves |
|---|---|
| Dark-blue field with white/yellow text | hardware 80-column text overlay |
| Title + body lines render in the PREPPIE font | `overlay_text_font` (font→CHBASE) + `overlay_print` |
| A 16×16 block showing all 256 glyphs | `overlay_put_char` across the full font |

## `atari_vbxe_sprites` (`vbxe_sprites_over_bitmap.cpp`)

Double-buffered `Background::Bitmap`: the background is drawn once with
`Game::gfx()` into the VRAM master, published, and two sprites slide over it.
The screen is a pure-overlay `DisplayLayout<OverlayRegion<VBXE_SR, 240>>`, so
`set_screen` keeps ANTIC DMA off automatically — without that, the hidden ANTIC
playfield's DMA would contend the VRAM bus and stall the blitter's per-frame
restore copies, throttling motion to ~8 Hz. (Earlier versions of this demo
achieved the same with a manual `Game::antic_playfield(false)` call.)

| Observation | Proves |
|---|---|
| The `vbxe_gfx` picture as a steady background | `gfx()` master canvas + `overlay_publish_background()` |
| A red ball and a multi-colour pixel sprite slide across | `make_sprite` (Packed1bpp) + `make_pixel_sprite` (Pixel8bpp) |
| The background stays intact under the moving sprites | per-frame footprint restore from the master (flicker-free) |
| The sprites move at full speed (~60 px/s) | pure-overlay layout keeps ANTIC DMA off, freeing the VRAM bus for the blitter |

# Arena game demo (`arena_base.xex` / `arena_vbxe.xex`)

A small complete Berzerk-style game (`demo/arena/arena.cpp`): a three-screen flow
(title → play → game-over), a bordered brick room, a HUD (score / level / lives),
homing enemies, shooting, explosions, a difficulty ramp, and a high score. It is
the worked example of one game source targeting two rendering backends from the
**same game logic**, selected at compile time:

- **`arena_base`** — baseline Atari: ANTIC Mode 4 (4-colour) text screens for the
  room/HUD/text, 1bpp Player/Missile sprites (player + up to 3 enemies, single
  multiplex zone), P/M hardware missiles, and a DLI HUD/play colour split.
- **`arena_vbxe`** — VBXE "Tier 2": the **entire** scene renders through the VBXE
  overlay with the ANTIC playfield off (no bus contention). The room, HUD, and text
  are drawn into the overlay bitmap (reusing the same charset glyphs), and the
  player, enemies, and bullets are multi-colour `Pixel8bpp` blitter sprites
  composited over it. Everything outside a few `#ifdef EDGE_VBXE` blocks (platform
  typedef, sprite shapes, palette, and the overlay render helpers) is shared
  verbatim with the baseline.

```sh
cmake --build build --target arena_base     # -> build/arena_base.xex
cmake --build build --target arena_vbxe     # -> build/arena_vbxe.xex  (-DEDGE_VBXE=1)
```

## Setup (`arena_vbxe`)

Same as the other VBXE demos (FX core, BASIC disabled — see "Common setup" above),
with one extra requirement baked into the demo: it runs `atari::vbxe::detect()`
before bringing up the overlay and prints **"VBXE REQUIRED"** on the OS text screen
(then halts) if no VBXE is found, instead of showing a black/garbage overlay.

> **MEMAC window note:** `arena_vbxe` places its MEMAC-A window at **`$A000`**, not
> the engine default `$B000`. On this binary the llvm-mos soft (call) stack lives
> at ~`$BC01`, inside the default `$B000-$BFFF` window — and enabling the window for
> VRAM access then aliases the call stack onto VRAM, corrupting both the display and
> the program. Any overlay program whose stack reaches into the window must move it.
> See [`docs/CONSTRAINTS.md`](../docs/CONSTRAINTS.md) and ADR-030.

## What to look for (`arena_vbxe`)

| Observation | Proves |
|---|---|
| Title / game-over text legible, room + HUD on the play screen | overlay bitmap text + tile rendering (`gfx()` into the master) |
| Blue multi-colour player, red multi-colour enemies, white bullets | `Pixel8bpp` blitter sprites composited over the bitmap |
| Score/level/lives update, enemies explode, death → game-over → title | shared game logic driving the overlay render path |
| Full-speed play, no progressive screen corruption | pure-overlay (ANTIC off) + MEMAC window clear of the stack |
