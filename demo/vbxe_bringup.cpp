// demo/vbxe_bringup.cpp — Edge engine VBXE Phase 4a bring-up smoke test.
//
// A minimal program that brings the VBXE overlay up through the portable engine
// and paints a diagnostic pattern, producing a loadable .xex (build with the
// mos-atari8-dos target; see CMakeLists.txt). Run it on Altirra (or hardware)
// with a VBXE FX core to confirm the Phase 1-4a plumbing works end to end:
// MEMAC window config, framebuffer writes through MEMAC, the full-screen XDL
// upload + address registers, palette upload, and VIDEO_CONTROL enable.
//
// Expected result: eight horizontal colour bars filling the screen (a 320x240
// SR-mode overlay drawn over ANTIC at top priority). If instead you see a normal
// ANTIC text screen, the overlay isn't compositing; if you see a black screen,
// the framebuffer writes aren't reaching VRAM (check the points below).
//
// ── Setup notes (read before running) ─────────────────────────────────────
//  * VBXE register base is $D640 (the engine default). A $D740 board needs a
//    Config override: gfx::VBXE<vbxe::Config<...RegBase::D740...>>.
//  * Run with BASIC DISABLED. The default MEMAC-A window is $B000-$BFFF; with
//    BASIC enabled that range is ROM and the CPU's framebuffer writes won't reach
//    VRAM (ROM has priority over the MEMAC window). On Altirra: hold OPTION at
//    boot / disable the BASIC ROM.
//  * Altirra: enable the VBXE device with the FX core (not the GTIA-emu core).
//
// ── Pure engine API (+ documented power-user header) ──────────────────────
// Bring-up and the frame loop are pure engine::Core. The diagnostic paint uses
// the atari::vbxe power-user header directly (palette upload + MEMAC framebuffer
// writes) — that is the intended escape hatch for raw VRAM access; the engine
// itself never paints demo content into the overlay.

#include <stdint.h>

#include <engine/core.h>
#include <engine/platform/atari/platform.h>
#include <engine/platform/atari/vbxe.h>   // power-user: MemacWindow, palette, layout

using engine::u8;
using engine::u16;
using engine::u32;
namespace M = atari;
namespace V = atari::vbxe;

// Diagnostic mode: 0 = colour bars, 1 = solid fill, 2 = VRAM round-trip probe,
// 3 = two animated sprites (blitter). Override at configure time with
// `cmake -DEDGE_VBXE_BRINGUP_MODE=N` (passes -DVBXE_BRINGUP_MODE=N); default 0.
// Defined here (before the Config typedef) because it selects the buffer policy.
#ifndef VBXE_BRINGUP_MODE
#define VBXE_BRINGUP_MODE 0
#endif

// ── Platform + game configuration ────────────────────────────────────────
//
// VBXE config: SR_320, $D640, MEMAC-A $B000/4K. The buffer policy tracks the
// diagnostic mode. MODE 3 (sprites) is DOUBLE-buffered for flicker-free animation
// (render the hidden page, then flip). The static modes (0/1/2) MUST be
// SINGLE-buffered: the engine's frame service flips pages every VBI in
// double-buffer mode, and since those modes only paint fb_a once, a flip would
// show the unpainted (transparent, index 0) back page — the ANTIC playfield reads
// through and the painted content appears to vanish.
#if VBXE_BRINGUP_MODE == 3
using Cfg = V::Config<V::Mode::SR_320, V::Buffers::Double>;   // sprite demo: flip pages
#else
using Cfg = V::Config<V::Mode::SR_320, V::Buffers::Single>;   // static paint: no flip
#endif
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
    static constexpr u8 max_sprites    = 4;
    static constexpr u8 sound_channels = 1;
};

using Game = engine::Core<Platform, GameConfig>;

// Sprite-demo shapes (MODE 3). kBall is a Packed1bpp P/M-style 8x8 bitmap
// (coloured per-instance via sprite_color); kPix is a Pixel8bpp 8x8 shape whose
// rows carry palette-1 indices 1..8 directly (index 0 = transparent).
[[maybe_unused]] constexpr auto kBall = engine::make_sprite<8, 8>({
    0b00111100, 0b01111110, 0b11111111, 0b11111111,
    0b11111111, 0b11111111, 0b01111110, 0b00111100,
});
[[maybe_unused]] constexpr auto kPix = engine::make_pixel_sprite<8, 8>({
    1,1,1,1,1,1,1,1,  2,2,2,2,2,2,2,2,
    3,3,3,3,3,3,3,3,  4,4,4,4,4,4,4,4,
    5,5,5,5,5,5,5,5,  6,6,6,6,6,6,6,6,
    7,7,7,7,7,7,7,7,  8,8,8,8,8,8,8,8,
});

// Eight vivid, fully-saturated colours loaded into palette 1 (the overlay's
// default palette), indices 1..8. Components are 7-bit values in bits 7..1, so
// 0xFE is full-on and 0x00 off. Indices are non-zero, so every bar is opaque
// (index 0 is transparent in SR mode).
static void load_test_palette() {
    V::set_color<Cfg>(1, 1, 0xFE, 0x00, 0x00);   // red
    V::set_color<Cfg>(1, 2, 0x00, 0xFE, 0x00);   // green
    V::set_color<Cfg>(1, 3, 0x00, 0x00, 0xFE);   // blue
    V::set_color<Cfg>(1, 4, 0xFE, 0xFE, 0x00);   // yellow
    V::set_color<Cfg>(1, 5, 0x00, 0xFE, 0xFE);   // cyan
    V::set_color<Cfg>(1, 6, 0xFE, 0x00, 0xFE);   // magenta
    V::set_color<Cfg>(1, 7, 0xFE, 0x80, 0x00);   // orange
    V::set_color<Cfg>(1, 8, 0xFE, 0xFE, 0xFE);   // white
    V::set_color<Cfg>(1, 9, 0x00, 0x00, 0x40);   // index 9 = dark blue (MODE 3 background)
}

