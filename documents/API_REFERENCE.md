# EDGE API Reference

> **Applies to EDGE v0.5.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

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
- `<engine/gfx.h>`

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
- the backend commits hardware-facing state at the proper time, typically during the frame service

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
- `Game::init(tileset)`

`Game::init(tileset)` copies the provided tileset into engine-managed character-set RAM and binds the character-set base to it before starting the backend services.

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
- `Game::gfx()` — the bitmap-drawing subsystem (accessor, not a data member)

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
- `static constexpr engine::u8 max_raster_hooks = ...;`
- `static constexpr engine::u8 max_frame_hooks = ...;`
- `static constexpr engine::u8 user_zp_bytes = ...;`
- `using bitmap_region = engine::BitmapRegion<...>;` — gives the bitmap subsystem
  (`Game::gfx()`) its software-view type on baseline platforms. Omit it on
  blitter-only platforms (then `Game::gfx()` is blitter-only and the software
  path static-asserts if reached). See the Graphics / Bitmap API below.

### Platform

`Platform` is a backend type selected at compile time. The engine model only requires that it expose:

- `Platform::capabilities`
- `Platform::hal`

The current concrete platform implementation is Atari and is described later in this document and in [Atari Platform Guide](./PLATFORM_ATARI.md).

## Display and Screen API

### Region Descriptors

The engine exposes three display-region kinds:

- `engine::TextRegion<Mode, Height>`
- `engine::BitmapRegion<Mode, Height>`
- `engine::OverlayRegion<Mode, Height>` — a VBXE overlay region whose pixels live
  in VBXE VRAM (zero screen-buffer RAM). `Mode` is a VBXE overlay mode
  (`atari::Mode::VBXE_SR`/`VBXE_HR`/`VBXE_LR`/`VBXE_T80`). Region order sets the
  overlay's vertical position. A layout of only `OverlayRegion`s is a *pure
  overlay*: `set_screen` keeps ANTIC DMA off for it automatically (no manual
  playfield-DMA disable needed), and its `OverlayRegion` mode/height are checked
  against the platform's VBXE `Config` at compile time.

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

Today, the `Mode` type comes from the Atari backend (`atari::Mode`); the region templates take it as a
backend mode token and derive geometry through `engine::display::traits<ModeT>`.
NOTE: This has been abstracted.  doc needs update.

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

## Graphics / Bitmap API

The bitmap subsystem (`engine/gfx.h`, type `engine::BitmapOps`) is a portable
drawing surface over a bitmap *canvas*. Reach it through the accessor:

```cpp
auto& g = Game::gfx();
g.clear(0);
g.fill_rect(20, 20, 120, 40, 3);
g.line(10, 10, 200, 150, 5);
```

Methods:

- `clear(color)`
- `plot(x, y, color)`
- `hline(x1, x2, y, color)`
- `vline(x, y1, y2, color)`
- `fill_rect(x, y, w, h, color)`
- `line(ax, ay, bx, by, color)` — integer Bresenham
- `blit(x, y, src, w, h)`

The key idea is portability through compile-time capability dispatch. The same
calls compile to one of two backends, chosen by the platform's `has_blitter`
capability:

- **Blitter platforms** (e.g. Atari VBXE): the canvas is the overlay framebuffer
  in VRAM. Rectangle fills go through the hardware blitter; single pixels go
  through the memory window. No bitmap region is required — leave
  `GameConfig::bitmap_region` unset.
- **Baseline platforms**: the canvas is an ANTIC `BitmapRegion`, and operations
  delegate to that region's `BitmapRegionView` (which owns the mode-dependent
  pixel packing). Declare `using bitmap_region = engine::BitmapRegion<...>;` in
  `GameConfig`; `set_screen()` binds the canvas to that region's screen-buffer
  bytes automatically.

Colours are palette indices interpreted by the active backend. On baseline the
canvas coordinates are 8-bit; on blitter platforms they are 16-bit (the overlay
is up to 320 px wide). The game code is identical either way.

## Overlay API

Some backends provide a *hardware overlay plane* composited above the normal
display. EDGE exposes it through engine-neutral seams on `Game` (today these are
backed by the Atari VBXE overlay; see the platform guide). They are no-ops on
platforms without an overlay.

Hardware text overlay:

- `Game::overlay_text_font(glyphs, bytes)` — upload an 8×8 font to the overlay's
  character memory
- `Game::overlay_text_clear(ch, attr)` — fill the character map
- `Game::overlay_put_char(col, row, ch, attr)`
- `Game::overlay_print(col, row, const char*, attr)`

Background / compositing (for sprites-over-bitmap):

- `Game::set_overlay_background(color)` — set the flat opaque background colour
- `Game::overlay_publish_background()` — publish the bitmap drawn via `Game::gfx()`
  to the live display page(s); sprite footprints are then restored from it each
  frame
