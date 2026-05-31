# EDGE API Reference

This reference is organized in two layers:

1. the general engine-facing API a programmer writes against
2. the current backend-specific details that leak into the public surface today

Where a type or example uses Atari names, that is because the current backend is Atari-first, not because the entire engine model is supposed to be Atari-only.

## Include Strategy

Most applications can start with:

```cpp
#include <engine/engine.h>
```

Or include individual headers as needed:

- `<engine/core.h>`
- `<engine/display.h>`
- `<engine/input.h>`
- `<engine/pool.h>`
- `<engine/sprites.h>`
- `<engine/sound.h>`
- `<engine/scroll.h>`
- `<engine/tiles.h>`
- `<engine/interrupt.h>`

Backend headers, such as `<engine/platform/atari/platform.h>`, are only needed when selecting a concrete target platform.

## General Program Model

An EDGE program is centered around:

- a concrete `Platform` type
- a compile-time `GameConfig`
- a `using Game = engine::Core<Platform, GameConfig>;` alias

`Game` is the main façade. It exposes initialization, frame loop entry points, screen switching, convenience rendering calls, subsystem singletons, and compile-time resource queries.

The frame model is:

- input is captured once per frame
- your callback runs with an immutable input snapshot
- you update game state and write logical render/audio requests
- the backend commits hardware-facing state at the proper time, typically during VBI

## Basic Engine Types

From `engine/types.h`:

- `engine::u8`, `engine::u16`, `engine::u32`
- `engine::i8`, `engine::i16`, `engine::i32`
- `engine::bit_mask[8]`

`engine::bit_mask` is primarily useful for bitmaps, pool helpers, and manual SoA patterns.

## Core Type

```cpp
using Game = engine::Core<Platform, GameConfig>;
```

### Core Initialization

- `Game::init()`
- `Game::init(charset)`

`Game::init(charset)` loads the provided charset into engine-managed charset RAM before starting the backend services.

### Main Loop

- `Game::run(callback)`
- `Game::run_until(callback)`
- `Game::frame_overrun()`

Callback forms:

```cpp
void callback(const engine::Input& input);
bool callback(const engine::Input& input);
```

Use `run_until` when a screen or state should exit back to the caller.

### Screen and Region Access

- `Game::set_screen<ScreenType>(transition_callback)`
- `Game::region<N>()` for single-screen programs
- `Game::region<ScreenType, N>()` for multi-screen programs

Convenience text helpers on `Game` target region 0 of the active single-screen layout:

- `Game::print(col, row, const char*)`
- `Game::put_char(col, row, tile)`
- `Game::print_num(col, row, value, digits)`

### Core Subsystems

These are available as static sub-objects on `Game`:

- `Game::sound`
- `Game::scroll`
- `Game::tiles`
- `Game::interrupts`
- `Game::sprites`
- `Game::multiplex`
- `Game::hooks`

`Game::multiplex` is an alias of the sprite manager exposed under a more intention-revealing name for multiplex queries.

### Compile-Time Queries

- `Game::max_display_ram`
- `Game::ram_usage`
- `Game::user_zp_base`
- `Game::zp_remaining`

These let you reason about memory budgets without runtime instrumentation.

## Configuration Types

### GameConfig

At minimum, `GameConfig` provides:

- `using screens = engine::ScreenSet<...>;`
- `static constexpr engine::u8 max_sprites = ...;`
- `static constexpr engine::u8 sound_channels = ...;`

Optional fields implemented today include:

- `using initial_screen = SomeScreen;`
- `static constexpr engine::u8 max_dlis = ...;`
- `static constexpr engine::u8 max_vbi_hooks = ...;`
- `static constexpr engine::u8 user_zp_bytes = ...;`

### Platform

`Platform` is a backend type selected at compile time. The engine model only requires that it expose:

- `Platform::capabilities`
- `Platform::hal`

The current concrete platform implementation is Atari and is described later in this document and in [Atari Platform Guide](./PLATFORM_ATARI.md).

## Display and Screen API

### Region Descriptors

