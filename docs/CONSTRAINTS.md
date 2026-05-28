# Constraints

Hardware budgets, platform targets, and non-negotiable rules that
every design decision must respect.

## Project Goal

A usable, compact, fast, flexible game engine for 6502-based
computers, implemented in C++ targeting llvm-mos. The engine is
designed to be platform-agnostic at its core, with platform-specific
backends. The initial target is the Atari 8-bit computer family.

Future platforms (Commodore 64, Apple II) are anticipated in the
architecture but not actively developed.

## Target Hardware

### Primary: Atari 8-bit Family

#### Baseline Hardware (400/800/XL/XE)

- CPU: 6502C at 1.79 MHz (NTSC) / 1.77 MHz (PAL)
- RAM: 48KB usable (typical XL/XE configuration)
- Graphics: ANTIC + GTIA
- Sound: POKEY (4 voices)
- Sprites: 4 Players + 4 Missiles (Player-Missile graphics)
- Scrolling: Hardware fine + coarse scroll (both axes)
- Interrupts: DLI (Display List Interrupt) + VBI (Vertical Blank)
- Input: PIA (joystick ports) + POKEY (keyboard scan)

All extensions below are independent axes. A real Atari can have
any combination — VBXE without PokeyMax, Rambo RAM with PokeyMax,
U1MB with VBXE and stereo POKEY, or pure baseline. The engine’s
capability system treats each axis as an independent template
parameter, not a linear tier progression.

#### Memory Extensions (independent axis)

##### 130XE / Rambo XL

- Additional RAM via bank switching (64KB-256KB)
- CPU-side only, not directly visible to ANTIC
- Useful for bulk data storage (maps, tilesets, sound data)
  that gets copied into conventional RAM as needed

##### U1MB (Ultimate 1 Megabyte)

- Up to 1MB RAM via bank switching
- Flash ROM replacement
- CPU-side banking, same ANTIC visibility constraints
- Larger data budgets but same rendering constraints

#### Graphics Extensions (independent axis)

##### VBXE (Video Board XE)

- 512KB dedicated video RAM
- Hardware blitter
- Overlay mode composited over ANTIC output
- Fundamentally different rendering pipeline
- Treated as an alternate rendering backend, not a simple
  extension of ANTIC/GTIA capabilities
- Games targeting VBXE may use a completely different sprite
  and tile rendering path

#### Sound Extensions (independent axis)

##### Stereo POKEY (dual POKEY)

- Two physical POKEY chips, 8 voices total
- Left/right stereo separation (4 voices per side)
- Common community upgrade, widely supported
- Engine treats as doubled voice count with stereo placement

##### PokeyMax (and successors)

- FPGA-based POKEY replacement
- Emulates multiple POKEYs (stereo, quad)
- Additional synthesis modes beyond stock POKEY capabilities
- Treated as an extended sound backend, parallel to how
  VBXE is an extended rendering backend
- Engine exposes additional voices and capabilities through
  the capability system rather than special-casing

#### Platform Configuration Model

Extensions compose as independent template parameters:

```cpp
using MyPlatform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Rambo256,
    atari::Graphics::Baseline,
    atari::Sound::Stereo
>;
```

Any combination is valid. The engine uses `if constexpr` on each
axis independently to select implementations. A game compiled for
baseline graphics + PokeyMax works. VBXE + single POKEY works.
The capability system enforces this orthogonality.

#### Variant Considerations

- NTSC vs PAL: different CPU speed, frame rate (60 vs 50 Hz),
  visible scanline count. Engine must support both.
- 5200: shares ANTIC/GTIA/POKEY but has different I/O mapping.
  Not in initial scope but architecture should not preclude it.

## Memory Rules

### No Dynamic Allocation

`new`, `delete`, `malloc`, `free` are prohibited. All memory is
statically allocated at compile time. Pool sizes, buffer counts,
and sprite limits are template parameters, not runtime values.

### Explicit Section Placement

The engine uses `__attribute__((section(...)))` and llvm-mos
zero-page support to place data explicitly:

- Zero page: reserved for engine hot-path variables (pointers,
  loop counters, interrupt state)