- `atari::set_playfield_dma(bool enable)` — enable/disable the ANTIC playfield
  (character/bitmap) DMA. **Atari platform-layer function, opt-in escape hatch** —
  it lives in the Atari platform headers, *not* on `Game::` (ADR-031: platform-
  specific escape hatches belong on the platform). Reach it by including the Atari
  platform header your game already uses. The common case is now automatic: a
  *pure-overlay* screen (`DisplayLayout` of only `OverlayRegion`s) keeps ANTIC DMA
  off through `set_screen`, so you do **not** need to call this. When an opaque VBXE
  overlay covers the screen the ANTIC playfield is invisible, but its per-scanline
  VRAM DMA starves the blitter's restore copies and can collapse the compositor's
  per-frame budget (e.g. `Background::Bitmap` ran the loop at ~8 Hz instead of 60).
  Use it only for the cases the engine can't infer: a transparent overlay that shows
  ANTIC through (leave it enabled), or a mixed overlay+ANTIC layout where you want
  the playfield fetch off anyway. It toggles only the playfield bits; any
  `set_screen` on a non-pure-overlay layout re-enables them.

Overlay collision snapshots, latched at the frame service:

- `Game::overlay_collision()`
- `Game::overlay_blit_collision()`

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

System console buttons:

- `system_primary()`
- `system_secondary()`
- `system_option()`

Keyboard state:

- `key()`
- `key_pressed()`

Out-of-range port queries return `false`.

General contract:

- input is captured once per frame
- the game reads a stable snapshot
- edge detection is handled by the engine

## Sprite API

### Asset Builders

```cpp
constexpr auto shape = engine::make_sprite<8, 8>({ ... });        // 1 bit/pixel
constexpr auto pic   = engine::make_pixel_sprite<8, 8>({ ... });  // 1 byte/pixel
```

Both return types carry compile-time width and height metadata. There are two
shape formats:

- `make_sprite<W, H>` — `Packed1bpp`, one bit per pixel (the P/M-style format),
  coloured per instance with `sprite_color`. Works on any platform.
- `make_pixel_sprite<W, H>` — `Pixel8bpp`, one byte per pixel of palette indices,
  for richer multi-colour art. Requires a blitter platform (e.g. VBXE); using one
  on a baseline platform is a compile-time error.

Both shape types pass through the same `Game::sprite(slot, shape, x, y)` call.

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
- `missile` drives a separate, simpler projectile object on every backend; on a
  blitter platform it is the hardware P/M projectile rendered *below* the overlay
  (see the platform guide for the visibility caveat under an opaque overlay)

### Collision Snapshot

Use the frame-latched snapshot returned by:

- `Game::sprite_collisions()`

Queries:

- `sprite_to_background(s)`
- `sprite_to_sprite(s)`
- `projectile_to_background(p)`
- `projectile_to_sprite(p)`

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
    {engine::audio::Waveform::Tone, frequency, volume, duration_frames},
});
```

The waveform field is an `engine::audio::Waveform` (`Tone`, `Noise`, `Buzz`, `Silent`). An internal
`Silent` terminal frame is appended automatically.

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

Timbre is described portably with `engine::audio::Waveform`; the backend maps each value to its sound
hardware (the Atari mapping to POKEY distortion is covered in the backend guide).

## Scroll API

### Declaring a scrolling region

Wrap any region in `engine::ScrollRegion<Inner, MapW, MapH>` inside a
`DisplayLayout`, then bind a game-held `engine::TileMap` as its source:

```cpp
struct PlayScreen {
    using display = engine::DisplayLayout<
        engine::TextRegion<ModeA, 2>,                              // fixed HUD
        engine::ScrollRegion<engine::TextRegion<ModeA, 22>, 64, 32> // 64×32 map
    >;
};

static engine::TileMap<64, 32> world = engine::make_map<64, 32>({ ... });

