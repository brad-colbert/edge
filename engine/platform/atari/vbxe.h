#ifndef ENGINE_PLATFORM_ATARI_VBXE_H
#define ENGINE_PLATFORM_ATARI_VBXE_H

// vbxe.h — direct VBXE access for power users.
//
// The portable Game:: API drives VBXE through the normal engine surface (sprites,
// bitmaps, palette) via the neutral overlay HAL seam. This umbrella header is the
// documented escape hatch: game code that needs direct blitter control, custom
// XDLs, raw VRAM access, or overlay priority manipulation includes it and works
// in the atari::vbxe namespace.
//
// Including a platform header from game code intentionally steps outside
// "game code never includes platform headers" (ARCHITECTURE.md Dependency Rule 1).
// That is the deal for direct hardware access; the portable API stays clean.

#include "vbxe_config.h"
#include "vbxe_registers.h"
#include "vbxe_memac.h"
#include "vbxe_layout.h"
#include "vbxe_xdl.h"
#include "vbxe_blitter.h"
#include "vbxe_palette.h"

// Game code:
//   #include <engine/platform/atari/vbxe.h>
//   atari::vbxe::MemacWindow<Cfg>::write(...);
//   atari::vbxe::upload_palette<Cfg>(1, pal);   etc.

#endif // ENGINE_PLATFORM_ATARI_VBXE_H
