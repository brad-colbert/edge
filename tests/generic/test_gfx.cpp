// test_gfx.cpp — unit tests for engine/gfx.h (BitmapOps).
//
// Two things are checked:
//   1. The BASELINE (software) backend delegates correctly to BitmapRegionView —
//      verified by asserting the 2bpp packed bytes a few primitives produce
//      (mirrors tests/generic/test_screen.cpp's BitmapRegionView asserts).
//   2. The VBXE (blitter) backend COMPILES — BitmapOps<VbxePlatform, void> drives
//      real $D6xx MMIO through the overlay seams, so it can't run here; we force
//      its instantiation by taking the address of a function that exercises it.
//
// Built for mos-sim; main() returns the failure count (0 = pass) for CTest.

#include <stdint.h>
#include <stdio.h>

#include <engine/gfx.h>
#include <engine/platform/atari/platform.h>   // real Atari platforms (caps + HAL)

using engine::u8;
using engine::u16;
using engine::BitmapOps;
using engine::BitmapRegion;

namespace M = atari;

// Baseline (no blitter) and VBXE (blitter) platforms, real HAL.
using BaselinePlatform = M::Platform<
    M::Machine::XL, M::RAM::Baseline, M::gfx::Baseline, M::Sound::Mono, M::TV::NTSC>;
using VbxePlatform = M::Platform<
    M::Machine::XL, M::RAM::U1MB, M::gfx::VBXE<>, M::Sound::Mono, M::TV::NTSC>;

static_assert(!BaselinePlatform::capabilities::has_blitter, "baseline = software gfx path");
static_assert( VbxePlatform::capabilities::has_blitter,     "VBXE = blitter gfx path");

static unsigned g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── 1. Baseline software path: BitmapOps -> BitmapRegionView (2bpp BITMAP_E) ──
//
// BITMAP_E is 2bpp: 4 pixels/byte, leftmost pixel in the high bits, 40 bytes/line.
static void test_software_path() {
    using Canvas = BitmapRegion<M::Mode::BITMAP_E, 4>;   // 40*4 = 160 bytes
    u8 buf[160];
    BitmapOps<BaselinePlatform, Canvas> g;
    g.attach(buf);

    // clear: every byte becomes the colour replicated across all 4 pixels.
    g.clear(0);
    CHECK(buf[0] == 0x00 && buf[159] == 0x00);

    // plot packs into the high bits; a second plot ORs into the next pixel.
    g.plot(0, 0, 3);                 // 0b11000000
    CHECK(buf[0] == 0xC0);
    g.plot(1, 0, 1);                 // 0b00010000 -> 0xD0
    CHECK(buf[0] == 0xD0);

    // fill_rect: a 4-pixel-wide run of colour 3 fills one whole byte (0xFF).
    g.clear(0);
    g.fill_rect(0, 0, 4, 1, 3);
    CHECK(buf[0] == 0xFF);

    // hline of colour 2 across one byte: 0b10 in each pixel -> 0xAA, on row 1 (byte 40).
    g.clear(0);
    g.hline(0, 3, 1, 2);
    CHECK(buf[40] == 0xAA);

    // vline of colour 1 down column 0: high 2 bits of byte 0 on rows 0..2 (0x40 each).
    g.clear(0);
    g.vline(0, 0, 2, 1);
    CHECK(buf[0] == 0x40 && buf[40] == 0x40 && buf[80] == 0x40);

    // line (horizontal here) of colour 3 across one byte -> 0xFF.
    g.clear(0);
    g.line(0, 0, 3, 0, 3);
    CHECK(buf[0] == 0xFF);
}

// ── 2. VBXE blitter path: compile-only (never called; address taken) ──
static void touch_vbxe_gfx() {
    static BitmapOps<VbxePlatform, void> g;   // Region == void: blitter-only canvas
    const u8 img[4] = { 1, 2, 3, 4 };
    g.clear(0);
    g.plot(1, 1, 2);
    g.hline(0, 9, 1, 3);
    g.vline(2, 0, 9, 4);
    g.fill_rect(0, 0, 4, 4, 5);
    g.line(0, 0, 9, 9, 6);
    g.blit(0, 0, img, 2, 2);
}
static void (* volatile g_sink_vbxe_gfx)() = &touch_vbxe_gfx;

int main() {
    test_software_path();
    CHECK(g_sink_vbxe_gfx != nullptr);   // force the VBXE instantiation to be emitted

    if (g_failures == 0) {
        printf("test_gfx: OK\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