Game::scroll_map(world);   // bind, after set_screen / init
```

The map width and height must match the `ScrollRegion`'s declared `MapW`/`MapH`
(checked at compile time).

### Driving the scroll

On `Game::scroll`:

- `set(x, y)`
- `move(dx, dy)`
- `x()`
- `y()`
- `suspend()`
- `resume()`
- `active()`
- `suspended()`

General contract:

- scroll position is stored as logical viewport state
- the frame service splits the position into fine (sub-tile) and coarse (per-line
  load address) scroll, clamps it at the map edges, and keeps the tile viewport
  (`Game::tiles`) in sync — the game loop only calls `move`/`set`
- suspended scroll stops hardware writes without discarding the tracked position

Activation and the per-line load-address patching are driven by the engine from
the `ScrollRegion` declaration; games do not call them directly.

## Tiles API

Terminology: a **tile** is one character-sized visual element; a **tile code**
is the byte stored in a **map cell** that selects a tile from the active
**tileset**; a **tile map** is a row-major grid of map cells; the **viewport**
is the moving visible window. See ARCHITECTURE.md "Tile terminology".

### Asset Builders

```cpp
constexpr auto tileset = engine::make_tileset(data_array);   // TilesetData
constexpr auto map = engine::make_map<Width, Height>(tile_codes);
```

### TileMap API

A `TileMap` owns its own row-major grid of map cells (one tile code each):

- `tile_at(col, row)` — read the tile code stored in a map cell
- `set_tile(col, row, tile_code)` — store a tile code (RAM-resident maps only)

### TileDisplay API

On `Game::tiles` (type `engine::TileDisplay`):

- `init_charset(tileset, dest)` — copy a tileset into character-set RAM
- `bind_charset_page(page)` — point the character-set base at it
- `set_viewport(x, y)`
- `viewport_x()`
- `viewport_y()`

General contract:

- tileset and tile-map assets are plain compile-time data objects
- `TileDisplay` coordinates tileset load (into charset RAM), the charset-page
  binding, and viewport state
- map ownership remains with the game or asset data, not `TileDisplay`, which
  performs no map-cell lookup
- map *chunk* loading/residency is out of scope (not implemented)

## Interrupt API

On `Game::interrupts`:

- `add_raster_hook(scanline, handler)`
- `add_raw_raster_hook(scanline, handler)`
- `add_persistent_raster_hook(scanline, handler)`
- `add_persistent_raw_raster_hook(scanline, handler)`
- `remove_raster_hook(scanline)`
- `clear_transient()`
- `begin_dynamic()`
- `add_dynamic_raster_hook(scanline, handler)`
- `prepare_chain(display_program, dl_size)`
- `arm_dispatch()`

Frame hooks:

- `add_frame_hook(handler)`
- `remove_frame_hook(handler)`
- `run_frame_hooks()`

General contract:

- handlers must be plain non-capturing `void (*)()` functions
- static and dynamic raster-hook chains are merged and sorted each frame
- persistent handlers survive screen transitions

### RasterContext

For C++ raster-hook handlers:

```cpp
engine::RasterContext<Platform>{}.set_playfield_color<2>(0xC4);
```

Available write helpers:

- `set_playfield_color<N>(v)` (field `N` is 0..3, a template argument)
- `set_background_color(v)`
- `set_charset_base(v)`
- `set_fine_scroll_x(v)`
- `set_fine_scroll_y(v)`

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
- the engine concepts map onto Atari hardware: `engine::audio::Waveform` → POKEY distortion, sprites/
  projectiles → P/M graphics, raster hooks → DLI, frame hooks/service → VBI

For the full backend-specific guide, see [Atari Platform Guide](./PLATFORM_ATARI.md).

## Network API

EDGE exposes a dual-lane transport API through `Game::net` when the platform
capability `has_network` is true. On `Network::None` platforms `Game::net` does
not exist (compile-time absent, zero storage). On `Network::Fujinet` both lanes
are available.

> **Implementation status:**
> - **Session lane** — optionally wired to real fujinet-lib TCP transport.
>   Enable with `-DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON` at configure time
>   (requires an external fujinet-lib checkout; OFF by default).
>   When OFF, session methods return `Unsupported`/`WouldBlock` stubs.
> - **Realtime lane** — wired to an EDGE-owned FujiNet Netstream path (fixed
>   16-byte packets, all-or-nothing TX/RX, **no wire framing**; no fujinet-lib).
>   The data path is validated against the fujinet-pc emulator stack
>   (NetSIO + Altirra + Docker UDP peer, Mode B); it is **not yet validated on
>   physical FujiNet hardware**. Packet boundaries are implicit and cannot
>   recover from lost bytes. See `docs/DECISIONS.md` ADR-033.
> - `net_dual_lane_demo` compiles and illustrates the intended flow; it does
>   not perform real network I/O unless the session option is enabled, and no
>   real-gameplay realtime demo exists yet.
>
> See [PLATFORM_ATARI.md — Networking](./PLATFORM_ATARI.md#networking-on-atari)
> for CMake options, blocking-risk notes, and the hardware validation checklist.

### Architecture

```
Game::net.realtime   — fixed-size packet lane (UDP-seq intent)
Game::net.session    — framed byte-stream lane (TCP intent)
```

**Realtime lane** (`Game::net.realtime`):
- intended for frame-rate multiplayer state snapshots
- fixed 16-byte packets by default (`engine::net::default_realtime_packet_bytes`)
- RX ring holds up to 8 packets by default; overflow drops oldest, increments `rx_dropped()`
- TX ring holds 2 packets by default; flushed during `poll()`
- do not use for critical reliable messages

**Session lane** (`Game::net.session`):
- intended for lobby, login, chat, match setup, and control/reliable data
- framed variable-length messages: `kind` (1 byte) + `size` (2 bytes) + payload
- RX and TX byte rings (256 bytes each by default)
- maximum message payload 128 bytes by default

Both lanes are **nonblocking** and **explicitly polled** by game code once per
frame. No network work happens from VBI or frame service context (ADR-016).

### Memory cost (approximate, defaults)

| Configuration | Core net storage |
|---|---|
| `Network::None` | 0 bytes |
| `Network::Fujinet` (both lanes) | ≈ 840–880 bytes |

Realtime packet storage: RX 8 × 16 = 128 bytes, TX 2 × 16 = 32 bytes.
Session buffers dominate: two 256-byte byte rings plus a 128-byte message store.

### Enabling the network

```cpp
using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::gfx::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC,
    atari::Network::Fujinet   // enables Game::net.realtime and Game::net.session
