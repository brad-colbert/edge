# Architecture Decision Records

> **Applies to EDGE v0.5.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

Decisions made during design, with rationale. These explain the
"why" behind architectural choices and document tradeoffs.

---

## ADR-001: Bitmap-Based Slot Pools Over Free Lists

**Status:** Accepted

**Context:**
Object pools are fundamental to the engine. Small pools (4-16
slots) are common for game entities. We needed to choose a pool
structure that minimized both memory overhead and iteration cost
on a 6502.

**Options Considered:**

1. **Bitmap pool** (chosen): One byte bitmap per 8 slots. Acquire
   finds first set bit (O(N) worst case, ~40 cycles). Release is
   O(1) bit set. Iteration needs per-slot bitmap test (~8 cycles
   per slot).

2. **Free list pool**: N-byte free list for intrusive linking.
   Acquire/release O(1) but requires separate active-tracking.
   Total overhead: N+1 bytes vs 1 byte for bitmap.

3. **Packed pool**: Dense array with count, swap-on-release.
   Acquire O(1), release is swap (~20 cycles for 4-byte struct).
   Iteration zero overhead. Trade: unstable indices.

**Decision:**
Offer two pool types. `SlotPool` uses bitmap for stable
indices (required for sprite-to-hardware mapping). `PackedPool`
uses dense packing for iteration-heavy collections (particles,
timers, events).

