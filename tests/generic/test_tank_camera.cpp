// test_tank_camera.cpp — Stage 4 following-camera invariants (color-clock
// coherent). Built for mos-sim, run under CTest. Pure, demo-local math.

#include <stdint.h>
#include <stdio.h>

#include "demo/tank/tank_camera.h"
#include "demo/tank/tank_motion.h"

using engine::u16;
using engine::i16;

static unsigned g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// Legal hull-centre world range (matches tank_motion clamps).
static constexpr i16 kMinX = 8,  kMaxX = 632;
static constexpr i16 kMinY = 8,  kMaxY = 376;

// 1-7: horizontal camera at key X.
static void test_camera_x_points() {
    CHECK(tank::camera_scroll_x_for_world_x(8)   == 0);     // 1 min
    CHECK(tank::camera_scroll_x_for_world_x(160) == 0);     // 2 just below left threshold
    CHECK(tank::camera_scroll_x_for_world_x(162) == 1);     // 3 at/after left threshold (follows)
    CHECK(tank::camera_scroll_x_for_world_x(320) == 80);    // 4 interior centre
    CHECK(tank::camera_scroll_x_for_world_x(480) == 160);   // 5 right threshold (clamped)
    CHECK(tank::camera_scroll_x_for_world_x(482) == 160);   // 6 just above right threshold
    CHECK(tank::camera_scroll_x_for_world_x(632) == 160);   // 7 max
}

// 8-12: vertical camera at key Y.
static void test_camera_y_points() {
    CHECK(tank::camera_scroll_y_for_world_y(8)   == 0);     // 8 min
    CHECK(tank::camera_scroll_y_for_world_y(96)  == 0);     // 9 top threshold
    CHECK(tank::camera_scroll_y_for_world_y(192) == 96);    // 10 interior
    CHECK(tank::camera_scroll_y_for_world_y(288) == 192);   // 11 bottom threshold (clamped)
    CHECK(tank::camera_scroll_y_for_world_y(376) == 192);   // 12 max
}

// 13, 14: camera ranges over the full legal sweep.
static void test_camera_ranges() {
    for (i16 x = kMinX; x <= kMaxX; ++x) {
        const u16 c = tank::camera_scroll_x_for_world_x(x);
        CHECK(c <= 160);
    }
    for (i16 y = kMinY; y <= kMaxY; ++y) {
        const u16 c = tank::camera_scroll_y_for_world_y(y);
        CHECK(c <= 192);
    }
}

// 15, 16: tank stays centred through the interior follow range.
static void test_centering() {
    // Horizontal: while the camera follows (0 < cam < 160), screen_cc == 80.
    for (i16 x = kMinX; x <= kMaxX; ++x) {
        const u16 cam = tank::camera_scroll_x_for_world_x(x);
        if (cam > 0 && cam < 160) CHECK(tank::screen_color_clock_x(x) == 80);
    }
    // Vertical: while the camera follows (0 < cam < 192), screen_sl == 96.
    for (i16 y = kMinY; y <= kMaxY; ++y) {
        const u16 cam = tank::camera_scroll_y_for_world_y(y);
        if (cam > 0 && cam < 192) CHECK(tank::screen_scanline_y(y) == 96);
    }
}

// 17-20: after a clamp, the tank moves from centre toward the viewport edge.
static void test_edge_slide() {
    // 17 left: moving left from X160 (centre, screen_cc 80) to X8 -> smaller screen_cc.
    CHECK(tank::screen_color_clock_x(160) == 80);
    CHECK(tank::screen_color_clock_x(8) < 80);
    CHECK(tank::screen_color_clock_x(8) == 4);     // near the left edge
    // 18 right: moving right from X480 (centre) to X632 -> larger screen_cc.
    CHECK(tank::screen_color_clock_x(480) == 80);
    CHECK(tank::screen_color_clock_x(632) > 80);
    CHECK(tank::screen_color_clock_x(632) == 156);  // near the right edge (viewport 160 cc)
    // 19 top: from Y96 (centre) to Y8 -> smaller screen_sl.
    CHECK(tank::screen_scanline_y(96) == 96);
    CHECK(tank::screen_scanline_y(8) == 8);
    // 20 bottom: from Y288 (centre) to Y376 -> larger screen_sl.
    CHECK(tank::screen_scanline_y(288) == 96);
    CHECK(tank::screen_scanline_y(376) == 184);
}

