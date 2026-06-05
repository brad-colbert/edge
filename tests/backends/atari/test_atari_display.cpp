// test_atari_display.cpp — Atari-backend tests for the ANTIC display program.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// These are the backend-specific counterparts to tests/generic/test_screen.cpp:
// ANTIC mode geometry, the atari::DisplayProgram byte encoding (LMS/JVB opcodes
// and the 4K-scan-boundary reload rule), and the SDMCTL bits set_screen drives.
// The live ANTIC programming is verified separately on Altirra / Fujisan.

#include <stdint.h>
#include <stdio.h>

#include <engine/display.h>
#include <engine/screen.h>
#include <engine/platform/atari/platform.h>
#include <engine/platform/atari/registers.h>   // atari::dmactl bit constants

using engine::u8;
using engine::u16;

using engine::DisplayLayout;
using engine::TextRegion;
using engine::BitmapRegion;
using engine::OverlayRegion;
using engine::ScreenSet;

namespace M = atari;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records the installed display-program address and an SDMCTL shadow RMW'd the
// way the real HAL does, so set_screen's ANTIC byte output and DMA bits can be
// checked without a real ANTIC.
struct MockHal {
    static const u8* last_program;
    static u8        sdmctl;      // SDMCTL shadow, RMW'd as the real HAL does

    static void display_dma_disable() { sdmctl &= M::dmactl::PM_DMA_MASK; }
    static void display_dma_enable(u8 mode_bits = M::dmactl::PLAYFIELD_NORMAL) {
        sdmctl = static_cast<u8>((sdmctl & M::dmactl::PM_DMA_MASK) |
                                 M::dmactl::DL_ENABLE | mode_bits);
    }
    static void set_display_program(const u8* p) { last_program = p; }
};
const u8* MockHal::last_program = nullptr;
u8        MockHal::sdmctl       = 0;

struct MockPlatform {
    using hal = MockHal;
    template <typename Layout>
    using display_program = atari::DisplayProgram<Layout>;
};

// ── Test screens / config ─────────────────────────────────────────────

struct ScreenText {
    using display = DisplayLayout<TextRegion<M::Mode::MODE_2, 24>>;
};

struct ScreenMixed {
    using display = DisplayLayout<
        TextRegion<M::Mode::MODE_2, 1>,
        BitmapRegion<M::Mode::BITMAP_E, 180>,
        TextRegion<M::Mode::MODE_2, 1>
    >;
};

struct GameConfig {
    using screens = ScreenSet<ScreenText, ScreenMixed>;
};

using TextLayout = ScreenText::display;

// ── ANTIC mode geometry (the basis for the layout math) ────────────────

static_assert(M::bytes_per_line(M::Mode::MODE_2) == 40,   "Mode 2 = 40 cols");
static_assert(M::bytes_per_line(M::Mode::BITMAP_E) == 40, "Mode E = 40 bytes/line");
static_assert(M::bits_per_pixel(M::Mode::BITMAP_E) == 2,  "Mode E is 2bpp");

// ── VBXE overlay mode geometry (unified into atari::Mode, see modes.h) ──

static_assert(M::is_vbxe(M::Mode::VBXE_T80),              "VBXE_T80 is a VBXE mode");
static_assert(!M::is_vbxe(M::Mode::MODE_2),              "MODE_2 is not a VBXE mode");
static_assert(M::bytes_per_line(M::Mode::VBXE_SR) == 320, "VBXE_SR = 320 bytes/line");
static_assert(M::bytes_per_line(M::Mode::VBXE_HR) == 320, "VBXE_HR = 320 bytes/line (640px 4bpp)");
static_assert(M::bytes_per_line(M::Mode::VBXE_LR) == 160, "VBXE_LR = 160 bytes/line");
static_assert(M::bytes_per_line(M::Mode::VBXE_T80) == 160,"VBXE_T80 = 160 bytes/line (80 char+attr)");
static_assert(M::bits_per_pixel(M::Mode::VBXE_SR) == 8,   "VBXE_SR is 8bpp");
static_assert(M::bits_per_pixel(M::Mode::VBXE_HR) == 4,   "VBXE_HR is 4bpp");
static_assert(M::is_text(M::Mode::VBXE_T80),              "VBXE_T80 is text");
static_assert(!M::is_text(M::Mode::VBXE_SR),             "VBXE_SR is not text");
static_assert(M::scanlines_per_line(M::Mode::VBXE_SR) == 1, "VBXE modes are 1 scanline/line");
static_assert(M::dl_mode_byte(M::Mode::VBXE_SR) == 0,    "VBXE modes have no ANTIC DL byte");

