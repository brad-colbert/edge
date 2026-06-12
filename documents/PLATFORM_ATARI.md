# EDGE Atari Platform Guide

> **Applies to EDGE v0.5.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

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
    atari::gfx::Baseline,     // graphics is a TYPE axis (see below)
    atari::Sound::Mono,
    atari::TV::NTSC           // required; Network defaults to None
>;
```

Axes (in template order):

- `atari::Machine`: `A400`, `A800`, `XL`, `XE` *(enum)*
- `atari::RAM`: `Baseline`, `XE128`, `Rambo256`, `U1MB` *(enum)*
- graphics — a **type**, not an enum: `atari::gfx::Baseline` (stock ANTIC/GTIA) or
  `atari::gfx::VBXE<Config>` (the VBXE overlay; `Config` defaults to a standard
  setup). This is what lets the bitmap subsystem pick the blitter path at compile
  time. See "VBXE / Graphics" below.
- `atari::Sound`: `Mono`, `Stereo`, `PokeyMax` *(enum)*
- `atari::TV`: `NTSC`, `PAL` *(enum, required — PAL/NTSC are separate binaries)*
- `atari::Network`: `None`, `Fujinet` *(enum, trailing, defaults to `None`)*

Common aliases:

- `atari::StockXL_NTSC` — `XL, Baseline, gfx::Baseline, Mono, NTSC`
- `atari::StockXL_PAL` — the PAL counterpart
- `atari::ExpandedXE` — `XE, XE128, gfx::Baseline, Mono, NTSC`
- `atari::FullUpgrade` — `XL, U1MB, gfx::VBXE<>, PokeyMax, NTSC, Fujinet`

For a PAL build of any alias, spell the platform out with `atari::TV::PAL`.

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
- `make_sprite` shapes (`Packed1bpp`) render as P/M graphics; `make_pixel_sprite`
  shapes (`Pixel8bpp`, full colour) require the VBXE path and render through the
  overlay blitter (see "VBXE / Graphics" below)
- `Game::missile()` always drives the GTIA hardware missiles, on **both** backends.
  On the VBXE overlay backend the logical sprites composite in VRAM but the missiles
  remain P/M projectiles, rendered on the P/M layer *below* the overlay. With an
  opaque overlay they are therefore hidden — a full-overlay game that wants visible
  projectiles should draw them as `make_pixel_sprite` blitter sprites instead (the
  `arena_vbxe` demo does exactly this for its bullets)

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

## VBXE / Graphics

The graphics axis selects the backend that powers the portable bitmap subsystem
(`Game::gfx()`) and the overlay seams (`Game::overlay_*`):

- `atari::gfx::Baseline` — stock ANTIC/GTIA. `Game::gfx()` draws into an ANTIC
  `BitmapRegion` in software (the game declares `GameConfig::bitmap_region`).
- `atari::gfx::VBXE<Config>` — the VBXE FX accelerator. `Game::gfx()` draws into a
  256-colour overlay framebuffer in VRAM: rectangle fills use the hardware
  blitter, single pixels go through the MEMAC memory window. `make_pixel_sprite`
  and the hardware text overlay become available.

### Configuring the overlay

```cpp
namespace V = atari::vbxe;
using Cfg = V::Config<V::Mode::SR_320, V::Buffers::Double, V::RegBase::D640,
                      V::MEMAC_A, 0x00000, V::Background::Bitmap>;
using Platform = atari::Platform<
    atari::Machine::XL, atari::RAM::Baseline,
    atari::gfx::VBXE<Cfg>, atari::Sound::Mono, atari::TV::NTSC>;