>;
```

With `atari::Network::None` (the default), `Game::net` is not a member of `Game`.
Accessing it is a compile error.

### Realtime lane API

On `Game::net.realtime`:

```
open_udp_seq(host, remote_port, local_port = 0) -> NetStatus
bind_udp_seq(local_port) -> NetStatus
close()
active() -> bool
poll() -> NetStatus
send(packet) -> NetStatus          // packet must be sizeof == packet_bytes()
recv(packet) -> bool               // copy-out; returns false if empty
rx_count() -> u8
rx_capacity() -> u8
rx_dropped() -> u16                // cumulative drop counter
consume_rx_overflowed() -> bool    // sticky flag; clears on read
last_error() -> NetError
```

### Session lane API

On `Game::net.session`:

```
connect_tcp(host, port) -> NetStatus
close()
connected() -> bool
poll() -> NetStatus
send(message) -> NetStatus         // sizeof(message) must be <= max_message_bytes()
send_bytes(data, size, kind = 0) -> NetStatus
recv(SessionMessageView& view) -> bool
bytes_pending() -> u16
last_error() -> NetError
```

`SessionMessageView` fields: `kind` (`u8`), `data` (`const u8*`), `size` (`u16`).
The view is valid only until the next `poll()`, `recv()`, or session buffer mutation.

### NetStatus values

```
Ok, WouldBlock, Closed, Overflow, InvalidArgument, BadConfig,
Unsupported, TransportError
```

### Example usage

```cpp
// init
Game::net.realtime.open_udp_seq("192.168.1.100", 9001);
Game::net.session.connect_tcp("192.168.1.100", 9000);

// frame loop
Game::net.realtime.poll();
engine::net::RealtimePacket16 pkt{};
while (Game::net.realtime.recv(pkt)) {
    // apply remote state from pkt.bytes[16]
}
engine::net::RealtimePacket16 local{};
local.bytes[0] = tick++;
Game::net.realtime.send(local);

Game::net.session.poll();
engine::net::SessionMessageView msg{};
while (Game::net.session.recv(msg)) {
    // handle reliable data from msg.kind / msg.data / msg.size
}
static constexpr u8 hello[] = {'h','e','l','l','o'};
Game::net.session.send_bytes(hello, sizeof(hello), 0);
```

See `demo/net_dual_lane/net_dual_lane.cpp` for the full example.

For a worked realtime-lane diagnostic that uses **only** this public API (open /
poll / send / recv / `rx_count` / `rx_dropped` / `consume_rx_overflowed` /
`last_error`) — an on-screen HUD of TX/RX sequence, RTT, jitter, and measured
throughput/loss — see `demo/edge_net_realtime_meter.cpp` and its host peer
[`tools/net/edge_realtime_peer.py`](../tools/net/README.md). Note its
`MeterPacket16` is a demo diagnostic payload only — **not** an EDGE wire format;
the realtime lane carries raw fixed 16-byte units with no framing, and the consumer
reassembles them from the byte stream.

### Design constraints

- no heap allocation, no exceptions, no RTTI, no virtual dispatch
- fixed-size buffers, compile-time capacity
- compile-time capability gating: unavailable lanes fail at compile time
- all network I/O is polled explicitly by game code
- no network work from VBI or frame service (ADR-016)
- backend-specific transport details stay below the platform HAL boundary
- `session.poll()` never calls `network_write`; no hidden flush in the frame loop
- `network_write` is called only from the explicit `send` / `send_bytes` path
- `network_write` is not used by the realtime lane; realtime uses the EDGE-owned FujiNet Netstream path (ADR-033)

---

## Current Limits

1. Call `Game::init()` before using rendering, sound, or loop services.
2. Treat the frame callback as logical frame work, not direct hardware commit time.
3. Prefer engine subsystems first and reach for low-level hooks only when needed.