**Rationale:**
Trying to unify both patterns into a single pool type either
wastes memory (free list adds N bytes) or sacrifices features
(can't support both stable indices and dense iteration). Two
types are clearer, each optimized for its use case.

**Tradeoff:**
Game authors must pick the right pool type. Documentation and
naming (`SlotPool` vs `PackedPool`) make the choice clear.

---

## ADR-002: No Zero-Initialization on Pool Acquire

**Status:** Accepted

**Context:**
When a game acquires an object from a pool, should the engine
zero-initialize it?

**Options Considered:**

1. **Zero-initialize on acquire** (rejected): Costs ~4 cycles
   per byte of struct per acquire. For a 4-byte enemy struct,
   that's ~16 cycles. In a game that spawns 60 enemies per
   second, that's ~960 cycles per frame wasted on zeroing.

2. **Don't initialize; caller fills in all fields** (chosen):
   Caller is responsible for initializing. Mirrors `malloc`
   semantics, which C++ developers understand.

**Decision:**
Acquired slots have undefined content. Caller must initialize
every field they use.

**Rationale:**
On a 6502 with a tight frame budget, 960 wasted cycles per
frame is unacceptable. The invariant is simple and well-
understood. C++ developers already expect this from `malloc`.

**Tradeoff:**
Risk of uninitialized-field bugs if the caller forgets.
Mitigation: document it prominently and provide examples
showing proper usage.

---

## ADR-003: Both Pointer and Index Release Overloads

**Status:** Accepted

**Context:**
`SlotPool::release()` should accept what type of argument?

**Options Considered:**

1. **Pointer only**: `release(T* ptr)`. Natural when you have
   the pointer from `acquire()`. Cost: pointer subtraction to
   recover index if needed elsewhere.

2. **Index only**: `release(uint8_t idx)`. Efficient on 6502.
   Cost: game author must track indices separately.

3. **Both overloads** (chosen): `release(T*)` and
   `release(uint8_t)`. Code size cost: ~20 bytes of ROM.
   Ergonomic benefit: major.

**Decision:**
Provide both `release(T*)` and `release(uint8_t)`.

**Rationale:**
Pointer release is natural after `acquire()`. Index release is
natural in collision resolution (hardware slot tells you the
index). Supporting both removes the need for ugly pointer
arithmetic in hot code. 20 bytes of ROM is cheap insurance.

**Tradeoff:**
Slight code size increase. No performance impact (both are
O(1)).

---

## ADR-004: Capabilities as Compile-Time Traits, Not Runtime Flags

**Status:** Accepted

**Context:**
How should the engine adapt to different hardware? Runtime
checks (`if (has_blitter)`) or compile-time decisions
(`if constexpr (caps::has_blitter)`)?

**Options Considered:**

1. **Runtime flags**: Check capability at runtime. Flexible but
   unused code paths remain in the binary. Dead code cannot be
   eliminated. Especially problematic for platforms where
   capability tiers have completely different rendering paths.

2. **Compile-time traits** (chosen): Capabilities as `static
   constexpr` members. The compiler's `if constexpr` eliminates
   dead branches entirely. Unused platform code generates zero
   bytes.

**Decision:**
Capabilities are `static constexpr` members on a platform's
capability profile. The engine queries them via `if constexpr`.

**Rationale:**
On a 6502 with severe ROM constraints, dead code elimination is
essential. A runtime check might add 20 bytes; the wrong code
path adds 500 bytes. Compile-time resolution gives the compiler
perfect information for optimization.

**Tradeoff:**
Less runtime flexibility. A game cannot dynamically detect
hardware and adapt (e.g., "is VBXE present?"). This is
acceptable because:

- The platform is known at compile time. Games target a specific
  platform configuration.
- The user can provide multiple builds targeting different
  hardware.
- Dynamic detection would require the binary to include code for
  all variants anyway.

---

## ADR-005: Independent Hardware Extension Axes Over Linear Tiers

**Status:** Accepted

**Context:**
Atari hardware extensions (VBXE, PokeyMax, U1MB, RAM) are
independent. How should the architecture model this?

**Options Considered:**

1. **Linear tiers**: Tier 0 (baseline), Tier 1 (extended RAM),
   Tier 2 (VBXE). Each tier is a step up with all previous
   capabilities. Problem: a real Atari can have any combination.
   U1MB + VBXE is valid. VBXE + single POKEY is valid. Linear
   tiers force false dependencies.

2. **Independent axes** (chosen): Each extension (RAM, graphics,
   sound) is a separate template parameter. Any combination is
   valid. The platform type composes them.

**Decision:**
Platform configuration uses independent axes:

```cpp
using MyPlatform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Rambo256,
    atari::Graphics::Baseline,
    atari::Sound::Stereo
>;
```

**Rationale:**
Accurately models real hardware. A game compiled for (Baseline
graphics, PokeyMax sound) works. (VBXE, single POKEY) works.
(Baseline everything) works. The capability system treats each
axis orthogonally, and dead code elimination ensures only
reachable code paths are compiled.

**Tradeoff:**
Platform configuration is slightly more verbose (four template
parameters instead of one). The explicitness is worth it — it
forces clarity about what hardware is expected.

---

## ADR-006: User Assembly Integration Via Defined Seams

**Status:** Accepted

**Context:**
Should the engine prevent user assembly, or allow it? If
allowed, how do we avoid the engine and user code fighting
over the same hardware resources?

**Options Considered:**

1. **No assembly allowed**: Pure C++ only. Safe but excludes
   real Atari developers who need cycle-critical or hardware-
   specific routines. A ceiling, not a floor.

2. **Assembly is allowed, users on their own**: No contracts,
   no integration. Results: silent register clobbers, DLI chain
   breakage, timing overruns. Disasters.

3. **Defined seams with cooperative resource release** (chosen):
   Assembly integrates through specific APIs (raster-hook registration,
   frame hooks, ZP reservation, resource release). The engine
   documents the contract at each seam.

**Decision:**
Six integration points:

- Raster-hook registration (C++ or raw assembly)
- Frame-hook registration
- Zero page reservation (compile-time declaration)
- Render phase hooks (pre-commit, post-render)
- Resource release/reclamation (channels, players, scroll)
- Direct hardware access (permitted, consequences documented)

**Rationale:**
Atari development is incomplete without assembly. Rather than
prevent it, provide clean contracts. The resource release
pattern is the key innovation: instead of fighting over a
register, the user tells the engine "I'm taking over channel 3"
and the engine stops touching it. Cooperation.

**Tradeoff:**
Some rope to hang yourself. A determined user can still break
things. This is acceptable because the contracts are explicit,
the user chose to ignore them, and experienced Atari developers
expect this level of control.

---

## ADR-007: Static Dispatch Over Virtual Functions

**Status:** Accepted

**Context:**
How should subsystems use polymorphism (e.g., different sprite
renderers for VBXE vs baseline hardware)?

**Options Considered:**

1. **Virtual functions** (rejected): `virtual void render_sprite()`.
   Vtable pointer costs 2 bytes per object. With 64 sprites on
   screen, that's 128 bytes wasted. Indirect calls also add
   overhead on 6502 (no branch prediction, no pipelining).

2. **Static dispatch via templates** (chosen): `if constexpr`
   selects implementation at compile time. Dead branches
   eliminated entirely. Zero runtime overhead.

3. **Function pointers** (rejected for hot paths): Used sparingly
   (e.g., user assembly entry points) but not for inner loops.

**Decision:**
Use `if constexpr` and templates for polymorphism. No virtual
functions in the engine.

**Rationale:**
On a 6502, every byte counts. Virtual functions waste memory
and cycles. The compiler is smart enough to eliminate dead
branches entirely, so static dispatch is both smaller and
faster than dynamic dispatch.

**Tradeoff:**
Less flexibility at runtime. But the flexibility isn't needed —
the platform and its capabilities are compile-time constants.

---

## ADR-008: Compile-Time Asset Construction Over Runtime Conversion

**Status:** Accepted

**Context:**
Sound effects, sprite shapes, and tilesets are constant data.
When should they be converted to hardware format?

**Options Considered:**

1. **Runtime conversion**: Load asset data, convert to hardware
   format at startup. Flexible. Problem: costs RAM for temporary
   buffers, costs cycles on startup, asset data must be editable
   (ROM not available).

2. **Compile-time construction** (chosen): Use `constexpr`
   functions to build assets in hardware format at compile time.
   Data lives in ROM. Zero startup cost, zero RAM cost.

**Decision:**
Provide `constexpr` asset builders:

- `Game::make_sprite<W,H>(data)`
- `Game::make_charset(data)`
- `Game::make_sound(data)`

These compile into ROM-resident data with no runtime conversion.

**Rationale:**
On a 6502, compile-time work is free (cost is moved to the
compiler, not the target). ROM is cheap; RAM is scarce. Why
burn RAM and cycles converting data at runtime when the compiler
can do it once?

**Tradeoff:**
Assets must be expressed in code (not loaded from disk at
runtime). This is acceptable for a game engine targeting
embedded systems. Asset pipelines are a future concern.

---

## ADR-009: One-Frame Render Buffering for Atomic Sprite Commits

**Status:** Accepted

**Context:**
When should sprite positions be written to P/M RAM? During the
visible frame (risky) or during the frame service (safe)?

**Options Considered:**

1. **Write during visible frame**: Game updates sprite positions,
   writes directly to P/M RAM immediately. Risky: ANTIC may be
   reading P/M RAM while the CPU is writing it, causing visual
   glitches (sprite corruption mid-frame).

2. **Write during VBI** (chosen): Game updates sprite positions
   into a buffer during the visible frame. The frame service commits all
   buffered positions to hardware at once. Safe: atomic update,
   no tearing. One-frame latency.

**Decision:**
Sprite positions are buffered during the game frame and
committed to hardware during the frame service.

**Rationale:**
Atomic commits avoid tearing. The one-frame latency is expected
on Atari (standard practice). Game authors write to the buffer
(no knowledge of hardware timing needed), the engine handles
the commit.

**Tradeoff:**
Input-to-visual latency is two frames minimum (one frame for
game logic, one frame for hardware commit). This is acceptable
and expected on 6502 systems.

---

## ADR-010: Explicit ZP Allocation Over Free-For-All

**Status:** Accepted

**Context:**
Zero page is scarce (~128 usable bytes after OS). Should it be
managed explicitly or left as a free-for-all?

**Options Considered:**

1. **Free-for-all**: Each subsystem and user code grabs whatever
   ZP it wants. Problem: collisions are silent and catastrophic.

2. **Explicit allocation** (chosen): Engine declares its ZP
   usage. Game config declares user ZP needs. Linker script
   places each region. Compiler enforces boundaries.

**Decision:**
ZP allocation is declared at compile time and verified by the
linker.

**Rationale:**
ZP collisions are subtle bugs. Explicit allocation makes
conflicts impossible (linker will error if you request too
much). The game author knows their ZP budget upfront.

**Tradeoff:**
Slightly more boilerplate in the config (declare
`user_zp_bytes`). Worth it to prevent a class of catastrophic
bugs.

---

## ADR-011: No Floating Point, Fixed-Point Instead

**Status:** Accepted

**Context:**
Math on a 6502. Floating point or fixed-point?

**Options Considered:**

1. **Floating point**: IEEE 754 or similar. No FPU on 6502.
   Software FP is very slow and very large (~500+ bytes per
   operation).

2. **Fixed-point** (chosen): Represent numbers as scaled integers
   (e.g., 8.8 fixed-point). Add, subtract, multiply use integer
   ops. Divide and square root use lookup tables.

**Decision:**
The engine provides fixed-point helpers and lookup tables.
Games use fixed-point for any math beyond integers.

**Rationale:**
6502 has no FPU. Fixed-point is the practical middle ground
between "integer only" and "floating point at any cost."

**Tradeoff:**
Game author must understand fixed-point scaling and be careful
with overflow. Mitigation: provide examples and a small math
library.

---

## ADR-012: Header-Only Engine Libraries Over Linked Modules

**Status:** Accepted (with caveats)

**Context:**
Should engine code be header-only or split into translation
units?

**Options Considered:**

1. **Separate translation units**: Faster compilation, smaller
   object files. Problem: link-time overhead on a 6502 build,
   requires careful symbol management.

2. **Header-only** (chosen, with exceptions): Templates are
   header-only by necessity. Most small subsystems are header-
   only. Larger subsystems that need per-platform implementation
   may have a thin .cpp file.

**Decision:**
Prefer header-only. Use `.cpp` files only when necessary for
platform-specific code generation.

**Rationale:**
Header-only avoids link-time overhead. The compiler has full
visibility for optimization. Easier mental model: one concept =
one header.

**Tradeoff:**
Longer compilation times. Mitigation: most games will not
rebuild the engine frequently (it's stable); only game code
changes often.

---

## ADR-013: No Scanline-Sync Hook (Busy-Wait Is Anti-Pattern)

**Status:** Accepted

**Context:**
Should the engine provide an easy way to say "run this code at
scanline N"?

**Options Considered:**

1. **Scanline-sync hook**: `Game::at_scanline(100, callback)`.
   Internally polls VCOUNT until scanline 100. Problem: wastes
   hundreds of cycles per frame polling.

2. **No dedicated hook; redirect to better patterns** (chosen):
   - DLI for scanline-precise effects
   - Main-thread effect for non-precise effects
   - POKEY timer for timed interrupts

**Decision:**
Do not provide an `at_scanline` hook. Document why in
ARCHITECTURE.md with three recommended patterns.

**Rationale:**
The naive `at_scanline` is a footgun. Inexperienced developers
waste cycles and struggle to understand performance problems.
Better to steer toward patterns that scale (DLI, main-thread,
timer).

**Tradeoff:**
Slightly less convenience. Gain: honesty about costs, and game
authors learn the right patterns.

---

## ADR-014: Shared-Buffer Screen Switching Over Per-Screen Allocation

**Status:** Accepted

**Context:**
Games have multiple display states (title, gameplay, high
scores) each with different ANTIC display lists, screen memory
layouts, DLI chains, and active subsystems. How should the
engine manage memory across these states?

**Options Considered:**

1. **Per-screen allocation**: Each screen configuration gets its
   own screen memory. All screens allocated simultaneously.
   Problem: a title screen bitmap (7680 bytes) + gameplay bitmap
   (7200 bytes) + high score text (960 bytes) = 15,840 bytes of
   screen RAM. Wasteful — only one screen is active at a time.

2. **Manual management**: Engine provides no screen concept. Game
   author tears down and rebuilds display lists, DLI chains, and
   screen memory manually on each transition. Works, but this is
   exactly the boilerplate an engine should eliminate.

3. **Shared buffer with compile-time union** (chosen): Engine
   computes the maximum screen RAM across all declared screens.
   Allocates one shared buffer of that size. Each screen's
   display list points into the same buffer. `set_screen<S>()`
   reconfigures the display list, clears memory, and rebuilds
   the DLI chain.

**Decision:**
All screen configurations declared in `ScreenSet<...>` share a
single screen memory buffer sized to the largest screen. Screen
transitions are managed by `Game::set_screen<S>()`.

**Rationale:**
This mirrors what experienced Atari developers do manually —
reuse screen RAM across game states. The engine automates the
bookkeeping while the game author declares configurations and
transitions.

The compile-time `ScreenSet` declaration gives the engine
perfect knowledge of all possible screens, enabling:

- Exact shared buffer sizing (no over-allocation)
- `static_assert` validation of RAM budgets per screen
- Type-safe region access after `set_screen`

**Tradeoffs:**

- Screen memory is cleared on transition. If a game needs to
  preserve screen content across a transition (e.g., pause
  overlay), it must save and restore manually or use a
  dedicated buffer outside the shared pool.
- `set_screen` has a cost of 1-2 frames for display list
  rebuild and memory clear. Not suitable for per-frame
  switching. This is acceptable — screen transitions are
  infrequent events.
- Raster hooks registered with `add_raster_hook` are cleared on screen change.
  Persistent raster hooks require `add_persistent_raster_hook`. This prevents
  stale raster hooks from firing on a screen they weren't
  designed for.

---

## ADR-015: State Sharing Over Input Sharing for Multiplayer

**Status:** Accepted

**Context:**
Multiplayer games need to synchronize across machines. Two
primary models exist: input sharing (every machine runs the
full simulation with shared inputs) and state sharing (one
authoritative host sends state snapshots to clients).

**Options Considered:**

1. **Input sharing**: Each player sends joystick state to all
   others. Every machine runs the full simulation identically.
   Low bandwidth. Problem: requires perfectly deterministic
   game logic. Same inputs must produce same results on every
   machine. Random numbers, frame timing differences, and
   math precision drift cause desync. Debugging desync is
   extremely difficult.

2. **State sharing** (chosen): One machine is the authoritative
   host. It runs the simulation, sends state snapshots to
   clients. Clients send their input to the host and render
   received state. Higher bandwidth but tolerates non-
   determinism. No desync possible — host is the truth.

**Decision:**
The engine uses state sharing. One machine hosts, others are
clients. The host runs the game simulation. Clients are thin
renderers that send input and display received state.

**Rationale:**
State sharing is more robust on a 6502 where random number
generators, timing, and math precision can vary between
machines. Deterministic input sharing is fragile in this
environment.

Fujinet's SIO latency (~1-3ms per transaction) makes frame-
by-frame input sharing impractical anyway. State snapshots
sent every 2-4 frames work within the bandwidth budget.

**Tradeoffs:**

- Higher bandwidth than input sharing. Mitigation: state
  messages are game-defined and can be as compact as needed.
  A 16-byte snapshot every 2 frames is ~480 bytes/sec.
- Visible lag on clients (2-4 frames, 33-66ms at 60 FPS).
  Usually acceptable. Client-side prediction can reduce
  perceived lag.
- Host has more CPU load (simulation + network I/O).
- The game must define and maintain compact state structs.

---

## ADR-016: Polled Network I/O Over Interrupt-Driven

**Status:** Accepted

**Context:**
When should network I/O happen during the frame?

**Options Considered:**

1. **VBI-driven**: Poll network during vertical blank. Problem:
   Fujinet SIO transactions take 1-3ms. VBI budget is already
   contested by sound, input, and sprite commits. A 2ms SIO
   transaction would consume most of the available VBI time.

2. **Interrupt-driven**: Network hardware fires interrupt on
   packet arrival. Problem: Fujinet uses SIO which is polled,
   not interrupt-driven. Would require custom hardware support.

3. **Polled during game loop** (chosen): Game calls
   `Game::net.poll()` once per frame during the main game
   callback. Predictable timing, no VBI budget impact.

**Decision:**
Network I/O is polled by the game during its frame callback.
The game controls when the SIO transaction happens.

**Rationale:**
SIO transactions are too slow for VBI context. Polling gives
the game author explicit control over when network I/O happens
in their frame budget. They can skip polling on frames where
they're tight, or poll early when they have cycles to spare.

**Tradeoff:**
Adds ~1-3ms to the game's frame time on frames that poll.
The game must explicitly call `poll()`. Forgetting to poll
means no network activity. This is acceptable — explicit is
better than hidden cost.

---

## ADR-017: Register Pointers as Inline Const, Not Constexpr

**Status:** Accepted

**Context:**
The engine needs to provide hardware register addresses as
typed pointers (e.g., `*atari::reg::COLPF0 = 0x2A`). The
natural C++ approach is `constexpr volatile uint8_t*`, but
`reinterpret_cast` from integer to pointer is not a constant
expression in standard C++. This was discovered empirically
when `mos-clang++` rejected the constexpr form.

**Options Considered:**

1. **`constexpr` pointer** (rejected): Not legal C++.
   `reinterpret_cast<volatile uint8_t*>(0xD016)` is not a
   constant expression. The compiler rejects it.

2. **`inline volatile uint8_t* const`** (chosen): Statically
   initialized to the absolute address. No `.init_array` or
   runtime initialization overhead. Usage syntax is identical
   to constexpr (`*atari::reg::COLPF0 = value`).

3. **Preprocessor macros** (rejected): `#define COLPF0
   (*(volatile uint8_t*)0xD016)`. Works but loses type safety,
   namespace scoping, and IDE support. Not C++.

**Decision:**
Register pointers are `inline volatile uint8_t* const`.
Each register also gets a `constexpr uint16_t NAME_ADDR`
constant for compile-time address arithmetic.

**Rationale:**
The `inline const` form produces identical code to a constexpr
pointer — the compiler statically embeds the address. The
`constexpr` address constant covers the rare case where
compile-time math on register addresses is needed.

**Impact:**
This applies to every platform HAL, not just Atari. All future
platform register definitions should follow this pattern.

---

## ADR-018: PAL/NTSC as a Sixth Platform Axis

**Status:** Accepted

**Context:**
PAL and NTSC Atari machines differ in CPU frequency, frame
rate, cycles per frame, and visible scanline count. These
differences affect timing budgets throughout the engine. The
original design listed PAL/NTSC under "Variant Considerations"
but did not model it as a platform axis.

**Options Considered:**

1. **Runtime detection**: Read the PAL register at startup and
   set a runtime flag. Flexible. Problem: timing budgets are
   used in compile-time calculations (DLI cycle budgets, frame
   budget validation). Runtime detection defeats `static_assert`
   validation and forces both code paths into the binary.

2. **GameConfig parameter**: Put PAL/NTSC in the game config
   rather than the platform. Problem: it's a property of the
   hardware, not the game. A game doesn't choose PAL or NTSC —
   the machine determines it.

3. **Platform axis** (chosen): Add `atari::TV::NTSC` /
   `atari::TV::PAL` as a sixth template parameter on
   `atari::Platform`. Consistent with the independent-axis
   model. PAL and NTSC builds are separate binaries with
   different timing constants.

**Decision:**
TV standard is a sixth independent axis on the Platform
template:

```cpp
using MyPlatform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::Graphics::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC,
    atari::Network::None
>;
```

**Rationale:**
PAL/NTSC is a hardware property, not a game choice. It affects
timing capabilities that are queried at compile time. Modeling
it as an axis means:

- `static_assert` on frame budgets uses the correct cycle count
- DLI cycle budgets are accurate for the target system
- Dead code elimination removes the wrong timing path entirely
- A developer ships two binaries (PAL and NTSC), which is
  standard practice in Atari development

**Tradeoff:**
The Platform template now has six parameters. This is more
verbose but each parameter is meaningful. Type aliases
(`StockXL_NTSC`, `StockXL_PAL`) mitigate the verbosity.

TV is the 5th parameter (before Network) because it has no
default — you must specify your target system. Network is 6th
with a default of None, since most games don't use it. C++
requires defaulted template parameters to follow non-defaulted
ones.

Developers who want a single binary that detects PAL/NTSC at
runtime can still do so by querying the PAL register and
branching — but they lose compile-time timing validation. The
engine doesn't prevent this; it just doesn't optimize for it.

---

## ADR-019: Two-Tier DLI Dispatch (Engine Raw, User Wrapped)

**Status:** Accepted

**Context:**
DLI handlers on the Atari have tight cycle budgets (50-100
usable cycles depending on the scanline). A general-purpose
dispatcher that saves all registers, looks up the handler in
a table, calls it via self-modifying JSR, chains to the next
handler, restores registers, and executes RTI costs roughly
80 cycles of overhead. That leaves almost nothing for actual
work.

llvm-mos does not support `__attribute__((interrupt))`, so
the compiler cannot generate minimal interrupt prologues
automatically. Manual assembly wrappers are required.

**Options Considered:**

1. **All DLIs through dispatcher**: Uniform, simple. Problem:
   80-cycle overhead makes most DLIs useless. The multiplexer
   needs to reprogram P/M registers in tight scanline windows
   and can't afford the overhead.

2. **All DLIs raw**: No overhead. Problem: every user who wants
   a simple color change must write assembly.

3. **Two-tier dispatch** (chosen): Engine-internal DLIs
   (multiplex, scroll) are raw handlers authored by the engine,
   saving only the registers they actually use (~20-30 cycle
   overhead). User C++ DLIs go through the dispatcher (~80
   cycle overhead). User raw DLIs bypass the dispatcher
   entirely (zero engine overhead).

**Decision:**
Engine DLIs are raw handlers. User C++ DLIs go through a
dispatcher. User raw DLIs are direct.

**Rationale:**
The engine knows exactly which registers its own handlers
touch and can minimize save/restore. User C++ handlers get
the convenience of automatic save/restore at the cost of
~80 cycles — acceptable for color changes and register writes
on generous scanlines. Users who need tighter timing write raw
handlers, which is expected for advanced Atari development.

The cycle overhead (~80 cycles for C++ handlers) is documented
in the API so the user can make an informed choice.

**Tradeoff:**
User C++ DLI handlers have limited useful cycle budget.
Documentation must be clear about the overhead and when to
switch to raw handlers.

---

## ADR-020: Non-Capturing Lambdas Only for C++ Raster-Hook Handlers

**Status:** Accepted

**Context:**
The C++ raster-hook handler API accepts a lambda:

```cpp
Game::interrupts.add_raster_hook(scanline, [](RasterContext& ctx) {
    ctx.set_playfield_color<0>(0x2A);
});
```

Should capturing lambdas be supported?

**Options Considered:**

1. **Allow capturing lambdas**: The raster-hook slot must store the
   capture data alongside the function pointer. For a lambda
   capturing one `u8`, that's 1 extra byte per slot. For
   arbitrary captures, storage is unbounded. The dispatcher
   must pass the capture data to the handler, adding complexity
   and cycles.

2. **Non-capturing only** (chosen): The lambda converts to a
   plain function pointer (`void (*)()`). The raster-hook slot remains
   4 bytes. No capture storage, no capture passing overhead.
   Any data the handler needs must be in static variables.

**Decision:**
C++ raster-hook handlers must be non-capturing lambdas (or plain
function pointers). The RasterContext object provides typed
register write methods. Any runtime data (colors, scroll
values) should be in static variables the handler references.

**Rationale:**
Raster-hook handlers run in interrupt context with brutal cycle
budgets. Every byte of capture storage and every cycle of
capture-passing overhead matters. Non-capturing lambdas are
zero-cost — the compiler converts them to plain function
pointers. Static variables for handler data is the standard
6502 pattern and has zero indirection overhead.

**Tradeoff:**
Slightly less ergonomic — the user can't write
`[color](auto& ctx) { ctx.set_playfield_color<0>(color); }`. They
must use a static variable:

```cpp
static u8 dli_color = 0x2A;
Game::interrupts.add_raster_hook(scanline, [](RasterContext& ctx) {
    ctx.set_playfield_color<0>(dli_color);
});
```

This is a minor inconvenience that avoids a class of memory
and performance problems.

---

## ADR-021: Self-Modifying JSR for DLI Dispatcher

**Status:** Accepted

**Context:**
The DLI dispatcher needs to call a handler whose address is
known only at runtime (looked up from a table). The 6502 does
not have a JSR (indirect) instruction. How does the dispatcher
call the handler?

**Options Considered:**

1. **JMP (indirect)**: 6502 has JMP ($addr). Problem: JMP
   doesn't push a return address, so the handler can't return
   to the dispatcher. Also has the NMOS page-boundary bug.

2. **RTS trick**: Push return_address-1 on the stack, then JMP
   to the handler. Handler executes RTS which pops the return
   address. Works but adds stack manipulation overhead (~10
   extra cycles).

3. **Self-modifying JSR** (chosen): Patch the operand of a JSR
   instruction in RAM before executing it. The dispatcher
   writes the handler address into the two bytes following the
   JSR opcode, then falls through to execute the patched JSR.
   Standard 6502 technique.

**Decision:**
The dispatcher uses self-modifying code to patch a JSR target.
This is safe because all code runs from RAM (no ROM cartridges
in scope — see ADR-018 discussion), and the modification
happens in the dispatcher's own code, not in user code.

**Rationale:**
Self-modifying code is a standard, well-understood 6502
pattern. It's the most cycle-efficient way to do an indirect
subroutine call. The code is in RAM (disk-loaded programs,
not burned ROMs), so modification is safe. The JSR pushes a
proper return address, so the handler returns cleanly with RTS.

**Tradeoff:**
Self-modifying code is harder to reason about than pure code.
The modification is confined to a single well-documented
location in the dispatcher. Future platforms that use ROM-
resident code would need a different approach (RTS trick or
jump table).

---

## ADR-022: Tracked-Range P/M Clear Over Full Clear

**Status:** Accepted

**Context:**
Each frame, the sprite commit phase must clear old sprite data
from player memory before writing new positions. A full clear
zeros all 4 player strips (4 × 256 = 1024 bytes), costing
roughly 5000 cycles. Most of those bytes were already zero.

**Options Considered:**

1. **Full clear** (rejected): Zero all 1024 bytes. Simple but
   wastes ~4000 cycles clearing bytes that were never written.

2. **Tracked-range clear** (chosen): Track the min and max Y
   scanline written for each player last frame. Clear only
   that range. Cost: 8 bytes of bookkeeping (min_y + max_y
   per player). Savings: typically clear ~256 bytes instead
   of 1024, saving ~3750 cycles.

**Decision:**
Track min/max Y per player. Clear only the dirty range.

**Rationale:**
On a 6502, 3750 saved cycles is roughly 12% of the NTSC
frame budget. That's significant. The bookkeeping cost (8
bytes RAM, ~32 cycles to update min/max during write) is
trivial in comparison.

**Tradeoff:**
Slightly more complex commit logic. If a sprite moves from
Y=20 to Y=180 between frames, the clear range spans most of
the strip anyway. But this worst case is no worse than full
clear, and the common case (sprites move by small deltas) is
dramatically better.

**Revision (2026-06-05): per-sprite exact extents, not per-player min/max.**
The min/max-per-player range was refined to a per-sprite exact
footprint clear (the strip, Y, and height each sprite wrote last
frame). The trigger was multiplexing: a single hardware player
holds *several* logical sprites spread down the screen, so its
min/max range spans the whole strip every frame — the "worst case"
above becomes the *common* case, and the commit overran one frame
(hardware-confirmed on the 9-sprite multiplex demo, which then
stuttered as the VBI re-entry guard skipped services). Clearing
exact extents costs ≤ MaxSprites × height bytes regardless of how
the sprites are distributed. Bookkeeping grew from 8 bytes to
3 × MaxSprites (prev player/Y/height per logical sprite). The
decision (incremental clear over full clear) stands; only the
granularity changed.

---

## ADR-023: P/M Resolution as Per-Screen Configuration

**Status:** Accepted

**Context:**
ANTIC supports single-line (256 bytes per player, 1-scanline
precision) and double-line (128 bytes per player, 2-scanline
steps) P/M resolution. Should this be a global setting or
per-screen?

**Options Considered:**

1. **Global (GameConfig)**: One resolution for all screens.
   Simpler. Problem: a title screen with no sprites wastes
   memory if resolution is set for gameplay.

2. **Per-screen** (chosen): Each screen struct declares its
   resolution. P/M memory block is allocated at the maximum
   size needed by any screen. No extra memory cost since the
   block is sized to the largest anyway.

**Decision:**
Sprite vertical resolution (player/missile resolution on Atari) is a per-screen setting:

```cpp
struct GameplayScreen {
    static constexpr auto pm_resolution = SpriteVerticalResolution::SingleLine;
    // ...
};
```

**Rationale:**
P/M memory is a single block separate from screen memory,
always allocated at the maximum size. Per-screen resolution
adds no memory cost. A game can use double-line for a
lower-fidelity screen (minimap, menu) and single-line for
gameplay. The screen manager handles the DMACTL bit change
during `set_screen`.

**Tradeoff:**
Slightly more complex screen configuration. Most games will
use single-line everywhere, which is the default.

---

## ADR-024: Always-Sort Sprites Over Skip-Sort

**Status:** Accepted

**Context:**
The multiplexer needs sprites sorted by Y position each frame
for zone assignment. Should it sort unconditionally, or detect
changes first and skip the sort when nothing moved?

**Options Considered:**

1. **Skip-sort**: Compare each sprite's Y to its previous Y.
   If nothing changed, skip the sort. Cost: ~90 cycles for
   comparison + N bytes of bookkeeping (old_y array). Risk:
   misses non-position changes (sprite added, removed,
   activated, deactivated).

2. **Always-sort** (chosen): Insertion sort every frame.
   Cost: ~80-120 cycles on nearly-sorted data (common case,
   since sprites move by small deltas). No bookkeeping. No
   edge cases.

**Decision:**
Sort sprites by Y every frame using insertion sort.

**Rationale:**
Insertion sort on nearly-sorted data is nearly O(N). For 9
sprites, it costs ~80-120 cycles in the common case — trivial
relative to the ~30,000 cycle frame budget. The skip-sort
approach saves maybe 80 cycles on static frames but adds
bookkeeping, complexity, and failure modes (forgetting to
detect sprite addition/removal). Simplicity wins.

**Tradeoff:**
Wastes ~80 cycles on frames where nothing moved. This is
negligible and the code is simpler and more robust.

---

## ADR-025: Deferred Missile Multiplexing

**Status:** Accepted

**Context:**
Should the engine multiplex missiles (4 hardware, 2-bit-
packed memory) the same way it multiplexes players?

**Analysis:**
Missile multiplexing is technically feasible:
- DLI cost: +32 cycles per zone (HPOSM0-3 writes)
- Combined zone DLI: ~85 cycles (players + missiles)
- Memory write: bit-packed read-modify-write, ~1.75× cost
  per scanline versus players
- Data structures: +8 bytes per zone for missile assignments

However, 4 hardware missiles cover most game use cases
(bullets, projectiles are typically few on screen). Adding
missile multiplexing increases zone DLI time from 53 to 85
cycles, reducing margin for user DLIs on the same scanline.

**Decision:**
Design ZoneInfo and the DLI handler to accommodate missile
multiplexing in the future, but do not implement it initially.
Missiles are limited to the 4 hardware missiles in the first
version.

**Rationale:**
Shipping sooner with 4 missiles is better than shipping later
with multiplexed missiles most games won't use. The zone
DLI handler is modular — adding missile position writes is
an extension, not a rewrite.

**Tradeoff:**
Games needing more than 4 on-screen projectiles must manage
them via software (e.g., bitmap blitting into the playfield).
This is acceptable for the initial release.

---

## ADR-026: Two-Phase Display List Construction

**Status:** Accepted

**Context:**
The engine constructs ANTIC display lists from `DisplayLayout`
templates. The display list structure (mode bytes, blank lines,
DLI bits) is known at compile time, but the LMS (Load Memory
Scan) addresses pointing into screen memory depend on where
the shared buffer lands in RAM — a link-time decision.

**Options Considered:**

1. **Fully runtime construction**: Build the display list byte
   by byte at `set_screen` time. Flexible. Problem: wastes
   cycles computing what the compiler already knows.

2. **Fully compile-time construction**: `constexpr` display
   list with embedded addresses. Problem: screen memory
   addresses aren't `constexpr` (link-time resolved).

3. **Two-phase construction** (chosen): Compile time builds
   the display list template with placeholder LMS addresses
   and records where the LMS bytes are. `set_screen` copies
   the template and patches the LMS addresses.

**Decision:**
Phase 1 (compile time): build display list template — the
byte sequence with placeholder LMS addresses, region offsets
into the shared buffer, and a table of LMS byte positions.

Phase 2 (`set_screen` time): copy the template into the
active display list area, patch LMS addresses using the
actual shared buffer address plus region offsets.

**Rationale:**
Maximizes compile-time work. The `set_screen` phase only
copies the template (~30-200 bytes) and patches 1-4 LMS
entries (6-12 byte writes). Fast and predictable.

**Tradeoff:**
Each screen needs its own display list in RAM (they have
different structures). Display lists are small (30-200 bytes)
so this is acceptable. The template data lives in ROM; the
active display list is in RAM (ANTIC reads it via DMA).

## ADR-027: Per-Line-LMS Hardware Scroll with a Portable/Backend Split

**Status:** Accepted

**Context:**
Hardware scrolling has two parts: fine scroll (sub-cell, the
HSCROL/VSCROL registers) and coarse scroll (whole cells, by
repointing the display list's load addresses into the map).
Two problems shape the design. First, when horizontal scrolling
is enabled ANTIC fetches a *wider* line than it displays (Mode 2:
48 bytes vs 40), so for a map wider than the display, a single
load address per region (ADR-026) leaves successive mode lines
fetching at the wrong stride — each scrolled line must point at
its own map row. Second, `engine::scroll.h` is part of the
portable layer and must reach hardware only through `Platform::hal`
(Dependency Rule 2), yet coarse scroll is intrinsically about the
backend's display-program byte encoding.

**Options Considered:**

1. **One LMS per region + CPU row copy**: keep ADR-026's single
   load address and blit map rows into a screen buffer each step.
   Portable, but spends CPU every coarse step and needs a second
   buffer.

2. **Per-line LMS, all logic in ScrollManager**: give each scroll
   line its own load address and have the generic ScrollManager
   patch the display-list bytes directly. Correct, but bakes the
   ANTIC LMS encoding (3-byte stride, opcode bits, fine-register
   direction) into the portable layer.

3. **Per-line LMS with a portable/backend split** (chosen): the
   map *is* the screen memory (no per-frame copy); the generic
   ScrollManager owns only position, the fine/coarse split math,
   and the fine-register writes via the HAL; the backend display
   program owns the load-address patching.

**Decision:**
A scrolling region is declared with `ScrollRegion<Inner, MapW,
MapH>`; its pixels live in a game-held `engine::TileMap` bound via
`Game::scroll_map()` (the region contributes `ram_bytes = 0`). The
backend builder gives each visible line its own load address
(stride = map width) plus the fine-scroll-enable bits, and owns a
`patch_scroll()` that repoints them each frame. `ScrollManager`
stays portable: it computes `coarse_col()`/`coarse_row()` and
writes the fine registers through `Platform::hal`; the screen
manager hands the coarse offsets to the backend. Backend-specific
conventions — per-axis fine-scroll inversion (on ANTIC the
horizontal register opposes the coarse pointer, the vertical one
does not) and the fetch width — are supplied as data through the
display traits, never assumed in the generic code.

**Rationale:**
The map doubles as screen memory, so coarse scroll is just an
address patch (no blit). Keeping the encoding in the backend means
the portable scroll API names no ANTIC specifics and a second
platform supplies its own conventions through the same trait seam.

**Tradeoff:**
A scroll region's display list grows (one 3-byte load instruction
per visible line instead of one per region — e.g. 24 × 3 bytes),
and the map must carry a few margin columns on the horizontal axis
(the HSCROLL fetch overscan), with coarse scroll clamped to the
fetch width. Both are small and standard for ANTIC scrolling.

---

## ADR-028: Re-Entry Guard on the Deferred VBI

**Status:** Accepted

**Context:**
The engine's per-frame service runs as the Atari OS *deferred*
VBI (VVBLKD), reached through a `[[gnu::naked]]` trampoline that
saves the llvm-mos zero-page imaginary registers ($80-$9F,
including the soft-stack pointer __rc0/__rc1), JSRs the C++
service, restores them, and exits via XITVBV.

The VBI is an NMI, and an NMI can interrupt another NMI. A heavy
service — the sprite multiplexer with a full screen of sprites —
can overrun a frame. When it does, the *next* frame's VBI NMI
fires while the trampoline is still mid-service and re-enters it.
The inner save loop overwrites the single `edge_vbi_zp_save`
buffer with the outer call's already-allocated $80-$9F, so on
unwind the main thread's soft-stack pointer comes back one
service frame (~$1E) **lower** than it went in. That drift
accumulates every overrun until the pointer walks down out of
free RAM into the program's `.text`, and the next service's
stack writes corrupt the code — a crash that surfaces ~15 s in,
far from its cause. (Hardware-diagnosed on the 9-sprite multiplex
demo with an Altirra write-watchpoint; the JAM PC was inside a
math helper whose bytes had been overwritten by `sta ($80),y`.)

**Options Considered:**

1. **Make the service always fit one frame** (insufficient):
   necessary for smoothness, but not a *correctness* guarantee —
   any future heavy frame, or jitter, re-opens the window.

2. **Mask the VBI NMI during the service** (rejected): the VBI
   NMI must keep being delivered for the OS clock/timers; masking
   it breaks the system.

3. **Re-entry guard flag** (chosen): a one-byte `edge_vbi_busy`
   flag in the trampoline. On entry, if set, jump straight to
   XITVBV (skip the save/service/restore entirely); otherwise set
   it, run the service, clear it. A re-entrant VBI becomes a
   no-op — one frame's service is skipped — instead of corrupting
   the saved soft-stack pointer.

**Decision:**
The deferred-VBI trampoline guards against re-entry with a busy
flag and skips (not corrupts) on overrun.

**Rationale:**
The corruption is silent and catastrophic; a dropped frame's
service is a cosmetic hiccup. The guard is two instructions on
the hot path and makes the trampoline robust regardless of how
heavy a future service becomes. Independently, the service gates
DLI NMIs off (NMIEN) while it rebuilds the raster chain, so a
boundary DLI can't fire against half-written chain tables — a
related but distinct race.

**Tradeoff:**
An overrunning frame is silently skipped rather than reported.
Persistent overrun therefore shows as a halved frame rate, which
is the signal to trim the service (the P/M commit's per-sprite
clear, ADR-022, was the first such trim). The busy flag is global
mutable state in the trampoline, acceptable because the deferred
VBI is strictly single-instance.

## ADR-029: P/M Hardware Missiles on the Blitter Backend

**Status:** Accepted

**Context:**
Projectiles are the four direct (non-multiplexed) P/M missiles
(ADR-025). The original blitter (VBXE) sprite commit,
`SpriteManager::commit_blitter()`, only composited the logical
sprites into VRAM and ignored missiles entirely, and the blitter
init path never armed P/M DMA. So `Game::missile()` produced
nothing on a VBXE overlay build — the arena demo's bullets were
invisible (collisions still worked, since the game uses software
AABB). The missile logic in `commit_pm()` (a shared 2-bit-field
strip, exact-extent erase, HPOSM writes) is independent of the
zone/player machinery that does not exist on a blitter backend.

**Options Considered:**

1. **Leave missiles P/M-only on baseline** (rejected): bullets
   invisible on VBXE; a game would have to special-case projectile
   rendering per backend.
2. **Re-implement bullets as blitter sprites in the engine**
   (rejected): missiles are a deliberately separate, cheaper object
   (ADR-025); forcing them onto the sprite path conflates the two
   and costs sprite slots. (A *game* may still choose to draw
   bullets as sprites — `arena_vbxe` does, for visibility over the
   opaque overlay — but that is a game decision, not the engine's.)
3. **Share the missile commit across both backends** (chosen):
   factor the missile half of `commit_pm()` into `commit_missiles()`
   and call it from both `commit_pm()` and `commit_blitter()`; arm
   P/M DMA on the blitter init path too.

**Decision:**
Missiles are committed by one shared `commit_missiles()` on both
the P/M and blitter backends; the blitter backend arms P/M DMA so
the hardware projectiles render (on the P/M layer, below the VBXE
overlay) identically to baseline.

**Rationale:**
Keeps `Game::missile()` meaning the same on every backend with no
game-facing branch, reuses the tested P/M missile code verbatim,
and leaves the zone/player commit (which is genuinely backend-
specific) untouched. P/M DMA is a few bytes/scanline — negligible
bus load next to the blitter.

**Tradeoff:**
On an *opaque* overlay the P/M missiles render underneath and are
hidden, so a full-overlay game that wants visible projectiles
draws them as blitter sprites instead (a game choice). The blitter
backend still pays a small always-on P/M DMA cost even when a game
uses no missiles.

## ADR-030: VBXE MEMAC Window Clear of the Soft Stack

**Status:** Accepted

**Context:**
The CPU reaches VBXE VRAM through a banked MEMAC aperture; the
engine default is MEMAC-A at `$B000-$BFFF`. While the window is
enabled (MGE set), every CPU access in that 4K range is redirected
to VRAM instead of main RAM. The llvm-mos soft (call) stack is
placed by the atari8-dos runtime at the top of RAM — for the arena
binary the runtime soft-stack pointer (`$80`/`$81`) sat at
~`$BC01`, **inside** the default window. The engine enables the
window briefly for each VRAM transfer (the per-frame BCB upload in
`BlitterQueue::submit`, MEMAC writes); whenever a CPU stack access
landed in the window while it was enabled, the stack push/pop hit
VRAM instead of RAM. The result: every overlay operation slowly
corrupted both the displayed overlay (progressive on-screen noise)
and the program's own return addresses/locals, ending in a JAM a
few seconds in — far from the cause, and indistinguishable at first
from a dozen other suspects (it cost a long bisection through
sprites, bitmap writes, buffer mode, screen config, and game code,
all of which reproduced identically). Hardware-diagnosed by reading
the soft-stack pointer at ZP `$80`/`$81` and noticing it sat inside
`$B000-$BFFF`. (The sprites-over-bitmap demo happens to survive the
default window — its brief window-open intervals never coincide
with an in-use stack slot — so the trap is latent.)

**Options Considered:**

1. **Keep `$B000` and hope** (rejected): latent for any overlay
   program whose stack reaches into the window.
2. **Reserve `$B000-$BFFF` in the linker / cap the stack below it**
   (deferred): the robust engine-level fix — make the stack/heap
   physically unable to land in the window region. Not yet done;
   needs platform linker work.
3. **Move the window off the stack per build** (chosen for now):
   place the MEMAC window in free RAM clear of the stack and heap
   via the Config (`MEMAC_A_Cfg<0xA0>` → `$A000-$AFFF` for arena,
   between BSS/heap below and the stack above).

**Decision:**
A VBXE overlay program must place its MEMAC window clear of the
soft stack. `arena_vbxe` uses `$A000`; the latent default `$B000`
is documented as a trap (CONSTRAINTS.md) pending a linker-level
reservation.

**Rationale:**
Moving the window is a one-line Config change with no runtime cost
and immediately removes the overlap. The window/stack collision is
silent and catastrophic, so it warrants an explicit constraint and
a diagnostic recipe ($80/$81) even before the robust linker fix.

**Tradeoff:**
The fix is per-build (the author must pick a window address that
misses that binary's stack/heap) rather than guaranteed by the
toolchain. Until the window region is reserved in the linker, a
binary whose heap grows up or stack grows down into the chosen
window could still collide — verify against the `.map` if the
binary grows substantially.

## ADR-031: Platform-Specific Escape Hatches Belong on the Platform, Not Game::

**Status:** Accepted

**Context:**
During initial implementation, `Game::antic_playfield(bool)` was added
to `engine/core.h` as a convenience method for disabling ANTIC playfield
DMA on VBXE overlay screens. It was placed on the `Game::` facade because
that was the path of least resistance — game code already uses `Game::`
for everything else.

A subsequent portability audit (the header leakage cleanup) flagged the
method: `antic_playfield` embeds a chip name in the generic engine
facade and has no meaningful implementation on any non-Atari backend.
The initial FIXME proposed renaming it to a neutral portable name.

That diagnosis was wrong. The problem is not the name — it is the
location. There is no portable concept to rename it to. A C64 backend
has no equivalent DMA control; a hypothetical Apple II backend doesn't
either. The operation is intrinsically Atari-specific.

**Options Considered:**

1. **Rename to a neutral name on Game::** (rejected): `set_playfield_dma`,
   `set_background_fetch`, etc. Forces every future backend to implement
   or stub a concept that may not exist on its hardware. The `Game::`
   facade becomes polluted with lowest-common-denominator wrappers around
   hardware quirks. A C64 game author sees a method that does nothing on
   their platform. Wrong abstraction.

2. **Gate behind `if constexpr` on Game::** (rejected): exposes the
   method only when `caps::has_antic_dma_control` or similar is true.
   Keeps it on `Game::` but invisible on non-Atari builds. Still wrong —
   it couples the engine facade to a platform capability that exists
   solely because one chip works a particular way. Capability flags should
   describe features game code cares about, not hardware quirks the engine
   needs to manage internally.

3. **Remove from Game:: and expose through the platform layer** (chosen):
   The method is removed from `engine/core.h`. The equivalent function is
   exposed directly in the Atari platform headers
   (`engine/platform/atari/antic.h` or similar). Atari game code that
   needs it calls the platform function directly. Non-Atari game code
   never sees it.

**Decision:**
`Game::antic_playfield()` is removed from `engine/core.h`.
The functionality is exposed as a free function in the Atari platform
layer. Atari game code accesses it by including the Atari platform
header directly. The `Game::` facade exposes no platform-specific
escape hatches.

**Rationale:**
The `Game::` facade exists to give game code a single portable surface.
It should expose only operations that every backend can meaningfully
implement. Platform-specific escape hatches — DMA controls, chip-specific
registers, hardware quirks — belong on the platform layer, reachable
directly by game code that has already committed to a specific platform
by virtue of its platform type alias.

This is the same reason `Game::sid_volume()` would be wrong on a
C64 backend: not because SID is a bad name, but because it doesn't
belong on the generic facade. Platform-specific code reaches
platform-specific hardware through platform-specific APIs. The engine
facade stays clean.

**Principle established:**
If a method on `Game::` has no meaningful implementation on a
hypothetical second backend, it does not belong on `Game::`. Move it
to the platform layer. Game code that is already platform-specific
(by platform type alias) can include platform headers directly without
compromising portability.

**Tradeoff:**
Atari game code that calls `Game::antic_playfield()` must be updated to
call the platform function directly. Since EDGE is pre-1.0 and the only
callers are the engine's own demos, this is a one-site update. Future
callers will find the function where it semantically belongs — in the
Atari platform headers — rather than on a generic facade that implies
cross-platform availability.

---

## ADR-032: Session Lane Uses network_write; Realtime Lane Must Not

**Status:** Accepted

**Context:**
After wiring the FujiNet session lane to fujinet-lib (Stages 8E–8F), the
engine has two network lanes with different latency and reliability contracts:

- **Session lane** (`Game::net.session`) — TCP/reliable, intended for lobby,
  login, control messages, and match setup. Occasional multi-millisecond stalls
  are acceptable here.
- **Realtime lane** (`Game::net.realtime`) — intended for frame-rate multiplayer
  state snapshots. Frame stalls are not acceptable here.

The question is: which lane is permitted to call `network_write`, and what
invariants must hold about hidden flushes?

**Options Considered:**

1. **network_write permitted on both lanes:** Realtime lane also calls
   `network_write` for frame-rate sends. Problem: `network_write` performs a
   full CIO/FujiNet SIO transaction and may stall for 1–3 ms. A stall every
   frame would break the realtime lane's timing contract entirely.

2. **network_write gated behind a compile flag but available on both lanes:**
   Game code could opt in. Problem: the compile flag is a build-time choice, not
   a per-call choice. It cannot prevent game code from accidentally calling send
   on the realtime lane from a timing-critical path.

3. **network_write restricted to session lane; realtime lane uses Netstream/UDP-seq
   when wired** (chosen): The session adapter is the only site that calls
   `network_write`. The realtime lane adapter never calls it. When the realtime
   lane is wired in a future stage, it will use a non-blocking UDP-seq / Netstream
   path.

**Decision:**
`network_write` is called only from `FujinetLibSessionAdapter::session_send_nb`.
No realtime path, no `session_poll`, and no frame-service code calls
`network_write`. This is enforced by code structure (only the session adapter
file contains the call) and documented in the blocking-risk comment at the call
site.

**Rationale:**
The session lane already accepts occasional multi-ms stalls by design (ADR-016
discusses SIO latency). The realtime lane must not stall. Keeping `network_write`
strictly inside `session_send_nb` makes the boundary explicit and auditable
with a simple grep.

**Blocking-risk note recorded at call site:**
```
// NOTE (Stage 8F): network_write may block or perform a full CIO/FujiNet
// transaction. This call is only safe on the session/control lane where
// occasional frame stalls are acceptable. Do NOT use this path for
// realtime traffic; the realtime lane must use Netstream/UDP-seq when
// that stage is implemented. Avoid large writes during timing-critical
// frames; prefer small, bounded messages on the session lane.
```

**Invariants established:**
- `session.poll()` does **not** call `network_write` (no hidden flush).
- `network_write` is **not** referenced in any realtime lane file.
- `network_write` is **not** referenced in VBI or frame-service context.
- The realtime lane adapter (`fujinet_realtime_*.h`) will use a separate
  non-blocking symbol when wired in a future stage.

**Tradeoff:**
The session send path may stall the frame by 1–3 ms on the frame it is called.
Game code should send only small control messages on the session lane, not bulk
data, and should not call session send from frames with tight timing budgets.
This is the same tradeoff accepted in ADR-016 for polling in general.

---

## ADR-033: FujiNet Netstream Realtime Lane — Wire Policy and Validation Status

**Status:** Accepted

**Context:**
ADR-032 split the two network lanes and deferred the realtime lane, stating it
"will use a non-blocking UDP-seq / Netstream path" when "wired in a future stage."
Stages 9O–9R.3 wired it. This ADR records the final realtime Netstream
architecture, the concrete Netstream policy the adapter applies, the precise
validation status reached, and the remaining risks.

The realtime lane (`Game::net.realtime`) carries frame-rate multiplayer state and
must not stall the frame, so it cannot use the session lane's fujinet-lib/CIO/SIO
path (ADR-032, ADR-016). It is instead driven by an EDGE-owned Netstream assembly
handler that moves bytes through POKEY serial I/O via interrupt-driven rings.

**Decision — architecture:**
- **Dual-lane, separated transports.** The **session lane** (`Game::net.session`)
  remains fujinet-lib/CIO `N:` TCP, framed, reliable, and may stall a few ms. The
  **realtime lane** is EDGE-owned Netstream asm — no fujinet-lib, no per-byte
  CIO/SIOV — feeding interrupt-driven RX/TX rings. The realtime lane must **not**
  use fujinet-lib.
- **Adapter policy.** The engine `RealtimeLane` (`engine/net_api.h`) +
  `RealtimePacketQueues` (`engine/net_ring.h`) own the packet rings and hand the
  Atari adapter (`fujinet_netstream_realtime.h`) one **fixed 16-byte** packet at a
  time. TX and RX are **all-or-nothing**: the adapter pre-checks `tx_space()` /
  `bytes_avail()` and either moves the whole 16-byte unit or returns `WouldBlock`
  having moved nothing, so a partial transfer can never straddle the implicit
  packet boundary.
- **No wire framing.** The adapter adds no header, checksum, sequence number, or
  resync marker. Packet boundaries are *implicit* (every 16 bytes on the
  Netstream byte stream).
- **Public API unchanged.** `Game::net` and the `open_udp_seq` / `send` / `recv` /
  `poll` surface are exactly as before; the baud/flags/port policy below is
  internal to the adapter.

**Decision — Netstream policy** (constants in
`engine/platform/atari/fujinet_netstream_realtime.h`):
- **Flags `0x26`** = UDP (bit0=0) + UDP-seq (`0x20`) + TX external clock (`0x04`)
  + register (`0x02`). RX stays internal (`0x08` clear). The PAL bit (`0x10`) is
  left 0 in the seed and derived from `DetectPAL` — it is not pre-set.
- **Nominal baud `31250`** (BaudTable row `0x7A12`, AUDF3 = 21).
- **External TX clock required.** FujiNet/NetSIO drives the transmit clock.
- **30 RTCLOK-frame settle** (~0.5 s NTSC) after `begin` before the first
  transmit, so FujiNet/NetSIO can renegotiate the external SIO clock to the
  Netstream baud.
- **Port byte order** = host order byte-swapped into the DCB (low byte → DAUX1,
  high byte → DAUX2). Confirmed: `23 28` is decoded by the firmware as port `9000`.

**Reason for the change from the earlier attempt:**
The initial configuration used flags `0x20` and baud `19200` on an *internal* TX
clock and never transmitted. FujiNet/NetSIO drives an *external* transmit clock,
so `TX_EXT` (`0x04`) is required; without it POKEY never clocks the serial output
and the output-ready IRQ never fires. Switching to flags `0x26` / baud `31250`
with the external clock matches the proven upstream reference
(`fujinet-atari-netstream/examples/udp-sequence/atari_udp_sequence.c`).

**Validation status (precise — do not overclaim):**
- **mos-sim / static validated** — lifecycle state machine exercised via
  `FakeOps` under the simulator; CTests 19/19.
- **Altirra Mode A no-device clean-failure validated** — open with no FujiNet
  device fails cleanly without hanging or corrupting state.
- **fujinet-pc + NetSIO + Altirra + Docker UDP peer Mode B validated** — firmware
  enabled Netstream; flags `0x26`; AUDF3 `21`; baud `31250`; STREAM-OUT `A0..AF`;
  STREAM-IN `50..5F`; adapter open/active/send/recv/close all passed; the TX IRQ
  diagnostic showed the handler count advancing and the ring draining; production
  `.bss` remained 359.
- **NOT physical FujiNet hardware validated.** All Mode-B evidence is from the
  emulator/FujiNet-PC stack.

**Risks / future work:**
- Physical FujiNet hardware validation is pending.
- The fixed 16-byte boundaries are *implicit*: if bytes are lost on the stream,
  the receiver desynchronizes and cannot recover on its own (no resync marker).
- Wire framing / resync / checksum / sequence is separate future work.
- A real gameplay demo over the realtime lane is still needed; `net_dual_lane_demo`
  only illustrates the API shape.
- Physical Atari / SIO timing (clock renegotiation, IRQ latency, bus contention)
  may differ from the emulator/FujiNet-PC stack; the 30-frame settle and baud
  policy may need retuning on hardware.

**Relationship to ADR-032:**
This ADR fulfils the deferral in ADR-032. The `network_write`-on-session-only
invariant still holds: the realtime lane does not reference `network_write` and
uses the Netstream asm path exclusively.