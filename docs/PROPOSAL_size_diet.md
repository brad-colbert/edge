# Proposal — the size diet, priced against the current tree

**Answering:** ATank's "EDGE writeup — the size diet (pay-for-what-you-use)",
2026-08-12 (Slice 18, Prompt 5)
**Branch:** `size_diet`
**Status:** proposal. Nothing in this document is implemented; every number
below was obtained by ablation against an unmodified tree, which was restored
after each measurement.

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

This is the only ask that crosses 2,523 by itself, and it depends on ATank
actually shipping per-phase builds. If they hold at one `.xex`, B2 returns
nothing and the threshold has to come from Ask 1 + Ask 3 together.

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

(b) composes with work already queued and is the one I'd pursue. Either way
this is the ask most likely to slip, so it should not be on the threshold path.

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

| stage | work | code returned | confidence |
|---|---|---|---|
| **A** | Ask 1 specialization package | **616 B** | measured, low risk |
| **B1** | session lane trait plumbing | 0 (+637 B RAM) | measured |
| **B2** | `net_lanes` selection + ATank phase builds | ≤2,596 B | ATank-reported, unmeasured here |
| **C** | init-scratch mechanism (memory-epochs) | ~1,500 B | mechanism unproven |
| **D** | assembly: `prepare_chain` tables, `apply_scroll`, `sprites.commit` | remainder | not estimated |

**Threshold path (2,523 B):** A + B2. A alone is 616 B — a quarter of the way.
A + B2 clears it with margin *if* ATank ships per-phase builds; if it does not,
the threshold path becomes A + C, which is slower and less certain.

**Target path (~6 KB):** A + B1 + B2 + C ≈ 4.7 KB of code plus 637 B RAM. The
last ~1.3 KB has to come from D. I would not commit to 6 KB until B2 is measured
on ATank and the epoch mechanism from ask 3 is proven on one screen.

## What I need from ATank

1. **Re-measure A on the real image.** The 616 B is a proxy figure; the demo is
   similar but not identical.
2. **Is per-phase building actually on the table?** B2's entire value depends
   on it. If ATank must ship a single `.xex`, say so now and I will re-rank
   toward C.
3. **Does ATank use `sound_channels` and the keyboard?** `sound.tick` (162 B)
   and `input.update` (177 B) are gateable on the same pattern as
   `uses_hw_collisions`, but only if genuinely unused — worth ~340 B together.
