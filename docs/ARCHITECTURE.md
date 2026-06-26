# Architecture

> **Applies to EDGE v0.7.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

System design, component relationships, and the abstraction
boundaries that make the engine portable across 6502 platforms
while generating optimal code for each target.

## Layered Architecture

The engine is organized into four layers. Dependencies flow
downward only — no layer references anything above it.

```
┌─────────────────────────────────────────────┐
│              GAME CODE (user)                │
│  Game state, logic, entity behavior, assets  │
├─────────────────────────────────────────────┤
│            ENGINE API (portable)             │
│  Pools, Sprites, Tiles, Sound, Input, Loop   │
├──────────────┬──────────────────────────────┤
│ CAPABILITIES │      PLATFORM HAL            │
│  Compile-time│  Hardware registers, DMA,     │
│  feature     │  interrupts, memory mapping,  │
│  profiles    │  display construction          │
├──────────────┴──────────────────────────────┤
│           HARDWARE (actual silicon)           │
└─────────────────────────────────────────────┘
```

### Layer Responsibilities

**Game Code** — written by the game author. Uses only the Engine
API. Never touches hardware registers directly. Compiled against
a specific platform configuration but contains no platform-
specific logic. A well-written game can be recompiled for a
different platform by changing only the platform type alias.

**Engine API** — the portable interface. Provides subsystems for
sprites, tiles, sound, input, timing, and static memory pools.
Parameterized by platform type. Uses `if constexpr` on capability
queries to select behavior. Contains no hardware addresses, no
register names, no platform-specific types.

**Capabilities** — compile-time declarations of what a platform
can and cannot do. Expressed as `static constexpr` members on
a capabilities struct. The Engine API queries these to make
decisions. Capabilities describe features (has_hardware_sprites,
sound_voices, has_blitter), not platform identity (is_atari,
is_c64).

**Platform HAL** — the hardware abstraction layer. One
implementation per platform family (Atari, C64, Apple II).
Provides concrete functions for register access, interrupt
installation, display construction, DMA configuration. The
Engine API calls the HAL through the platform type — static
dispatch via templates, never virtual functions.

### Why Capabilities Are Separate From the HAL

The HAL knows *how* to do things on specific hardware. The
Capabilities know *what* the hardware can do. The Engine API
only looks at Capabilities to decide *which* code path to take,
then calls the HAL to execute it.

This separation matters because:

- The Engine API never needs to include platform headers
- Adding a new platform means writing a HAL + a capability
  profile, not modifying the engine
- Capability queries are `constexpr` — the compiler eliminates
  dead paths entirely, so unused platform code generates zero
  bytes in the binary

## Platform Configuration

A platform is fully described by a type that composes a machine
identity with independent hardware extension axes:

```cpp
using MyPlatform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Rambo256,
    atari::Graphics::Baseline,
    atari::Sound::Stereo,
    atari::TV::NTSC,
    atari::Network::Fujinet     // optional, default: None
>;
```

This type carries the capability profile and the HAL
implementation. The engine is instantiated against it:

```cpp
using Game = engine::Core<MyPlatform, GameConfig>;
```

From this point forward, every engine subsystem knows exactly
what hardware is available, how much memory exists, how many
voices the sound system has, and whether a blitter is present.
All of this is resolved at compile time.

### Capability Profile Structure

Each platform type exposes a nested `capabilities` type with
`static constexpr` members. The engine queries these by name,
never by platform identity:

```cpp
// Engine code — platform-agnostic
template<typename Platform>
void render_sprites(...) {
    using caps = typename Platform::capabilities;

    if constexpr (caps::has_blitter) {
        Platform::hal::blit_sprite(...);
    } else if constexpr (caps::has_hardware_sprites) {
        Platform::hal::set_pm_graphics(...);
    } else {
        software_sprite_render(...);
    }
}
```

### Capability Categories

Capabilities are organized by subsystem:

**Graphics capabilities:**

- `has_hardware_sprites` — P/M graphics, VIC-II sprites, etc.
- `max_hardware_sprites` — count of hardware sprite objects
- `has_missiles` — separate missile objects (Atari-specific)
- `max_missiles` — count of missile objects
- `has_hardware_scroll` — hardware fine/coarse scroll support
- `has_blitter` — hardware blitter (VBXE)
- `has_display_list` — programmable display (ANTIC)
- `has_raster_interrupt` — mid-frame interrupt (DLI, raster IRQ)
- `sprite_width_options` — supported sprite widths
- `sprite_multiplexing` — whether engine should auto-multiplex

