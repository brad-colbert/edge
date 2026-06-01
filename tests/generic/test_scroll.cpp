// test_scroll.cpp — unit tests for engine/scroll.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// Everything here is platform-independent: the fine/coarse split, the LMS
// display-list patch, and the suspend/activate gating are driven through a MOCK
// HAL so no real ANTIC is touched. The live ANTIC scroll registers in the Atari
// HAL (and the hardware scroll direction) are verified separately on Fujisan.

#include <stdint.h>
#include <stdio.h>

#include <engine/scroll.h>

using engine::u8;
using engine::u16;
using engine::ScrollManager;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records the last fine-scroll values written and how many writes occurred
// (so suspend/inactive can be checked by write count).
struct MockHal {
    static u8       hscrol;
    static u8       vscrol;
    static unsigned hscrol_writes;
    static unsigned vscrol_writes;

    static void set_fine_scroll_x(u8 v) { hscrol = v; ++hscrol_writes; }
    static void set_fine_scroll_y(u8 v) { vscrol = v; ++vscrol_writes; }

    static void reset() {
        hscrol = vscrol = 0;
        hscrol_writes = vscrol_writes = 0;
    }
};
u8       MockHal::hscrol        = 0;
u8       MockHal::vscrol        = 0;
unsigned MockHal::hscrol_writes = 0;
unsigned MockHal::vscrol_writes = 0;

struct MockPlatform {
    using hal = MockHal;
};

// ── Runtime harness ────────────────────────────────────────────────────

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static u16 read16(const u8* b, u16 i) {
    return static_cast<u16>(b[i]) | (static_cast<u16>(b[i + 1]) << 8);
}

// ── Shared test geometry ───────────────────────────────────────────────
//
// A minimal display list holding one LMS instruction (Mode 4 | LMS, then its
// 2-byte address) initialised to point at the tilemap origin. The low byte of
// the address sits at index 1, so lms_pos == 1.
static constexpr u16 SCREEN_BASE = 0x4000;   // tilemap origin (address value)
static constexpr u16 MAP_WIDTH   = 64;       // full row stride, > bytes_per_line
static constexpr u16 LMS_POS     = 1;

// Geometry the screen manager would pass for a Mode-4 scroll region:
//   bytes_per_line = 40, scanlines_per_line = 8, fine_scroll_range = 16.
static constexpr u8 BPL   = 40;
static constexpr u8 SPLPL = 8;
static constexpr u8 FSR   = 16;

static u8* screen_base() { return reinterpret_cast<u8*>(SCREEN_BASE); }

static void init_dl(u8* dl) {
    dl[0] = static_cast<u8>(0x04 | 0x40);     // Mode 4 | DL_LMS
    dl[1] = static_cast<u8>(SCREEN_BASE & 0xFF);
    dl[2] = static_cast<u8>(SCREEN_BASE >> 8);
}

// ── Zero scroll: registers cleared, LMS unchanged ──────────────────────

static void test_zero() {
    u8 dl[3];
    init_dl(dl);
    MockHal::reset();

    ScrollManager<MockPlatform> sm;
    sm.activate(BPL, SPLPL, FSR);
    sm.set(0, 0);
    sm.apply(dl, LMS_POS, screen_base(), MAP_WIDTH);

    CHECK(MockHal::hscrol == 0);
    CHECK(MockHal::vscrol == 0);
    CHECK(read16(dl, LMS_POS) == SCREEN_BASE);   // coarse 0 -> base, unchanged
}

// ── Small scroll: fine only, no coarse step ────────────────────────────

static void test_fine_only() {
    u8 dl[3];
    init_dl(dl);
    MockHal::reset();

    ScrollManager<MockPlatform> sm;
    sm.activate(BPL, SPLPL, FSR);
    sm.set(3, 5);
    sm.apply(dl, LMS_POS, screen_base(), MAP_WIDTH);

    CHECK(MockHal::hscrol == 3);                 // 3 % 16
    CHECK(MockHal::vscrol == 5);                 // 5 % 8
    CHECK(read16(dl, LMS_POS) == SCREEN_BASE);   // 3/16 == 0, 5/8 == 0
}

// ── Larger scroll: fine + coarse, LMS patched ──────────────────────────

static void test_fine_and_coarse() {
    u8 dl[3];
    init_dl(dl);
    MockHal::reset();

    ScrollManager<MockPlatform> sm;
    sm.activate(BPL, SPLPL, FSR);
    sm.set(20, 17);
    sm.apply(dl, LMS_POS, screen_base(), MAP_WIDTH);

    CHECK(MockHal::hscrol == 4);                 // 20 % 16
    CHECK(MockHal::vscrol == 1);                 // 17 % 8

    // coarse_col = 20/16 = 1, coarse_row = 17/8 = 2.
    const u16 expected = static_cast<u16>(SCREEN_BASE + 2 * MAP_WIDTH + 1);
    CHECK(read16(dl, LMS_POS) == expected);
}

// ── move(): relative scroll arithmetic ─────────────────────────────────

static void test_move() {
    ScrollManager<MockPlatform> sm;
    sm.set(10, 10);
    sm.move(5, -3);
    CHECK(sm.x() == 15);
    CHECK(sm.y() == 7);

    // Negative past the origin clamps at 0 rather than wrapping.
    sm.move(-100, -100);
    CHECK(sm.x() == 0);
    CHECK(sm.y() == 0);
}

// ── suspend()/resume() gate hardware writes ────────────────────────────

static void test_suspend() {
    u8 dl[3];
    init_dl(dl);

    ScrollManager<MockPlatform> sm;
    sm.activate(BPL, SPLPL, FSR);

    // Suspended: apply() touches neither the registers nor the LMS.
    sm.suspend();
    sm.set(20, 17);
    MockHal::reset();
    sm.apply(dl, LMS_POS, screen_base(), MAP_WIDTH);
    CHECK(MockHal::hscrol_writes == 0);
    CHECK(MockHal::vscrol_writes == 0);
    CHECK(read16(dl, LMS_POS) == SCREEN_BASE);   // LMS left untouched

    // Resumed: writes occur and the LMS is patched.
    sm.resume();
    sm.apply(dl, LMS_POS, screen_base(), MAP_WIDTH);
    CHECK(MockHal::hscrol_writes > 0);
    CHECK(MockHal::vscrol_writes > 0);
    CHECK(read16(dl, LMS_POS) == static_cast<u16>(SCREEN_BASE + 2 * MAP_WIDTH + 1));
}

// ── apply() before activate() is inert ─────────────────────────────────

static void test_inactive() {
    u8 dl[3];
    init_dl(dl);
    MockHal::reset();

    ScrollManager<MockPlatform> sm;   // never activated
    sm.set(20, 17);
    sm.apply(dl, LMS_POS, screen_base(), MAP_WIDTH);
    CHECK(MockHal::hscrol_writes == 0);
    CHECK(MockHal::vscrol_writes == 0);
    CHECK(read16(dl, LMS_POS) == SCREEN_BASE);
}

int main() {
    test_zero();
    test_fine_only();
    test_fine_and_coarse();
    test_move();
    test_suspend();
    test_inactive();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
