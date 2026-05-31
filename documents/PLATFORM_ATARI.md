# EDGE Atari Platform Guide

This document describes the first concrete EDGE backend: the Atari 8-bit family.

Read this after the general engine docs. The intent is:

- use the portable engine model first
- understand which types and behaviors currently come from the Atari backend
- write most game code against `engine::Core` and subsystem APIs
- drop to Atari-specific details only when you need them

## What Is Atari-Specific Today

The current backend affects these parts of the public API:

- platform selection uses `atari::Platform<...>`
- display mode names come from `atari::Mode`
- text output ultimately writes Atari internal screen codes
- sprite rendering maps to Atari player/missile graphics
- sound constants map to POKEY waveform bits
- raster work uses Atari concepts like DLI and VBI

That does not mean your whole program should be Atari-specific. In a typical EDGE application, most game logic still lives in:

- `engine::Core`
- `engine::Input`
- `engine::SlotPool` / `engine::PackedPool`
- sprite, sound, tile, scroll, and interrupt subsystem APIs

## Platform Type

The concrete target is chosen with a compile-time platform type:

```cpp
using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::Graphics::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC,
    atari::Network::None
>;
```

Axes:

- `atari::Machine`: `A400`, `A800`, `XL`, `XE`
- `atari::RAM`: `Baseline`, `XE128`, `Rambo256`, `U1MB`
- `atari::Graphics`: `Baseline`, `VBXE`
- `atari::Sound`: `Mono`, `Stereo`, `PokeyMax`
- `atari::TV`: `NTSC`, `PAL`
- `atari::Network`: `None`, `Fujinet`

Common aliases:

- `atari::StockXL_NTSC`
- `atari::StockXL_PAL`
- `atari::ExpandedXE`
- `atari::FullUpgrade`

Capability queries come from `Platform::capabilities`.

Examples:

```cpp
static_assert(Platform::capabilities::has_hardware_sprites);
static_assert(Platform::capabilities::sound_voices >= 4);
```

## Display Vocabulary

Display layouts currently use `atari::Mode` values because the implemented display backend is ANTIC-based.

Examples:

```cpp
engine::TextRegion<atari::Mode::MODE_2, 24>
engine::TextRegion<atari::Mode::MODE_4, 24>
engine::BitmapRegion<atari::Mode::BITMAP_E, 180>
```

Useful properties of the current mode system:

- text and bitmap regions are type-distinct at compile time
- bytes-per-line and scanline geometry are compile-time constants
- the screen manager builds and patches Atari display lists for you

Today, if you configure a display layout, you are choosing Atari ANTIC mode bytes indirectly through these types.

## Text Output and Screen Codes

`Game::print` accepts ASCII strings, but screen memory stores Atari internal screen codes. The conversion is handled by the text region view.

If you want a single converted byte yourself, use:

```cpp
atari::ascii_to_internal('A')
```

This is mainly relevant when using `put_char` directly.

## Sprites on Atari

The portable sprite API maps to Atari player/missile graphics.

Portable calls:

- `Game::sprite(slot, shape, x, y)`
- `Game::missile(index, x, y, height)`
- `Game::sprite_color(slot, color)`
- `Game::sprite_hide(slot)`

Atari-specific behavior behind those calls:

- up to 4 hardware players exist at once
- up to 4 hardware missiles exist at once
- additional logical sprites are multiplexed across vertical zones
- buffered sprite state is committed during VBI to avoid tearing
- collision data comes from GTIA collision registers latched once per frame

Useful multiplex queries:

- `Game::multiplex.zone_count()`
- `Game::multiplex.sprites_on_player(hw_player)`
- `Game::multiplex.player_for_sprite(logical_index)`
- `Game::multiplex.zone_for_sprite(logical_index)`

### P/M Resolution

The sprite system supports:

- `engine::PMRes::SingleLine`
- `engine::PMRes::DoubleLine`

Single-line gives finer vertical precision and uses more P/M memory.

## Sound on Atari

The current sound effect builders use POKEY-oriented waveform constants:

- `engine::pokey::PURE`
- `engine::pokey::NOISE`
- `engine::pokey::BUZZ`
- `engine::pokey::SILENT`

An EDGE sound effect is still an engine asset built with `engine::make_sound`, but its current interpretation is Atari POKEY playback.

Example:

```cpp
constexpr auto sfx = engine::make_sound({
    {engine::pokey::PURE, 80, 10, 12},
    {engine::pokey::NOISE, 40, 12, 8},
});
```

Advanced integration point:

- `Game::sound.release_channel(channel)` lets custom code take over a POKEY voice
- `Game::sound.reclaim_channel(channel)` returns that voice to EDGE

## Input on Atari

`engine::Input` exposes a portable snapshot, but today its data comes from Atari joystick, console, and keyboard state.

Relevant behavior on this backend:

- joystick ports come from the selected Atari machine profile
- `start()`, `select()`, and `option()` map to Atari console keys
- `key()` returns the current keyboard scancode, or `0`

The key point for the game author is still portable: input is captured once per frame and read as an immutable snapshot.

## Interrupts on Atari

The general interrupt API maps to Atari raster interrupts and vertical blank handling.

Portable registration surface:

- `Game::interrupts.add_dli(scanline, handler)`
- `Game::interrupts.add_raw_dli(scanline, handler)`
- `Game::interrupts.add_persistent_dli(scanline, handler)`
- `Game::interrupts.add_vbi_hook(handler)`

Atari-specific meaning:

- DLI means Display List Interrupt
- VBI means Vertical Blank Interrupt
- the interrupt manager programs DLI bits on the active display list
- sprite multiplexing also consumes DLI slots internally

For C++ DLI handlers, use `engine::DLIContext<Platform>` to write hardware-facing values without reaching into platform headers directly.

Example:

```cpp
static void split_color() {
    engine::DLIContext<Platform>{}.write_colpf2(0xC4);
}
```

## Minimal Current Atari Example

This is the smallest realistic compileable shape for the current backend.

```cpp
#include <engine/engine.h>
#include <engine/platform/atari/platform.h>

namespace M = atari;
using engine::u8;

using Platform = atari::StockXL_NTSC;

struct MainScreen {
    using display = engine::DisplayLayout<
        engine::TextRegion<M::Mode::MODE_2, 24>
    >;
};

struct GameConfig {
    using screens = engine::ScreenSet<MainScreen>;
    static constexpr u8 max_sprites = 1;
    static constexpr u8 sound_channels = 1;
};

using Game = engine::Core<Platform, GameConfig>;

constexpr auto ship = engine::make_sprite<8, 8>({
    0b00011000,
    0b00111100,
    0b01111110,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000,
});

static u8 x = 100;
static u8 y = 80;

static void frame_step(const engine::Input& in) {
    if (in.left())  x -= 1;
    if (in.right()) x += 1;
    if (in.up())    y -= 1;
    if (in.down())  y += 1;

    Game::sprite(0, ship, x, y);
    Game::print(0, 0, "EDGE");
}

int main() {
    Game::init();
    Game::sprite_color(0, 0x46);
    Game::run(frame_step);
}
```

Atari-specific pieces in that example:

- `atari::StockXL_NTSC`
- `M::Mode::MODE_2`
- the color value passed to `sprite_color`

Everything else is part of the general EDGE authoring model.

## Current Limits

- `engine/gfx.h` is still a placeholder
- `engine/net.h` is still a placeholder
- the display mode vocabulary is not yet abstracted away from Atari types
- the first backend is Atari, so some examples necessarily use Atari terminology

That is expected at this stage of the project. The docs in this folder are organized so the general engine shape stays clear even where the concrete implementation is currently Atari-first.