# Architecture Decision Records

Decisions made during design, with rationale. These explain the
“why” behind architectural choices and document tradeoffs.

-----

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
1. **Free list pool**: N-byte free list for intrusive linking.
   Acquire/release O(1) but requires separate active-tracking.
   Total overhead: N+1 bytes vs 1 byte for bitmap.
1. **Packed pool**: Dense array with count, swap-on-release.
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
(can’t support both stable indices and dense iteration). Two
types are clearer, each optimized for its use case.

**Tradeoff:**
Game authors must pick the right pool type. Documentation and
naming (`SlotPool` vs `PackedPool`) make the choice clear.

-----

## ADR-002: No Zero-Initialization on Pool Acquire

**Status:** Accepted

**Context:**
When a game acquires an object from a pool, should the engine
zero-initialize it?

**Options Considered:**

1. **Zero-initialize on acquire** (rejected): Costs ~4 cycles
   per byte of struct per acquire. For a 4-byte enemy struct,
   that’s ~16 cycles. In a game that spawns 60 enemies per
   second, that’s ~960 cycles per frame wasted on zeroing.
1. **Don’t initialize; caller fills in all fields** (chosen):
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

-----

## ADR-003: Both Pointer and Index Release Overloads

**Status:** Accepted

**Context:**
`SlotPool::release()` should accept what type of argument?

**Options Considered:**

1. **Pointer only**: `release(T* ptr)`. Natural when you have
   the pointer from `acquire()`. Cost: pointer subtraction to
   recover index if needed elsewhere.
1. **Index only**: `release(uint8_t idx)`. Efficient on 6502.
   Cost: game author must track indices separately.
1. **Both overloads** (chosen): `release(T*)` and
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

-----

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
1. **Compile-time traits** (chosen): Capabilities as `static constexpr` members. The compiler’s `if constexpr` eliminates
   dead branches entirely. Unused platform code generates zero
   bytes.

**Decision:**
Capabilities are `static constexpr` members on a platform’s
capability profile. The engine queries them via `if constexpr`.

**Rationale:**
On a 6502 with severe ROM constraints, dead code elimination is
essential. A runtime check might add 20 bytes; the wrong code
path adds 500 bytes. Compile-time resolution gives the compiler
perfect information for optimization.

**Tradeoff:**
Less runtime flexibility. A game cannot dynamically detect
hardware and adapt (e.g., “is VBXE present?”). This is
acceptable because:

- The platform is known at compile time. Games target a specific
  platform configuration.
- The user can provide multiple builds targeting different
  hardware.
- Dynamic detection would require the binary to include code for
  all variants anyway.