// ── Runtime harness ────────────────────────────────────────────────────

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static u16 addr_of(const void* p) {
    return static_cast<u16>(reinterpret_cast<uintptr_t>(p));
}

static u16 read16(const u8* b, u16 i) {
    return static_cast<u16>(b[i]) | (static_cast<u16>(b[i + 1]) << 8);
}

// ── DisplayProgram inserts an LMS at a 4K page crossing ────────────────
//
// Mode E, 192 lines x 40 = 7680 bytes. Based at $4028 the data spans
// $4028..$5F27, crossing exactly one 4K boundary ($5000). The first mode line
// to enter page $5000 starts at $5018 (line 102), so the builder emits a second
// LMS reloading $5018 in addition to the region's own LMS at $4028.
using CrossLayout = DisplayLayout<BitmapRegion<M::Mode::BITMAP_E, 192>>;
static atari::DisplayProgram<CrossLayout> g_cross_dl;   // 204 bytes -> static, not stack

static void test_4k_crossing() {
    g_cross_dl.build(0x4028, 0x4028);

    // Exactly two LMS: region start + one boundary crossing.
    CHECK(g_cross_dl.lms_count == 2);

    // Region's first LMS reloads the base $4028 (opcode = Mode E | LMS = $4E).
    const u16 p0 = g_cross_dl.region_lms_pos[0];
    CHECK(g_cross_dl.bytes[p0 - 1] == 0x4E);
    CHECK(read16(g_cross_dl.bytes, p0) == 0x4028);

    // The crossing LMS reloads $5018 (first line in page $5000).
    const u16 p1 = g_cross_dl.lms_pos[1];
    CHECK(g_cross_dl.bytes[p1 - 1] == 0x4E);
    CHECK(read16(g_cross_dl.bytes, p1) == 0x5018);
}

// ── No crossing when a region fits within a 4K page ────────────────────

static atari::DisplayProgram<TextLayout> g_nocross_dl;

static void test_no_crossing() {
    // 40x24 text = 960 bytes, page-aligned base -> stays within one 4K page.
    g_nocross_dl.build(0x4000, 0x4000);
    CHECK(g_nocross_dl.lms_count == 1);                 // one LMS, the region's own
    CHECK(g_nocross_dl.lms_count == TextLayout::region_count);
    CHECK(read16(g_nocross_dl.bytes, g_nocross_dl.region_lms_pos[0]) == 0x4000);
}

// ── set_screen emits the right ANTIC bytes + SDMCTL bits ───────────────

static engine::ScreenManager<MockPlatform, GameConfig> g_sm;

static void test_set_screen_bytes() {
    g_sm.set_screen<ScreenMixed>([]() {});

    const u8* dl   = g_sm.active_dl();
    const u16 size = g_sm.active_dl_size();
    const u16 buf  = addr_of(g_sm.buffer());

    // No P/M here, so SDMCTL is just DL-enable | normal playfield (0x22).
    CHECK(MockHal::last_program == dl);
    CHECK(MockHal::sdmctl == (M::dmactl::DL_ENABLE | M::dmactl::PLAYFIELD_NORMAL));

    // Region 0's first LMS sits after 3 blank lines and reloads buffer+0; region 1's
    // first LMS follows it and reloads buffer+40 (deterministic regardless of any
    // later 4K crossings).
    CHECK(dl[3] == (0x02 | 0x40));                      // Mode 2 | LMS
    CHECK(read16(dl, 4) == buf);
    CHECK(dl[6] == (0x0E | 0x40));                      // Mode E | LMS
    CHECK(read16(dl, 7) == static_cast<u16>(buf + 40));

    // The list ends in JVB looping back on its own address.
    CHECK(dl[size - 3] == 0x41);
    CHECK(read16(dl, size - 2) == addr_of(dl));
}

// ── Overlay regions: pure-overlay JVB stub ─────────────────────────────
//
// A layout of only OverlayRegions drives no ANTIC mode lines — its pixels live
// in VBXE VRAM and ANTIC DMA is disabled by the screen manager. The display
// list collapses to a 3-byte self-looping JVB that only satisfies DLISTL.
using PureOverlayLayout = DisplayLayout<OverlayRegion<M::Mode::VBXE_SR, 240>>;
static atari::DisplayProgram<PureOverlayLayout> g_pure_overlay_dl;

