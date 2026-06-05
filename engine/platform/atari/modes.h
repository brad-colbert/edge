#ifndef ENGINE_PLATFORM_ATARI_MODES_H
#define ENGINE_PLATFORM_ATARI_MODES_H

// platform/atari/modes.h — the Atari display-mode vocabulary and the per-mode
// geometry the display subsystem is built on.
//
// This header is the single source of truth for Atari mode geometry: both ANTIC
// display modes and the VBXE overlay modes live in one `atari::Mode` enum, and
// every constexpr helper below switches on it. The portable display layer
// (engine/display.h) is parameterised on a backend mode token and reaches all
// per-mode geometry through engine::display::traits<ModeT>
// (engine/display_traits.h); the Atari specialisation in
// platform/atari/display_traits.h forwards to the helpers here. So the generic
// display layer never includes this header — the backend binds to it via the
// trait specialisation, and a second platform family supplies its own.
//
// ANTIC display-list encoding (the opcode/DLI/LMS bits and the P/M layout) lives
// in antic.h, which includes this header.
//
// Depends only on hardware documentation (Dependency Rule 7) and types.h.

#include "../../types.h"

namespace atari {

using engine::u8;
using engine::u16;

// ── Display modes ────────────────────────────────────────────────────
//
// For ANTIC modes the enum value is the ANTIC display-list mode nibble, so the
// display-list "mode line" instruction byte for a region is simply `u8(Mode)`
// (optionally OR'd with the LMS/DLI bits in antic.h). Text modes draw characters
// from the set at CHBASE; bitmap modes draw packed pixels.
//
//   text:    MODE_2 (40×24, 1bpp text)   MODE_4 (40×24, 4-colour text)
//            MODE_6 (20×24, 5-colour)    MODE_3/5/7 variants
//   bitmap:  MODE_8 (40×24, 4-colour)    BITMAP_D (160×192, 2bpp, "GR.7")
//            BITMAP_E (160×192, 2bpp)    BITMAP_F (320×192, 1bpp, "GR.8")
//
// VBXE overlay modes occupy the high range (>= 0x80). They are not ANTIC
// display-list bytes (dl_mode_byte() returns 0); they describe the VBXE overlay
// plane the gfx::VBXE axis draws over/under ANTIC output (see vbxe_config.h).
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

    // VBXE overlay modes (>= 0x80).
    VBXE_SR  = 0x80,   // Standard Resolution: 320 px, 256 colours, 1 byte/pixel
    VBXE_HR  = 0x81,   // High Resolution:     640 px, 16 colours, 4bpp (320 bytes/line)
    VBXE_LR  = 0x82,   // Low Resolution:      160 px, 256 colours, 1 byte/pixel
    VBXE_T80 = 0x83,   // 80-column text:      char + attribute pairs (160 bytes/line)
};

// True for the VBXE overlay modes (the high range), false for ANTIC modes.
constexpr bool is_vbxe(Mode m) { return static_cast<u8>(m) >= 0x80; }

// ── Mode geometry ────────────────────────────────────────────────────
//
// `bytes_per_line` is the screen-memory width of one mode line. For text modes
// it is the column count; for bitmap modes it is ceil(width_px * bpp / 8). The
// VBXE overlay modes reach 320 bytes/line, so this returns u16.
constexpr u16 bytes_per_line(Mode m) {
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
        case Mode::VBXE_SR: case Mode::VBXE_HR:
            return 320;                      // SR: 320 px × 1 byte; HR: 640 px × 4bpp
        case Mode::VBXE_LR: case Mode::VBXE_T80:
            return 160;                      // LR: 160 px × 1 byte; T80: 80 chars × 2
    }
    return 40;
}

// True for character (text) modes, false for bitmap/map/overlay modes. Among the
// VBXE modes only VBXE_T80 is text.
constexpr bool is_text(Mode m) {
    const u8 v = static_cast<u8>(m);
    return (v >= 0x02 && v <= 0x07) || m == Mode::VBXE_T80;
}

// Bits per pixel for bitmap modes (undefined/irrelevant for text modes, where
// "pixels" are characters). F is hi-res 1bpp; 8/9/A..E are 2bpp; B is 1bpp.
// VBXE: SR/LR are 8bpp (1 byte/pixel), HR is 4bpp.
constexpr u8 bits_per_pixel(Mode m) {
    switch (m) {
        case Mode::BITMAP_F: case Mode::MODE_8: case Mode::BITMAP_C:
            return 1;
        case Mode::BITMAP_9: case Mode::BITMAP_B:
            return 1;
        case Mode::VBXE_SR: case Mode::VBXE_LR:
            return 8;
        case Mode::VBXE_HR:
            return 4;
        default:
            return 2;                        // 4/D/E and the 4-colour map modes
    }
}

// Scanlines ANTIC draws for one mode line of this mode. Text rows are 8 tall;
// the high-resolution bitmap modes (C/E/F) are 1 scanline per line, D is 2. VBXE
// overlay modes are addressed per-scanline, so 1.
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
        case Mode::VBXE_SR: case Mode::VBXE_HR:
        case Mode::VBXE_LR: case Mode::VBXE_T80:
            return 1;
    }
    return 1;
}

// The display-list mode-line instruction byte for this mode (no LMS/DLI bits).
// VBXE overlay modes are not ANTIC display-list bytes, so they return 0.
constexpr u8 dl_mode_byte(Mode m) {
    if (is_vbxe(m)) return 0;
    return static_cast<u8>(m);
}

// ── Hardware scrolling geometry ──────────────────────────────────────
//
// When a mode line has DL_HSCROLL set, ANTIC fetches the *next wider* playfield
// so there is off-screen content to slide in: a normal-width 40-column text line
// fetches 48 bytes instead of 40. `scroll_margin` is those extra bytes; a scroll
// map's row stride must be at least `scroll_fetch_width` so every fetched byte
// is real map data. (VBXE overlay modes don't ANTIC-scroll.)
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

constexpr u16 scroll_fetch_width(Mode m) {
    return static_cast<u16>(bytes_per_line(m) + scroll_margin(m));
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

#endif // ENGINE_PLATFORM_ATARI_MODES_H
