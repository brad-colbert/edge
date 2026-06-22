# EDGE Atari Platform Guide

> **Applies to EDGE v0.6.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

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
| `Game::net.realtime` | EDGE-owned FujiNet Netstream / UDP-seq | **Wired** — emulator-validated; not physical-HW validated |

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

#### Realtime lane — FujiNet Netstream

The realtime lane (`Game::net.realtime`) is wired to an **EDGE-owned FujiNet
Netstream** assembly path — no fujinet-lib, no per-byte CIO/SIO. Bytes move
through interrupt-driven POKEY serial rings; the engine `RealtimeLane` /
`RealtimePacketQueues` hand the adapter one **fixed 16-byte packet** at a time
with **all-or-nothing** TX/RX (it pre-checks ring space and moves a whole packet
or nothing). The adapter adds **no wire framing** — packet boundaries are
implicit (every 16 bytes) and cannot recover from lost bytes. The public
`Game::net` API is unchanged.

Netstream (UDP-seq) is an **unframed byte stream**: the firmware forwards serial
bytes to UDP in whatever chunks they arrive, so a 16-byte unit may split across
datagrams (or several may share one). Any consumer must **reassemble** fixed
16-byte units from the byte stream rather than assume one datagram == one packet —
the Atari adapter does this on its serial RX ring, and the host peer
(`tools/net/edge_realtime_peer.py`) mirrors it, resyncing on the `E7 01` marker
after a lost/reordered datagram. There is still no checksum/sequence to recover
lost bytes.

Adapter Netstream policy (internal constants in
`engine/platform/atari/fujinet_netstream_realtime.h`):

- **flags `0x26`** — UDP + UDP-seq (`0x20`) + TX external clock (`0x04`) +
  register (`0x02`); RX internal; PAL bit derived from `DetectPAL`, not pre-set
- **nominal baud `31250`** (BaudTable row `0x7A12`, AUDF3 = 21)
- **external TX clock required** (FujiNet/NetSIO drives the transmit clock)
- **30 RTCLOK-frame settle** (~0.5 s NTSC) after `begin` before the first send,
  for SIO clock renegotiation
- **port byte order** host→swapped (low → DCB DAUX1, high → DAUX2); confirmed
  `23 28` decoded by firmware as port `9000`

An earlier attempt used flags `0x20` / baud `19200` on an internal TX clock and
never transmitted; under fujinet-pc + NetSIO the emulated FujiNet drives the
transmit clock, so the external clock (`TX_EXT`) is required there or POKEY never
clocks serial output. See `docs/DECISIONS.md` ADR-033.

**TX-clock build-time override (hardware bring-up).** Because real SIO timing may
differ from the emulator, `kNetstreamFlags` is overridable at build time via
`-DEDGE_NETSTREAM_FLAGS=<value>`:

- **`0x26`** (default) — external TX clock; the emulator-validated value.
- **`0x22`** — clears `TX_EXT` (`0x04`), selecting an **internal/local** POKEY-
  generated TX clock at 31250 baud. The handler already programs this case
  (`flags & 0x0c == 0x00` → SKCTL `0x30`, RX int + TX int). This is **experimental
  and not yet validated on any stack** (it is the *more* plausible mode on real
  hardware, where the device relies on the Atari to clock the line, unlike NetSIO).

This is not the old failed config: the register bit, 31250 baud, the settle, and
the confirmed port byte order all remain. Example:
`cmake --build build-ns --target edge_net_realtime_meter` after configuring with
`-DEDGE_NETSTREAM_FLAGS=0x22` (or pass it directly to the demo compile).

**Remaining risks / future work:** physical FujiNet hardware validation is
pending; implicit 16-byte boundaries desync on lost bytes (no resync); wire
framing / resync / checksum / sequence is separate future work; a real gameplay
demo over the realtime lane is still needed; physical Atari/SIO timing may differ
from the emulator/FujiNet-PC stack (settle and baud may need retuning on HW).

#### Hardware / emulator validation checklist

**Realtime lane** — validated against the fujinet-pc emulator stack
(fujinet-pc firmware + NetSIO hub + Altirra + Docker UDP peer):

- [x] mos-sim / static: lifecycle state machine via FakeOps (CTests 19/19)
- [x] Altirra Mode A no-device clean failure (open fails cleanly, no hang)
- [x] fujinet-pc + NetSIO + Altirra + Docker UDP peer (Mode B): firmware enabled
      Netstream; flags `0x26`; AUDF3 `21`; baud `31250`; STREAM-OUT `A0..AF`;
      STREAM-IN `50..5F`; open/active/send/recv/close passed; TX IRQ diagnostic
      showed handler count advancing + ring draining; production `.bss` 359
- [ ] **physical FujiNet hardware** (open/send/recv/close on a real device)
- [ ] real gameplay demo over the realtime lane

**Session lane** — before relying on it in production, validate against real
hardware or an emulator with a FujiNet device:

