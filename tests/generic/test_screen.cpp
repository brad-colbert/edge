// test_screen.cpp — generic unit tests for engine/display.h and engine/screen.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// These assertions are backend-neutral: the DisplayLayout / ScreenSet compile-time
// geometry, the typed-view pixel/char packing, and the set_screen view-rebind +
// callback behaviour (driven through a MOCK HAL so no real display is touched). The
// backend display-program byte encoding (opcodes, 4K-crossing reloads, DMA bits) is
// asserted separately in tests/backends/atari/test_atari_display.cpp.
//
// The Atari backend is the only one that exists today, so its mode tokens
// (atari::Mode) and display-program builder are named here as the concrete backend
// — but every assertion below is about the engine's portable behaviour, not any
// ANTIC byte.

#include <stdint.h>
#include <stdio.h>

#include <engine/display.h>
#include <engine/screen.h>
#include <engine/platform/atari/platform.h>   // backend mode trait + display-program builder

using engine::u8;
using engine::u16;

using engine::DisplayLayout;
using engine::TextRegion;
using engine::BitmapRegion;
using engine::TextRegionView;
using engine::BitmapRegionView;
using engine::ScreenSet;

namespace M = atari;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records the installed display-program address and that DMA was cycled, so
// set_screen's portable steps can be checked without a real display backend.
struct MockHal {
    static const u8* last_program;
    static bool      dma_enabled;

    static void display_dma_disable() { dma_enabled = false; }
    static void display_dma_enable()  { dma_enabled = true; }
    static void set_display_program(const u8* p) { last_program = p; }
};
const u8* MockHal::last_program = nullptr;
bool      MockHal::dma_enabled  = false;

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
static_assert(Screens::screen_count == 2,      "two screens");
static_assert(Screens::max_screen_ram == 7280, "max(960, 7280) == 7280");

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

    // print converts ASCII to screen codes through the backend trait ('A' -> 0x21).
    view.print(0, 1, "A");
    CHECK(buf[40] == engine::display::traits<M::Mode>::to_screen_code('A'));
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

// ── ScreenManager::set_screen builds the program and rebinds views ─────

static engine::ScreenManager<MockPlatform, GameConfig> g_sm;

static void test_set_screen() {
    bool callback_ran = false;
    g_sm.set_screen<ScreenMixed>([&]() { callback_ran = true; });
    CHECK(callback_ran);

    const u8* program = g_sm.active_dl();
    const u16 buf     = addr_of(g_sm.buffer());

    // The portable steps ran: DMA re-enabled, a display program installed, and the
    // active program is what the HAL received.
    CHECK(MockHal::dma_enabled);
    CHECK(MockHal::last_program == program);
    CHECK(program != nullptr);
    CHECK(g_sm.active_dl_size() > 0);

    // Typed views bind to their buffer slices (text vs bitmap is compile-time):
    // region 0 at buffer+0, region 1 at buffer+40 (after the 40-byte header).
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
    test_set_screen();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
