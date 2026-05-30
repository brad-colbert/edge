// test_math.cpp — unit tests for engine/math.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.

#include <stdio.h>

#include <engine/math.h>

using engine::u8;
using engine::u16;
using engine::fixed88;

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Direction tables ─────────────────────────────────────────────────

static void test_direction_tables() {
    // 0=up, 1=right, 2=down, 3=left
    CHECK(engine::dir_x[0] == 0);
    CHECK(engine::dir_x[1] == 1);
    CHECK(engine::dir_x[2] == 0);
    CHECK(engine::dir_x[3] == -1);

    CHECK(engine::dir_y[0] == -1);
    CHECK(engine::dir_y[1] == 0);
    CHECK(engine::dir_y[2] == 1);
    CHECK(engine::dir_y[3] == 0);
}

// ── Trig tables ──────────────────────────────────────────────────────

static void test_trig_tables() {
    CHECK(engine::sin8[0]  == 0);     // sin(0) == 0
    CHECK(engine::sin8[64] == 127);   // sin(90°) == +1  (quarter-period peak)

    CHECK(engine::cos8[0]  == 127);   // cos(0) == +1
    CHECK(engine::cos8[64] == 0);     // cos(90°) == 0

    // Range stays within signed 8-bit [-127, 127] for every entry.
    bool in_range = true;
    for (u16 i = 0; i < 256; ++i) {
        if (engine::sin8[i] < -127 || engine::sin8[i] > 127) in_range = false;
        if (engine::cos8[i] < -127 || engine::cos8[i] > 127) in_range = false;
    }
    CHECK(in_range);

    // Trough at 3/4 period: sin(270°) == -1.
    CHECK(engine::sin8[192] == -127);
}

// ── Fixed<8,8> ───────────────────────────────────────────────────────

static void test_fixed_construction() {
    // from_int round-trip
    fixed88 two = fixed88::from_int(2);
    CHECK(two.integer() == 2);
    CHECK(two.fraction() == 0);
    CHECK(two.raw() == 0x0200);

    // from_raw round-trip: 0x0080 == 0.5
    fixed88 half = fixed88::from_raw(0x0080);
    CHECK(half.integer() == 0);
    CHECK(half.fraction() == 0x80);
    CHECK(half.raw() == 0x0080);
}

static void test_fixed_add_sub() {
    fixed88 two  = fixed88::from_int(2);
    fixed88 half = fixed88::from_raw(0x0080);

    fixed88 sum = two + half;            // 2.5
    CHECK(sum.integer() == 2);
    CHECK(sum.fraction() == 0x80);
    CHECK(sum.raw() == 0x0280);

    fixed88 diff = sum - half;           // back to 2.0
    CHECK(diff.integer() == 2);
    CHECK(diff.fraction() == 0);
    CHECK(diff == two);
}

static void test_fixed_multiply() {
    // 2.0 * 3.0 == 6.0
    fixed88 r1 = fixed88::from_int(2) * fixed88::from_int(3);
    CHECK(r1.integer() == 6);
    CHECK(r1.fraction() == 0);

    // 2.0 * 0.5 == 1.0
    fixed88 r2 = fixed88::from_int(2) * fixed88::from_raw(0x0080);
    CHECK(r2.integer() == 1);
    CHECK(r2.fraction() == 0);

    // 0.5 * 0.5 == 0.25 (0x40). Exercises the wide intermediate: raw 0x80 *
    // 0x80 == 0x4000, which overflows u16 fraction math without promotion.
    fixed88 r3 = fixed88::from_raw(0x0080) * fixed88::from_raw(0x0080);
    CHECK(r3.integer() == 0);
    CHECK(r3.fraction() == 0x40);

    // 1.5 * 1.5 == 2.25.  raw 0x0180 * 0x0180 == 0x24000, >>8 == 0x0240.
    fixed88 r4 = fixed88::from_raw(0x0180) * fixed88::from_raw(0x0180);
    CHECK(r4.integer() == 2);
    CHECK(r4.fraction() == 0x40);
}

// ── random() ─────────────────────────────────────────────────────────

static void test_random() {
    // Over a short run: no zeros, and no immediate repeats (maximal-length
    // LFSR visits 255 distinct nonzero states before cycling).
    u8 prev = engine::random();
    CHECK(prev != 0);

    bool any_zero = false;
    bool any_immediate_repeat = false;
    bool any_change = false;
    for (u8 i = 0; i < 64; ++i) {
        u8 r = engine::random();
        if (r == 0) any_zero = true;
        if (r == prev) any_immediate_repeat = true;
        if (r != prev) any_change = true;
        prev = r;
    }
    CHECK(!any_zero);
    CHECK(!any_immediate_repeat);
    CHECK(any_change);              // it actually advances
}

int main() {
    test_direction_tables();
    test_trig_tables();
    test_fixed_construction();
    test_fixed_add_sub();
    test_fixed_multiply();
    test_random();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
