# Proposal — the size diet, priced against the current tree

**Answering:** ATank's "EDGE writeup — the size diet (pay-for-what-you-use)",
2026-08-12 (Slice 18, Prompt 5)
**Branch:** `size_diet`; round 2 on `size_diet_2`
**Status:** revision 3, after ATank's stage-A field measurement. Stages A, B1 and
C are **landed**; C's code half measures **845 B**, not the ~1,500 B estimated.
**Round 2** (items 1-3 of ATank writeup 19's shortlist) is landed and measured —
see "Round 2" at the end: **-993 B** on ATank's wall 1 against an estimated -930.


## Revision 3 — C-code measured, and it is smaller than estimated

ATank's field numbers for stage A: **606 B** on the real image against my 616 B
proxy. The incrementals agree closely (specialization 429 vs 448) even though the
absolutes do not (bare knob 176 vs 69, `uses_hw_collisions` 30 vs 99 — ATank has
less latching to delete). ATank's control leg measured within one byte of its
pre-diet baseline, confirming the default-path discipline on a second consumer.

C's code half is now **measured at 845 B**, replacing the ~1,500 B estimate that
came from `main`'s inlined size. That estimate was too optimistic: not all of
`main` is relocatable setup code.

**The consequence: C-code does not close ATank's remaining wall.** Against
1,370 B remaining, 845 B leaves **525 B** still to find. The `prepare_chain`
assembly reserve (~692 B) does close it, and the two together would fund the
remaining prompts — but C-code alone will not, contrary to the expectation that
either lever closes the gap by itself.

**Status:** revision 2 notes retained below. Stages A and B1 are **landed**;
B2's plumbing is landed with its payoff uncounted. Every number was obtained by
ablation, and the stage-A/B1 figures have since been re-confirmed against the
landed code.

## Revision 2 — what ATank's answers changed

1. **One binary is the standing decision.** Per-phase builds are the declared
   last resort, under evaluation but not chosen. B2's 2,596 B therefore does
   **not** count toward the threshold, and C moves onto the critical path.
2. **Do not gate input or sound.** Keyboard is used for name entry; sound is
   unused today but reserved for imminent weapon work. The ~340 B those two
   would have returned is off the table by instruction, not by oversight.
3. **ATank will re-measure A on its real image** once `size_diet` carries it.

**The consequence, stated plainly: the re-ranked plan no longer clears the
threshold.** A (616, landed) + C (~1,500, unproven) = **2,116 B — 407 B short of
2,523**, with the two easiest remaining levers (input/sound) ruled out. The gap
is real and needs a decision; see "The 407-byte gap" below.

## Verdict up front

The request is sound and the engine's share is real — `frame_service` measures
**3,462 B**, within 1.1% of ATank's reported ~3,500. But the ranking in the
writeup does not survive measurement:

- **Ask 1 already exists as a knob** (`GameConfig::max_raster_hooks` /
  `max_frame_hooks`, engine/core.h:67-78) and ATank can set it today. As
  shipped it returns **69 B**, not the estimated 800–1,200, because capacity
  only sizes arrays — it never specializes code. Making it a *specialization
  trigger* raises the same knob to a measured **616 B**. That is the cheapest
  real win in the whole request and it is mostly unrealized today.
- **Ask 2's storage half returns ~0 against ATank's binding constraint.**
  ATank is blocked on `.text+.rodata`; per-direction lane sizing is
  RAM-only (measured **−637 B .bss**, **−2 B code**). It is still worth doing —
  ATank says the RAM unblocks game-side overlays — but it does not count
  toward the 2,523.
- **Ask 2's code half is the single biggest lever** and the only one that
  crosses the threshold on its own.
- **~6 KB is not reachable from asks 1–4 as scoped.** Asks 1+2+3 realistically
  total ≈4.7 KB of code. The remainder has to come from ask 4's assembly work
  on code ATank genuinely uses.

## How these numbers were obtained

Proxy target: `atari_tank_dual_net_demo` at `-Os`
(`-DEDGE_BUILD_DEMO=ON -DEDGE_HOT_OPT=-Os`, `SimulatedNetwork` phase-1 source).
It is the closest structural analogue to ATank in the tree — scroll map,
direct-bind sprites, both network lanes, same `-Os` whole-program profile.

Method: remove one block, rebuild, diff `llvm-size -A` sections and
`llvm-nm --print-size`. Because `prepare_chain` and `apply_scroll` each have
**exactly one call site** (`frame_service`, engine/core.h:478 and :555), their
ablation deltas are genuine compile-out, not merely de-inlining.

**Caveat that matters:** these are proxy numbers. ATank's own figures will
differ — most of all for the session lane, whose code is not even linked into
this demo. Every figure below should be re-measured on ATank before anyone
commits to a threshold date.

Baseline: `.text+.rodata` **17,514** · `.data` 139 · `.bss` 10,948 ·
`frame_service` 3,462.

## Where the frame service's 3,462 bytes actually are

| block | Δ `.text+.rodata` | Δ `frame_service` | ATank needs it? |
|---|---|---|---|
| `interrupts.prepare_chain` | **1,114** | 1,102 | only on change |
| ├ `sort_slots` | 422 | — | no (1 hook) |
| ├ `program_raster_lines` (DL walk) | 420 | — | once, at init |
| └ table build + rearm | ~272 | — | once, at init |
| `screen.apply_scroll` + `tiles.set_viewport` | **960** | 587 | **yes** |
| `sprites.commit` | **954** | 953 | **yes** |
| `update_zones` + `build_raster_hooks` | 340 | 338 | no (direct-bind) |
| `input.update` | 177 | 72 | partly |
| `sound.tick` | 162 | 151 | probably |
| hardware collision latch | 87 | 18 | no (software AABB) |

The two largest survivors — `apply_scroll` (960) and `sprites.commit` (954) —
are code ATank actually runs every frame. They are not removable by
configuration. That is the structural reason asks 1–4 cannot reach 6 KB, and
why ask 4's assembly half is load-bearing rather than optional.

## Ask-by-ask

### Ask 1 — compile-time capacity configuration · **616 B measured** · low risk

Partly shipped, badly underused. `max_raster_hooks`/`max_frame_hooks` already
size the tables; they do not specialize the code that walks them, because the
compiler cannot prove the runtime `total_count_ <= MaxRasterHooks` invariant.

Measured, cumulatively, on the proxy:

| change | `.text+.rodata` | Δ | `.data` |
|---|---|---|---|
| baseline (12/4 default) | 17,514 | — | 139 |
| ATank sets 1/0 — *available today* | 17,445 | −69 | 26 |
| `+ if constexpr (MaxRasterHooks > 1) sort_slots();` | 17,075 | **−439** | 26 |
| `+ run_frame_hooks` constexpr-empty at 0 | 16,997 | −517 | 26 |
| `+ uses_hw_collisions = false` gate | 16,898 | **−616** | 26 |

Deliverable — three small edits in engine/interrupt.h plus one new trait:

1. `if constexpr (MaxRasterHooks > 1) sort_slots();` — a one-slot chain cannot
   need an insertion sort. **422 B, one line.**
2. `run_frame_hooks()` returns immediately under `if constexpr (MaxFrameHooks
   == 0)`; declare storage as `hooks_[MaxFrameHooks ? MaxFrameHooks : 1]` so a
   zero capacity stays legal C++ rather than a zero-length-array extension.
3. New `GameConfig::uses_hw_collisions` (default `true`, same detection idiom
   as the existing `uses_missiles` at engine/core.h:92) gating the 16-read
   latch + `clear_collisions()` in `frame_service`. ATank reads collisions via
   software AABB, so it pays 87 B for nothing today.

No API break: every trait defaults to current behaviour.

### Ask 2 — session/realtime as alternatives · **the threshold lever** · medium risk

Today `NetManager` (engine/net_api.h:386-390) gates each lane on a *platform*
capability — `caps_of_t<Platform>::has_network_realtime` / `has_network_session`.
A consumer cannot express "one lane at a time"; the platform decides. This is
exactly the structural blocker the writeup describes, and it is confirmed:
`SessionLane` code is **not linked at all** in the proxy build, yet its storage
is still allocated (`net_facet` = 999 B `.bss`).

Two separable deliverables:

**B1 — storage sizing (RAM only, trivial).** `session_facet` hardcodes
`SessionLane<Platform>` (256/256/128). Plumb `GameConfig::session_rx_bytes` /
`session_tx_bytes` / `session_max_message` through exactly the way
`realtime_packet_bytes` already is (engine/net_api.h:147-155) — the class is
already templated on all three, so this is trait plumbing, not surgery.
Measured: 16/16/16 → **−592 B .bss**; 1/1/1 → **−637 B .bss**; code unchanged.

**B2 — lane selection (the code half).** Add `GameConfig::net_lanes`
(`Realtime` | `Session` | `Both`, defaulting to the platform capability) as the
`HasRealtime`/`HasSession` source in `NetManager`. In a per-phase build the
unused lane becomes unreachable and the linker drops it — ATank's reported
**2,596 B**. I could not measure this on the proxy: the dual-net demo uses both
lanes by construction, so forcing either off fails the build. It must be
measured on ATank.

**Revision 2 — decided.** ATank ships one binary; per-phase builds are the
declared last resort. The trait is landed and defaults to current behaviour, so
it costs nothing and is ready the day that decision changes, but **its 2,596 B
is not counted toward the threshold**. This was the only ask that crossed 2,523
by itself; without it the threshold has to come from Ask 1 + Ask 3, which as
costed do not reach it.

### Ask 3 — init-only code made discardable · ~1,500 B · **highest engineering cost**

Real but the most expensive to deliver honestly. The display-program builders
are inlined into `main` (1,547 B); `program_for`'s display-list static is a
further 273 B `.bss`.

The trap: on a 6502 `.xex` you cannot "discard" `.text` — you can only *overlay*
it. Moving init code to a cold, `minsize`, out-of-line path (the `EDGE_COLD`
attribute already in engine/attributes.h) buys frame-path speed and near-zero
image size, because these are single-call-site functions that were going to be
emitted once regardless. Returning the bytes requires one of:

- **(a)** an engine-sanctioned init segment placed at an address the game
  later reclaims as data — needs linker-script work and a documented contract
  about what may not live there; or
- **(b)** builders emitting into consumer-provided scratch that the game
  reclaims — the mechanism from the memory-epochs writeup already with this
  table.

(b) composes with work already queued and is the one I'd pursue.

**Revision 2 — now the critical path.** With B2 uncounted this is the largest
remaining lever, and ATank has asked that the mechanism be proven on one screen
so it stops being the unproven leg. That is the right next move, with one
caveat worth stating in advance: this was ranked "most likely to slip" on
engineering risk, and putting it on the critical path does not lower that risk —
it raises the cost of it slipping. The ~1,500 B is an estimate from the inlined
size of `main`, not a measurement of a working mechanism.

### Ask 4 — frame-service diet · configuration part folded into Ask 1

The configurable savings here (collision latch, zone/hook residue, frame-hook
dispatch) are already counted in Ask 1's 616 B — they are the same edits. What
remains is genuinely-used code: `apply_scroll` 960, `sprites.commit` 954, and
`prepare_chain`'s init-time residue ~420. Those move only by hand assembly,
per the operator's standing directive, or by algorithmic change.

One cheap non-assembly win found while measuring: `prepare_chain` currently
rebuilds unconditionally whenever the chain is non-empty. A "chain unchanged
since last prepare" early-out costs 62 B of code and skips the whole rebuild
every frame for a static chain. That is a **frame-time** win, not a size win —
recorded here so it is not mistaken for one.

### Asks 5 & 6 — charset bind-don't-copy, PMG prefix accessor

Both RAM-side, both cheap, neither on the threshold path. Ask 6 in particular
is an accessor plus a documented `.bss`-zeroing caveat over memory that already
exists — worth taking whenever Ask 2's storage work touches the same area.

## Recommended sequence

| stage | work | code returned | status |
|---|---|---|---|
| **A** | Ask 1 specialization package | **616 B** | **landed** — measured on the landed code |
| **B1** | Session lane trait plumbing | 0 (+496–637 B RAM) | **landed** — measured |
| **B2** | `net_lanes` trait | not counted | **plumbing landed**, defaults to current behaviour |
| **C** | Init-scratch mechanism (memory epochs) | ~1,500 B | **critical path** — mechanism still unproven |
| **D** | Assembly: chain tables, `apply_scroll`, `sprites.commit` | not estimated | after C |

### The 407-byte gap

With B2 uncounted and input/sound ruled out, the arithmetic no longer reaches
the unblock threshold:

| | bytes |
|---|---|
| A — landed, measured | 616 |
| C — estimated, unproven | ~1,500 |
| **subtotal** | **2,116** |
| threshold | 2,523 |
| **short by** | **407** |

Three ways to close it, in the order I'd try them:

1. **ATank's own priced fallback** — the second `.xex` segment via linker-script
   deviation, which ATank already costed at ~736 B. A + C + that = 2,852, clearing
   the threshold by 329 B. This is the only combination on the table today that
   closes with margin and without new unknowns.
2. **Stage D on `prepare_chain`'s init-time residue** — the display-list walk and
   table build (~692 B combined) are the most self-contained assembly targets in
   the frame service, and unlike `apply_scroll`/`sprites.commit` they run once
   rather than every frame, so hand-assembly there risks no gameplay timing.
3. **Revisit B2** — if the last-resort decision goes to per-phase builds, the gap
   closes several times over and C stops being load-bearing at all.

**Target path (~6 KB):** not reachable on the current ranking. A + C ≈ 2.1 KB;
even with the fallback and all of D's safe targets it is roughly half the ask.
Reaching 6 KB requires either per-phase builds (B2) or accepting the
input/sound gating that is currently ruled out. I would not plan around 6 KB
until one of those changes.

## What ATank sets to collect stage A

```cpp
struct GameConfig {
    // ... existing fields ...
    static constexpr u8   max_raster_hooks   = 1;      // was 12 by default
    static constexpr u8   max_frame_hooks    = 0;      // was 4 by default
    static constexpr bool uses_hw_collisions = false;  // software AABB

    // B1 — size to ATank's actual session protocol; these are illustrative.
    static constexpr u16  session_rx_bytes    = 64;    // was 256
    static constexpr u16  session_tx_bytes    = 32;    // was 256
    static constexpr u16  session_max_message = 48;    // was 128
};
```

Setting nothing changes nothing: every field defaults to prior behaviour, which
is why the unchanged demo still measures byte-identical. The 616 B only arrives
when ATank declares the first three.

One caution on `uses_hw_collisions`: set it `false` only if ATank never calls
`Game::sprite_collisions()`. The query still compiles when gated off — it
reports all zeroes — so a stray reader fails silently rather than loudly. The
dual-net demo in this tree is exactly such a reader (it takes GTIA wall hits),
which is why the demo does **not** set it.

## Open questions

1. **Which way to close the 407 B?** My recommendation is ATank's own `.xex`
   segment fallback — it is already priced, already understood on your side, and
   clears with 329 B of margin. C alone will not get there.
2. **Should I start C now, or the safe half of D first?** C is the larger prize
   but the mechanism is unproven; D's `prepare_chain` residue (~692 B) is
   smaller, self-contained, and runs once rather than per frame, so it carries no
   gameplay-timing risk. If the answer to (1) is the fallback, D may be
   unnecessary entirely.

## Round 2 — the shortlist from ATank writeup 19, measured

**Answering:** ATank's "EDGE writeup — size diet, round 2" (2026-09-17) and its
2026-10-10 addendum, items 1-3 of the Round-2 shortlist. Items 4-6 were out of
scope. **Branch:** `size_diet_2`, three commits, one per item, each with its own
ablation number.

### Method, and why the baseline needed saying twice

Measured on ATank itself, not on a proxy: a measurement build of ATank against this
working tree (`build-diet2-base`), never flown, per charter §3.3. The method was
validated first by reproducing ATank's leg of record from a worktree at the pin
`61ce793` — `.text` 22,539 + `.rodata` 962 ending `$7BF7`, `.xex` 37,093 B, every
figure matching the leg exactly.

That reference mattered, because **the branch point does not link in ATank**.
`b7df758` (the netstream init-status reporting, landed on `master` before this round)
costs ATank **19 B of `.text`** and **1 B of `.bss`**, which puts `.text`+`.rodata`
at `$7C0A` — 10 bytes *past* the charset at `$7C00`. Writeup 17 set the budget for
the next engine commit at "fewer than 10 bytes, or return some"; that commit spent
19 and broke the client's link. So the round-2 ledger reads: `61ce793` +9 B free ->
`master` **10 B over** -> item 1 -> item 2 -> item 3. The diet repays it many times,
but ATank cannot pin `master` without this branch.

Five walls were tracked on every build, because code that moves between sections can
break any of them while wall 1 improves:

| wall | limit | `61ce793` | after round 2 |
|---|---|---|---|
| 1 `.text`+`.rodata` | `$7C00` | +9 | **+983** |
| 2 `.data` | `$A800` | +28 | +9 |
| 3 `.bss` | `$B578` | +2 | **+111** |
| 4 init gap + 384 B stack | `$BC20` | +17 | **+55** |
| 5 `.atank_hiram` | `$BFF6` | +0 | **+92** |

### The three items

| item | addendum estimate | measured | zone |
|---|---|---|---|
| 1 display-list emitters | -480 | **-517** | `.text`, init path |
| 2 multiplexer under `Direct` | -300 | **-288** | `.text`, frame path |
| 3 scroll geometry | -150 | **-168** (+20 storage follow-up) | `.text`, frame path |
| **total** | **-930** | **-993** | |

**Item 1 — one emitter for every layout** (`22fde08`). `emit_lms`/`emit_load` were
private members of `DisplayProgram<Layout>`, so each layout got its own copy, and
each took the write index by `u16&` — every `p++` an indirect 16-bit
read-modify-write. 682 B (226 ×2 + 230) for a three-byte-store helper. Now one
`detail::dl_emit_load` parameterised on the index width, never on `Layout`, through a
cursor: **165 B**, and 39 B off the placed init section as well.

**Item 2 — the multiplexer under a direct binding** (`308c2d4`). The per-frame
boundary-table build, hook registration, fire index, init-time arming and the
frame-service "split across zones" test all compile out; `zones_`/`mux_table_` size to
the one zone the binding can produce. Also **-94 B `.bss`** and **-64 B hiram**, the
two walls that were full. Cost: **+19 B `.data`**, which is `Core::interrupts`
evicted from zero page by the compiler's allocator after the change — a reshuffle,
not new data (zero-page statics fell 45 -> 33 B; the VBI save range is unchanged at
`[$DB,$FF]`).

**Item 3 — the scroll region's geometry** (`dd79bb2`, completed by `HEAD`). `ScrollManager` now takes an
`engine::ScrollGeometry` template argument that `Core` derives from the game's own
screens. The divisors the addendum named were worth only **52 B**; the two inversion
conventions — the same kind of runtime copy of a compile-time fact, in the same
functions — were worth **116**, which is where the item's return actually is. A
follow-up commit finished the item on the storage side: a fixed geometry no longer
*stores* the four values it never reads (a further **-20 B** wall 1, -16 `.bss`,
-28 hiram), which is also what returned `atari_tank_dual_net_demo`'s link — see
Gates.

### What was tried and rejected

1. **Unconditional `noinline` on the shared emitter** (item 1). Correct for a
   consumer that places init code, wrong for one that does not: `atari_tank_demo`
   **+143 B**, `atari_mode4_geometry_probe` **+101 B**, because they inline `build()`
   outright. Gated instead behind a new `EDGE_INIT_SHARED`, which is `EDGE_COLD` only
   when the consumer has opted into init placement — the same measurement that keeps
   `EDGE_INIT_FN` empty by default. Gated, every demo gains from the cursor alone.
2. **Dropping the `Direct` staging round-trip** (item 2, ~95 B). `update_zones` stages
   `zones_[0].hpos/color` from `sprites_`, and `commit_pm` reads them straight back
   one step later — a copy serving no one under this binding. It was **not** removed,
   because it is not free: the staging happens at the *end* of the previous frame's
   service, so the X register is written from a sample one frame older than the shape
   and Y, which `commit_pm` takes live from `sprites_`. Reading `sprites_` directly
   would be 95 B cheaper *and* make X agree with Y — a behaviour change, out of scope
   for a round whose contract is that nothing changes. **It is also a finding in its
   own right: under `Direct`, a sprite's horizontal position currently lags its shape
   and vertical position by one frame**, which shows up as a slight shear on
   diagonally-moving sprites. Worth a deliberate decision rather than a silent fix.
3. **Removing the backend's raw boundary handler** (item 2). It is module-level
   assembly in the backend, so nothing garbage-collects it: with every call site gone
   it stays in the image, unreferenced. Gating it would mean teaching the platform
   layer about a game-level binding, which the layering does not allow.

### ATank's side

- **`__udivmodhi4` (248 B) is still linked.** Item 3 removed the engine's two call
  sites, but ATank's own `menu_num` still divides, so the helper stays. Moving that
  one site off it drops 248 B more — the ATank half of shortlist item 3, as the
  addendum scoped it.
- **`master` cannot be pinned as-is** (the `b7df758` 19 B above). A pin to this
  branch's tip gets both that commit and its repayment.
- **`atari_tank_net_demo`'s zero-page link failure is pre-existing** and unrelated:
  it fails 3/3 at `61ce793` and at `master` in a parallel build, and links when built
  alone. Known saturated-zero-page nondeterminism, untouched by this round.

### Gates

Two gate failures of this round's own making are worth recording, because both were
found after the commit that caused them and neither was visible in its own measurement:

1. **`atari_tank_dual_net_demo` stopped linking at item 2.** That demo sits at
   *exactly* 96/96 bytes of zero page (47 `.zp.data` + 7 `.zp.bss` + 42 static
   stack) at `master` and after items 1 and 3; item 2's compile-outs changed the
   static-frame overlay enough to need one byte more, and the link failed
   deterministically, 3/3. Item 2's commit message claims "all demos build at -O2";
   that was premature. Item 3's storage follow-up returns the byte and makes it true.
   The saturation is pre-existing (and a documented dead end), but this round tipped it.
2. **Item 2's dual-emulator check was first run on a stale binary** — a demo built
   before that item's edits. It was re-run on the finished tree, where it passes on
   both emulators; the numbers quoted are the re-run's.

Every commit: 48/48 host tests, all demos build at `-O2`, and the ATank measurement
build links with the item's number in the commit message. Items 2 and 3 are frame-path
work and took the dual-emulator check round 1 used: `atari_tank_demo` — a direct-bound
sprite over a scrolled map, exercising both items — renders **pixel-identical**
(`compare -metric AE` = 0) to the pre-diet build on **Altirra** and on **Fujisan**,
each capture confirmed to show the live demo rather than a blank screen. Item 3 is an
arithmetic identity and is proven under mos-sim besides: 300 positions per
configuration against the runtime form, both inversion conventions, a wider cell, and
a cell height that is not a power of two. Items 2 and 3 are mutation-proven.

## Round 3 — two deliberate behaviour changes, on a size budget

**Answering:** ATank's `EDGE-kickoff-round-3-immediate-vbi-and-x-lag.md` (2026-10-19,
ruled R279): writeup 18's immediate-VBI servicing (item A) and writeup 19 item 21,
the `Direct` X lag (item B). **Branch:** `size_diet_3` from `d137def`, three commits
plus docs, each with its own measurement. This round is not a diet — it *spends* wall
1 — so the budget, not the saving, is the number to watch.

