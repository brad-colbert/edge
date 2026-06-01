#ifndef ENGINE_PLATFORM_ATARI_ANTIC_H
#define ENGINE_PLATFORM_ATARI_ANTIC_H

// platform/atari/antic.h — ANTIC display modes, display-list encoding, and the
// per-mode geometry the display subsystem is built on.
//
// This header is the single source of truth for ANTIC mode geometry. The
// portable display layer (engine/display.h) is parameterised on a backend mode
// token and reaches all per-mode geometry through engine::display::traits<ModeT>
// (engine/display_traits.h); the Atari specialisation in
// platform/atari/display_traits.h forwards to the constexpr helpers here. So the
// generic display layer never includes this header — the backend binds to it via
// the trait specialisation, and a second platform family supplies its own.
//
// Depends only on hardware documentation (Dependency Rule 7) and types.h.

#include "../../types.h"

namespace atari {

using engine::u8;
using engine::u16;

// ── ANTIC display modes ──────────────────────────────────────────────
//
// The enum value is the ANTIC display-list mode nibble, so the display-list
// "mode line" instruction byte for a region is simply `u8(Mode)` (optionally
// OR'd with the LMS/DLI bits below). Text modes draw characters from the set at
// CHBASE; bitmap modes draw packed pixels.
//
//   text:    MODE_2 (40×24, 1bpp text)   MODE_4 (40×24, 4-colour text)
//            MODE_6 (20×24, 5-colour)    MODE_3/5/7 variants
//   bitmap:  MODE_8 (40×24, 4-colour)    BITMAP_D (160×192, 2bpp, "GR.7")
//            BITMAP_E (160×192, 2bpp)    BITMAP_F (320×192, 1bpp, "GR.8")
enum class Mode : u8 {
    MODE_2   = 0x02,
    MODE_3   = 0x03,
    MODE_4   = 0x04,
    MODE_5   = 0x05,
    MODE_6   = 0x06,
    MODE_7   = 0x07,
    MODE_8   = 0x08,
    BITMAP_9 = 0x09,
    BITMAP_A = 0x0A,
    BITMAP_B = 0x0B,
    BITMAP_C = 0x0C,
    BITMAP_D = 0x0D,
    BITMAP_E = 0x0E,
    BITMAP_F = 0x0F,
};

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

// ── Mode geometry ────────────────────────────────────────────────────
//
// `bytes_per_line` is the screen-memory width of one mode line. For text modes
// it is the column count; for bitmap modes it is ceil(width_px * bpp / 8).
constexpr u8 bytes_per_line(Mode m) {
    switch (m) {
        case Mode::MODE_2: case Mode::MODE_3:
        case Mode::MODE_4: case Mode::MODE_5:
            return 40;                       // 40-column text
        case Mode::MODE_6: case Mode::MODE_7:
            return 20;                       // 20-column text
        case Mode::MODE_8: case Mode::BITMAP_9:
            return 10;                       // 40 px, 2bpp / 1bpp narrow
        case Mode::BITMAP_A: case Mode::BITMAP_B:
            return 20;                       // 80 px
        case Mode::BITMAP_C: case Mode::BITMAP_D: case Mode::BITMAP_E:
            return 40;                       // 160 px (2bpp) / 320 px (1bpp C)
        case Mode::BITMAP_F:
            return 40;                       // 320 px, 1bpp
    }
    return 40;
}

// True for character (text) modes, false for bitmap/map modes.
constexpr bool is_text(Mode m) {
    return static_cast<u8>(m) >= 0x02 && static_cast<u8>(m) <= 0x07;
}

// Bits per pixel for bitmap modes (undefined/irrelevant for text modes, where
// "pixels" are characters). F is hi-res 1bpp; 8/9/A..E are 2bpp; B is 1bpp.
constexpr u8 bits_per_pixel(Mode m) {
    switch (m) {
        case Mode::BITMAP_F: case Mode::MODE_8: case Mode::BITMAP_C:
            return 1;
        case Mode::BITMAP_9: case Mode::BITMAP_B:
            return 1;
        default:
            return 2;                        // 4/D/E and the 4-colour map modes
    }
}

// Scanlines ANTIC draws for one mode line of this mode. Text rows are 8 tall;
// the high-resolution bitmap modes (C/E/F) are 1 scanline per line, D is 2.
constexpr u8 scanlines_per_line(Mode m) {
    switch (m) {
        case Mode::MODE_2: case Mode::MODE_4: case Mode::MODE_6:
        case Mode::MODE_8:
            return 8;
        case Mode::MODE_3:
            return 10;
        case Mode::MODE_5: case Mode::MODE_7:
            return 16;
        case Mode::BITMAP_9: case Mode::BITMAP_A:
            return 4;
        case Mode::BITMAP_B: case Mode::BITMAP_D:
            return 2;
        case Mode::BITMAP_C: case Mode::BITMAP_E: case Mode::BITMAP_F:
            return 1;
    }
    return 1;
}

// The display-list mode-line instruction byte for this mode (no LMS/DLI bits).
constexpr u8 dl_mode_byte(Mode m) { return static_cast<u8>(m); }

// ── Hardware scrolling geometry ──────────────────────────────────────
//
// When a mode line has DL_HSCROLL set, ANTIC fetches the *next wider* playfield
// so there is off-screen content to slide in: a normal-width 40-column text line
// fetches 48 bytes instead of 40. `scroll_margin` is those extra bytes; a scroll
// map's row stride must be at least `scroll_fetch_width` so every fetched byte
// is real map data.
constexpr u8 scroll_margin(Mode m) {
    switch (m) {
        case Mode::MODE_2: case Mode::MODE_3:
        case Mode::MODE_4: case Mode::MODE_5:
            return 8;                        // 40-col text: 40 -> 48
        case Mode::MODE_6: case Mode::MODE_7:
            return 4;                        // 20-col text: 20 -> 24
        default:
            return 8;
    }
}

constexpr u8 scroll_fetch_width(Mode m) {
    return static_cast<u8>(bytes_per_line(m) + scroll_margin(m));
}

// Fine-scroll modulus: the number of HSCROL color-clock positions that span one
// character cell, i.e. how far HSCROL advances before a one-byte coarse step.
// The normal playfield is 160 color clocks wide, so a 40-column cell is 4 color
// clocks and a 20-column cell is 8. (Vertical fine scroll uses scanlines_per_line
// as its modulus instead.) THIS is the value to verify against real hardware if a
// fine->coarse handoff ever jumps — see demo/atari_scroll_test.cpp.
constexpr u8 fine_scroll_range(Mode m) {
    switch (m) {
        case Mode::MODE_2: case Mode::MODE_3:
        case Mode::MODE_4: case Mode::MODE_5:
            return 4;                        // 160 color clocks / 40 cells
        case Mode::MODE_6: case Mode::MODE_7:
            return 8;                        // 160 color clocks / 20 cells
        default:
            return 4;
    }
}

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

// ── Character encoding ────────────────────────────────────────────────
//
// Convert an ASCII byte to its ANTIC internal screen code (the value stored in
// screen memory, which differs from ATASCII). The internal code rotates ASCII:
//   $20-$3F -> $00-$1F,  $40-$5F -> $20-$3F,  $60-$7F -> $40-$5F,
//   $00-$1F -> $40-$5F.  Used by TextRegionView::print / print_num.
constexpr u8 ascii_to_internal(char c) {
    const u8 a = static_cast<u8>(c);
    if (a < 0x20)  return static_cast<u8>(a + 0x40);
    if (a < 0x60)  return static_cast<u8>(a - 0x20);
    return a;                                 // $60-$7F map to themselves
}

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_ANTIC_H
