// demo/native_gfx.cpp — Edge engine native (ANTIC bitmap) primitive-drawing demo.
//
// The baseline counterpart to demo/vbxe_gfx.cpp. It draws the same picture with the
// portable engine bitmap primitives (clear, fill_rect, hline/vline, line, plot,
// blit) on stock ANTIC/GTIA hardware — no VBXE. Press FIRE to cycle three native
// bitmap modes that all share ONE screen buffer:
//
//   ANTIC D  (GR.7) : 160×96  4-colour, 2 scanlines/line → full height, 3840 bytes
//   ANTIC E         : 160×192 4-colour, 1 scanline/line             → 7680 bytes
//   ANTIC F  (GR.8) : 320×192 hi-res 2-colour, 1 scanline/line      → 7680 bytes
//
// The shared buffer is sized for the largest screen (E/F, 7680 B); D uses only the
// first half of it. A small row of 1/2/3 squares at the top tags the current mode.
//
// Each mode packs pixels differently (D/E are 2bpp, F is 1bpp), so Game::gfx() —
// which binds ONE region type — can't serve all three. Instead the demo keeps one
// engine::BitmapOps per mode and points each at its screen's canvas base; the
// software primitive path (has_blitter == false) does the mode-correct packing.
//
// The 192-line E/F buffers (7680 B) cross a 4K scan boundary; the screen manager
// front-aligns each canvas so no mode line straddles it (see engine/screen.h).
//
// ── Setup notes ───────────────────────────────────────────────────────────
//  * Needs no VBXE. Run with BASIC DISABLED so the demo owns RAM. NTSC.
//  * The software view addresses pixels with u8 x, so the 320-wide F mode can only
//    reach x ≤ 255 — its right ~64px stay blank by design.

#include <stdint.h>

#include <engine/core.h>
#include <engine/gfx.h>
#include <engine/platform/atari/platform.h>

using engine::u8;
using engine::u16;
namespace M = atari;

// ── Platform + screens ─────────────────────────────────────────────────────
//
// Baseline graphics axis (no blitter). Three pure-bitmap screens share the buffer;
// GameConfig declares no bitmap_region, so Core's own Game::gfx() stays unbound and
// the demo drives its own per-mode BitmapOps below.
using Platform = M::Platform<
    M::Machine::XL,
    M::RAM::Baseline,
    M::gfx::Baseline,
    M::Sound::Mono,
    M::TV::NTSC>;

struct ScreenD { using display = engine::DisplayLayout<engine::BitmapRegion<M::Mode::BITMAP_D, 96>>;  };
struct ScreenE { using display = engine::DisplayLayout<engine::BitmapRegion<M::Mode::BITMAP_E, 192>>; };
struct ScreenF { using display = engine::DisplayLayout<engine::BitmapRegion<M::Mode::BITMAP_F, 192>>; };

struct GameConfig {
    using screens = engine::ScreenSet<ScreenD, ScreenE, ScreenF>;
    static constexpr u8 max_sprites    = 1;   // unused; Core needs a non-zero sprite set
    static constexpr u8 sound_channels = 1;
};
using Game = engine::Core<Platform, GameConfig>;

// One software bitmap-ops per mode (each owns only a view pointer).
static engine::BitmapOps<Platform, engine::BitmapRegion<M::Mode::BITMAP_D, 96>>  gD;
static engine::BitmapOps<Platform, engine::BitmapRegion<M::Mode::BITMAP_E, 192>> gE;
static engine::BitmapOps<Platform, engine::BitmapRegion<M::Mode::BITMAP_F, 192>> gF;

// ── Palettes (set_color_pf field map: 0-3 = COLPF0-3, 4 = COLBK) ───────────
static void palette_4colour() {
    Platform::hal::set_color_pf(4, 0x90);   // COLBK  : dark blue field  (value 0)
    Platform::hal::set_color_pf(0, 0x0E);   // COLPF0 : white            (value 1)
    Platform::hal::set_color_pf(1, 0x1A);   // COLPF1 : gold             (value 2)
    Platform::hal::set_color_pf(2, 0x44);   // COLPF2 : red              (value 3)
}
static void palette_hires() {
    // ANTIC hi-res (mode F): lit pixels take luminance from COLPF1 and hue from the
    // background; black background + high-luminance COLPF1 ⇒ crisp white ink.
    Platform::hal::set_color_pf(4, 0x00);   // COLBK  : black field
    Platform::hal::set_color_pf(2, 0x00);   // COLPF2 : black (hi-res background hue)
    Platform::hal::set_color_pf(1, 0x0E);   // COLPF1 : white ink (luminance)
}

