// demo/vbxe_text.cpp — Edge engine VBXE 80-column text-mode demo.
//
// Brings up the VBXE overlay in hardware text mode (Mode::VBXE_T80), uploads the
// supplied PREPPIE font to VBXE VRAM (CHBASE), and prints text into the character
// map through the portable engine API (Game::overlay_*). Produces a loadable .xex
// (mos-atari8-dos target; see CMakeLists.txt). Run on Altirra (or hardware) with a
// VBXE FX core.
//
// Expected result: a dark-blue field filling the screen with white text — a title
// and a few body lines — plus a 16x16 block showing the whole 256-glyph font, so
// you can see it is the PREPPIE font. Everything is steady (drawn once, persists).
//
// ── Setup notes (same as the other VBXE demos) ────────────────────────────
//  * VBXE register base $D640 (engine default). Run with BASIC DISABLED (the
//    MEMAC-A window is $B000-$BFFF). Altirra: enable the VBXE device, FX core.

#include <stdint.h>

#include <engine/core.h>
#include <engine/platform/atari/platform.h>
#include <engine/platform/atari/vbxe.h>   // power-user: set_color (palette upload)

#include "PREPPIEPC_FNT.h"                  // edge::assets::PREPPIEPC_FONT[256][8]

using engine::u8;
using engine::u16;
namespace M = atari;
namespace V = atari::vbxe;

// ── Platform + game configuration ────────────────────────────────────────
//
// 80-column text overlay, single-buffered. No sprites — the picture persists.
using Cfg = V::Config<M::Mode::VBXE_T80, V::Buffers::Single>;
using Platform = M::Platform<
    M::Machine::XL,
    M::RAM::Baseline,
    M::gfx::VBXE<Cfg>,
    M::Sound::Mono,
    M::TV::NTSC>;

// A minimal ANTIC screen (hidden under the opaque text overlay; the engine needs
// some screen layout to bring up).
struct ScreenText {
    using display = engine::DisplayLayout<engine::TextRegion<M::Mode::MODE_2, 24>>;
};
struct GameConfig {
    using screens = engine::ScreenSet<ScreenText>;
    static constexpr u8 max_sprites    = 1;
    static constexpr u8 sound_channels = 1;
};
using Game = engine::Core<Platform, GameConfig>;

// Text colours live in the overlay palette (palette 1). The attribute byte selects
// the foreground from indices 0..127; with attribute bit 7 set the background is
// the index (fg+128). So fg index N pairs with bg index N+128.
//   white-on-blue  : attr 0x81  (fg = idx 1 white, bg = idx 129 dark blue)
//   yellow-on-blue : attr 0x84  (fg = idx 4 yellow, bg = idx 132 dark blue)
static constexpr u8 ATTR_BODY  = 0x81;
static constexpr u8 ATTR_TITLE = 0x84;

static void load_palette() {
    // Foregrounds (0..127).
    V::set_color<Cfg>(1, 1, 0xFE, 0xFE, 0xFE);   // white
    V::set_color<Cfg>(1, 4, 0xFE, 0xFE, 0x00);   // yellow
    // Backgrounds (fg+128). Both pairs use the same dark-blue field.
    V::set_color<Cfg>(1, 129, 0x00, 0x00, 0x40); // dark blue (bg for fg 1)
    V::set_color<Cfg>(1, 132, 0x00, 0x00, 0x40); // dark blue (bg for fg 4)
}

int main() {
    Game::init();        // overlay_init: clears the char map, builds the Text_80 XDL
    load_palette();

    // Upload the PREPPIE font to VBXE VRAM (the CHBASE font region).
    Game::overlay_text_font(&edge::assets::PREPPIEPC_FONT[0][0],
                            edge::assets::PREPPIEPC_FONT_BYTE_COUNT);

    // Fill the whole 80x30 char map with blank cells on the dark-blue field.
    Game::overlay_text_clear(0x20, ATTR_BODY);

    // Title + body.
    Game::overlay_print(2, 1, "EDGE ENGINE - VBXE 80-COLUMN TEXT MODE", ATTR_TITLE);
    Game::overlay_print(2, 3, "Hardware text overlay: char + attribute pairs in VRAM,", ATTR_BODY);
    Game::overlay_print(2, 4, "8x8 glyphs from the PREPPIE font at CHBASE.", ATTR_BODY);
    Game::overlay_print(2, 6, "The quick brown fox jumps over the lazy dog.", ATTR_BODY);
    Game::overlay_print(2, 7, "0123456789  !\"#$%&'()*+,-./:;<=>?@[\\]^_{|}~", ATTR_BODY);

    // Full 256-glyph table (16x16) so the whole font is visible.
    Game::overlay_print(2, 10, "Full font (codes 0x00..0xFF):", ATTR_BODY);
    for (u8 hi = 0; hi < 16; ++hi)
        for (u8 lo = 0; lo < 16; ++lo)
            Game::overlay_put_char(static_cast<u8>(6 + lo),
                                   static_cast<u8>(12 + hi),
                                   static_cast<u8>(hi * 16 + lo), ATTR_BODY);

    // Idle — the text persists (no sprites, single buffer, nothing redraws it).
    Game::run([](const auto&) {});
}
