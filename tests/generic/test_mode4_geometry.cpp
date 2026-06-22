// test_mode4_geometry.cpp — portable invariants behind the Stage 1.1 Mode 4
// geometry probe (demo/atari_mode4_geometry_probe.cpp).
//
// Built for `mos-sim` and run under CTest (exit 0 = pass). These are the
// backend-neutral, machine-checkable parts of the probe's claims: the
// fine/coarse decomposition for Mode 4 geometry, the derived maximum coarse
// column/row, the logical<->physical column mapping, and — the safety invariant —
// that NO legal camera value produces an out-of-range scroll LMS fetch into the
// 88x48 physical map. The hardware-facing parts (true visible column, PMG origin,
// bottom-row behaviour) are measured in Altirra, not here.

#include <stdint.h>
#include <stdio.h>

#include <engine/scroll.h>

using engine::u8;
using engine::u16;
using engine::u32;
using engine::ScrollManager;

// ── Mock HAL: record the fine-scroll register writes (mirrors test_scroll) ──
struct MockHal {
    static u8 hscrol;
    static u8 vscrol;
    static void set_fine_scroll_x(u8 v) { hscrol = v; }
    static void set_fine_scroll_y(u8 v) { vscrol = v; }
    static void reset() { hscrol = vscrol = 0; }
};
u8 MockHal::hscrol = 0;
u8 MockHal::vscrol = 0;
struct MockPlatform { using hal = MockHal; };

static unsigned g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Probe geometry (must match demo/atari_mode4_geometry_probe.cpp) ────────
//
// Mode 4 traits (engine/platform/atari/modes.h): fetch_width 48, scanlines/line 8,
// fine_scroll_range 4, invert_x true, invert_y false.
static constexpr u16 MAP_W   = 88;   // physical width  (4 left pad + 80 logical + 4 right pad)
static constexpr u16 MAP_H   = 48;   // physical height (= logical; no vertical pad)
static constexpr u8  VIS     = 24;   // visible mode lines
static constexpr u8  FETCH   = 48;   // bytes fetched per scrolled line
static constexpr u8  SPLPL   = 8;    // scanlines per mode line
static constexpr u8  FSR     = 4;    // color clocks per cell (horizontal fine modulus)
static constexpr u16 LEFTPAD = 4;    // provisional left padding
static constexpr u16 LOG_W   = 80;
static constexpr u16 LOG_H   = 48;
static constexpr u8  VIS_COLS = 40;  // visible columns

// Derived camera limits the report/demo rely on.
static constexpr u16 MAX_COARSE_COL = MAP_W - FETCH;             // 40
static constexpr u16 MAX_COARSE_ROW = MAP_H - VIS;               // 24
static constexpr u16 MAX_SCROLL_X   = (LOG_W - VIS_COLS) * FSR;  // 160 color clocks
static constexpr u16 MAX_SCROLL_Y   = (LOG_H - VIS) * SPLPL;     // 192 scanlines

static void activate(ScrollManager<MockPlatform>& sm) {
    sm.activate(MAP_W, MAP_H, VIS, FETCH, SPLPL, FSR, /*invert_x=*/true, /*invert_y=*/false);
}

// ── Fine/coarse decomposition across every HSCROL / VSCROL phase ───────────

static void test_fine_phases() {
    ScrollManager<MockPlatform> sm;
    activate(sm);

    // Horizontal inverts: HSCROL = (rx==0) ? 0 : 4-rx ; coarse carries +1 when rx!=0.
    struct HCase { u16 sx; u8 hscrol; u16 coarse; };
    const HCase hc[] = {
        {0, 0, 0}, {1, 3, 1}, {2, 2, 1}, {3, 1, 1}, {4, 0, 1}, {5, 3, 2},
    };
    for (auto& c : hc) {
        MockHal::reset();
        sm.set(c.sx, 0);
        sm.write_fine();
        CHECK(MockHal::hscrol == c.hscrol);
        CHECK(sm.coarse_col() == c.coarse);
    }

    // Vertical does NOT invert: VSCROL = sy%8 ; coarse_row = sy/8.
    for (u16 sy = 0; sy <= 8; ++sy) {
        MockHal::reset();
        sm.set(0, sy);
        sm.write_fine();
        CHECK(MockHal::vscrol == static_cast<u8>(sy % 8));
        CHECK(sm.coarse_row() == static_cast<u16>(sy / 8));
    }
}

