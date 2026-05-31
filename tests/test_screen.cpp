// test_screen.cpp — unit tests for engine/display.h and engine/screen.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// Everything here is platform-independent: the DisplayLayout / ScreenSet
// compile-time geometry, the typed-view pixel/char packing, and the set_screen
// LMS/JVB patching (driven through a MOCK HAL so no real ANTIC is touched). The
// live ANTIC programming in the Atari HAL is verified separately on Fujisan.

#include <stdint.h>
#include <stdio.h>

#include <engine/display.h>
#include <engine/screen.h>
#include <engine/platform/atari/registers.h>   // atari::dmactl bit constants

using engine::u8;
using engine::u16;

using engine::DisplayLayout;
using engine::TextRegion;
using engine::BitmapRegion;
using engine::TextRegionView;
using engine::BitmapRegionView;
using engine::ScreenSet;
using engine::DisplayListTemplate;

namespace M = atari;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records the installed display-list address so the set_screen patch can be
// checked without a real ANTIC.
struct MockHal {
    static const u8* last_dl;
    static u8        sdmctl;      // SDMCTL shadow, RMW'd as the real HAL does

    static void antic_dma_disable() { sdmctl &= M::dmactl::PM_DMA_MASK; }
    static void antic_dma_enable(u8 mode_bits = M::dmactl::PLAYFIELD_NORMAL) {
        sdmctl = static_cast<u8>((sdmctl & M::dmactl::PM_DMA_MASK) |
                                 M::dmactl::DL_ENABLE | mode_bits);
    }
    static void set_display_list(const u8* dl) { last_dl = dl; }
};
const u8* MockHal::last_dl = nullptr;
u8        MockHal::sdmctl  = 0;

