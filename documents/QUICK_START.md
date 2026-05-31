# Quick Start: Build Your First Edge Application

This guide shows the smallest practical flow for writing an app/game with the Edge engine.

## 1. Define Target Platform

Choose hardware axes at compile time.

```cpp
#include <engine/engine.h>
#include <engine/platform/atari/platform.h>

using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::Graphics::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC
>;
```

You can also use aliases such as `atari::StockXL_NTSC` and `atari::StockXL_PAL`.

## 2. Define Screen + Game Config

At minimum, define one screen with a display layout and core capacities.

```cpp
namespace M = atari;
using engine::u8;

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
```

## 3. Define Assets

Assets are normally `constexpr` and live in ROM.

```cpp
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
    {engine::pokey::PURE, 80, 10, 8},
});
```

## 4. Write Per-Frame Callback

`Game::run` calls your callback once per frame with an immutable input snapshot.

```cpp
static engine::u8 x = 100;
static engine::u8 y = 80;

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
```

## 5. Initialize and Run

```cpp
int main() {
    Game::init();

    Game::sprite_color(0, 0x46);
    Game::print(0, 1, "READY");

    Game::run(frame_step);
}
```

## Runtime Model (Important)

- Your game callback writes logical state for this frame.
- Engine VBI service commits buffered sprite/sound/hardware state on vertical blank.
- Collision data (`Game::pm_collisions()`) is latched once per frame.

## Common First Steps After This

1. Add a second screen and use `Game::set_screen<ScreenType>(callback)`.
2. Switch from simple text to mixed text+bitmap using multiple display regions.
3. Add object pools (`engine::SlotPool` / `engine::PackedPool`) for entities.
4. Register DLI or VBI hooks with `Game::interrupts` as needed.

For full API details, continue with [API Reference](./API_REFERENCE.md).