Bitmap mode support is intrinsic to ANTIC and does not require
a separate capability flag for Atari targets. When future
platforms are added, bitmap capabilities may be needed to
distinguish platforms with fundamentally different pixel modes.

**Sound capabilities:**

- `has_dedicated_sound` — dedicated sound chip present
- `sound_voices` — number of independent voices
- `has_stereo` — stereo output available
- `has_noise_generator` — hardware noise channel
- `has_filter` — hardware filter (SID, POKEY high-pass)
- `has_extended_sound` — PokeyMax or equivalent present
- `extended_sound_voices` — additional voice count

**Memory capabilities:**

- `main_ram_bytes` — conventional RAM available to game
- `has_extended_ram` — bank-switched memory present
- `extended_ram_bytes` — total extended RAM
- `extended_bank_size` — size of each switchable bank
- `zp_available` — zero page bytes available to engine + game

**Input capabilities:**

- `joystick_ports` — number of joystick inputs
- `has_keyboard` — keyboard input available
- `has_paddle` — analog paddle input

**Timing capabilities:**

- `cpu_frequency` — CPU clock in Hz
- `frames_per_second` — display refresh rate
- `cycles_per_frame` — total CPU cycles per frame
- `has_vbi` — vertical blank interrupt
- `has_raster_interrupt` — (shared with graphics)

**Network capabilities:**

- `has_network` — any network hardware present (true if either lane is enabled)
- `has_network_realtime` — fixed-packet realtime lane available
- `has_network_session` — framed byte-stream session lane available
- `network_realtime_transport` — transport kind for the realtime lane (UDP, TCP, Serial, None)
- `network_session_transport` — transport kind for the session lane
- `network_realtime_max_payload` — maximum bytes per realtime send
- `network_session_max_message` — maximum bytes per session message
- `network_session_reliable` — whether session transport guarantees delivery
- `network_latency_ms` — approximate round-trip latency hint

Compatibility aliases (older code paths): `network_transport`, `network_reliable`,
`network_max_payload`, `network_latency_ms`.

`Network::None` sets all flags false/zero. `Game::net` is compile-time absent on
these platforms, adding no storage to `Core`. `Network::Fujinet` enables both lanes.

## Engine Subsystems

The Engine API is composed of subsystems. Each subsystem is a
template parameterized by the platform type and optionally by
game configuration values. Subsystems are not independent
objects — they are facets of the single `engine::Core<Platform,
GameConfig>` instance, sharing the same platform type and
configuration.

```
┌────────── engine::Core<Platform, GameConfig> ────────┐
│                                                       │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌─────────┐ │
│  │  Sprites  │ │  Tiles   │ │  Sound   │ │  Input  │ │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬────┘ │
│       │            │            │             │       │
│  ┌────┴─────┐ ┌────┴─────┐     │        ┌────┴────┐ │
│  │Multiplex │ │ Scroll   │     │        │ Network │ │
│  └────┬─────┘ └────┬─────┘     │        └────┬────┘ │
│       │            │            │             │       │
│  ┌────┴────────────┴────────────┴─────────────┴────┐ │
│  │              Interrupt Manager                   │ │
│  │           (raster-hook chain, frame dispatch)              │ │
│  └─────────────────────┬───────────────────────────┘ │
│                        │                              │
│  ┌─────────────────────┴───────────────────────────┐ │
│  │              Screen Manager                      │ │
│  │   (display lists, screen transitions, regions)   │ │
│  └─────────────────────┬───────────────────────────┘ │
│                        │                              │
│  ┌─────────────────────┴───────────────────────────┐ │
│  │              Bitmap Drawing (gfx)                │ │
│  │     (plot, hline, blit — bitmap regions only)    │ │
│  └─────────────────────────────────────────────────┘ │
│                                                       │
│  ┌─────────────────────────────────────────────────┐ │
│  │                  Game Loop                       │ │
│  │       (frame sync, phase dispatch, run_until)    │ │
│  └─────────────────────────────────────────────────┘ │
│                                                       │
│  ┌─────────────────────────────────────────────────┐ │
│  │             Static Pools (engine::Pool)          │ │
│  │          (used by all subsystems above)          │ │
│  └─────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────┘
```

### Subsystem Descriptions

**Static Pools** — `SlotPool<T,N>` and `PackedPool<T,N>`.
Fixed-size object pools with compile-time capacity. SlotPool
provides stable indices (required for sprite-to-hardware
mapping). PackedPool provides dense iteration (optimal for
iteration-only collections). No dynamic allocation. See
API_DESIGN.md for full specification.