struct MockPlatform {
    using hal = MockHal;
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

// ── Compile-time checks: DisplayLayout geometry ───────────────────────

// 1. Single full-screen text region: 40 cols x 24 rows = 960 bytes, 1 region.
using TextLayout = ScreenText::display;
static_assert(TextLayout::region_count == 1, "text layout has 1 region");
static_assert(TextLayout::total_ram == 960,  "40x24 text = 960 bytes");

// 2. Mixed layout: 40 + 7200 + 40 = 7280, offsets 0 / 40 / 7240.
using MixedLayout = ScreenMixed::display;
static_assert(MixedLayout::region_count == 3, "mixed layout has 3 regions");
static_assert(MixedLayout::total_ram == 7280, "40 + 7200 + 40 = 7280");
static_assert(MixedLayout::offset(0) == 0,    "region 0 at offset 0");
static_assert(MixedLayout::offset(1) == 40,   "region 1 after the 40-byte header");
static_assert(MixedLayout::offset(2) == 7240, "region 2 after header + bitmap");

// 3. ScreenSet shared buffer is sized to the largest screen.
using Screens = GameConfig::screens;
static_assert(Screens::screen_count == 2,        "two screens");
static_assert(Screens::max_screen_ram == 7280,   "max(960, 7280) == 7280");

// Mode geometry sanity (the basis for the above).
static_assert(M::bytes_per_line(M::Mode::MODE_2) == 40,   "Mode 2 = 40 cols");
static_assert(M::bytes_per_line(M::Mode::BITMAP_E) == 40, "Mode E = 40 bytes/line");
static_assert(M::bits_per_pixel(M::Mode::BITMAP_E) == 2,  "Mode E is 2bpp");

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

// ── TextRegionView::put_char writes the right offset ───────────────────

static void test_text_view_put_char() {
    u8 buf[256] = {};
    TextRegionView<M::Mode::MODE_2, 24> view{buf};

    // (col=5, row=2) -> 2*40 + 5 = 85.
    view.put_char(5, 2, 0x41);
    CHECK(buf[85] == 0x41);
    CHECK(view.get_char(5, 2) == 0x41);

    // (col=0, row=0) -> 0.
    view.put_char(0, 0, 0x21);
    CHECK(buf[0] == 0x21);

    // print converts ASCII to ANTIC internal codes ('A'=0x41 -> 0x21).
    view.print(0, 1, "A");
    CHECK(buf[40] == 0x21);
}

// ── BitmapRegionView::plot packs 2bpp pixels correctly ─────────────────

static void test_bitmap_view_plot() {
    u8 buf[64] = {};
    BitmapRegionView<M::Mode::BITMAP_E, 180> view{buf};

    // 2bpp, 4 pixels/byte, leftmost pixel in the high bits.
    // plot(0,0,3): top 2 bits -> 0b11000000 = 0xC0.
    view.plot(0, 0, 3);
    CHECK(buf[0] == 0xC0);

    // plot(1,0,1): next 2 bits -> 0b00010000, OR'd in -> 0xD0.
    view.plot(1, 0, 1);
    CHECK(buf[0] == 0xD0);

    // Read-back of both pixels.
    CHECK(view.point(0, 0) == 3);
    CHECK(view.point(1, 0) == 1);

    // A pixel one byte over: plot(4,0,2) -> byte 1 high bits = 0b10000000 = 0x80.
    view.plot(4, 0, 2);
    CHECK(buf[1] == 0x80);
}

// ── DisplayListTemplate inserts an LMS at a 4K page crossing ───────────
//
// Mode E, 192 lines x 40 = 7680 bytes. Based at $4028 the data spans
// $4028..$5F27, crossing exactly one 4K boundary ($5000). The first mode line
// to enter page $5000 starts at $5018 (line 102), so the builder emits a second
// LMS reloading $5018 in addition to the region's own LMS at $4028.
using CrossLayout = engine::DisplayLayout<BitmapRegion<M::Mode::BITMAP_E, 192>>;
static DisplayListTemplate<CrossLayout> g_cross_dl;   // 204 bytes -> static, not stack

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

static DisplayListTemplate<TextLayout> g_nocross_dl;

static void test_no_crossing() {
    // 40x24 text = 960 bytes, page-aligned base -> stays within one 4K page.
    g_nocross_dl.build(0x4000, 0x4000);
    CHECK(g_nocross_dl.lms_count == 1);                 // one LMS, the region's own
    CHECK(g_nocross_dl.lms_count == TextLayout::region_count);
    CHECK(read16(g_nocross_dl.bytes, g_nocross_dl.region_lms_pos[0]) == 0x4000);
}

// ── ScreenManager::set_screen builds the list and rebinds views ────────

static engine::ScreenManager<MockPlatform, GameConfig> g_sm;

static void test_set_screen() {
    bool callback_ran = false;
    g_sm.set_screen<ScreenMixed>([&]() { callback_ran = true; });
    CHECK(callback_ran);

    const u8* dl   = g_sm.active_dl();
    const u16 size = g_sm.active_dl_size();
    const u16 buf  = addr_of(g_sm.buffer());

    // HAL was driven: DMA re-enabled, display list installed at the built list.
    // No P/M here, so SDMCTL is just DL-enable | normal playfield (0x22).
    CHECK(MockHal::last_dl == dl);
    CHECK(MockHal::sdmctl == (M::dmactl::DL_ENABLE | M::dmactl::PLAYFIELD_NORMAL));

    // Region 0's first LMS sits at a fixed position (after 3 blank lines) and
    // reloads buffer+0; region 1's first LMS follows it and reloads buffer+40.
    // (These are deterministic regardless of any later 4K crossings.)
    CHECK(dl[3] == (0x02 | 0x40));                      // Mode 2 | LMS
    CHECK(read16(dl, 4) == buf);
    CHECK(dl[6] == (0x0E | 0x40));                      // Mode E | LMS
    CHECK(read16(dl, 7) == static_cast<u16>(buf + 40));

    // The list ends in JVB looping back on its own address.
    CHECK(dl[size - 3] == 0x41);
    CHECK(read16(dl, size - 2) == addr_of(dl));

    // Typed views bind to their buffer slices (text vs bitmap is compile-time).
    auto& header = g_sm.region<ScreenMixed, 0>();
    auto& field  = g_sm.region<ScreenMixed, 1>();
    CHECK(addr_of(header.ptr) == buf);
    CHECK(addr_of(field.ptr) == static_cast<u16>(buf + 40));

    // And the views actually draw into the shared buffer at the right place.
    field.plot(0, 0, 3);
    CHECK(g_sm.buffer()[40] == 0xC0);
}

int main() {
    test_text_view_put_char();
    test_bitmap_view_plot();
    test_4k_crossing();
    test_no_crossing();
    test_set_screen();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
