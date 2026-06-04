// demo/vbxe_sprites_over_bitmap.cpp — Edge engine sprites-over-bitmap demo.
//
// Draws a bitmap background into the VBXE overlay and animates sprites over it,
// proving the background survives under the moving sprites. This is the union of
// the two earlier demos: the drawn picture of vbxe_gfx.cpp plus the animated
// blitter sprites of vbxe_bringup.cpp (mode 3). Produces a loadable .xex
// (mos-atari8-dos target; see CMakeLists.txt). Run on Altirra (or hardware) with
// a VBXE FX core.
//
// Expected result: a dark-blue field with eight colour-bar rectangles down the
// left, a white frame, two crossing diagonals, and a small chequered image — and
// a ball + pixel sprite sliding horizontally across it. The background stays
// fully intact behind the sprites with NO black/flat trail (the bug this feature
// fixes: a flat-colour erase would smear bg_color_ over the picture where sprites
// had been).
//
// Background::Bitmap keeps an authoritative "master" copy of the picture in VRAM.
// The game draws into the master via Game::gfx(), publishes it once to seed the
// display, and the compositor restores each sprite footprint from the master
// every frame instead of clearing to a flat colour. Double-buffered, so the
// moving sprites are flicker-free.
//
// ── Setup notes (same as the other VBXE demos) ────────────────────────────
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
// Double-buffered VBXE (SR_320) with Background::Bitmap: gfx() draws into the
// VRAM master canvas, and the sprite compositor restores footprints from it.
using Cfg = V::Config<V::Mode::SR_320, V::Buffers::Double, V::RegBase::D640,
                      V::MEMAC_A, 0x00000, V::Background::Bitmap>;
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
    static constexpr u8 max_sprites    = 2;
    static constexpr u8 sound_channels = 1;
};
using Game = engine::Core<Platform, GameConfig>;

// Sprite shapes (MODE 3): kBall is a Packed1bpp P/M-style 8x8 bitmap (coloured
// per-instance); kPix is a Pixel8bpp 8x8 shape carrying palette-1 indices.
constexpr auto kBall = engine::make_sprite<8, 8>({
    0b00111100, 0b01111110, 0b11111111, 0b11111111,
    0b11111111, 0b11111111, 0b01111110, 0b00111100,
});
constexpr auto kPix = engine::make_pixel_sprite<8, 8>({
    8,8,8,8,8,8,8,8,  8,1,1,1,1,1,1,8,
    8,1,8,8,8,8,1,8,  8,1,8,1,1,8,1,8,
    8,1,8,1,1,8,1,8,  8,1,8,8,8,8,1,8,
    8,1,1,1,1,1,1,8,  8,8,8,8,8,8,8,8,
});

// Palette 1 (the overlay's palette): index 0 transparent; 1..8 vivid; 9 the dark
// field the picture is drawn over.
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

// Draw the background picture into the master canvas (gfx() targets the master in
// Background::Bitmap mode). Same content as vbxe_gfx.cpp.
static void draw_background() {
    auto& g = Game::gfx();

    g.clear(9);                                   // opaque dark-blue field

    for (u8 i = 0; i < 8; ++i)
        g.fill_rect(20, static_cast<u16>(18 + i * 26), 120, 20, static_cast<u8>(i + 1));

    constexpr u16 L = 8, R = 311, T = 16, B = 223;
    g.hline(L, R, T, 8);
    g.hline(L, R, B, 8);
    g.vline(L, T, B, 8);
    g.vline(R, T, B, 8);

    g.line(170, 20, 300, 210, 4);
    g.line(300, 20, 170, 210, 2);

    u8 img[16 * 16];
    for (u8 y = 0; y < 16; ++y)
        for (u8 x = 0; x < 16; ++x)
            img[y * 16 + x] = static_cast<u8>(((x ^ y) & 7) + 1);
    g.blit(216, 70, img, 16, 16);
}

int main() {
    Game::init();        // brings up the VBXE overlay (has_blitter path)
    load_palette();

    draw_background();               // draw into the VRAM master canvas
    Game::overlay_publish_background();  // seed both display pages from the master

    Game::sprite_color(0, 1);        // kBall coloured palette-1 index 1 (red)

    // Two sprites sliding horizontally over the (unchanging) background.
    Game::run([](const auto&) {
        static u16 t = 0; ++t;
        const u8 x0 = static_cast<u8>(40 + (t % 220));
        const u8 x1 = static_cast<u8>(40 + ((t + 110) % 220));
        Game::sprite(0, kBall, x0, 120);
        Game::sprite(1, kPix,  x1, 150);
    });
}