// 21-23: world -> PMG at the three reference positions.
static void test_pmg_points() {
    CHECK(tank::pmg_x_for_world_x(8)   == 48  && tank::pmg_y_for_world_y(8)   == 32);   // 21 top-left
    CHECK(tank::pmg_x_for_world_x(320) == 124 && tank::pmg_y_for_world_y(192) == 120);  // 22 centre
    CHECK(tank::pmg_x_for_world_x(632) == 200 && tank::pmg_y_for_world_y(376) == 208);  // 23 bottom-right
}

// 24: every legal tank centre -> a fully-visible footprint.
static void test_all_visible() {
    bool ok = true;
    for (i16 x = kMinX; x <= kMaxX; ++x)
        for (i16 y = kMinY; y <= kMaxY; ++y) {
            if (!tank::tank_fully_visible(tank::screen_color_clock_x(x),
                                          tank::screen_scanline_y(y))) ok = false;
        }
    CHECK(ok);
}

// 25: odd nominal world X shares one color-clock origin with its even neighbour.
static void test_odd_x_coherent() {
    for (i16 x = kMinX; x + 1 <= kMaxX; x += 2) {
        // x (even) and x+1 (odd) quantize to the same color clock (x>>1).
        CHECK(tank::camera_scroll_x_for_world_x(x) == tank::camera_scroll_x_for_world_x(x + 1));
        CHECK(tank::screen_color_clock_x(x)        == tank::screen_color_clock_x(x + 1));
        CHECK(tank::pmg_x_for_world_x(x)           == tank::pmg_x_for_world_x(x + 1));
    }
}

// 26: PMG X is monotonic non-decreasing across the sweep (no wobble/oscillation
// from unquantized camera subtraction).
static void test_pmg_monotonic() {
    i16 prev = tank::pmg_x_for_world_x(kMinX);
    for (i16 x = kMinX + 1; x <= kMaxX; ++x) {
        const i16 cur = tank::pmg_x_for_world_x(x);
        CHECK(cur >= prev);
        prev = cur;
    }
}

// 27, 28: camera always yields legal coarse map coords and never exceeds the
// validated scroll range (so physical padding is never exposed).
static void test_coarse_and_padding() {
    for (i16 x = kMinX; x <= kMaxX; ++x) {
        const u16 cam = tank::camera_scroll_x_for_world_x(x);
        const u16 coarse_col = static_cast<u16>(cam / 4 + (cam % 4 ? 1 : 0));   // invert_x
        CHECK(coarse_col <= 40);          // max in-map coarse column (Stage 1.1)
        CHECK(cam <= 160);                // never beyond the padding-safe range
    }
    for (i16 y = kMinY; y <= kMaxY; ++y) {
        const u16 cam = tank::camera_scroll_y_for_world_y(y);
        CHECK(static_cast<u16>(cam / 8) <= 24);   // max coarse row
        CHECK(cam <= 192);
    }
}

// 29, 30: heading/silhouette and movement/clamp behaviour unchanged (spot check;
// fully covered by test_tank_motion).
static void test_unchanged() {
    CHECK(tank::silhouette_for(0) == 0 && tank::silhouette_for(4) == 2);
    CHECK(tank::heading_cw(15) == 0 && tank::heading_ccw(0) == 15);
    tank::TankState s{ static_cast<i16>(320 << 4), static_cast<i16>(192 << 4), tank::H_E, 0 };
    tank::move_tank(s, +1);
    CHECK(s.world_x_q4 == static_cast<i16>((320 << 4) + 8));   // forward adds dx
    // clamp still holds at the edge.
    tank::TankState w{ static_cast<i16>(10 << 4), static_cast<i16>(200 << 4), tank::H_W, 0 };
    for (int i = 0; i < 40; ++i) tank::move_tank(w, +1);
    CHECK(w.world_x_q4 == tank::kMinCenterXq4);
}

int main() {
    test_camera_x_points();
    test_camera_y_points();
    test_camera_ranges();
    test_centering();
    test_edge_slide();
    test_pmg_points();
    test_all_visible();
    test_odd_x_coherent();
    test_pmg_monotonic();
    test_coarse_and_padding();
    test_unchanged();
    if (g_failures == 0) printf("ALL TESTS PASSED\n");
    else                 printf("%u FAILURES\n", g_failures);
    return g_failures != 0;
}
