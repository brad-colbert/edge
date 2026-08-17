#ifndef ENGINE_ATTRIBUTES_H
#define ENGINE_ATTRIBUTES_H

// attributes.h — portable function attributes shared across the engine and demos.
//
// EDGE_COLD marks a function as a COLD path: keep it out of the hot, -O2-inlined main
// loop and compile it for minimum size. clang honours `minsize` per-function regardless
// of the translation unit's -O2, so the 60 fps hot path stays fast while one-shot /
// setup / loading code is size-optimised — no gameplay-framerate impact. `noinline` also
// stops the cold body from being inlined (and thus duplicated/bloated) into a caller.
//
// Use on: screen setup, title/menu/game-over callbacks, network-download / asset-loading
// code, and other code that runs only outside the per-frame gameplay loop.
#define EDGE_COLD [[gnu::noinline, clang::minsize]]

// EDGE_INIT marks SETUP-PHASE code: it runs while the game is being brought up and
// while screens are being constructed, and is dead once the game has made its last
// screen transition. It is EDGE_COLD plus, when the consumer opts in, placement in
// a named section the consumer's link step can position wherever it likes —
// typically over memory the game reclaims as data once setup is done, which is the
// only way to get these bytes back on a platform whose image cannot discard code.
//
// Opt in by defining EDGE_INIT_SECTION to the section name, e.g.
//   -DEDGE_INIT_SECTION=".edge_init"
// and adding a rule for that section to the link script. Without the define this
// is exactly EDGE_COLD, so a consumer that has not arranged placement links
// unchanged — an unplaced named section is a link-time surprise, never a default.
//
// THE CONTRACT, which the engine cannot enforce: code marked EDGE_INIT is valid
// until the consumer reuses the memory under it. Reclaiming that memory and then
// calling into the engine in a way that re-enters setup — most obviously another
// set_screen — jumps into whatever now occupies those addresses. A game with a
// one-way splash->play transition is the safe shape; a game that returns to a
// menu screen later is not.
// Without the opt-in this expands to NOTHING, not to EDGE_COLD. These are mostly
// single-call-site functions, where the `noinline` that a named section requires
// costs more than the out-of-lining saves — marking them EDGE_COLD by default
// measured +235 bytes on the dual-net demo. A consumer who has not asked for the
// section pays exactly zero.
#ifdef EDGE_INIT_SECTION
#  define EDGE_INIT [[gnu::noinline, clang::minsize, gnu::section(EDGE_INIT_SECTION)]]
#else
#  define EDGE_INIT
#endif

#endif  // ENGINE_ATTRIBUTES_H