**Game Loop** — owns the main loop and frame synchronization.
Waits for vertical blank, dispatches update and render phases,
manages frame timing. The game provides a callback via `run()`
(never returns) or `run_until()` (returns when callback signals
exit). The loop handles everything else.

**Screen Manager** — owns display list construction and screen
transitions. Uses two-phase display list construction (see
DECISIONS.md ADR-026): at compile time, builds a display list
template from the `DisplayLayout` (mode bytes, blank lines,
region structure) with placeholder LMS addresses and a table
of LMS byte positions; at `set_screen` time, copies the
template into RAM and patches LMS addresses to point into the
shared screen buffer at the correct offsets.

Each screen has its own display list in RAM (30-200 bytes
depending on complexity). All screens share a single screen
memory buffer sized to the largest screen's requirements.

A `DisplayLayout` may also contain `OverlayRegion`s (VBXE): their
pixels live in VBXE VRAM, so they cost no screen-buffer RAM and
emit blank lines (not mode lines) in the ANTIC display list. A
*pure-overlay* layout (every region is an `OverlayRegion`)
collapses the ANTIC display list to a 3-byte JVB stub; the VBXE
overlay drives the display through its own XDL, and the screen
manager leaves ANTIC DMA off (step 12 below is skipped) so the
playfield never contends the VRAM bus with the blitter. On a
blitter backend the logical sprites are composited into VRAM, but
the four P/M *missiles* remain the GTIA hardware projectiles
(ADR-025/ADR-029) — a shared `commit_missiles()` writes the P/M
strip on both backends, so they render below the overlay. (The
CPU's MEMAC VRAM window must be placed clear of the soft stack —
ADR-030 / CONSTRAINTS.md.) The
`set_screen<S>()` transition sequence:

1. Wait for the frame service
2. Disable ANTIC DMA (screen goes blank)
3. Clear shared screen buffer
4. Copy display list template, patch LMS addresses
5. Set ANTIC DLISTL/H to the new display list
6. Update region view pointers
7. Clear non-persistent raster hooks, rebuild the raster-hook chain
8. Enable/disable P/M DMA per sprites_active
9. Set P/M resolution per pm_resolution (DMACTL bit)
10. Enable/disable scroll per scroll_active
11. Generate row address table if use_row_table
12. Re-enable ANTIC DMA (skipped for a pure-overlay screen — ANTIC stays off)
13. Call user transition callback
14. Resume frame loop

Region views are typed handles (`TextRegionView`,
`BitmapRegionView`) that store a pointer into the shared
buffer and expose only operations valid for their mode type.
Invalid operations (e.g., `plot()` on a text region) are
compile errors.

**Bitmap Drawing (gfx)** — provides pixel-level drawing
primitives for bitmap display regions. Available only on
`BitmapRegion` types. Core primitives: `plot`, `point`,
`clear`, `hline`, `blit`. Supports opt-in row address lookup
tables for fast pixel addressing. Additional primitives (line,
rect, masked blit) planned for future iterations.

**Interrupt Manager** — owns raster-hook chain construction and
frame dispatch. (On the Atari backend a raster hook is delivered as a
Display List Interrupt and the frame service runs from the Vertical
Blank Interrupt; the engine vocabulary is raster hook / frame hook /
frame service, and the Atari delivery terms appear below where they
describe the hardware mechanism.) Uses a two-tier raster-hook model
(see DECISIONS.md ADR-019): engine-internal raster hooks (multiplex,
scroll) are raw handlers with minimal overhead (~20-30 cycles); user
C++ raster hooks go through a dispatcher with automatic save/restore
(~80 cycles overhead); user raw hooks bypass the dispatcher entirely.
All hook types coexist in a single chain, sorted by scanline, with
priority ordering: engine multiplex first, engine scroll second, user
handlers third.

The chain is stored as a table of raster-hook slots (4 bytes each:
scanline, flags, handler pointer) with parallel handler and
next-pointer tables for fast indexed access during interrupts.
Static hooks (registered at screen setup) and dynamic hooks
(rebuilt per-frame by the multiplexer) share the single table.
The frame service merges and sorts them, sets the per-line DLI bits
in the display list, and points VDSLST at the first handler.

When `set_screen<S>()` is called, all non-persistent user hooks
are cleared and the engine rebuilds the chain based on the new
screen's configuration. Raster-hook priority order applies per-screen.

C++ raster-hook handlers must be non-capturing lambdas or plain
function pointers (see DECISIONS.md ADR-020). The dispatcher
uses a self-modifying JSR for indirect handler calls (see
DECISIONS.md ADR-021).

