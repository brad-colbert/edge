// demo/vbxe_gfx.cpp — Edge engine VBXE bitmap-drawing (gfx) demo.
//
// Draws a static picture into the VBXE overlay framebuffer using the portable
// engine::Core gfx API (Game::gfx()): a solid field, colour-bar rectangles, a
// drawn border + diagonals, and a small blitted 8bpp image. Produces a loadable
// .xex (mos-atari8-dos target; see CMakeLists.txt). Run on Altirra (or hardware)
// with a VBXE FX core.
//
// Expected result: a dark-blue field covering the screen with eight colour-bar
// rectangles down the left, a white frame around the edge, two crossing diagonal
// lines on the right, and a small chequered image — all steady (the picture is
// drawn once and persists).
//
// Single-buffered on purpose: with no sprites the frame service never clears the
// framebuffer, and a single-buffer present is a no-op, so the drawing stays put.
//
// ── Setup notes (same as the bring-up demo) ───────────────────────────────
//  * VBXE register base $D640 (engine default); a $D740 board needs a Config
//    override. Run with BASIC DISABLED (MEMAC-A window is $B000-$BFFF). Altirra:
//    enable the VBXE device with the FX core.

#include <stdint.h>

#include <engine/core.h>
#include <engine/platform/atari/platform.h>
#include <engine/platform/atari/vbxe.h>   // power-user: set_color (palette upload)

using engine::u8;
using engine::u16;
namespace M = atari;
namespace V = atari::vbxe;

// ── Platform + game configuration ────────────────────────────────────────
//
// Single-buffered VBXE (SR_320). No bitmap_region in GameConfig, so the gfx
// canvas is the overlay framebuffer (Region == void, the blitter path).
using Cfg = V::Config<M::Mode::VBXE_SR, V::Buffers::Single>;
using Platform = M::Platform<
    M::Machine::XL,
    M::RAM::Baseline,
    M::gfx::VBXE<Cfg>,
    M::Sound::Mono,
    M::TV::NTSC>;

struct ScreenText {
    using display = engine::DisplayLayout<engine::TextRegion<M::Mode::MODE_2, 24>>;
};
struct GameConfig {
    using screens = engine::ScreenSet<ScreenText>;
    static constexpr u8 max_sprites    = 1;
    static constexpr u8 sound_channels = 1;
};
using Game = engine::Core<Platform, GameConfig>;

// Palette 1 (the overlay's palette): index 0 is transparent; 1..8 are vivid
// colours; 9 is the dark field the picture is drawn over.
static void load_palette() {
    V::set_color<Cfg>(1, 1, 0xFE, 0x00, 0x00);   // red
    V::set_color<Cfg>(1, 2, 0x00, 0xFE, 0x00);   // green
    V::set_color<Cfg>(1, 3, 0x40, 0x40, 0xFE);   // blue
    V::set_color<Cfg>(1, 4, 0xFE, 0xFE, 0x00);   // yellow
    V::set_color<Cfg>(1, 5, 0x00, 0xFE, 0xFE);   // cyan
    V::set_color<Cfg>(1, 6, 0xFE, 0x00, 0xFE);   // magenta
    V::set_color<Cfg>(1, 7, 0xFE, 0x80, 0x00);   // orange
    V::set_color<Cfg>(1, 8, 0xFE, 0xFE, 0xFE);   // white
    V::set_color<Cfg>(1, 9, 0x00, 0x00, 0x40);   // dark-blue field
}

int main() {
    Game::init();        // brings up the VBXE overlay (has_blitter path)
    load_palette();

    auto& g = Game::gfx();

    g.clear(9);                                   // opaque dark-blue field

    // Eight colour-bar rectangles down the left (blitter fills).
    for (u8 i = 0; i < 8; ++i)
        g.fill_rect(20, static_cast<u16>(18 + i * 26), 120, 20, static_cast<u8>(i + 1));

    // A white frame, inset into the NTSC-safe area. The overlay is 240 lines but
    // only ~224 are displayed, so rows 0 and 239 fall in the TV overscan; inset
    // the top/bottom (and match the sides) so the whole border is visible.
    constexpr u16 L = 8, R = 311, T = 16, B = 223;
    g.hline(L, R, T, 8);
    g.hline(L, R, B, 8);
    g.vline(L, T, B, 8);
    g.vline(R, T, B, 8);

    // Two crossing diagonals on the right half (Bresenham via plot/MEMAC).
    g.line(170, 20, 300, 210, 4);
    g.line(300, 20, 170, 210, 2);

    // A small 16×16 chequered 8bpp image, blitted near the right.
    u8 img[16 * 16];
    for (u8 y = 0; y < 16; ++y)
        for (u8 x = 0; x < 16; ++x)
            img[y * 16 + x] = static_cast<u8>(((x ^ y) & 7) + 1);
    g.blit(216, 70, img, 16, 16);

    // Idle — the picture persists (no sprites, single buffer, nothing redraws it).
    Game::run([](const auto&) {});
}
