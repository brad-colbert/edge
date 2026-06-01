// test_scroll.cpp — unit tests for engine/scroll.h (the portable ScrollManager).
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// ScrollManager is now purely portable: it owns the position, the fine/coarse
// split, and the fine-register writes (through a MOCK HAL). It knows NOTHING of
// the display-list bytes — that patching lives in the backend DisplayProgram and
// is covered by tests/backends/atari/test_atari_scroll.cpp. So these tests check
// the split math (coarse_col/coarse_row with edge clamps), the fine-register
// writes, and the active/suspend flags only.

#include <stdint.h>
#include <stdio.h>

#include <engine/scroll.h>

using engine::u8;
using engine::u16;
using engine::ScrollManager;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records the last fine-scroll values written and how many writes occurred.
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

// ── Shared test geometry (a Mode-2 scroll region over a 64x32 map) ──────
//
//   bytes_per_line = 40, scanlines_per_line = 8, fine_scroll_range = 4.
//   map 64 wide x 32 tall, 22 visible lines.
static constexpr u16 MAP_W   = 64;
static constexpr u16 MAP_H   = 32;
static constexpr u8  VIS     = 22;
static constexpr u8  BPL     = 40;
static constexpr u8  SPLPL   = 8;
static constexpr u8  FSR     = 4;

static void activate(ScrollManager<MockPlatform>& sm) {
    sm.activate(MAP_W, MAP_H, VIS, BPL, SPLPL, FSR);
}

// ── Zero scroll: registers cleared, coarse 0 ───────────────────────────

static void test_zero() {
    MockHal::reset();
    ScrollManager<MockPlatform> sm;
    activate(sm);
    sm.set(0, 0);
    sm.write_fine();

    CHECK(MockHal::hscrol == 0);
    CHECK(MockHal::vscrol == 0);
    CHECK(sm.coarse_col() == 0);
    CHECK(sm.coarse_row() == 0);
}

// ── Small scroll: fine only, no coarse step ────────────────────────────

static void test_fine_only() {
    MockHal::reset();
    ScrollManager<MockPlatform> sm;
    activate(sm);
    sm.set(3, 5);
    sm.write_fine();

    CHECK(MockHal::hscrol == 3);        // 3 % 4
    CHECK(MockHal::vscrol == 5);        // 5 % 8
    CHECK(sm.coarse_col() == 0);        // 3 / 4
    CHECK(sm.coarse_row() == 0);        // 5 / 8
}

// ── Larger scroll: fine + coarse ───────────────────────────────────────

static void test_fine_and_coarse() {
    MockHal::reset();
    ScrollManager<MockPlatform> sm;
    activate(sm);
    sm.set(20, 17);
    sm.write_fine();

    CHECK(MockHal::hscrol == 0);        // 20 % 4
    CHECK(MockHal::vscrol == 1);        // 17 % 8
    CHECK(sm.coarse_col() == 5);        // 20 / 4
    CHECK(sm.coarse_row() == 2);        // 17 / 8
}

// ── Coarse offsets clamp at the map's right / bottom edge ──────────────

static void test_edge_clamp() {
    ScrollManager<MockPlatform> sm;
    activate(sm);

    // Horizontal: max coarse col = map_width - bytes_per_line = 64 - 40 = 24.
    sm.set(4 * 100, 0);                 // would be coarse_col 100
    CHECK(sm.coarse_col() == 24);

    // Vertical: max coarse row = map_height - visible_lines = 32 - 22 = 10.
    sm.set(0, 8 * 100);                 // would be coarse_row 100
    CHECK(sm.coarse_row() == 10);
}

// ── move(): relative scroll arithmetic, clamped at the origin ──────────

static void test_move() {
    ScrollManager<MockPlatform> sm;
    sm.set(10, 10);
    sm.move(5, -3);
    CHECK(sm.x() == 15);
    CHECK(sm.y() == 7);

    sm.move(-100, -100);
    CHECK(sm.x() == 0);
    CHECK(sm.y() == 0);
}

// ── active()/suspend()/resume() flags ──────────────────────────────────

static void test_flags() {
    ScrollManager<MockPlatform> sm;
    CHECK(!sm.active());                // inactive before activate()
    CHECK(!sm.suspended());

    activate(sm);
    CHECK(sm.active());

    sm.suspend();
    CHECK(sm.suspended());
    sm.resume();
    CHECK(!sm.suspended());

    sm.deactivate();
    CHECK(!sm.active());
}

int main() {
    test_zero();
    test_fine_only();
    test_fine_and_coarse();
    test_edge_clamp();
    test_move();
    test_flags();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
