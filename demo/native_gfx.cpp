// demo/native_gfx.cpp — Edge engine native (ANTIC bitmap) primitive-drawing demo.
//
// The baseline counterpart to demo/vbxe_gfx.cpp. It draws a static picture into a
// stock-hardware ANTIC bitmap region using the SAME portable gfx API
// (Game::gfx()): clear, fill_rect, hline/vline, line (Bresenham), plot and blit.
// No VBXE — this is pure ANTIC/GTIA, so the canvas is a baseline `BitmapRegion`
// drawn through its software `BitmapRegionView` (engine/display.h), and the gfx
// dispatch selects the software path (has_blitter == false).
//
// Two builds from this one source (mode chosen by NATIVE_GFX_HIRES):
//   * default          → BITMAP_E : 160×192, 4 colours (2bpp).  →  native_gfx.xex
//   * -DNATIVE_GFX_HIRES → BITMAP_F : 320×192, hi-res 2-colour (1bpp).  → native_gfx_hires.xex
//
// Expected result: a framed safe-area border, a column of colour-bar rectangles
// (one bar in the hi-res build), two crossing diagonals, a dotted accent line and
// a small chequered image — all steady (drawn once; nothing redraws it).
//
// ── Setup notes ───────────────────────────────────────────────────────────
//  * Needs no VBXE. Run with BASIC DISABLED so the demo owns RAM. NTSC.
//  * The view addresses pixels with u8 x/y, so a 320-wide mode (BITMAP_F) can only
//    reach x ≤ 255 through the API — the hi-res build leaves the right ~64px blank.

#include <stdint.h>

#include <engine/core.h>
#include <engine/platform/atari/platform.h>

using engine::u8;
using engine::u16;
namespace M = atari;

// ── Mode selection ─────────────────────────────────────────────────────────
#ifdef NATIVE_GFX_HIRES
constexpr auto kMode   = M::Mode::BITMAP_F;  // 320×192, 1bpp (hi-res, 2-colour)
constexpr u8   kRight  = 255;                // u8 view x: cannot reach the full 320
constexpr u8   kBpp    = 1;
// Hi-res is 1bpp: every "ink" colour collapses to pixel value 1.
constexpr u8   cInkA = 1, cInkB = 1, cInkC = 1;
#else
constexpr auto kMode   = M::Mode::BITMAP_E;  // 160×192, 2bpp (4-colour)
constexpr u8   kRight  = 159;
constexpr u8   kBpp    = 2;
// 2bpp pixel values map to playfield registers: 1→COLPF0, 2→COLPF1, 3→COLPF2.
constexpr u8   cInkA = 1, cInkB = 2, cInkC = 3;
#endif
// Full screen height. The 192-line buffer (7680 B) crosses a 4K scan boundary; the
// engine's screen manager front-aligns the canvas so no mode line straddles it.
constexpr u8 kHeight = 192;
constexpr u8 kBottom = kHeight - 1;          // 191

// ── Platform + game configuration ──────────────────────────────────────────
//
// Baseline graphics axis (no blitter). The gfx canvas is the screen's bitmap
// region; GameConfig::bitmap_region gives Game::gfx() its software view type, and
// set_screen() binds the view to the region's screen-memory slice.
using Platform = M::Platform<
    M::Machine::XL,
    M::RAM::Baseline,
    M::gfx::Baseline,
    M::Sound::Mono,
    M::TV::NTSC>;

struct ScreenBitmap {
    using display = engine::DisplayLayout<engine::BitmapRegion<kMode, kHeight>>;
};
struct GameConfig {
    using screens        = engine::ScreenSet<ScreenBitmap>;
    using bitmap_region  = engine::BitmapRegion<kMode, kHeight>;
    static constexpr u8 max_sprites    = 1;   // pure bitmap; sprites unused
    static constexpr u8 sound_channels = 1;
};
using Game = engine::Core<Platform, GameConfig>;