The interrupt manager requires 2 bytes of zero page (chain
index and scratch). MaxDLIs and MaxVBIHooks are template
parameters on the InterruptManager, not GameConfig fields.

**Input** — captures joystick and keyboard state once per frame
during the frame service. Provides level (held) and edge (pressed/released)
detection. Debouncing handled internally. Game code receives
a snapshot, never polls hardware directly.

**Sprites** — manages logical sprites mapped to hardware P/M
resources. Each logical sprite has position (x, y), shape
pointer, height, and active flag (6 bytes per sprite). The
game writes logical sprite state during its render phase;
the engine commits to P/M hardware during the frame service.

P/M memory management uses per-sprite exact-extent clearing
(see DECISIONS.md ADR-022): each frame the commit erases only
the bytes each sprite actually wrote last frame (its strip,
Y, and height), then redraws. This replaced an earlier
per-player min/max "dirty range" clear, which degenerated to
the whole strip under multiplexing (a player holds several
sprites spread down the screen, so their min/max span covers
the gaps too) and pushed the commit past one frame.

P/M resolution (single-line or double-line) is per-screen
(see ADR-023). Single-line: 1-scanline precision, 256 bytes
per player, 1792 bytes total. Double-line: 2-scanline steps,
128 bytes per player, 896 bytes total. The P/M memory block
is allocated at the maximum size needed by any screen.

**Multiplex** — a sub-component of Sprites, active when
logical sprite count exceeds 4 hardware players. Divides the
screen into vertical zones (default max 4 zones = 16 sprites).
Zone assignment algorithm:

1. Sort all active sprites by Y position using insertion sort
   every frame (see ADR-024 — nearly-sorted data makes this
   ~80-120 cycles for typical sprite counts)
2. Walk sorted list, greedily assign to zones: each zone holds
   up to 4 sprites, zone boundary placed at the midpoint
   between adjacent groups
3. Validate no sprite straddles a zone boundary (boundary must
   fall in a gap between sprites)
4. Build per-zone data: player assignments and HPOS values

Zone raster-hook handlers are engine-internal raw handlers
(`edge_multiplex_dli`, ~150 cycles): save A/X/Y, copy the
zone's eight pre-baked bytes (HPOSP0-3 + COLPM0-3) from a flat
table to the GTIA registers, then JMP into the C++ dispatcher's
chain tail to advance the index and re-point VDSLST. The flat
table and fire index are built every frame by the multiplexer;
these dynamic hooks are registered RAW (no $80-$9F save) and
stay well under one mode line so closely-spaced boundaries don't
re-enter. Colours are written here (not in the VBI commit) for
zones 1+ because the DLI runs *after* the OS PCOLR→COLPM copy;
zone 0's colour goes through the PCOLR shadow in the commit
instead (it runs before that copy). The boundary scanline is
the gap midpoint biased up by one mode line so the switch lands
between sprites, not mid-sprite.

Collision reverse-mapping: GTIA collision registers accumulate
across the whole frame and don't distinguish zones. The
multiplex provides a candidates table — for each hardware
player, a bitmask of which logical sprites were assigned to it
across all zones. The game uses bounding-box overlap checks to
disambiguate. For non-multiplexed games (≤ 4 sprites), each
player maps to exactly one sprite with no ambiguity.

**Direct binding** — an opt-in alternative to multiplexing,
selected per game with `GameConfig::sprite_binding =
SpriteBinding::Direct` (default is `Multiplexed`; requires
`max_sprites ≤ 4`). It pins logical slot *i* to hardware player
*i* for the whole frame: `update_zones()` builds a single zone
with `player_assignment[p] = p` and **does not sort by Y or
reassign players**, so `build_raster_hooks()` installs no
boundary hooks and `commit_pm()` runs unchanged. The reason it
exists: even at ≤ 4 sprites the multiplexer's single zone still
re-derives the slot→player assignment from the per-frame Y-sort,
so when two sprites cross in Y the assignment swaps — and because
zone 0's colour goes through the PCOLR shadow (a frame behind the
shape), the two sprites' colours flip for one frame. Games where
sprites routinely cross in Y and must keep a stable player/colour
(e.g. a chasing second tank) use direct binding to avoid that;
the trade-off is no multiplexing, so the slot count is hard-capped
at the four hardware players.

Missiles are not multiplexed in the initial implementation
(see ADR-025). The 4 hardware missiles are available directly.
ZoneInfo reserves space for future missile multiplexing.