- [ ] llvm-mos link test with fujinet-lib archive passes
- [ ] `connect_tcp()` opens a TCP connection to a test server
- [ ] `send_bytes()` delivers a small control message via `network_write`
- [ ] `recv()` delivers a small response via `network_read_nb`
- [ ] Measure whether `network_write` blocks noticeably (expected: 1–3 ms)
- [ ] Verify behavior when the server closes the connection
- [ ] Verify error codes from a failed host/port (unreachable host)

#### Realtime diagnostic demo + host peer

`demo/edge_net_realtime_meter.cpp` is a user-facing diagnostic for the realtime
lane (not a game, not a protocol layer). It uses **only** the public
`Game::net.realtime` API and renders a text-mode HUD plus custom-charset
sparklines for TX/RX sequence, RX count/age, round-trip delay, clock offset,
jitter, stale state, the public drop/overflow indicators, and a `LINK QUALITY (1S)`
block (measured TX/RX packets-per-second and bytes-per-second, plus round-trip /
forward / reverse loss). Because the HUD is on-screen, it needs no H: host device —
it is the practical way to read realtime-lane results **on real hardware**.

It pairs with `tools/net/edge_realtime_peer.py`, a generic stdlib-only UDP peer
(it knows nothing about Atari/SIO/POKEY/FujiNet/Netstream): `--mode echo` stamps
`T2/T3/peer_seq` and replies once per 16-byte unit; `--mode ticker` streams to a
target; `--stats-interval` reports authoritative forward-path packets/s, bytes/s,
and loss; `--tv ntsc|pal` drives a virtual jiffy clock. It performs the byte-stream
reassembly described above. See [`tools/net/README.md`](../tools/net/README.md) for
the full peer reference, the reliability/throughput readouts, and the end-to-end
Mode B recipe.

Build and run:

```sh
# Build with the REAL Netstream adapter (else the HAL is a stub: HUD shows ACTIVE
# but no SIO happens). Peer endpoint comes from the .cpp constants, or override:
cmake --build build-ns --target edge_net_realtime_meter      # peer from source
#   …configured with -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON
#   optional: -DEDGE_NET_PEER_HOST=192.168.1.50 -DEDGE_NET_PEER_PORT=9000
#   optional: -DEDGE_NETSTREAM_FLAGS=0x22   (experimental internal TX clock)

# Host peer (must be a DISTINCT IP from the Atari/FujiNet — the firmware binds
# 0.0.0.0:port and also sends to host:port, so a same-host peer self-loops):
python3 tools/net/edge_realtime_peer.py --mode echo --tv ntsc
```

Validated end-to-end over fujinet-pc + NetSIO + Altirra + a Docker UDP peer
(Mode B): after reassembly the forward path measures 0% loss / 0 corruption; the
packet *rate* is capped by the emulated NetSIO external clock stalling the CPU (an
emulation-stack characteristic, not loss or EDGE code, and not real-hardware
behaviour). **Not** validated on physical FujiNet hardware.

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

## Altirra headless probe runner

Atari `.xex` "probes" (e.g. `netstream_adapter_altirra_probe`) validate real hardware
behaviour that can't run under mos-sim (SIOV, POKEY, the OS vector page). `scripts/altirra_probe.sh`
runs one in Altirra (under wine) and captures its result **automatically** — no manual
debugger interaction:

```bash
scripts/altirra_probe.sh build-test-9p/netstream_adapter_altirra_probe.xex A   # Mode A: no FujiNet
scripts/altirra_probe.sh build-test-9p/netstream_adapter_altirra_probe.xex B   # Mode B: FujiNet present
```

It prints the captured bytes (and keeps the last capture at `/tmp/altirra_probe_last.bin`).

For **demos** (which render to the screen instead of self-dumping to H:), the companion
`scripts/altirra_screenshot.sh <program.xex> [out.png] [seconds]` boots the `.xex` in
Altirra and captures a PNG of the display (grabbing the emulator window on `DISPLAY=:0`
via ImageMagick `import`, cropped to the Atari display pane). It uses the same
`/debug …/debugcmd:g` startup-trap bypass and always tears the emulator down with
`wineserver -k`, so nothing is left parked in the debugger:

```bash
scripts/altirra_screenshot.sh build-demo/atari_hw_test.xex /tmp/hw.png   # then view the PNG
```

Example Mode A output for the adapter probe (page 6 `$0600..$064F`):

```
000010 07 00 8a 20 27 00 00 02 ...   # $0610: open=TransportError, active=0, DSTATS=$8A, AUDF3=39
000020 70 01 f0 8a c7 28 0f 00 40 00 23 28   # $0620 DCB: $70/1/$F0, buf, timeout, 64 bytes, DAUX=swap(9000)
```

### How it works (each piece is load-bearing)

- **Capture channel = the H: device, not the debugger.** Altirra's CLI `/debugcmd:` accepts
  only a *single space-free token*, so the debugger memory-dump commands (`.run <file>`,
  `ba w <addr> …`, `.writemem …`) cannot be driven from the command line. Instead the probe
  **self-dumps** its snapshot to `H1:NSDUMP.BIN` via CIO (`tests/backends/atari/atari_hostdump.h`,
  `edge_host_dump()`), and the script mounts H: to a temp host dir with `/hdpathrw <dir>` and
  reads the file back (Altirra lowercases it to `nsdump.bin`). Add `edge_host_dump(...)` at the
  end of any new probe to make it capturable.