// Fill the whole framebuffer with one opaque colour index. The cleanest possible
// overlay diagnostic: a perfectly uniform screen means the MEMAC fill (including
// every 4KB bank crossing) covered all 320x240 bytes with no gaps.
[[maybe_unused]] static void fill_solid(u8 color) {
    V::MemacWindow<Cfg>::fill(V::VRAMLayout<Cfg>::fb_a,
                              static_cast<u32>(Cfg::fb_width) * 240u, color);
}

// Paint eight horizontal colour bars (indices 1..8) into framebuffer A.
[[maybe_unused]] static void paint_test_bars() {
    using Layout = V::VRAMLayout<Cfg>;
    using Memac  = V::MemacWindow<Cfg>;

    constexpr u16 stride = static_cast<u16>(Cfg::fb_width);  // 320 bytes/line
    constexpr u8  bands  = 8;
    constexpr u8  band_h = 240 / bands;                      // 30 lines each

    for (u8 b = 0; b < bands; ++b) {
        const u8  color = static_cast<u8>(b + 1);            // 1..8 (opaque, vivid)
        const u32 start = Layout::fb_a + static_cast<u32>(b) * band_h * stride;
        Memac::fill(start, static_cast<u32>(band_h) * stride, color);
    }
}

// Blank the text region (the shared screen buffer isn't cleared on set_screen,
// so wipe it before printing the probe result over a transparent overlay).
[[maybe_unused]] static void clear_text() {
    for (u8 r = 0; r < 24; ++r)
        for (u8 c = 0; c < 40; ++c)
            Game::put_char(c, r, 0);          // ANTIC internal code 0 = blank
}

// VRAM round-trip self-test: write a sentinel across several MEMAC bank
// boundaries in a non-displayed VRAM region, read it back, and count mismatches.
// The MEMAC helpers are now internally VBI-atomic (NmiGuard), so with the VBI
// running this must report 0 — that is the regression gate for the interleave
// fix. (No manual NMI masking here: the engine owns NMIEN now.)
[[maybe_unused]] static void run_probe() {
    using Memac = V::MemacWindow<Cfg>;
    constexpr u32 SCRATCH = 0x40000;   // mid-VRAM, away from framebuffer/XDL/fonts
    constexpr u32 LEN     = 12288;     // 3 banks -> crosses 2 bank boundaries

    Memac::fill(SCRATCH, LEN, 0x00);   // clear
    Memac::fill(SCRATCH, LEN, 0x55);   // write sentinel

    u16 bad = 0;
    u16 first_off = 0xFFFF;
    for (u32 i = 0; i < LEN; ++i) {
        u8 v = 0;
        Memac::read(SCRATCH + i, &v, 1);
        if (v != 0x55 && first_off == 0xFFFF) first_off = static_cast<u16>(i);
        if (v != 0x55) ++bad;
    }

    // Bank-register readback sanity ($80 MGE | 5 = 133).
    Memac::set_bank(5);
    const u8 bank_rb = static_cast<u8>(V::Regs<Cfg>::MEMAC_BANK_SEL);

    clear_text();
    Game::print(0, 0, "VBXE VRAM ROUNDTRIP PROBE");
    Game::print(0, 2, "MISMATCHES (of 12288):");
    Game::print_num(0, 3, bad, 5);
    Game::print(0, 5, "FIRST BAD OFFSET:");
    Game::print_num(0, 6, (first_off == 0xFFFF) ? 0 : first_off, 5);
    Game::print(0, 8, "BANK READBACK (want 133):");
    Game::print_num(0, 9, bank_rb, 5);
    Game::print(0, 11, (bad == 0) ? "RESULT: CLEAN - VBI ATOMIC"
                                  : "RESULT: STILL CORRUPT");
}

int main() {
    // Bring up the overlay: MEMAC window, cleared (transparent) framebuffer,
    // full-screen XDL, XDL enabled. (Core::init dispatches to the VBXE path
    // because Platform::capabilities::has_blitter is true.)
    Game::init();
    load_test_palette();

    // The MEMAC/palette helpers are internally VBI-atomic (NmiGuard), so no manual
    // NMI masking is needed here — the VBXE's $D6xx register accesses in the VBI
    // can't interleave a $B000 window burst.
#if VBXE_BRINGUP_MODE == 3
    // Double-buffered sprite demo: two sprites bouncing horizontally over an opaque
    // overlay (so the ANTIC backdrop is fully hidden). The engine presents + flips
    // automatically each frame (compose the hidden page, then show it) — flicker-free.
    Game::set_overlay_background(9);   // opaque dark-blue field (palette-1 index 9)
    Game::sprite_color(0, 1);          // kBall coloured palette-1 index 1 (red), sticky
    Game::run([](const auto&) {
        static u16 t = 0; ++t;
        const u8 x0 = static_cast<u8>(40 + (t % 200));
        const u8 x1 = static_cast<u8>(40 + ((t + 100) % 200));
        Game::sprite(0, kBall, x0, 100);
        Game::sprite(1, kPix,  x1, 140);
    });
#else
#  if VBXE_BRINGUP_MODE == 2
    run_probe();            // overlay stays transparent; text result shows through
#  elif VBXE_BRINGUP_MODE == 1
    fill_solid(1);          // solid red over the whole overlay
#  else
    paint_test_bars();      // eight vivid colour bars
#  endif
    // Idle loop — the frame service runs each VBI; nothing else to do.
    Game::run([](const auto&) {});
#endif
}