**Tiles** — supports character-mode or map-mode backgrounds. It
has three independent parts: a *tileset* asset (`TilesetData`, the
tile bitmap definitions, ROM-resident); a *tile map* (`TileMap`, a
row-major grid of map cells holding one tile code each, owned by the
game in ROM or RAM); and the *`TileDisplay`* coordinator, which
copies a tileset into character-set RAM, binds the character-set
base, and tracks the viewport position. `TileDisplay` owns no map
data and performs no map-cell lookup — the game reads the map via
`TileMap::tile_at()`. Works with Scroll for scrolling backgrounds.
Operations are scoped to TextRegion types in mixed displays. Map
*chunk* loading/residency is not part of this subsystem.

#### Tile terminology

These terms are used consistently across the engine, documentation,
demos, and tests (see ADR-034 in [DECISIONS.md](DECISIONS.md)):

- **Glyph** — one bitmap pattern used to render a character-sized
  element. On Atari ANTIC character modes a glyph is normally an
  8-byte character definition.
- **Tileset** — a collection of graphical tile definitions
  (`TilesetData`). The platform representation in ANTIC character
  modes is a *character set* / *charset*.
- **Charset** — used for operations and storage tied specifically to
  character-set RAM or the character-base hardware (e.g.
  `init_charset`, `bind_charset_page`, `Charset1K`).
- **Tile** — one character-sized visual element that may be placed in
  a tile map (nominally 8×8 for ANTIC 4).
- **Tile code** — the stored byte that selects a tile from the active
  tileset (for ANTIC 4 it may also encode colour behaviour).
- **Map cell** — one location in a tile map. A map cell holds one
  tile code; the rendered result is a tile.
- **Tile map** — a row-major 2D grid of map cells (`TileMap`). May
  reside in ROM (constexpr) or RAM.
- **Map chunk** — a fixed rectangular subdivision of a tile map used
  as an asset-loading / management unit (e.g. 40×24 cells). Reserved
  for future functionality (`MapChunk`, `ChunkGrid`, `ChunkLoader`,
  `ChunkManager`, `ChunkCache`); **not implemented** today.
- **Chunk grid** — the arrangement of map chunks over a tile map
  (future).
- **Viewport** — the moving visible window into a tile map,
  independent of map-chunk boundaries; may overlap several chunks.
- **Playfield** — the complete tile-mapped gameplay area (use
  *world* for gameplay coordinates).
- **Screen** — a display configuration or game screen (`ScreenSet`,
  `set_screen`). Not used as the name of a stored map chunk.

**Scroll** — manages hardware fine and coarse scrolling. The
`ScrollManager` is portable: it owns the viewport position and
the fine/coarse split, writes the fine-scroll registers through
`Platform::hal`, and exposes `coarse_col()`/`coarse_row()`. The
backend display program owns the load-address (coarse) patching —
one address per visible line of a `ScrollRegion`, repointed each
frame. Hardware conventions (per-axis fine-scroll inversion, fetch
width) come from the display traits, so the generic layer names no
backend specifics. The frame service applies scroll and keeps the
Tiles viewport in sync; coarse scroll is clamped to the map edges.

**Sound** — manages POKEY voice allocation and sound effect /
music playback. Sound data is `constexpr` ROM tables.
Processing happens during the frame service (envelope advancement, frequency
updates). Capability-gated for stereo POKEY and PokeyMax
extended voices.

**Network** — manages multiplayer connectivity via Fujinet or
equivalent hardware. Uses a state-sharing model: one machine
is the authoritative host running the full simulation, clients
send input and receive state snapshots. The game defines its
own state and input message structs; the engine handles
transport, framing, connection management, and polling.
Network I/O is polled once per frame during the main game
loop (not during the frame service — SIO transactions are too long for
interrupt context). Capability-gated: games compiled without
a network axis have no `Game::net` and incur zero cost.

The transport splits into two lanes (see ADR-032, ADR-033). The **session
lane** (`Game::net.session`) is framed, reliable TCP over fujinet-lib/CIO and
may stall a few ms; it is optionally wired to real fujinet-lib at configure
time. The **realtime lane** (`Game::net.realtime`) is an EDGE-owned FujiNet
Netstream assembly path (no fujinet-lib, no per-byte CIO) that moves
**fixed 16-byte packets** through interrupt-driven POKEY serial rings; the
adapter adds **no wire framing** (boundaries are implicit, every 16 bytes, so the
consumer reassembles units from the byte stream).
Implementation status: realtime lane wired and validated against the
fujinet-pc emulator stack (NetSIO + Altirra + Docker UDP peer, Mode B); **not
yet validated on physical FujiNet hardware**. The `edge_net_realtime_meter` demo
(public API only) plus the `tools/net/edge_realtime_peer.py` host peer exercise and
measure the lane end to end.

