# Quick Start

> **Applies to EDGE v0.2.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

This guide introduces EDGE from the engine author's point of view first, then shows the current concrete setup using the Atari backend.

## The Core Mental Model

An EDGE program is built from three pieces:

1. A `Platform` type that selects the hardware backend and capabilities.
2. A `GameConfig` type that declares screens and compile-time budgets.
3. A `Game` alias built as `engine::Core<Platform, GameConfig>`.

Most author-written code then talks to `Game` and the engine subsystems hanging off it.

The important architectural idea is that your game logic is frame-based and mostly engine-facing. You write game state updates, screen writes, sprite placement, sound requests, and object management. The backend-specific hardware work is committed by the engine at the right time.

## What Usually Stays Portable

In normal gameplay code, these are the main concepts you work with regardless of backend:

- `engine::Core<Platform, GameConfig>`
- `engine::Input`
- `engine::SlotPool` and `engine::PackedPool`
- `engine::make_sprite`, `engine::make_pixel_sprite`, `engine::make_sound`,
  `engine::make_charset`, `engine::make_map`
- `engine::audio::Waveform` (note timbre portably; the backend maps it to its sound hardware)
- `Game::run`, `Game::run_until`, `Game::set_screen`
- `Game::sound`, `Game::scroll`, `Game::tiles`, `Game::interrupts`, `Game::gfx()`

Today, the platform type and display mode names are still provided by the Atari backend because that is the first implemented target.

## Program Shape

At a high level, an EDGE application looks like this:

```cpp
// 1. Select a concrete backend platform type.
using Platform = /* backend-specific platform type */;

// 2. Declare one or more screens.
struct MainScreen {
    using display = engine::DisplayLayout</* backend-specific mode types */>;
};

// 3. Declare compile-time budgets and screen set.
struct GameConfig {
    using screens = engine::ScreenSet<MainScreen>;
    static constexpr engine::u8 max_sprites = ...;
    static constexpr engine::u8 sound_channels = ...;
};

// 4. Build the engine facade used by game code.
using Game = engine::Core<Platform, GameConfig>;

// 5. Define constexpr assets and static game state.
// 6. Write a per-frame callback.
// 7. Call Game::init() and Game::run(...).
```

## Current Concrete Example: Atari

The following example is intentionally small but real. It uses the current Atari backend while keeping the game-facing code on the general engine surface.

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
    static constexpr u8 max_sprites = 2;
    static constexpr u8 sound_channels = 2;
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

constexpr auto beep = engine::make_sound({
    {engine::audio::Waveform::Tone, 80, 10, 8},
});

static u8 x = 100;
static u8 y = 80;

static void frame_step(const engine::Input& in) {
    if (in.left()  && x > 40)  x -= 2;
    if (in.right() && x < 200) x += 2;
    if (in.up()    && y > 30)  y -= 2;
    if (in.down()  && y < 220) y += 2;

    if (in.fire_pressed()) {
        Game::sound.play(beep, 0);
    }

    Game::sprite(0, ship, x, y);
    Game::print(0, 0, "EDGE DEMO");
}

int main() {
    Game::init();
    Game::sprite_color(0, 0x46);
    Game::run(frame_step);
}
```

### Which Parts of That Example Are General EDGE API?

These lines reflect the general engine authoring model:

- `engine::Core<Platform, GameConfig>`
- `engine::DisplayLayout`, `engine::TextRegion`, `engine::ScreenSet`
- `engine::make_sprite`, `engine::make_sound`
- `engine::Input`
- `Game::init`, `Game::run`
- `Game::sound.play`, `Game::sprite`, `Game::print`, `Game::sprite_color`

### Which Parts Are Current Atari Details?

These are backend-specific today:

- `atari::StockXL_NTSC`
- `M::Mode::MODE_2`
- the meaning of the sprite color byte (and the concrete POKEY encoding the backend gives each `Waveform`)

## The Frame Model

EDGE is built around one callback per frame.

What your callback does:

- reads the frame input snapshot
- updates game state
- requests sounds
- writes text or bitmap content
- places logical sprites and missiles

What the engine does around that callback:

- captures input once per frame
- advances sound playback
- commits buffered sprite state during the frame service
- latches collision information once per frame
- rebuilds dynamic raster-hook work such as sprite multiplexing

That means you generally treat the engine as a double-buffered frame system, not as a register-poking loop.

## Recommended First Steps

After the minimal example works, the usual next steps are:

1. Add object pools for entities with `engine::SlotPool` or `engine::PackedPool`.
2. Split the program into screens and transition with `Game::set_screen<ScreenType>(callback)`.
3. Add tile and charset assets with `engine::make_charset` and `engine::make_map`.
4. Use `Game::sprite_collisions()` and multiplex queries if your game uses many sprites.
5. Add raster or frame hooks only after the basic game loop is stable.

### Drawing bitmaps

For pixel drawing, use the bitmap subsystem through `Game::gfx()`. It offers a
small portable API — `clear`, `plot`, `hline`, `vline`, `fill_rect`, `line`,
`blit` — and the same calls compile to a hardware blitter on capable platforms
(Atari VBXE) or to a software path on baseline. The software path needs a bitmap
region: declare `using bitmap_region = engine::BitmapRegion<...>;` in your
`GameConfig`. See the [API Reference](./API_REFERENCE.md) "Graphics / Bitmap API".

### Scrolling

For a scrolling playfield, wrap a region in `engine::ScrollRegion<Inner, MapW,
MapH>` inside its `DisplayLayout`, bind a `engine::TileMap` with
`Game::scroll_map(map)`, and drive it with `Game::scroll.move()` /
`Game::scroll.set()`. The engine splits the position into fine and coarse scroll
and keeps the tile viewport in sync each frame.

From here, read [API Reference](./API_REFERENCE.md) for the portable subsystem contracts, then [Atari Platform Guide](./PLATFORM_ATARI.md) for the backend-specific details currently exposed by the first implementation.