### Method and baseline

Same as round 2, and validated the same way first: a measurement build of ATank
against a clean export, never flown. The export of `d137def` reproduces ATank's leg
of record **byte for byte** — 36,829 B, sha256 `a2cf4b72…` — so these numbers are the
leg's, not a proxy's. (ATank's tree is unchanged since `583ba33`, which built that
leg; only records and the walk script moved.)

Note for anyone comparing with round 2's closing figures: round 2 reported wall 1 at
+983 and the `.xex` at 36,982 B, measured before ATank landed its own half of item 3
(`menu_num` off `__udivmodhi4`). The leg carries that, which is the 1014 / 36,829
below. Note also that the `.xex` size is **not** a proxy for wall 1 — the
`.text`+`.rodata` segment is padded to the charset at `$7C00`, so growth inside it
does not move the file size.

| wall | limit | leg (`d137def`) | after round 3 |
|---|---|---|---|
| 1 `.text`+`.rodata` | `$7C00` | +1014 | **+926** |
| 2 `.data` | `$A800` | +163 | +144 |
| 3 `.bss` | `$B578` | +94 | **+109** |
| 4 init gap + 384 B stack | `$BC20` | +55 | +55 |
| 5 `.atank_hiram` | `$BFF6` | +91 | +86 |

`.noinit` stays 0. **Round net on wall 1: −88 B.** The kickoff preferred ≤ 0 and
capped at +100; this is inside the cap, but only because item A came in cheaper than
estimated — item B was supposed to pay for it and did not (below).