static void test_pure_overlay() {
    // Capacity collapses to the 3-byte stub regardless of overlay height.
    CHECK(atari::DisplayProgram<PureOverlayLayout>::capacity == 3);

    g_pure_overlay_dl.build(0x4000, 0x3000);
    CHECK(g_pure_overlay_dl.size == 3);
    CHECK(g_pure_overlay_dl.bytes[0] == 0x41);          // DL_JVB
    CHECK(read16(g_pure_overlay_dl.bytes, 1) == 0x3000); // loops the list itself
    CHECK(g_pure_overlay_dl.lms_count == 0);
    CHECK(g_pure_overlay_dl.scroll_lms_count == 0);
}

// ── Overlay regions: overlay above ANTIC text ──────────────────────────
//
// A 200-scanline overlay precedes a 3-row Mode 2 text region. The overlay
// reserves its scanlines with blank-line instructions (ceil(200/8)=25) and
// emits NO LMS; the text region then emits its mode lines below it.
using MixedLayout =
    DisplayLayout<OverlayRegion<M::Mode::VBXE_SR, 200>, TextRegion<M::Mode::MODE_2, 3>>;
static atari::DisplayProgram<MixedLayout> g_mixed_dl;

static void test_overlay_above_antic() {
    g_mixed_dl.build(0x4000, 0x3000);

    // 3 leading 8-line blanks + 25 overlay blanks = bytes 0..27, all blank instrs.
    for (u16 i = 0; i < 28; ++i) CHECK(g_mixed_dl.bytes[i] == M::dl_blank(8));

    // The text region's LMS opcode (Mode 2 | LMS = $42) sits at byte 28, so its
    // recorded low-byte index is 29.
    CHECK(g_mixed_dl.bytes[28] == 0x42);
    CHECK(g_mixed_dl.region_lms_pos[1] == 29);

    // Only the text region contributes an LMS; the overlay contributes none.
    CHECK(g_mixed_dl.lms_count == 1);
    CHECK(g_mixed_dl.scroll_lms_count == 0);

    // Overlay region has the no-LMS sentinel; text LMS reloads buffer+0 (the
    // overlay contributes 0 ram_bytes, so the text offset is unchanged).
    CHECK(g_mixed_dl.region_lms_pos[0] == 0);
    CHECK(read16(g_mixed_dl.bytes, g_mixed_dl.region_lms_pos[1]) == 0x4000);

    // List still ends in a JVB looping on its own address.
    CHECK(g_mixed_dl.bytes[g_mixed_dl.size - 3] == 0x41);
    CHECK(read16(g_mixed_dl.bytes, g_mixed_dl.size - 2) == 0x3000);
}

// ── Overlay regions: overlay below ANTIC text ──────────────────────────
//
// Reversing the region order puts the text first and the overlay below it,
// proving that region order controls the overlay's vertical position.
using OverlayBelowLayout =
    DisplayLayout<TextRegion<M::Mode::MODE_2, 3>, OverlayRegion<M::Mode::VBXE_SR, 200>>;
static atari::DisplayProgram<OverlayBelowLayout> g_overlay_below_dl;

static void test_overlay_below_antic() {
    g_overlay_below_dl.build(0x4000, 0x3000);

    // Bytes 0..2 are the leading centring blanks.
    for (u16 i = 0; i < 3; ++i) CHECK(g_overlay_below_dl.bytes[i] == M::dl_blank(8));

    // Text region first: LMS opcode at byte 3, low-byte index 4, reloading buffer+0.
    CHECK(g_overlay_below_dl.bytes[3] == 0x42);
    CHECK(g_overlay_below_dl.region_lms_pos[0] == 4);
    CHECK(read16(g_overlay_below_dl.bytes, 4) == 0x4000);

    // 3 text rows: 1 LMS (3 bytes) + 2 plain mode bytes -> first overlay blank at
    // byte 8. The overlay then emits 25 blank instructions (bytes 8..32).
    CHECK(g_overlay_below_dl.region_lms_pos[1] == 0);   // overlay sentinel
    for (u16 i = 8; i < 33; ++i) CHECK(g_overlay_below_dl.bytes[i] == M::dl_blank(8));

    // Only the text region's LMS.
    CHECK(g_overlay_below_dl.lms_count == 1);

    // JVB follows the overlay blanks.
    CHECK(g_overlay_below_dl.bytes[33] == 0x41);
    CHECK(read16(g_overlay_below_dl.bytes, 34) == 0x3000);
    CHECK(g_overlay_below_dl.size == 36);
}

int main() {
    test_4k_crossing();
    test_no_crossing();
    test_set_screen_bytes();
    test_pure_overlay();
    test_overlay_above_antic();
    test_overlay_below_antic();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