## Data Flow Per Frame

A single frame follows this sequence:

```
┌─ VERTICAL BLANK (deferred VBI) ───────────┐
│                                            │
│  1. VBI fires                              │
│  2. Input: capture joystick/keyboard state │
│  3. Sound: advance envelopes, write POKEY  │
│  4. Sprites: write P/M positions for next  │
│     frame (from previous frame's render)   │
│  5. Collision: latch GTIA collision regs,  │
│     clear for next frame                   │
│  6. Multiplex: sort sprites by Y, compute  │
│     zone boundaries, build dynamic DLI     │
│     entries for this frame                 │
│  7. Interrupts: merge static + dynamic DLI │
│     entries sorted by scanline, build      │
│     handler/next tables, set DLI bits in   │
│     display list, set VDSLST to first      │
│     handler, reset chain index to 0        │
│  8. Frame hooks: run any user-provided     │
│     frame-service work (if installed)      │
│                                            │
├─ VISIBLE FRAME ───────────────────────────┤
│                                            │
│  9. Game callback runs:                    │
│     a. Read input snapshot                 │
│     b. Update game state                   │
│     c. Check collisions (from latched data)│
│     d. Render: update sprite positions,    │
│        tile changes, scroll offsets,       │
│        bitmap drawing                      │
│        (writes to buffers, not hardware)   │
│     e. Network: poll if multiplayer active  │
│                                            │
│ 10. DLIs fire as ANTIC draws:              │
│     - Engine raw DLIs: multiplex zone      │
│       boundaries (~20-30 cycle overhead),  │
│       scroll register updates              │
│     - User C++ DLIs: through dispatcher    │
│       (~80 cycle overhead, auto save/      │
│       restore)                             │
│     - User raw DLIs: direct handler,       │
│       zero engine overhead                 │
│     Each handler chains to the next via    │
│     VDSLST. Chain index advances per DLI.  │
│                                            │
├─ VERTICAL BLANK (next frame) ─────────────┤
│                                            │
│ 11. VBI fires — cycle repeats              │
│     Buffered sprite positions from step 9d │
│     are written to hardware in step 4      │
│                                            │
└────────────────────────────────────────────┘
```

Key insight: the game's render phase writes to buffers during
the visible frame. The frame service commits those buffers to hardware.
This one-frame latency avoids tearing and racing with ANTIC
DMA. The game author does not need to think about this — the
API handles the buffering internally.

The deferred VBI is **re-entry-guarded** (see DECISIONS.md
ADR-028). It runs as an NMI; a heavy service (e.g. the 9-sprite
multiplexer) can overrun a frame, and the next frame's VBI NMI
would then re-enter the trampoline mid-flight, corrupting the
saved llvm-mos soft-stack pointer ($80/$81) on unwind — a slow
drift that eventually walks the stack into code and crashes. A
busy flag in the trampoline turns a re-entrant VBI into a no-op
(straight to XITVBV), skipping one service rather than
corrupting state. The service also gates DLI NMIs off
(NMIEN) for its duration and re-arms them once the raster chain
is rebuilt, so a boundary DLI can't fire while the chain tables
are half-written.

## User Extension Points

The engine is designed to be extended. User code can integrate
assembly language routines and take direct control of hardware
resources at well-defined seams.

### 1. Raster Hook Registration

The interrupt manager owns the raster-hook chain. Users can register
handlers in two ways:

**C++ handler (engine-managed):**

```cpp
Game::interrupts.add_raster_hook(scanline, [](auto& ctx) {
    ctx.set_playfield_color<0>(0x2A);
});
```

The engine wraps the callback with register save/restore and
automatic chaining to the next raster-hook handler.

**Raw assembly handler (user-managed):**

```cpp
extern "C" void my_custom_dli();  // defined in .s file
Game::interrupts.add_raw_raster_hook(scanline, my_custom_dli);
```

The user is responsible for:

- Saving and restoring A/X/Y registers
- Writing the next DLI vector address to VDSLST
- Staying within the cycle budget (typically 50-100 cycles)
- Executing RTI to return control to ANTIC

The engine documents the interrupt context in MEMORY_MAP.md,
including the raster-hook chain next-handler vector address and
available stack space.

### 2. Frame Hook Slots

After the engine completes its frame-service work (input capture, sound
envelope processing, P/M register commits), it calls any
registered user frame hooks:

```cpp
extern "C" void my_vbi_work();
Game::interrupts.add_frame_hook(my_vbi_work);
```

The user hook runs in the deferred VBI (after OS processing).
Available cycle budget is documented and depends on the graphics
mode and engine housekeeping load.

The user hook must:

- Preserve A/X/Y (or use them temporarily and restore)
- Not install a new VBI vector (the engine owns the chain)
- Fit within the documented cycle budget

### 3. Zero Page Allocation

The game configuration reserves zero page bytes for user
assembly:

```cpp
struct GameConfig {
    static constexpr uint8_t user_zp_bytes = 8;
    // ...other config...
};
```

The engine's ZP allocator ensures:

- OS-reserved bytes are not touched
- Engine-reserved bytes are not touched
- User-reserved bytes are contiguous and documented
- Remaining ZP is free

The user can query the base address of their ZP region:

```cpp
constexpr uint8_t my_zp_base = Game::user_zp_base;
```

This symbol is available in both C++ and assembly. The user
can safely use `my_zp_base` through
`my_zp_base + user_zp_bytes - 1`.

### 4. Render Phase Hooks

The render phase has defined entry and exit points where user
code can inject work:

```cpp
// Before engine commits buffered sprite positions to P/M RAM
Game::hooks.pre_sprite_commit(my_sprite_modifier);

// After engine finishes all rendering, before frame ends
Game::hooks.post_render(my_custom_effect);
```

These hooks run in the main thread during the visible frame,
so they have access to the full frame cycle budget. They are
useful for effects that don't fit in a raster hook or need to
coordinate with the game logic.

### 5. Resource Release and Reclamation

Rather than conflict with the engine over hardware resources,
the user can explicitly release a resource and take ownership:

```cpp
// Release sound channel 3 — engine stops managing it
Game::sound.release_channel(3);
// Now the user can write directly to POKEY channel 3 registers

// Release Player 2 — engine won't multiplex onto it
Game::sprites.release_player(2);
// Now the user can manage Player 2 directly

// Suspend scroll management for this frame
Game::scroll.suspend();
// User can write scroll registers directly

// Later, reclaim resources
Game::sound.reclaim_channel(3);
Game::sprites.reclaim_player(2);
Game::scroll.resume();
```

After release, the engine will not touch that resource. The
user owns it entirely. This is the cooperative pattern — the
engine and user code coexist by explicitly dividing ownership.

### 6. Direct Hardware Access

The engine does not lock out hardware registers. You can write
to COLPF0, AUDF1, or any other register at any time.

If you do, you accept these consequences:

- Writes to GTIA color registers will be overwritten by the
  next raster hook or frame service that touches them, unless you've released
  that subsystem.
- Writes to POKEY audio registers will conflict with the sound
  subsystem. Call `Game::sound.release_channel(N)` first.
- Writes to ANTIC scroll registers will conflict with the scroll
  subsystem if active. Call `Game::scroll.suspend()` first.
- Writes to P/M position registers will be overwritten at next
  frame-service commit, unless you've released that player.

The engine provides register addresses as constexpr values so
assembly code doesn't need to hardcode magic numbers:

```cpp
#include <engine/platform/atari/registers.h>
// atari::reg::COLPF0, atari::reg::AUDF1, etc.
```

## Why No Scanline-Sync Hook?

You might want to write: "run this code at scanline 100." The
naive implementation is polling VCOUNT in a loop:

```cpp
while (read_vcount() < 100);
do_effect();
```

This works but wastes cycles. On a 6502 running at 1.79 MHz,
waiting for a specific scanline can burn hundreds of cycles per
frame, killing your frame budget.

Instead, use one of these patterns:

**Pattern 1: Raster Hook (recommended)**

If your effect needs scanline precision, write a raster-hook handler.
If you're out of raster-hook slots, the multiplexer is consuming them,
which means you're already pushing the engine hard. Consider
whether the effect justifies the complexity.

```cpp
Game::interrupts.add_raster_hook(100, [](auto& ctx) {
    ctx.set_playfield_color<0>(0x2A);  // effect
});
```

**Pattern 2: Main-Thread Effect (often better)**

If your effect doesn't need precise scanline placement, move it
into the game's update/render phase. Simpler, no cycle stress,
no interrupt context switching:

```cpp
void update(...) {
    // ...game logic...
    my_effect();  // runs in main thread
}
```

**Pattern 3: POKEY Timer (rare)**

If you truly need a timer interrupt separate from the frame service or raster hooks, use
POKEY's timer and poll it in a frame hook. This requires you to
manage POKEY timer state yourself.