### The three commits

| commit | what | wall 1 | other walls |
|---|---|---|---|
| instrument | count the frames the guard turns away | −11 | `.bss` −2 |
| item B | `Direct` X/colour commit live | −18 | — |
| item A | the work a frame must not lose | −59 | `.data` −19, `.bss` +17, hiram −5 |

**Item B did not pay what round 2 estimated, and the estimate was the wrong shape.**
Round 2 sized the `Direct` staging round-trip at ~95 B by measuring what *deleting*
it saved. Replacing it with a live read costs more than the staging did: the snapshot
was a flat array, the slot is a 7-byte stride, so `frame_service` grew **+19 B**
(2,471 → 2,490) and that was the only body that changed. Folding the reads into the
strip loop — which already holds the slot address and its visibility test — measured
**+82 B**, worse still, so the separate loop stands. Anyone re-planning against a
"dropping the staging returns 95 B" line should read it as "the staging machinery is
95 B", which is not the same number.

**Item A came in at 59 B against the ~90 B its own code costs**, because removing the
frame service's now-redundant re-arm and the call to it returned about 31.

### Zero page, after each item

| | `.zp.data` | `.zp.bss` | VBI save range |
|---|---|---|---|
| leg | 42 | 19 | `[$DD,$FF]` = 35 B |
| instrument | 42 | 19 | `[$DD,$FF]` = 35 B |
| item B | 42 | 19 | `[$DD,$FF]` = 35 B |
| item A | 23 | 36 | **`[$DB,$FF]` = 37 B** |

