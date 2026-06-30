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

#endif  // ENGINE_ATTRIBUTES_H