- Conventional RAM: game state, pools, screen buffers
- Extended banks: bulk data (tile sets, map data, sound tables)
- ROM: constant data (sprite shapes, lookup tables, asset data)

### Stack Budget

The 6502 hardware stack is 256 bytes, shared between game code,
engine code, and interrupt handlers. DLI handlers can nest on
top of the game stack. The engine must guarantee bounded stack
usage, and DLI handlers must be minimal (push/pop only what
they modify).

### Zero Page is Managed

Zero page is a scarce resource (~128 usable bytes after OS
reservation). The engine declares its ZP budget. Game code
gets a declared allocation. There is no free-for-all.

## C++ Rules

### Allowed

- Templates (aggressively — this is the primary abstraction tool)
- `constexpr` and `consteval` (compile-time computation is free)
- `if constexpr` (dead branch elimination for capability gating)
- Lambdas (non-capturing, or trivially capturing by value)
- Static dispatch (CRTP or template-based polymorphism)
- `static_assert` (compile-time validation of configurations)
- Fixed-width integer types (`uint8_t`, `int8_t`, `uint16_t`)

### Prohibited

- Exceptions (`throw`, `try`, `catch`)
- RTTI (`dynamic_cast`, `typeid`)
- Virtual functions (vtable pointer costs 2 bytes per object,
  indirect call overhead is severe on 6502)
- `new` / `delete` / heap allocation
- Standard library containers (`std::vector`, `std::string`, etc.)
- Floating point (no FPU; use fixed-point with engine helpers)
- `int` as a data type (ambiguous width; use explicit `uint8_t`
  or `uint16_t`)

### Allowed With Caution

- Lambdas with captures (each capture is stored; keep captures
  to 1-2 bytes, prefer capture-by-value)
- Inline assembly (for cycle-critical interrupt handlers;
  prefer C++ everywhere else)
- Multiple translation units (link-time overhead; prefer
  header-only where practical)

## Timing Budgets

### Frame Budget

- NTSC: 16.67ms per frame (29,780 CPU cycles at 1.79 MHz)
- PAL: 20ms per frame (35,280 CPU cycles at 1.77 MHz)
- Of this, ANTIC steals cycles for DMA (screen, P/M, display
  list). Actual available cycles vary by graphics mode.

### DLI Budget

- A DLI handler must complete before ANTIC needs the bus for
  the next scanline. Practical budget: 50-100 usable cycles
  depending on mode and DMA configuration.
- DLI handlers should be as short as possible: save registers,
  do work, restore registers, set next DLI vector.

### VBI Budget

- Immediate VBI: runs before OS processing. Limited to
  time-critical updates (P/M position writes, color changes).
- Deferred VBI: runs after OS processing. Available for
  game logic, sound updates, input polling.

## Design Philosophy

### Compile-Time Configuration

The engine favors compile-time decisions over runtime flexibility.
Graphics modes, sprite counts, pool sizes, sound channel
allocation, and hardware extension selection are template
parameters. This enables the compiler to eliminate dead code,
pre-compute layouts, and allocate exact memory budgets with no
runtime overhead.

Runtime ergonomics are acceptable only where they cost no more
than a few bytes of RAM and no measurable cycle overhead.

### Honest Cost Exposure

When the platform cannot natively support a requested feature
(e.g., more sprites than hardware provides), the engine does not
silently degrade. It exposes the cost: software sprite rendering
is available but the game author sees it in the type system and
can query the overhead. No hidden magic.

### Independent Capability Axes

Hardware extensions are modeled as orthogonal axes, not linear
tiers. The engine never asks “are we on an upgraded Atari?” —
it asks “does this configuration include hardware blitter
support?” or “how many sound voices are available?” This keeps
the architecture clean as new hardware extensions emerge and
prepares the capability system for future platforms where the
axes will be entirely different.

### Games That Ship

This engine targets games that must fit in real memory budgets
and run at stable frame rates. Tech demos and proof-of-concepts
have different constraints. Every feature must justify its RAM
and ROM cost against the question: “does a shipping game need
this?”