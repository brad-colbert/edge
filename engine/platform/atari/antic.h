#ifndef ENGINE_PLATFORM_ATARI_ANTIC_H
#define ENGINE_PLATFORM_ATARI_ANTIC_H

// platform/atari/antic.h — ANTIC display-list encoding and the Player/Missile
// memory layout.
//
// The display-mode vocabulary (`atari::Mode`) and its per-mode geometry helpers
// live in modes.h (the single source of truth), which this header includes so
// every existing includer of antic.h keeps seeing them. What remains here is the
// ANTIC-specific encoding: the display-list opcode/LMS/DLI bits and the P/M DMA
// layout.
//
// Depends only on hardware documentation (Dependency Rule 7) and types.h.

#include "../../types.h"
#include "modes.h"

namespace atari {

using engine::u8;
using engine::u16;

// ── Display-list instruction bits / opcodes ──────────────────────────
//
// A display list is a byte program ANTIC's DMA executes: blank-line counts,
// mode-line bytes (optionally with an LMS load-address prefix and/or a DLI
// trigger), and a terminating jump. See DECISIONS.md ADR-026.
inline constexpr u8 DL_LMS     = 0x40;   // mode byte | DL_LMS => 2 address bytes follow
inline constexpr u8 DL_DLI     = 0x80;   // mode byte | DL_DLI => fire a DLI on this line
inline constexpr u8 DL_HSCROLL = 0x10;   // mode byte | DL_HSCROLL => horizontal fine scroll on this line
inline constexpr u8 DL_VSCROLL = 0x20;   // mode byte | DL_VSCROLL => vertical fine scroll on this line
inline constexpr u8 DL_JMP     = 0x01;   // jump (2 address bytes follow)
inline constexpr u8 DL_JVB     = 0x41;   // jump + wait for vertical blank (loops the DL)

// Blank-line instruction for `n` blank scanlines (1..8). Encoded as (n-1)<<4.
constexpr u8 dl_blank(u8 n) { return static_cast<u8>((n - 1) << 4); }

// ── Player/Missile memory layout ──────────────────────────────────────
//
// The single source of truth for the ANTIC P/M DMA layout, the basis for where
// the sprite manager writes shape bytes (API_DESIGN.md "Sprite Vertical Resolution",
// DECISIONS.md ADR-022/023). `single` selects single-line resolution (2K block,
// 1-scanline Y precision) vs double-line (1K block, 2-scanline steps).
//
// Within a block the four player strips follow the missile strip; a player's
// byte offset for scanline Y is `pm_player_base + N*pm_strip_size + Y` (single)
// or `... + (Y>>1)` (double). The engine reaches these through Platform::hal
// (Dependency Rule 2) — see hal.h pm_player_offset.
constexpr u16 pm_player_base (bool single) { return single ? 1024 : 512; }
constexpr u16 pm_strip_size  (bool single) { return single ? 256  : 128; }
constexpr u16 pm_missile_base(bool single) { return single ? 768  : 384; }

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_ANTIC_H