// ── The picture ────────────────────────────────────────────────────────────
//
// Drawn with whichever mode's BitmapOps is active; geometry scales to the region's
// height (`bottom`) and width (`right`). `bpp` packs the blit source; a, b, c are
// the ink pixel values (in hi-res all three collapse to 1). `tag` (1..3) marks the
// mode with that many small squares.
template <typename G>
static void draw_scene(G& g, u8 right, u8 bottom, u8 bpp, u8 a, u8 b, u8 c, u8 tag) {
    const u8 L = 4, R = static_cast<u8>(right - 4), T = 6, B = static_cast<u8>(bottom - 6);

    g.clear(0);

    // Framed border, inset into the NTSC-safe area.
    g.hline(L, R, T, a);
    g.hline(L, R, B, a);
    g.vline(L, T, B, a);
    g.vline(R, T, B, a);

    // Three colour bars down the left, spaced to the region height.
    const u8 inks[3] = { a, b, c };
    const u8 span = static_cast<u8>((B - T - 8) / 3);
    const u8 barH = static_cast<u8>(span - 6);
    for (u8 i = 0; i < 3; ++i)
        g.fill_rect(12, static_cast<u16>(T + 4 + i * span), 40, barH, inks[i]);

    // Two crossing diagonals on the right (software Bresenham via plot).
    const u8 d0 = static_cast<u8>(right - 90), d1 = static_cast<u8>(right - 12);
    g.line(d0, static_cast<u8>(T + 6), d1, static_cast<u8>(B - 6), b);
    g.line(d1, static_cast<u8>(T + 6), d0, static_cast<u8>(B - 6), c);

    // Dotted accent across the lower band (plot every few pixels).
    for (u8 x = static_cast<u8>(L + 4); x < static_cast<u8>(R - 4); x = static_cast<u8>(x + 6))
        g.plot(x, static_cast<u8>(B - 4), a);

    // A 16×16 chequer, packed at this mode's bpp, then blitted near the right.
    constexpr u8 IMG_W = 16, IMG_H = 16;
    const u8 ppb    = static_cast<u8>(8 / bpp);
    const u8 mask   = static_cast<u8>((1u << bpp) - 1);
    const u8 stride = static_cast<u8>((IMG_W + ppb - 1) / ppb);
    u8 img[IMG_H * 4] = {0};                // 4 = max stride (2bpp); 1bpp uses 2
    for (u8 y = 0; y < IMG_H; ++y)
        for (u8 x = 0; x < IMG_W; ++x) {
            const u8 v  = static_cast<u8>(((x ^ y) >> 2) & mask);
            const u8 sh = static_cast<u8>((ppb - 1 - (x % ppb)) * bpp);
            img[y * stride + x / ppb] = static_cast<u8>(img[y * stride + x / ppb] | (v << sh));
        }
    g.blit(static_cast<u8>(R - 26), static_cast<u8>(T + 8), img, IMG_W, IMG_H);

    // Mode tag: `tag` small squares along the top (1 = D, 2 = E, 3 = F).
    for (u8 i = 0; i < tag; ++i)
        g.fill_rect(static_cast<u16>(72 + i * 12), static_cast<u16>(T + 4), 8, 8, a);
}

// ── Mode activation ────────────────────────────────────────────────────────
//
// Switch the display to the screen for `mode`, set its palette, bind that mode's
// BitmapOps to the screen's (front-aligned) canvas base, and draw the picture.
static void activate(u8 mode) {
    switch (mode) {
        case 0:                                            // ANTIC D — 4-colour, half buffer
            Game::set_screen<ScreenD>([] {});
            palette_4colour();
            gD.attach(Game::screen.canvas_base<ScreenD>());
            draw_scene(gD, 159, 95, 2, 1, 2, 3, 1);
            break;
        case 1:                                            // ANTIC E — 4-colour, full buffer
            Game::set_screen<ScreenE>([] {});
            palette_4colour();
            gE.attach(Game::screen.canvas_base<ScreenE>());
            draw_scene(gE, 159, 191, 2, 1, 2, 3, 2);
            break;
        default:                                           // ANTIC F — hi-res, full buffer
            Game::set_screen<ScreenF>([] {});
            palette_hires();
            gF.attach(Game::screen.canvas_base<ScreenF>());
            draw_scene(gF, 255, 191, 1, 1, 1, 1, 3);
            break;
    }
}

static u8   g_mode      = 0;       // 0 = D, 1 = E, 2 = F
static bool g_prev_fire = false;

static void frame(const engine::Input& in) {
    const bool fire = in.fire();
    if (fire && !g_prev_fire) {                 // rising edge: advance to the next mode
        g_mode = static_cast<u8>((g_mode + 1) % 3);
        activate(g_mode);
    }
    g_prev_fire = fire;
}

int main() {
    Game::init();        // baseline bring-up (initial screen is ScreenD)
    activate(g_mode);    // draw the first mode
    Game::run(frame);
}