```

`atari::vbxe::Config` knobs:

- `Mode`: `SR_320` (320 px, 256 colours), `HR_640`, `LR_160`, `Text_80`
  (80-column hardware text)
- `Buffers`: `Single` or `Double` (double-buffered gives flicker-free sprites —
  render the hidden page, then flip)
- `RegBase`: `D640` (engine default) or `D740` — match your VBXE board
- the MEMAC window placement (`MEMAC_A`, base page) and VRAM address
- `Background`: `Flat` (sprites composed over a solid colour) or `Bitmap`
  (sprites composed over a drawn bitmap; see below)

Palette entries are uploaded with the power-user helper
`atari::vbxe::set_color<Cfg>(palette, index, r, g, b)` from
`<engine/platform/atari/vbxe.h>`.

> **Place the MEMAC window clear of the call stack.** The CPU reaches VRAM through a
> banked MEMAC aperture (default MEMAC-A at `$B000-$BFFF`). While the window is
> enabled, *every* CPU access in that range goes to VRAM instead of RAM — including
> the llvm-mos soft (call) stack, which the atari8-dos runtime places at the top of
> RAM (often inside the default window). An overlapping window aliases the call stack
> onto VRAM on every VRAM access and corrupts both the display and the program
> (progressive on-screen noise, then a crash). Choose a window base in free RAM clear
> of the stack and heap — e.g. `MEMAC_A_Cfg<0xA0>` (`$A000`), as `arena_vbxe` does —
> and verify against the linker `.map`; diagnose a suspected collision by reading the
> soft-stack pointer at ZP `$80`/`$81`.

### Detecting VBXE at runtime

A program built for the VBXE overlay shows nothing on a machine without the board.
Probe for it before bringing the overlay up:

```cpp
#include <engine/platform/atari/vbxe.h>
if (!atari::vbxe::detect<Cfg>()) {
    // No VBXE — fall back / show a message on the OS text screen and halt.
}
```

`detect()` reads back the MEMAC bank-select register (a real board echoes the written
bank with its global-enable bit; absent `$D6xx` I/O floats); it uses two distinct
probe values plus a bus-flush so a floating bus can't produce a false positive.

### Hardware text overlay (`Mode::Text_80`)

In `Text_80` mode the overlay is an 80×30 character map of char + attribute
pairs in VRAM, with an 8×8 font at CHBASE. Drive it through the portable seams:

```cpp
Game::overlay_text_font(&Font[0][0], FontByteCount);  // font -> VRAM
Game::overlay_text_clear(0x20, attr);                 // blank field
Game::overlay_print(2, 1, "EDGE ENGINE", attr);
```

The attribute byte selects the foreground colour from the overlay palette; with
bit 7 set the background is taken from index `fg + 128`.

### Sprites over a bitmap (`Background::Bitmap`)

With `Background::Bitmap`, sprites are composited over a drawn background instead
of a flat colour. The pattern:

```cpp
Game::init();
draw_background();                    // draw with Game::gfx() into the VRAM master
Game::overlay_publish_background();    // seed both display pages from the master
Game::run([](const auto&) { /* move sprites; background persists */ });
```

Each frame the compositor restores every sprite's footprint from the master
canvas, so the drawn background survives under moving sprites with no flicker.
`Game::set_overlay_background(color)` sets the flat-mode background for
`Background::Flat` configs.

### Setup notes (hardware / Altirra)

- Run with **BASIC disabled** — the default MEMAC-A window is `$B000–$BFFF`, which
  is ROM when BASIC is enabled, so framebuffer writes wouldn't reach VRAM.
- On Altirra, enable the VBXE device with the **FX core** (not the GTIA-emu core).
- A `$D740` board needs `RegBase::D740` in the `Config`.

## Scrolling on Atari

Hardware scroll (declared with `engine::ScrollRegion` and driven by
`Game::scroll`) maps onto ANTIC's fine and coarse scroll:

- fine scroll within a tile uses the HSCROL / VSCROL registers
- coarse scroll repoints each display-list line's load address (per-line LMS)
- the engine clamps the position to the map's fetch width at the edges

One ANTIC quirk to know: horizontal fine scroll widens the playfield fetch, so
the leftmost map column lives in the left border (the HSCROLL fetch margin) and is
not visible. Reserve a margin column in your map. This is intentional hardware
behaviour, captured in ADR-027 of [`docs/DECISIONS.md`](../docs/DECISIONS.md) and
demonstrated by `atari_scroll_test`.

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

## Networking on Atari

The `atari::Network` axis selects the network backend:

- `atari::Network::None` (default) — no `Game::net` member; zero net storage in `Core`
- `atari::Network::Fujinet` — `Game::net.realtime` and `Game::net.session` both present

```cpp
using Platform = atari::Platform<
    atari::Machine::XL, atari::RAM::Baseline,
    atari::gfx::Baseline, atari::Sound::Mono, atari::TV::NTSC,
    atari::Network::Fujinet>;    // enables both lanes