The engine provides the tools for patterns 1 and 2, which cover
99% of real cases. Pattern 3 is available but requires manual
POKEY management.

## File Organization

```
engine/
├── core.h              # engine::Core — master template
├── pool.h              # SlotPool, PackedPool
├── input.h             # Input snapshot and edge detection
├── sound.h             # Sound subsystem (portable interface)
├── sprites.h           # Logical sprite management
├── tiles.h             # Tileset asset, tile map, and TileDisplay
├── scroll.h            # Scroll management
├── screen.h            # Screen manager, set_screen, ScreenSet
├── display.h           # DisplayLayout, TextRegion, BitmapRegion
├── gfx.h               # Bitmap drawing primitives
├── interrupt.h         # Interrupt manager (raster-hook / frame dispatch)
├── loop.h              # Game loop, run, run_until
├── hooks.h             # User extension hooks
├── net.h               # Network subsystem (state sharing)
├── math.h              # Fixed-point, lookup tables, fast math
├── types.h             # Common types, bit_mask[], utility
│
├── platform/
│   ├── atari/
│   │   ├── platform.h  # atari::Platform<Machine,RAM,Gfx,Snd,TV,Net>
│   │   ├── hal.h       # Hardware register access, DMA setup
│   │   ├── antic.h     # ANTIC register and display list types
│   │   ├── gtia.h      # GTIA register types
│   │   ├── pokey.h     # POKEY register and sound types
│   │   ├── pia.h       # PIA (joystick/port) types
│   │   ├── pm.h        # Player-Missile graphics HAL
│   │   ├── vbxe.h      # VBXE blitter and overlay HAL
│   │   ├── pokeymax.h  # PokeyMax extended sound HAL
│   │   ├── fujinet.h   # Fujinet network transport HAL
│   │   └── registers.h # Register address constants
│   │
│   └── (future: c64/, apple2/)
│
└── config/
    └── capabilities.h  # Capability trait definitions
```

## Dependency Rules

These rules are enforced by convention and verified by code
review. Breaking them means the architecture is degrading.

1. **Game code includes only `engine/*.h`** — never
   `engine/platform/*`. If game code needs a platform header,
   the engine API is missing an abstraction.

2. **Engine headers include `platform/*` only through the
   Platform template parameter** — never by name. No
   `#include "atari/hal.h"` in engine code. The platform
   type brings its own HAL.

3. **Capability queries use feature names, not platform
   identity** — `if constexpr (caps::has_blitter)`, never
   `if constexpr (is_atari_vbxe)`.

4. **No lateral dependencies between platform families** —
   `atari/` never references `c64/`. Shared concepts belong
   in `engine/` or `config/`.

5. **Subsystems depend downward only** — Sprites may use
   Pools and Interrupts. Sprites never reference Sound.
   The dependency order is:

   ```
   types / math (no dependencies)
     └─ pool (depends on types)
         └─ input, sound, network (depend on pool, types)
         └─ interrupt (depends on types)
             └─ screen (depends on interrupt, types)
                 └─ sprites, tiles, scroll, gfx (depend on
                    interrupt, screen, pool, types)
                     └─ core (depends on everything above)
                         └─ loop (depends on core)
   ```

6. **User assembly integrates through defined seams** —
   raw hardware access is permitted but documented as
   "you own the consequences." Cooperative resource release
   is the preferred pattern for coexistence.

7. **Platform HAL files depend only on hardware documentation**
   — they are thin wrappers over registers and DMA. They do
   not contain game logic or engine policy.

## Extension Points

### Adding a New Platform

1. Create `platform/newplatform/` directory
2. Implement `platform.h` with the `Platform<...>` template
   exposing a `capabilities` type and a `hal` type
3. Implement HAL functions matching the interface the engine
   expects (sprite positioning, sound register writes,
   interrupt installation, etc.)
4. The engine compiles against it with zero modifications

### Adding a New Hardware Extension (e.g., new Atari add-on)

1. Add a new axis value (e.g., `atari::NewHardware::Present`)
2. Add capability entries to the capability profile
3. Add HAL functions for the new hardware
4. Engine subsystems that can benefit add `if constexpr`
   branches — existing code paths are unaffected

### Adding a New Engine Subsystem

1. Create `engine/newsubsystem.h`
2. Template on `Platform` type
3. Query capabilities, call HAL as needed
4. Register interrupt needs with the Interrupt Manager
5. Integrate into `Core` — add as a facet
6. Game code accesses via `Game::newsubsystem.method()`