-----

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
1. **Independent axes** (chosen): Each extension (RAM, graphics,
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

-----

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
1. **Assembly is allowed, users on their own**: No contracts,
   no integration. Results: silent register clobbers, DLI chain
   breakage, timing overruns. Disasters.
1. **Defined seams with cooperative resource release** (chosen):
   Assembly integrates through specific APIs (DLI registration,
   VBI hooks, ZP reservation, resource release). The engine
   documents the contract at each seam.

**Decision:**
Six integration points:

- DLI handler registration (C++ or raw assembly)
- VBI hook registration
- Zero page reservation (compile-time declaration)
- Render phase hooks (pre-commit, post-render)
- Resource release/reclamation (channels, players, scroll)
- Direct hardware access (permitted, consequences documented)

**Rationale:**
Atari development is incomplete without assembly. Rather than
prevent it, provide clean contracts. The resource release
pattern is the key innovation: instead of fighting over a
register, the user tells the engine “I’m taking over channel 3”
and the engine stops touching it. Cooperation.

**Tradeoff:**
Some rope to hang yourself. A determined user can still break
things. This is acceptable because the contracts are explicit,
the user chose to ignore them, and experienced Atari developers
expect this level of control.

-----

## ADR-007: Static Dispatch Over Virtual Functions

**Status:** Accepted

**Context:**
How should subsystems use polymorphism (e.g., different sprite
renderers for VBXE vs baseline hardware)?

**Options Considered:**

1. **Virtual functions** (rejected): `virtual void render_sprite()`.
   Vtable pointer costs 2 bytes per object. With 64 sprites on
   screen, that’s 128 bytes wasted. Indirect calls also add
   overhead on 6502 (no branch prediction, no pipelining).
1. **Static dispatch via templates** (chosen): `if constexpr`
   selects implementation at compile time. Dead branches
   eliminated entirely. Zero runtime overhead.
1. **Function pointers** (rejected for hot paths): Used sparingly
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
Less flexibility at runtime. But the flexibility isn’t needed —
the platform and its capabilities are compile-time constants.

-----

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
1. **Compile-time construction** (chosen): Use `constexpr`
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

-----

## ADR-009: One-Frame Render Buffering for Atomic Sprite Commits

**Status:** Accepted

**Context:**
When should sprite positions be written to P/M RAM? During the
visible frame (risky) or during VBI (safe)?

**Options Considered:**

1. **Write during visible frame**: Game updates sprite positions,
   writes directly to P/M RAM immediately. Risky: ANTIC may be
   reading P/M RAM while the CPU is writing it, causing visual
   glitches (sprite corruption mid-frame).
1. **Write during VBI** (chosen): Game updates sprite positions
   into a buffer during the visible frame. VBI commits all
   buffered positions to hardware at once. Safe: atomic update,
   no tearing. One-frame latency.

**Decision:**
Sprite positions are buffered during the game frame and
committed to hardware during VBI.

**Rationale:**
Atomic commits avoid tearing. The one-frame latency is expected
on Atari (standard practice). Game authors write to the buffer
(no knowledge of hardware timing needed), the engine handles
the commit.

**Tradeoff:**
Input-to-visual latency is two frames minimum (one frame for
game logic, one frame for hardware commit). This is acceptable
and expected on 6502 systems.

-----

## ADR-010: Explicit ZP Allocation Over Free-For-All

**Status:** Accepted

**Context:**
Zero page is scarce (~128 usable bytes after OS). Should it be
managed explicitly or left as a free-for-all?

**Options Considered:**

1. **Free-for-all**: Each subsystem and user code grabs whatever
   ZP it wants. Problem: collisions are silent and catastrophic.
1. **Explicit allocation** (chosen): Engine declares its ZP
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

-----

## ADR-011: No Floating Point, Fixed-Point Instead

**Status:** Accepted

**Context:**
Math on a 6502. Floating point or fixed-point?

**Options Considered:**

1. **Floating point**: IEEE 754 or similar. No FPU on 6502.
   Software FP is very slow and very large (~500+ bytes per
   operation).
1. **Fixed-point** (chosen): Represent numbers as scaled integers
   (e.g., 8.8 fixed-point). Add, subtract, multiply use integer
   ops. Divide and square root use lookup tables.

**Decision:**
The engine provides fixed-point helpers and lookup tables.
Games use fixed-point for any math beyond integers.

**Rationale:**
6502 has no FPU. Fixed-point is the practical middle ground
between “integer only” and “floating point at any cost.”

**Tradeoff:**
Game author must understand fixed-point scaling and be careful
with overflow. Mitigation: provide examples and a small math
library.

-----

## ADR-012: Header-Only Engine Libraries Over Linked Modules

**Status:** Accepted (with caveats)

**Context:**
Should engine code be header-only or split into translation
units?

**Options Considered:**

1. **Separate translation units**: Faster compilation, smaller
   object files. Problem: link-time overhead on a 6502 build,
   requires careful symbol management.
1. **Header-only** (chosen, with exceptions): Templates are
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
rebuild the engine frequently (it’s stable); only game code
changes often.

-----

## ADR-013: No Scanline-Sync Hook (Busy-Wait Is Anti-Pattern)

**Status:** Accepted

**Context:**
Should the engine provide an easy way to say “run this code at
scanline N”?

**Options Considered:**

1. **Scanline-sync hook**: `Game::at_scanline(100, callback)`.
   Internally polls VCOUNT until scanline 100. Problem: wastes
   hundreds of cycles per frame polling.
1. **No dedicated hook; redirect to better patterns** (chosen):
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

-----

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
1. **Manual management**: Engine provides no screen concept. Game
   author tears down and rebuilds display lists, DLI chains, and
   screen memory manually on each transition. Works, but this is
   exactly the boilerplate an engine should eliminate.
1. **Shared buffer with compile-time union** (chosen): Engine
   computes the maximum screen RAM across all declared screens.
   Allocates one shared buffer of that size. Each screen’s
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
- DLIs registered with `add_dli` are cleared on screen change.
  Persistent DLIs require `add_persistent_dli`. This prevents
  stale DLI handlers from firing on a screen they weren’t
  designed for.