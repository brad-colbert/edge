# Edge Engine API Reference

This reference describes the currently implemented public API intended for game/application authors.

## Include Strategy

Use either the umbrella header:

```cpp
#include <engine/engine.h>
```

Or include specific headers when desired:

- `<engine/core.h>`
- `<engine/display.h>`
- `<engine/input.h>`
- `<engine/pool.h>`
- `<engine/sprites.h>`
- `<engine/sound.h>`
- `<engine/scroll.h>`
- `<engine/tiles.h>`
- `<engine/interrupt.h>`
- `<engine/platform/atari/platform.h>`

## Basic Types and Constants

From `engine/types.h`:

- `engine::u8`, `engine::u16`, `engine::u32`
- `engine::i8`, `engine::i16`, `engine::i32`
- `engine::bit_mask[8]`

## Platform Configuration

Platform is a compile-time type:

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
- `atari::Network`: `None`, `Fujinet` (optional parameter)

Common aliases:

- `atari::StockXL_NTSC`
- `atari::StockXL_PAL`
- `atari::ExpandedXE`
- `atari::FullUpgrade`

Capabilities are exposed as `Platform::capabilities::<field>`.

## Display Layout and Screen Model

### Regions

Text region:

```cpp
engine::TextRegion<atari::Mode::MODE_2, 24>
```

Bitmap region:

```cpp
engine::BitmapRegion<atari::Mode::BITMAP_E, 180>
```

Compose a screen layout:

```cpp
using Layout = engine::DisplayLayout<
    engine::TextRegion<atari::Mode::MODE_2, 1>,
    engine::BitmapRegion<atari::Mode::BITMAP_E, 180>,
    engine::TextRegion<atari::Mode::MODE_2, 1>
>;
```

### Screen Set

```cpp
struct TitleScreen { using display = engine::DisplayLayout<...>; };
struct PlayScreen  { using display = engine::DisplayLayout<...>; };

struct GameConfig {
    using screens = engine::ScreenSet<TitleScreen, PlayScreen>;
    using initial_screen = TitleScreen; // optional; defaults to first
    static constexpr engine::u8 max_sprites = 9;
    static constexpr engine::u8 sound_channels = 2;
};
```

## Core Type and Entry Points

```cpp
using Game = engine::Core<Platform, GameConfig>;
```

### Initialization

- `Game::init()`
- `Game::init(charset)`

`init(charset)` loads a charset into engine charset RAM and updates CHBASE.

### Main Loop

- `Game::run(callback)` -> never returns
- `Game::run_until(callback)` -> returns when callback returns `true`
- `Game::frame_overrun()` -> `true` when frame budget was exceeded

Callback signatures:

```cpp
void callback(const engine::Input& input);
bool callback(const engine::Input& input); // for run_until
```

### Screen and Region Access

- `Game::set_screen<ScreenType>(transition_callback)`
- `Game::region<N>()` for single-screen configurations
- `Game::region<ScreenType, N>()` for multi-screen configurations

Convenience text functions target region 0:

- `Game::print(col, row, const char*)`
- `Game::put_char(col, row, tile)`
- `Game::print_num(col, row, value, digits)`

### Subsystem Objects on Core

- `Game::sound`
- `Game::scroll`
- `Game::tiles`
- `Game::interrupts`
- `Game::sprites`
- `Game::multiplex` (alias of sprite manager)
- `Game::hooks`

### Compile-Time Queries

- `Game::max_display_ram`
- `Game::ram_usage`
- `Game::user_zp_base`
- `Game::zp_remaining`

## Input API

Type: `engine::Input` (alias of `engine::InputState<2>`).

Directional level state:

- `up(port=0)`, `down(port=0)`, `left(port=0)`, `right(port=0)`

Fire level/edges:

- `fire(port=0)`
- `fire_pressed(port=0)`
- `fire_released(port=0)`

Console keys:

- `start()`, `select()`, `option()`

Keyboard:

- `key()` (current scancode or 0)
- `key_pressed()` (edge)

Out-of-range port queries return `false`.

## Text Region API

`TextRegionView` methods:

- `put_char(col, row, tile)`
- `get_char(col, row)`
- `print(col, row, const char*)` (ASCII converted to ANTIC screen code)
- `print_num(col, row, value, digits)`

No bounds checks are performed.

## Bitmap Region API

`BitmapRegionView` methods:

- `plot(x, y, color)`
- `point(x, y)`
- `clear(color)`
- `hline(x1, x2, y, color)`
- `blit(x, y, src, w, h)`

Pixel packing is mode-dependent and handled internally.

## Sprite and Missile API

### Asset Builder

```cpp
constexpr auto shape = engine::make_sprite<8, 8>({ ... });
```

### Runtime Methods

- `Game::sprite(slot, shape, x, y)`
- `Game::missile(index, x, y, height)`
- `Game::sprite_hide(slot)`
- `Game::sprite_hide_all()`
- `Game::sprite_color(slot, color)`

### Collision Snapshot

- `Game::pm_collisions().player_to_playfield(p)`
- `Game::pm_collisions().player_to_player(p)`
- `Game::pm_collisions().missile_to_playfield(m)`
- `Game::pm_collisions().missile_to_player(m)`

