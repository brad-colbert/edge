# EDGE Atari Platform Guide

> **Applies to EDGE v0.1.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

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
- the generic `engine::audio::Waveform` timbres map to POKEY distortion settings
- raster hooks and frame hooks are delivered through the Atari DLI and VBI

That does not mean your whole program should be Atari-specific. In a typical EDGE application, most game logic still lives in:

- `engine::Core`
- `engine::Input`
- `engine::SlotPool` / `engine::PackedPool`
- sprite, sound, tile, scroll, and interrupt subsystem APIs

The generic engine concepts and the Atari mechanisms behind them map like this:

| Engine concept | Atari backing |
| --- | --- |
| raster hook (`add_raster_hook`) | Display List Interrupt (DLI) |
| frame hook / frame service | Vertical Blank Interrupt (VBI) |
| `engine::audio::Waveform` | POKEY AUDC distortion bits |
| sprite / projectile | player / missile (P/M) graphics |
| `RasterContext` | direct GTIA/ANTIC register stores during a DLI |

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
- buffered sprite state is committed during the frame service (the Atari deferred VBI) to avoid tearing
- collision data comes from GTIA collision registers latched once per frame

Useful multiplex queries:

- `Game::multiplex.zone_count()`
- `Game::multiplex.sprites_on_player(hw_player)`
- `Game::multiplex.player_for_sprite(logical_index)`
- `Game::multiplex.zone_for_sprite(logical_index)`

### Sprite Vertical Resolution

The sprite system supports:

- `engine::SpriteVerticalResolution::SingleLine`
- `engine::SpriteVerticalResolution::DoubleLine`

Single-line gives finer vertical precision and uses more sprite (P/M) memory.

## Sound on Atari

EDGE sound effects describe a note's timbre with the generic `engine::audio::Waveform` enum:

- `engine::audio::Waveform::Tone`
- `engine::audio::Waveform::Noise`
- `engine::audio::Waveform::Buzz`
- `engine::audio::Waveform::Silent` (terminal sentinel)

A sound effect is an engine asset built with `engine::make_sound`; on this backend the HAL maps each
`Waveform` to the matching POKEY AUDC distortion bits for playback.

Example:

```cpp
constexpr auto sfx = engine::make_sound({
    {engine::audio::Waveform::Tone, 80, 10, 12},
    {engine::audio::Waveform::Noise, 40, 12, 8},
});
```

Advanced integration point:

- `Game::sound.release_channel(channel)` lets custom code take over a POKEY voice
- `Game::sound.reclaim_channel(channel)` returns that voice to EDGE

## Input on Atari

`engine::Input` exposes a portable snapshot, but today its data comes from Atari joystick, console, and keyboard state.

Relevant behavior on this backend:

- joystick ports come from the selected Atari machine profile
- `system_primary()`, `system_secondary()`, and `system_option()` map to the Atari START / SELECT / OPTION console keys
- `key()` returns the current keyboard scancode, or `0`

The key point for the game author is still portable: input is captured once per frame and read as an immutable snapshot.

## Interrupts on Atari

The general interrupt API maps to Atari raster interrupts and vertical blank handling.

Portable registration surface:

- `Game::interrupts.add_raster_hook(scanline, handler)`
- `Game::interrupts.add_raw_raster_hook(scanline, handler)`
- `Game::interrupts.add_persistent_raster_hook(scanline, handler)`
- `Game::interrupts.add_frame_hook(handler)`

Atari-specific meaning:

- a raster hook is delivered as a Display List Interrupt (DLI)
- a frame hook runs from the Vertical Blank Interrupt (VBI)
- the interrupt manager programs the DLI bits on the active display list
- sprite multiplexing also consumes raster-hook slots internally

For C++ raster-hook handlers, use `engine::RasterContext<Platform>` to write hardware-facing values without reaching into platform headers directly. `set_playfield_color<N>` takes the field as a template argument so it stays a single register store.

Example:

```cpp
static void split_color() {
    engine::RasterContext<Platform>{}.set_playfield_color<2>(0xC4);
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
- display layouts still use the Atari mode enum (`atari::Mode`) as the concrete backend token, supplied
  through the `engine::display::traits` seam; a second backend would add its own mode type and traits
- the first backend is Atari, so some examples necessarily use Atari terminology

That is expected at this stage of the project. The docs in this folder are organized so the general engine shape stays clear even where the concrete implementation is currently Atari-first.