// ── Derived maximum coarse column / row (edge clamp) ───────────────────────

static void test_max_coarse() {
    ScrollManager<MockPlatform> sm;
    activate(sm);

    sm.set(MAX_SCROLL_X, 0);
    CHECK(sm.coarse_col() == MAX_COARSE_COL);     // 40
    sm.set(MAX_SCROLL_X + 100, 0);                // over-scroll clamps, no growth
    CHECK(sm.coarse_col() == MAX_COARSE_COL);

    sm.set(0, MAX_SCROLL_Y);
    CHECK(sm.coarse_row() == MAX_COARSE_ROW);     // 24
    sm.set(0, MAX_SCROLL_Y + 100);
    CHECK(sm.coarse_row() == MAX_COARSE_ROW);
}

// ── Safety: NO legal camera value yields an out-of-range scroll LMS fetch ──
//
// patch_scroll() points visible line i at map_base + (coarse_row+i)*MAP_W +
// coarse_col, and ANTIC then fetches FETCH bytes. For every camera position in
// [0,MAX_SCROLL_X] x [0,MAX_SCROLL_Y] (and a bit beyond), the last fetched byte
// of the last visible line must stay inside the MAP_W*MAP_H physical map.
static void test_no_oob_lms() {
    ScrollManager<MockPlatform> sm;
    activate(sm);
    const u32 map_bytes = static_cast<u32>(MAP_W) * MAP_H;

    for (u16 sx = 0; sx <= MAX_SCROLL_X + 8; ++sx) {
        for (u16 sy = 0; sy <= MAX_SCROLL_Y + 8; ++sy) {
            sm.set(sx, sy);
            const u16 cc = sm.coarse_col();
            const u16 cr = sm.coarse_row();
            // Column fetch stays within a row, and the row index stays in-map.
            CHECK(cc + FETCH <= MAP_W);
            CHECK(cr + VIS   <= MAP_H);
            // Last byte of the last visible line's fetch is in-map.
            const u32 last = static_cast<u32>(cr + VIS - 1) * MAP_W + cc + (FETCH - 1);
            CHECK(last < map_bytes);
        }
    }
}

// ── Logical <-> physical column mapping (matches the probe's cell_code) ─────

static void test_log_phys_columns() {
    // Physical = logical + left pad; the 80 logical columns live in [LEFTPAD,
    // LEFTPAD+LOG_W); everything else is padding.
    CHECK(LEFTPAD + LOG_W + LEFTPAD == MAP_W);          // 4 + 80 + 4 == 88
    for (u16 lc = 0; lc < LOG_W; ++lc) {
        const u16 phys = static_cast<u16>(lc + LEFTPAD);
        CHECK(phys >= LEFTPAD && phys < LEFTPAD + LOG_W);
        CHECK(static_cast<u16>(phys - LEFTPAD) == lc);
    }
    // Camera limits expose the whole logical world: left edge reaches logical 0
    // (coarse 0) and logical 40 (coarse MAX_COARSE_COL), covering cols 0..79.
    CHECK(MAX_COARSE_COL == LOG_W - VIS_COLS);          // 40
    CHECK(MAX_SCROLL_X == MAX_COARSE_COL * FSR);        // 160
    CHECK(MAX_SCROLL_Y == MAX_COARSE_ROW * SPLPL);      // 192
}

int main() {
    test_fine_phases();
    test_max_coarse();
    test_no_oob_lms();
    test_log_phys_columns();

    if (g_failures == 0) printf("ALL TESTS PASSED\n");
    else                 printf("%u FAILURES\n", g_failures);
    return g_failures != 0;
}