These are latched once per frame in VBI.

### Multiplex Queries

- `Game::multiplex.zone_count()`
- `Game::multiplex.active_count()`
- `Game::multiplex.sprites_on_player(hw_player)` -> bitmask
- `Game::multiplex.player_for_sprite(logical_index)` -> player or `0xFF`
- `Game::multiplex.zone_for_sprite(logical_index)` -> zone or `0xFF`

## Sound API

### Asset Builder

```cpp
constexpr auto sfx = engine::make_sound({
    {engine::pokey::PURE, 80, 10, 12},
    {engine::pokey::NOISE, 40, 12, 8},
});
```

Waveform constants:

- `engine::pokey::PURE`
- `engine::pokey::NOISE`
- `engine::pokey::BUZZ`
- `engine::pokey::SILENT` (terminal marker)

### Runtime Methods (`Game::sound`)

- `play(effect, channel)`
- `stop(channel)`
- `active(channel)`
- `release_channel(channel)`
- `reclaim_channel(channel)`
- `tick()` (normally called by engine VBI)

Calling `play` interrupts any currently playing effect on that channel.

## Scroll API

Methods on `Game::scroll`:

- `set(x, y)`
- `move(dx, dy)`
- `x()`, `y()`
- `suspend()`
- `resume()`

Internal/engine-managed methods (advanced use):

- `activate(bytes_per_line, scanlines_per_line, fine_scroll_range)`
- `deactivate()`
- `apply(display_list, lms_pos, screen_base, map_width_bytes)`

## Tiles API

### Asset Builders

```cpp
constexpr auto charset = engine::make_charset(data_array);
constexpr auto map = engine::make_map<Width, Height>(tile_indices);
```

### TileMap Methods

- `tile_at(col, row)`
- `set_tile(col, row, tile)`

### Tile Manager (`Game::tiles`)

- `init_charset(charset, dest)`
- `set_chbase(page)`
- `set_viewport(x, y)`
- `viewport_x()`, `viewport_y()`

## Interrupt API

Methods on `Game::interrupts`:

- `add_dli(scanline, handler)`
- `add_raw_dli(scanline, handler)`
- `add_persistent_dli(scanline, handler)`
- `add_persistent_raw_dli(scanline, handler)`
- `remove_dli(scanline)`
- `clear_transient()`
- `begin_dynamic()`
- `add_dynamic_dli(scanline, handler)`
- `prepare_chain(display_list, dl_size)`
- `arm_dispatch()`

VBI hooks:

- `add_vbi_hook(handler)`
- `remove_vbi_hook(handler)`
- `run_vbi_hooks()`

DLI handlers are plain `void (*)()` function pointers (non-capturing only).

DLI context helper for C++ handlers:

```cpp
engine::DLIContext<Platform>{}.write_colpf2(0xC4);
```

Other write helpers: `write_colpf0/1/3`, `write_colbk`, `write_chbase`, `write_hscrol`, `write_vscrol`.

## Hooks API

`Game::hooks` contains optional frame hooks:

- `pre_sprite_commit` (runs in VBI before sprite commit)
- `post_render` (runs after frame callback in loop)

Both are nullable `void (*)()` function pointers.

## Pool API

### SlotPool (stable indices)

```cpp
engine::SlotPool<Enemy, 8> pool;
```

Methods:

- `clear()`
- `acquire()`
- `acquire(slot_index_out)`
- `release(index)`
- `release(pointer)`
- `active(index)`
- `count()`, `available()`, `full()`, `empty()`, `capacity()`
- `operator[](index)`
- `for_each(fn)`
- `for_each_indexed(fn)`
- range-for iteration over active elements

Important: acquired memory is not zero-initialized.

### PackedPool (dense iteration, unstable indices)

```cpp
engine::PackedPool<Particle, 16> pool;
```

Methods:

- `clear()`
- `acquire()`
- `release(index)`
- `release(pointer)`
- `count()`, `available()`, `full()`, `empty()`, `capacity()`
- `operator[](index)`
- `for_each(fn)`
- range-for iteration over `[0, count)`

`release(index)` swaps in the last active element.

## Math API

Direction vectors:

- `engine::dir_x[4]`
- `engine::dir_y[4]`

Trig LUTs:

- `engine::sin8[256]`
- `engine::cos8[256]`

Fixed-point:

- `engine::Fixed<I, F>`
- `engine::fixed88`

Factories and operators:

- `Fixed::from_int(v)`
- `Fixed::from_raw(v)`
- `raw()`, `integer()`, `fraction()`
- `+`, `-`, `*`, `==`, `!=`

Random generator:

- `engine::random()` -> 8-bit LFSR value (non-zero cycle)

## Practical Notes

1. Call `Game::init()` before any rendering, sound, or loop usage.
2. Use `engine::SlotPool` for entities with stable external IDs (sprite/collision mapping).
3. Use `engine::PackedPool` for dense iteration-only collections.
4. Keep DLI handlers minimal and non-capturing.
5. Treat `run` callback as frame logic only; hardware commits happen in VBI.
6. `engine/gfx.h` and `engine/net.h` are placeholders and not yet usable APIs.
