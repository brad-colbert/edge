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

int main() {
    test_4k_crossing();
    test_no_crossing();
    test_set_screen_bytes();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