Item A changed the call graph (the service lost a callee), and llvm-mos's zero-page
allocator re-chose its residents — the same reshuffle round 2 saw with
`Core::interrupts`. Total zero-page statics fell 61 → 59 B, but `.zp.bss` grew, and
the trampoline's save range is `[.zp.bss end, $FF]`, so the per-frame save grew by
two bytes and ~24 cycles. `.data` gaining 19 B while `.bss` returns 17 is the other
half of the same reshuffle.

### The interrupts-off budget, which is the real constraint on item A

The immediate entry runs at the top of the frame NMI with interrupts masked, ahead of
the OS's own stage 1, so its length is added to the window in which the serial IRQ
cannot be taken — and POKEY buffers exactly one received byte. Counted from the
disassembly of the measurement build:

| | bytes | cycles |
|---|---|---|
| before | 15 | **19** |
| after | 105 | **137** (116 when the re-arm is skipped) |

The contiguous shadow-to-register mapping (`$02C0`→`$D012` through `$02C8`→`$D01A`)
would do the colour copy in an 11-byte indexed loop instead of 54 bytes of
straight-line stores — but in 128 cycles instead of 72. The bytes were spent on the
margin; see "the lane, measured end to end" below for what the margin actually is, and
for the circumstances in which the 43 bytes are worth taking back. If the window ever has to come down, the four
player-colour copies (32 cycles) are the droppable ones under a direct binding:
nothing clobbers those registers during the frame there, so they only carry
propagation of a colour *change*, which is latency-tolerant. Under a multiplexed
binding they are not droppable — the boundary hooks write those registers during the
visible frame.