The engine exposes two display-region kinds:

- `engine::TextRegion<Mode, Height>`
- `engine::BitmapRegion<Mode, Height>`

These are composed into a screen layout:

```cpp
using Layout = engine::DisplayLayout<
    engine::TextRegion<ModeA, 1>,
    engine::BitmapRegion<ModeB, 180>,
    engine::TextRegion<ModeA, 1>
>;
```

Important contract:

- text and bitmap regions are distinct types
- each region exposes only operations valid for that region kind
- region memory layout is compile-time derived

Today, the `Mode` type comes from the Atari backend.

### ScreenSet

```cpp
struct TitleScreen { using display = engine::DisplayLayout<...>; };
struct PlayScreen  { using display = engine::DisplayLayout<...>; };

struct GameConfig {
    using screens = engine::ScreenSet<TitleScreen, PlayScreen>;
    using initial_screen = TitleScreen;
};
```

All declared screens share one screen buffer sized to the largest screen.

### TextRegionView API

Methods:

- `put_char(col, row, tile)`
- `get_char(col, row)`
- `print(col, row, const char*)`
- `print_num(col, row, value, digits)`

There are no bounds checks.

### BitmapRegionView API

Methods:

- `plot(x, y, color)`
- `point(x, y)`
- `clear(color)`
- `hline(x1, x2, y, color)`
- `blit(x, y, src, w, h)`

Pixel packing and row addressing are handled by the view.

## Input API

Primary type:

- `engine::Input`

It is an alias for `engine::InputState<2>`.

Directional level state:

- `up(port=0)`
- `down(port=0)`
- `left(port=0)`
- `right(port=0)`

Fire level and edge state:

- `fire(port=0)`
- `fire_pressed(port=0)`
- `fire_released(port=0)`

Console-style buttons:

- `start()`
- `select()`
- `option()`

Keyboard state:

- `key()`
- `key_pressed()`

Out-of-range port queries return `false`.

General contract:

- input is captured once per frame
- the game reads a stable snapshot
- edge detection is handled by the engine

## Sprite API

### Asset Builder

```cpp
constexpr auto shape = engine::make_sprite<8, 8>({ ... });
```

The returned type carries compile-time width and height metadata.

### Runtime Methods

- `Game::sprite(slot, shape, x, y)`
- `Game::missile(index, x, y, height)`
- `Game::sprite_hide(slot)`
- `Game::sprite_hide_all()`
- `Game::sprite_color(slot, color)`

General contract:

- `sprite` updates logical buffered sprite state
- the backend commit happens later, not immediately at the call site
- hidden sprites are excluded from rendering and multiplex assignment

### Collision Snapshot

Use the frame-latched snapshot returned by:

- `Game::pm_collisions()`

Queries:

- `player_to_playfield(p)`
- `player_to_player(p)`
- `missile_to_playfield(m)`
- `missile_to_player(m)`

### Multiplex Queries

- `Game::multiplex.zone_count()`
- `Game::multiplex.active_count()`
- `Game::multiplex.sprites_on_player(hw_player)`
- `Game::multiplex.player_for_sprite(logical_index)`
- `Game::multiplex.zone_for_sprite(logical_index)`

These are especially useful when more logical sprites exist than the hardware can show simultaneously.

## Sound API

### Asset Builder

```cpp
constexpr auto sfx = engine::make_sound({
    {waveform, frequency, volume, duration_frames},
});
```

An internal terminal frame is appended automatically.

### Runtime Methods

On `Game::sound`:

- `play(effect, channel)`
- `stop(channel)`
- `active(channel)`
- `release_channel(channel)`
- `reclaim_channel(channel)`
- `tick()`

General contract:

- playback is channel-based
- `play` interrupts the current effect on that channel
- `tick` advances playback one frame and is normally engine-managed
- released channels are skipped by engine playback so custom low-level code can own them

The currently implemented waveform constants are Atari/POKEY-specific and are covered in the backend section.

## Scroll API

On `Game::scroll`:

- `set(x, y)`
- `move(dx, dy)`
- `x()`
- `y()`
- `suspend()`
- `resume()`

Advanced, engine-facing methods:

- `activate(bytes_per_line, scanlines_per_line, fine_scroll_range)`
- `deactivate()`
- `apply(display_list, lms_pos, screen_base, map_width_bytes)`

General contract:

- scroll position is stored as logical viewport state
- fine and coarse scrolling are split internally
- suspended scroll stops hardware writes without discarding the tracked position

## Tiles API

### Asset Builders

```cpp
constexpr auto charset = engine::make_charset(data_array);
constexpr auto map = engine::make_map<Width, Height>(tile_indices);
```

### TileMap API

- `tile_at(col, row)`
- `set_tile(col, row, tile)`

### Tile Manager API

On `Game::tiles`:

- `init_charset(charset, dest)`
- `set_chbase(page)`
- `set_viewport(x, y)`
- `viewport_x()`
- `viewport_y()`

General contract:

- charset and tilemap assets are plain compile-time data objects
- the tile manager coordinates charset load and viewport state
- map ownership remains with the game or asset data, not the manager

## Interrupt API

On `Game::interrupts`:

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

General contract:

- handlers must be plain non-capturing `void (*)()` functions
- static and dynamic DLI chains are merged and sorted each frame
- persistent handlers survive screen transitions

### DLIContext

For C++ DLI handlers:

```cpp
engine::DLIContext<Platform>{}.write_colpf2(0xC4);
```

Available write helpers:

- `write_colpf0`
- `write_colpf1`
- `write_colpf2`
- `write_colpf3`
- `write_colbk`
- `write_chbase`
- `write_hscrol`
- `write_vscrol`

## Hooks API

On `Game::hooks`:

- `pre_sprite_commit`
- `post_render`

Both are nullable `void (*)()` function pointers.

Use them when you need a small, engine-defined seam in the frame lifecycle without taking over a full interrupt path.

## Pool API

### SlotPool

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
- `count()`
- `available()`
- `full()`
- `empty()`
- `capacity()`
- `operator[](index)`
- `for_each(fn)`
- `for_each_indexed(fn)`
- range-for over active elements

Contract:

- indices are stable
- acquired storage is not zero-initialized
- best choice when external systems refer to slots by index

### PackedPool

```cpp
engine::PackedPool<Particle, 16> pool;
```

Methods:

- `clear()`
- `acquire()`
- `release(index)`
- `release(pointer)`
- `count()`
- `available()`
- `full()`
- `empty()`
- `capacity()`
- `operator[](index)`
- `for_each(fn)`
- range-for over dense active storage

Contract:

- indices are unstable
- releasing an element swaps in the previous last element
- best choice for iteration-heavy collections with no stable external references

## Math API

Direction lookup tables:

- `engine::dir_x[4]`
- `engine::dir_y[4]`

Trig lookup tables:

- `engine::sin8[256]`
- `engine::cos8[256]`

Fixed-point:

- `engine::Fixed<I, F>`
- `engine::fixed88`

Factories and queries:

- `Fixed::from_int(v)`
- `Fixed::from_raw(v)`
- `raw()`
- `integer()`
- `fraction()`

Operators:

- `+`
- `-`
- `*`
- `==`
- `!=`

Random number helper:

- `engine::random()`

## Current Backend-Specific Details

The following public pieces are currently tied to the Atari implementation:

- the concrete platform type comes from `atari::Platform<...>`
- display mode names come from `atari::Mode`
- sound waveform constants use `engine::pokey::*`
- sprite, collision, and interrupt behavior map to Atari hardware concepts such as P/M, DLI, and VBI

For the full backend-specific guide, see [Atari Platform Guide](./PLATFORM_ATARI.md).

## Current Limits

1. Call `Game::init()` before using rendering, sound, or loop services.
2. Treat the frame callback as logical frame work, not direct hardware commit time.
3. Prefer engine subsystems first and reach for low-level hooks only when needed.
4. `engine/gfx.h` and `engine/net.h` are placeholders and are not yet usable as public subsystems.