// ── Palette ────────────────────────────────────────────────────────────────
//
// set_color_pf field map: 0-3 = COLPF0-3, 4 = COLBK. Writes the OS colour shadow
// the VBI copies to the hardware registers each frame.
static void load_palette() {
#ifdef NATIVE_GFX_HIRES
    // ANTIC hi-res (mode F): lit pixels take their LUMINANCE from COLPF1 and their
    // HUE from the background. Black background + high-luminance COLPF1 ⇒ crisp
    // white ink. Set COLBK and COLPF2 (both candidate backgrounds) to black so the
    // field is black regardless of the GTIA hi-res quirk.
    Platform::hal::set_color_pf(4, 0x00);   // COLBK  : black field
    Platform::hal::set_color_pf(2, 0x00);   // COLPF2 : black (hi-res background hue)
    Platform::hal::set_color_pf(1, 0x0E);   // COLPF1 : white ink (luminance)
#else
    Platform::hal::set_color_pf(4, 0x90);   // COLBK  : dark blue field   (value 0)
    Platform::hal::set_color_pf(0, 0x0E);   // COLPF0 : white             (value 1)
    Platform::hal::set_color_pf(1, 0x1A);   // COLPF1 : gold              (value 2)
    Platform::hal::set_color_pf(2, 0x44);   // COLPF2 : red               (value 3)
#endif
}

int main() {
    Game::init();          // baseline bring-up; set_screen binds the gfx canvas
    load_palette();

    auto& g = Game::gfx();

    g.clear(0);            // background (pixel value 0)

    // A framed border, inset into the NTSC-safe area.
    constexpr u8 L = 4, R = kRight - 4, T = 6, B = kBottom - 6;
    g.hline(L, R, T, cInkA);
    g.hline(L, R, B, cInkA);
    g.vline(L, T, B, cInkA);
    g.vline(R, T, B, cInkA);

    // Colour-bar rectangles down the left (fill_rect). In the 4-colour build the
    // three bars cycle the playfield registers; in hi-res there is one ink bar.
    const u8 bars[3] = { cInkA, cInkB, cInkC };
    for (u8 i = 0; i < 3; ++i)
        g.fill_rect(12, static_cast<u16>(T + 14 + i * 52), 40, 28, bars[i]);

    // Two crossing diagonals on the right half (software Bresenham via plot).
    const u8 dx0 = static_cast<u8>(kRight - 90), dx1 = static_cast<u8>(kRight - 12);
    g.line(dx0, static_cast<u8>(T + 6), dx1, static_cast<u8>(B - 6), cInkB);
    g.line(dx1, static_cast<u8>(T + 6), dx0, static_cast<u8>(B - 6), cInkC);

    // A dotted accent line across the lower band (plot every few pixels).
    for (u8 x = L + 4; x < R - 4; x = static_cast<u8>(x + 6))
        g.plot(x, static_cast<u8>(B - 4), cInkA);

    // A small 16×16 chequered image, packed at the region's bpp, then blitted.
    // blit() expects source rows packed ceil(w / pixels_per_byte) bytes each, with
    // the leftmost pixel in the high bits of a byte (BitmapRegionView::blit).
    constexpr u8 IMG_W = 16, IMG_H = 16;
    constexpr u8 ppb    = static_cast<u8>(8 / kBpp);
    constexpr u8 mask   = static_cast<u8>((1u << kBpp) - 1);
    constexpr u8 stride = static_cast<u8>((IMG_W + ppb - 1) / ppb);
    u8 img[IMG_H * 4] = {0};               // 4 = max stride (BITMAP_E); F uses 2
    for (u8 y = 0; y < IMG_H; ++y)
        for (u8 x = 0; x < IMG_W; ++x) {
            const u8 v  = static_cast<u8>(((x ^ y) >> 2) & mask);   // chequer
            const u8 sh = static_cast<u8>((ppb - 1 - (x % ppb)) * kBpp);
            img[y * stride + x / ppb] = static_cast<u8>(img[y * stride + x / ppb] | (v << sh));
        }
    g.blit(static_cast<u8>(R - 26), 26, img, IMG_W, IMG_H);

    // Idle — the picture is static and persists (no sprites, nothing redraws it).
    Game::run([](const auto&) {});
}