### Proof

The probe (`tests/backends/atari/vbi_skip_altirra_probe.cpp`) manufactures each class
of unserviced frame independently, which is also what proves the new counter counts
the right one:

| leg | jiffies | serviced | skipped | class A | hook dispatched |
|---|---|---|---|---|---|
| interrupts-off window, before | 303 | 76 | **0** | 227 | 75 (24.8 %) |
| interrupts-off window, after | 303 | 76 | 0 | 227 | **300 (99.0 %)** |
| guard-held window, before | 302 | 75 | **227** | 0 | 75 (24.8 %) |
| guard-held window, after | 302 | 75 | 227 | 0 | **298 (98.7 %)** |

The counter ignores frames that never reached the deferred vector and catches every
frame that did; `serviced + skipped == jiffies` in the guard-held legs. The overrun
counter reads 0 in all four beside a 25 % service ratio — the old blind spot,
reproduced on purpose. The residual 3 frames in the "after" legs is a **constant**
offset at the measurement window's edges: tripling the run to 900 jiffies gives
900/903, the same 3.

**Both legs reproduce byte-for-byte on Fujisan** (round 2's gate 2): 50/305 before and
300/305 after, the same dump bytes as Altirra's at the same knob setting.

The colour half cannot be counted from inside — the background register is write-only —
so it is a pixel A/B, and the probe's terminal state is built for it: after the dump it
holds interrupts off forever, so not one further frame is serviced and the state under
test is permanent instead of a coin flip. Without the immediate restore the marker
palette covers the whole screen and the band does not even redraw (the chain tail
leaves the raster vector on the terminal and only a service would re-point it); with it,
the playfield's palette sits above the seam and the marker's below, held steady. Both
pictures carry the map's stripes, which is what shows the capture is the live probe.
Confirmed on **Altirra and Fujisan**, same two binaries.

That terminal state was added after a first Fujisan capture read as a pass it had not
earned: taken while the measurement loop was still running, a capture lands on a
serviced frame about one time in six and shows the healthy picture either way. Altirra
had happened to catch a skipped frame and Fujisan a serviced one, from the same build.

### The lane, measured end to end

`edge_net_realtime_meter` against the docker echo peer on its own IP, Altirra Mode B
with the real NetSIO bridge and fujinet-pc, 40 s per leg, the two builds differing only
by this branch:

| | TX | RX | DELAY | TXHZ/RXHZ | LOSS RT / FWD / REV | FRM DROP |
|---|---|---|---|---|---|---|
| `d137def` | 00525 | 00523 | 0033 ms | 014 / 014 | 01 % / 01 % / 00 % | 0 of 02101 |
| round 3 | 00525 | 00523 | 0033 ms | 014 / 014 | **00 % / 00 % / 00 %** | 0 of 02101 |

Identical throughput, delay and frame count; loss did not rise. (The 01 % in the
baseline leg is one early packet — both runs report WORST 01 %, BEST 00 %.)

**And the budget was never as tight as the kickoff assumed.** The kickoff put a byte at
"~150 µs ≈ 260 cycles, at hsioindex 6" — that is the *session* lane's high-speed SIO
rate. The realtime lane does not run at the SIO index: it runs at
`kNetstreamNominalBaud`, 31,250 nominal (~31,960 bps actual), and ATank does not
override it. Ten bits at that rate is **~560 cycles**, so the masked window — this
handler's measured 137 plus the NMI dispatch and the OS's own first stage, which is not
measured here but is on the order of 150 — sits at roughly 300 against a 560-cycle byte
time. A received byte cannot be overtaken.

Which means the 43 bytes the indexed colour loop would save **are available if ATank
wants them**: at 193 cycles for the handler the window would be ~360, still inside 560.
It is not taken here because the margin is the safety property on a lane that has no
resync — one lost byte costs the datagram stream — and because the OS's stage-1 cost is
an estimate rather than a measurement. That is the operator's call, and the number is
on the table.

### What was tried and rejected

1. **An indexed loop for the colour copy** — 11 bytes against 54, but 128 cycles
   against 72, inside the interrupts-off window. Rejected on cycles; see above.
2. **Folding item B's live reads into the strip loop** — the loop already has the
   slot address and the visibility test, so it looked free. Measured +82 B against the
   separate loop's +19. Rejected on measurement.
3. **Gating the re-arm on "guard non-zero"** — simpler than adding a third guard
   state, and wrong: the guard is also non-zero on the frames where the service has
   *finished* and the loop has not consumed it, which are exactly the frames the
   re-arm exists for. The third state distinguishes "mid-rewrite of the chain" from
   "finished", and only the first must block the re-arm.
4. **Enabling interrupts inside the immediate entry** so the serial IRQ could be taken
   during its 137 cycles. It would remove the budget pressure entirely, but it puts
   the OS's own stage 1 — which the OS runs masked, and which exists for code that
   must be mixed with time-critical SIO — under a different interrupt discipline than
   it was written for. Out of scope for this round; recorded as the lever if the
   window ever has to grow again.
5. **Dropping the sprite commit into the immediate entry** so a skipped frame kept its
   sprites. Rejected by the kickoff and confirmed here: the commit must stay whole, or
   a frame whose strips were never written gets a position anyway and the shear item B
   just removed comes back the other way round.

### Still open, and not this round's

- **The class-B rate is the consumer's to fix, not the engine's.** Nothing here makes
  the game loop cheaper, so frames the guard turns away stay turned away; the counter
  now reports them directly so the split can be read off a running image.
- **The interrupts-off windows in the netstream handler** are the only thing that
  removes class A at the root. Untouched: the handler is proven and delicate.
- The `hpos`/`color` members of the zone record are dead under a direct binding but
  still declared, because the multiplex path references them in code that is compiled
  though unreachable there. Conditional storage would return 8 B of `.bss`; not worth
  the mechanism at this wall's pressure.
