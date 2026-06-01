// test_atari_scroll.cpp — Atari-backend tests for hardware scrolling.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// The portable split math lives in tests/generic/test_scroll.cpp. Here we test
// the ANTIC-specific half: that atari::DisplayProgram emits one LMS per visible
// scroll line (with the HSCROLL/VSCROLL bits) striding by the map width, that
// patch_scroll() rewrites those addresses for a coarse offset, and that
// ScreenManager::apply_scroll() drives it end-to-end (including suspend gating)
// while a non-scroll region in the same layout still emits a single LMS.

#include <stdint.h>
#include <stdio.h>

#include <engine/display.h>
#include <engine/screen.h>
#include <engine/scroll.h>
#include <engine/platform/atari/platform.h>
#include <engine/platform/atari/registers.h>

using engine::u8;
using engine::u16;

using engine::DisplayLayout;
using engine::TextRegion;
using engine::ScrollRegion;
using engine::ScreenSet;
using engine::ScreenManager;
using engine::ScrollManager;

namespace M = atari;

// ── Mock platform ─────────────────────────────────────────────────────

struct MockHal {
    static const u8* last_program;
    static u8        sdmctl;
    static u8        hscrol;
    static u8        vscrol;
    static unsigned  fine_writes;

    static void display_dma_disable() { sdmctl &= M::dmactl::PM_DMA_MASK; }
    static void display_dma_enable(u8 mode_bits = M::dmactl::PLAYFIELD_NORMAL) {
        sdmctl = static_cast<u8>((sdmctl & M::dmactl::PM_DMA_MASK) |
                                 M::dmactl::DL_ENABLE | mode_bits);
    }
    static void set_display_program(const u8* p) { last_program = p; }
    static void set_fine_scroll_x(u8 v) { hscrol = v; ++fine_writes; }
    static void set_fine_scroll_y(u8 v) { vscrol = v; ++fine_writes; }
};
const u8* MockHal::last_program = nullptr;
u8        MockHal::sdmctl       = 0;
u8        MockHal::hscrol       = 0;
u8        MockHal::vscrol       = 0;
unsigned  MockHal::fine_writes  = 0;

struct MockPlatform {
    using hal = MockHal;
    template <typename Layout>
    using display_program = atari::DisplayProgram<Layout>;
};

// ── Test screen: a 2-row HUD over a 22-row window into a 64x32 map ──────

static constexpr u16 MAP_W = 64;
static constexpr u16 MAP_H = 32;
static constexpr u8  VIS   = 22;

struct ScrollScreen {
    using display = DisplayLayout<
        TextRegion<M::Mode::MODE_2, 2>,
        ScrollRegion<TextRegion<M::Mode::MODE_2, VIS>, MAP_W, MAP_H>>;
};

struct GameConfig {
    using screens = ScreenSet<ScrollScreen>;
};

using Layout = ScrollScreen::display;

// Scroll-line opcode: Mode 2 | LMS | HSCROLL | VSCROLL.
static constexpr u8 SCROLL_OP =
    static_cast<u8>(0x02 | M::DL_LMS | M::DL_HSCROLL | M::DL_VSCROLL);

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

// ── Compile-time layout metadata ───────────────────────────────────────

static_assert(Layout::has_scroll, "layout must report a scroll region");
static_assert(Layout::scroll_region_index() == 1, "scroll region is index 1");
static_assert(Layout::region_map_width[1] == MAP_W, "map width recorded");
static_assert(Layout::region_map_height[1] == MAP_H, "map height recorded");
static_assert(!Layout::region_is_scroll[0], "HUD region is not scroll");

// ── DisplayProgram emits one LMS per visible scroll line ───────────────

static atari::DisplayProgram<Layout> g_dl;   // ~80 bytes -> static, not stack

static void test_scroll_build() {
    g_dl.build(0x4000, 0x4000);

    // 22 scroll LMS + 1 for the HUD region = 23 total (no 4K crossing here).
    CHECK(g_dl.scroll_lms_count == VIS);
    CHECK(g_dl.lms_count == VIS + 1);

    // Every scroll line carries LMS | HSCROLL | VSCROLL.
    for (u16 i = 0; i < g_dl.scroll_lms_count; ++i) {
        const u16 pos = g_dl.scroll_lms_pos[i];
        CHECK(g_dl.bytes[pos - 1] == SCROLL_OP);
    }
    // The scroll region's first LMS is recorded as its region LMS.
    CHECK(g_dl.region_lms_pos[1] == g_dl.scroll_lms_pos[0]);

    // The HUD region (index 0) still emits a single plain LMS (Mode 2 | LMS).
    CHECK(g_dl.bytes[g_dl.region_lms_pos[0] - 1] == (0x02 | M::DL_LMS));
}

// ── patch_scroll repoints each line by the map-width stride ────────────

static void test_patch_scroll() {
    g_dl.build(0x4000, 0x4000);
    g_dl.patch_scroll(0x8000, MAP_W, /*col*/ 3, /*row*/ 2);

    for (u16 i = 0; i < g_dl.scroll_lms_count; ++i) {
        const u16 expect = static_cast<u16>(0x8000 + (2 + i) * MAP_W + 3);
        CHECK(read16(g_dl.bytes, g_dl.scroll_lms_pos[i]) == expect);
    }
}

// ── ScreenManager.apply_scroll drives the live list + suspend gating ───

static ScreenManager<MockPlatform, GameConfig> g_sm;
static u8 g_map[MAP_W * MAP_H];

// First scroll-line load address in the live display list (the first opcode that
// has the scroll bits set), or 0 if none found.
static u16 first_scroll_addr() {
    const u8* dl = g_sm.active_dl();
    const u16 sz = g_sm.active_dl_size();
    for (u16 k = 0; k + 2 < sz; ++k)
        if (dl[k] == SCROLL_OP) return read16(dl, k + 1);
    return 0;
}

static void test_apply_scroll() {
    g_sm.set_screen<ScrollScreen>([]() {});

    ScrollManager<MockPlatform> scroll;
    g_sm.bind_scroll_map<ScrollScreen>(scroll, g_map, MAP_W);

    const u16 map = addr_of(g_map);
    CHECK(scroll.active());
    CHECK(first_scroll_addr() == map);             // bound at coarse (0,0)

    // Scroll to (20,17): hscrol 0, vscrol 1, coarse col 5, row 2.
    scroll.set(20, 17);
    MockHal::fine_writes = 0;
    g_sm.apply_scroll(scroll);
    CHECK(MockHal::hscrol == 0);
    CHECK(MockHal::vscrol == 1);
    CHECK(MockHal::fine_writes == 2);
    CHECK(first_scroll_addr() == static_cast<u16>(map + 2 * MAP_W + 5));

    // Suspended: apply_scroll writes nothing and leaves the list unchanged.
    scroll.suspend();
    scroll.set(40, 33);
    MockHal::fine_writes = 0;
    g_sm.apply_scroll(scroll);
    CHECK(MockHal::fine_writes == 0);
    CHECK(first_scroll_addr() == static_cast<u16>(map + 2 * MAP_W + 5));
}

int main() {
    test_scroll_build();
    test_patch_scroll();
    test_apply_scroll();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