- **False-crash bypass.** Altirra spuriously traps a fine `.xex` at the crt0 RUN entry
  ("emulated system has stopped due to a program error"). `/debug /debugbrkrun /debugcmd:g`
  breaks at the run address and resumes with `g` (which is space-free, so it passes through
  `/debugcmd:`). Without this the modal error dialog hangs the run.
- **Mode A vs Mode B.** Mode B keeps the saved profile's devices — including the user's NetSIO
  FujiNet bridge (`netsio.atdevice`, registered as device tag `custom`), which acks SIO. Mode A
  must drop *only* that device with `/removedevice custom` (override via `$ALTIRRA_FUJINET_TAG`).
  **Do not use `/cleardevices`** — it also removes the H: host device even when `/hdpathrw`
  follows it, so the capture silently fails.
- **Path mapping.** wine exposes Linux `/` as drive `Z:\`; the script converts paths.
- **Never `pkill -f Altirra`** from a shell whose own command line contains "Altirra" — it
  kills the shell. Use `wineserver -k` to stop the emulator.
- **Cross-check Fujisan** if an `.xex` dies at startup in Altirra but you suspect the build —
  Altirra's startup trap is often the false-crash, not a real bug.

The script paths assume Altirra 4.50 at the location in `$ALTIRRA_DIR`; adjust for your install.

## Netstream Mode B emulator validation

The FujiNet **Netstream** realtime data path is validated end-to-end against a real FujiNet
responder — the **fujinet-pc firmware** bridged to Altirra by the **NetSIO hub** — plus a UDP
**echo peer**. This is *emulator / FujiNet-PC* validation, **not** physical FujiNet hardware
(which remains future work; see Current Limits). Stage 9R.3 proved a byte-perfect bidirectional
round trip this way.

The stack (Atari side → network):

```
probe.xex (Altirra)  →  netsio.atdevice  →  NetSIO hub (python -m netsiohub, TCP 9996 / UDP 9997)
                     →  fujinet-pc firmware  →  UDP :9000  →  echo peer (172.30.0.2)
```

**Peer (in this repo):** `tests/backends/atari/netstream_peer_echo.py` records inbound datagrams
and, once the probe's outbound pattern `A0..AF` has arrived (after the firmware's REGISTER
packet, possibly split across datagrams), replies exactly once with `50..5F`. It must run on a
**distinct IP** — the firmware binds `0.0.0.0:9000` *and* sends to `host:9000`, so a `127.0.0.1`
peer self-loops. `scripts/netstream_modeb_peer.sh` runs it in a Docker container at a fixed IP:

```bash
scripts/netstream_modeb_peer.sh up        # docker network 172.30.0.0/24 + echo @ 172.30.0.2:9000
# (start your NetSIO hub + fujinet-pc; Altirra's netsio "custom" device connects to the hub)
cmake --build <dir> --target netstream_datapath_altirra_probe   # NS_PEER_HOST cache var = 172.30.0.2
scripts/altirra_probe.sh <dir>/netstream_datapath_altirra_probe.xex B   # Mode B keeps the netsio device
scripts/netstream_modeb_peer.sh status     # shows recv.bin (expect REGISTER + A0..AF)
scripts/netstream_modeb_peer.sh down       # tear down container + network
```

**Pass criteria** (probe page-6 dump + peer file): open `Ok`, DSTATS `$01`, `active`; send `Ok`;
peer `recv.bin` == `A0..AF`; recv `Ok` with `$0630` == `50..5F`; close inactive; no overflow. The
fujinet-pc log shows `STREAM-OUT: A0..AF` and `STREAM-IN: 50..5F`.

**Adapter policy that makes it work** (`fujinet_netstream_realtime.h`): FujiNet netstream uses an
**external TX clock** — `kNetstreamFlags = 0x26` (UDP-seq + TX_EXT `0x04` + REGISTER `0x02`),
`kNetstreamNominalBaud = 31250`. The adapter also waits `kNetstreamSettleFrames` (~0.5 s) after
begin (`RealNetstreamOps::settle()`) so the external clock renegotiates before the first
transmit — without it the first ~5–9 stream bytes corrupt. `netstream_txirq_diag_probe` (built
with `EDGE_NETSTREAM_TEST_HOOKS`) dumps the serial-IRQ counters if the TX path regresses.

## Current Limits

- display layouts still use the Atari mode enum (`atari::Mode`) as the concrete backend token, supplied
  through the `engine::display::traits` seam; a second backend would add its own mode type and traits
- the first backend is Atari, so some examples necessarily use Atari terminology
- the Atari FujiNet session lane is optionally wired to fujinet-lib (OFF by default); the realtime lane is wired to the EDGE-owned Netstream path, emulator-validated (fujinet-pc/NetSIO/Altirra/Docker peer) but not yet validated on physical FujiNet hardware

That is expected at this stage of the project. The docs in this folder are organized so the general engine shape stays clear even where the concrete implementation is currently Atari-first.