```

The `FullUpgrade` alias includes Fujinet:

```cpp
using Platform = atari::FullUpgrade;   // XL, U1MB, VBXE<>, PokeyMax, NTSC, Fujinet
```

### Capability flags set by Fujinet

| Flag | Value |
|---|---|
| `has_network` | `true` |
| `has_network_realtime` | `true` |
| `has_network_session` | `true` |
| `network_realtime_transport` | `NetworkTransport::UDP` |
| `network_session_transport` | `NetworkTransport::TCP` |
| `network_realtime_max_payload` | 512 |
| `network_session_max_message` | 512 |
| `network_session_reliable` | `true` |
| `network_latency_ms` | 2 |

### Backend wiring status

| Lane | Backend | Status |
|---|---|---|
| `Game::net.session` | fujinet-lib / CIO `N:` TCP client | **Optional** — see below |
| `Game::net.realtime` | FujiNet Netstream / UDP-seq | Deferred — stubbed |

#### Session lane — optional fujinet-lib wiring

The session lane can be wired to the real [fujinet-lib](https://github.com/FujiNetWIFI/fujinet-lib)
C library using the `EDGE_ATARI_FUJINET_SESSION_FUJINETLIB` CMake option.
This is **OFF by default**; default builds require no external library.

```sh
# Enable at configure time and point to your fujinet-lib checkout:
cmake -S . -B build \
    -DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON \
    -DEDGE_FUJINETLIB_ROOT=/path/to/fujinetlib-llvm

# Or supply include dir and archive explicitly:
cmake -S . -B build \
    -DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON \
    -DEDGE_FUJINETLIB_INCLUDE_DIR=/path/to/fujinetlib-llvm/src/include \
    -DEDGE_FUJINETLIB_LIBRARY=/path/to/fujinetlib-llvm/lib/libfujinet.a
```

**When ON**, the session adapter calls:
- `network_init()` — once per adapter lifetime before the first open
- `network_open(devicespec, OPEN_MODE_RW, OPEN_TRANS_NONE)` — opens a TCP connection
- `network_close(devicespec)` — closes the connection
- `network_read_nb(devicespec, buf, len)` — nonblocking read
- `network_write(devicespec, buf, len)` — writes data to the session

Device spec format: `N:TCP://<host>:<port>/` (built internally; 96-byte fixed buffer).

**Blocking-risk note:** `network_write` (and `network_open`/`network_close`) may
perform a full CIO/FujiNet SIO transaction and can stall for 1–3 ms. This is
acceptable on the **session lane** for control/lobby messages. Do **not** use the
session lane for realtime frame-rate data. Avoid large session writes during
timing-critical frames.

`session.poll()` does **not** call `network_write`. It remains a lightweight
connected-flag check with no hidden flush.

**When OFF** (the default):
- no fujinet-lib headers are included
- no fujinet-lib symbols are referenced
- session methods return `Unsupported`/`WouldBlock` stubs
- all existing tests pass without the library

#### Realtime lane — deferred

The realtime lane (`Game::net.realtime`) remains **stubbed**. Every
`realtime_*` HAL method returns `WouldBlock` or a no-op. No FujiNet
Netstream or UDP-seq wiring is present. Wiring is deferred to a later stage.

#### Hardware / emulator validation checklist (deferred)

Before relying on the session lane in production, validate against real
hardware or an emulator with a FujiNet device:

- [ ] llvm-mos link test with fujinet-lib archive passes
- [ ] `connect_tcp()` opens a TCP connection to a test server
- [ ] `send_bytes()` delivers a small control message via `network_write`
- [ ] `recv()` delivers a small response via `network_read_nb`
- [ ] Measure whether `network_write` blocks noticeably (expected: 1–3 ms)
- [ ] Verify behavior when the server closes the connection
- [ ] Verify error codes from a failed host/port (unreachable host)

### Usage pattern

```cpp
static void init_network() {
    Game::net.realtime.open_udp_seq("192.168.1.100", 9001);
    Game::net.session.connect_tcp("192.168.1.100", 9000);
}

static void update_network_frame() {
    Game::net.realtime.poll();
    engine::net::RealtimePacket16 pkt{};
    while (Game::net.realtime.recv(pkt)) { /* apply remote state */ }

    Game::net.session.poll();
    engine::net::SessionMessageView msg{};
    while (Game::net.session.recv(msg)) { /* handle reliable data */ }
}
```

See `demo/net_dual_lane/net_dual_lane.cpp` for the full demo and
[API Reference — Network API](./API_REFERENCE.md#network-api) for the complete
method list.

## Current Limits

- display layouts still use the Atari mode enum (`atari::Mode`) as the concrete backend token, supplied
  through the `engine::display::traits` seam; a second backend would add its own mode type and traits
- the first backend is Atari, so some examples necessarily use Atari terminology
- the Atari FujiNet session lane is optionally wired to fujinet-lib (OFF by default); the realtime lane remains stubbed

That is expected at this stage of the project. The docs in this folder are organized so the general engine shape stays clear even where the concrete implementation is currently Atari-first.