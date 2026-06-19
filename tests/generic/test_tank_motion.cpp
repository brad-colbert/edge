// test_tank_motion.cpp — pure tank steering/movement invariants for Stage 3
// (demo/tank/tank_motion.h). Built for mos-sim, run under CTest (exit 0 = pass).
// No Atari hardware; all math is pure and demo-local.

#include <stdint.h>
#include <stdio.h>

#include "demo/tank/tank_motion.h"

using engine::u8;
using engine::i16;
using engine::i32;

static unsigned g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// Expected forward motion table (must match tank_motion.h).
static const tank::MotionVector kExpect[16] = {
    { 0, -8}, { 3, -7}, { 6, -6}, { 7, -3},
    { 8,  0}, { 7,  3}, { 6,  6}, { 3,  7},
    { 0,  8}, {-3,  7}, {-6,  6}, {-7,  3},
    {-8,  0}, {-7, -3}, {-6, -6}, {-3, -7},
};
// 16 headings -> 8 silhouettes (intermediate/odd rounds clockwise).
static const u8 kSil[16] = {0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,0};

// 1, 2, 3: heading count + wrap.
static void test_headings() {
    CHECK(tank::H_COUNT == 16);
    CHECK(tank::heading_cw(15) == 0);     // clockwise wrap
    CHECK(tank::heading_ccw(0) == 15);    // counterclockwise wrap
    for (u8 h = 0; h < 16; ++h) {
        CHECK(tank::heading_cw(h)  == static_cast<u8>((h + 1) & 15));
        CHECK(tank::heading_ccw(h) == static_cast<u8>((h + 15) & 15));
    }
}

// 4, 5: all movement vectors + approximate equal-speed tolerance.
static void test_vectors() {
    for (u8 h = 0; h < 16; ++h) {
        const tank::MotionVector v = tank::motion_vector(h);
        CHECK(v.dx_q4 == kExpect[h].dx_q4);
        CHECK(v.dy_q4 == kExpect[h].dy_q4);
        const i32 mag2 = i32(v.dx_q4) * v.dx_q4 + i32(v.dy_q4) * v.dy_q4;
        CHECK(mag2 >= 54 && mag2 <= 74);   // ~64 +/- ~16%
    }
}

// 6: reverse equals subtraction of the forward vector (forward then reverse with
// the same heading returns to the start, away from clamp edges).
static void test_reverse() {
    for (u8 h = 0; h < 16; ++h) {
        tank::TankState s{ static_cast<i16>(320 << 4), static_cast<i16>(192 << 4), h, 0 };
        const i16 sx = s.world_x_q4, sy = s.world_y_q4;
        tank::move_tank(s, +1);
        CHECK(s.world_x_q4 == static_cast<i16>(sx + kExpect[h].dx_q4));
        CHECK(s.world_y_q4 == static_cast<i16>(sy + kExpect[h].dy_q4));
        tank::move_tank(s, -1);            // subtract the same vector
        CHECK(s.world_x_q4 == sx && s.world_y_q4 == sy);
    }
}

// 7: silhouette mapping for all sixteen headings.
static void test_silhouettes() {
    for (u8 h = 0; h < 16; ++h) CHECK(tank::silhouette_for(h) == kSil[h]);
}

// 8, 9, 10: input cancellation + turning while moving.
static void test_input() {
    CHECK(tank::resolve_input(true,  true,  false, false).rotate == 0);  // L+R cancel
    CHECK(tank::resolve_input(false, false, true,  true ).move   == 0);  // U+D cancel
    CHECK(tank::resolve_input(true,  false, false, false).rotate == -1); // L = ccw
    CHECK(tank::resolve_input(false, true,  false, false).rotate == +1); // R = cw
    CHECK(tank::resolve_input(false, false, true,  false).move   == +1); // U = fwd
    CHECK(tank::resolve_input(false, false, false, true ).move   == -1); // D = rev
    // turning while moving forward (L+U) and reversing (R+D).
    tank::Intent a = tank::resolve_input(true, false, true, false);
    CHECK(a.rotate == -1 && a.move == +1);
    tank::Intent b = tank::resolve_input(false, true, false, true);
    CHECK(b.rotate == +1 && b.move == -1);
}

// 11, 12: Q12.4 accumulation + sub-pixel accumulation across frames.
static void test_accumulation() {
    // Heading E: dx=+8 q4 per forward frame. 2 frames = 16 q4 = exactly 1 nominal px.
    tank::TankState s{ static_cast<i16>(100 << 4), static_cast<i16>(100 << 4), tank::H_E, 0 };
    const i16 x0 = tank::world_x_nominal(s);
    tank::move_tank(s, +1);
    CHECK(s.world_x_q4 == static_cast<i16>((100 << 4) + 8));   // accumulated in Q4
    CHECK(tank::world_x_nominal(s) == x0);                     // <1 px so far (8/16)
    tank::move_tank(s, +1);
    CHECK(tank::world_x_nominal(s) == static_cast<i16>(x0 + 1)); // 16 q4 -> +1 px
    // Heading NNE: dy=-7 q4/frame accumulates fractionally; after 3 frames = -21 q4.
    tank::TankState t{ static_cast<i16>(300 << 4), static_cast<i16>(300 << 4), tank::H_NNE, 0 };
    for (int i = 0; i < 3; ++i) tank::move_tank(t, +1);
    CHECK(t.world_y_q4 == static_cast<i16>((300 << 4) - 21));
    CHECK(t.world_x_q4 == static_cast<i16>((300 << 4) + 9));   // dx=+3 * 3
}

// 13-16: world clamps (centre stays in [8,632] x [8,376] nominal).
static void test_clamp() {
    // Min X: drive west far from the left edge.
    tank::TankState s{ static_cast<i16>(10 << 4), static_cast<i16>(200 << 4), tank::H_W, 0 };
    for (int i = 0; i < 50; ++i) tank::move_tank(s, +1);
    CHECK(s.world_x_q4 == tank::kMinCenterXq4);   // 128 (= 8 px)
    // Max X: drive east.
    tank::TankState e{ static_cast<i16>(628 << 4), static_cast<i16>(200 << 4), tank::H_E, 0 };
    for (int i = 0; i < 50; ++i) tank::move_tank(e, +1);
    CHECK(e.world_x_q4 == tank::kMaxCenterXq4);   // 10112 (= 632 px)
    // Min Y: drive north.
    tank::TankState n{ static_cast<i16>(200 << 4), static_cast<i16>(10 << 4), tank::H_N, 0 };
    for (int i = 0; i < 50; ++i) tank::move_tank(n, +1);
    CHECK(n.world_y_q4 == tank::kMinCenterYq4);   // 128 (= 8 px)
    // Max Y: drive south.
    tank::TankState so{ static_cast<i16>(200 << 4), static_cast<i16>(372 << 4), tank::H_S, 0 };
    for (int i = 0; i < 50; ++i) tank::move_tank(so, +1);
    CHECK(so.world_y_q4 == tank::kMaxCenterYq4);  // 6016 (= 376 px)
}

// 17, 18: centre world -> screen and world -> PMG (fixed camera 160,96).
static void test_center_conversion() {
    const i16 sx = tank::screen_x_nominal(320);
    const i16 sy = tank::screen_y_nominal(192);
    CHECK(sx == 160 && sy == 96);
    CHECK(tank::tank_visible(sx, sy));
    CHECK(tank::pmg_x_from_screen(sx) == 124);   // 48 + (160>>1) - 4
    CHECK(tank::pmg_y_from_screen(sy) == 120);    // 32 + 96 - 8
}

// 19, 20: offscreen safety (no u8 wrap; hidden when fully out of the viewport).
static void test_offscreen() {
    // World top-left corner (8,8): screen (-152,-88) -> not visible.
    const i16 sx1 = tank::screen_x_nominal(8), sy1 = tank::screen_y_nominal(8);
    CHECK(sx1 == -152 && sy1 == -88);
    CHECK(!tank::tank_visible(sx1, sy1));        // negative -> hidden, never narrowed to u8
    // World bottom-right corner (632,376): screen (472,280) -> not visible.
    const i16 sx2 = tank::screen_x_nominal(632), sy2 = tank::screen_y_nominal(376);
    CHECK(sx2 == 472 && sy2 == 280);
    CHECK(!tank::tank_visible(sx2, sy2));
    // A viewport-edge position stays visible with safe u8 PMG coords.
    const i16 sx3 = tank::screen_x_nominal(472), sy3 = tank::screen_y_nominal(104);
    CHECK(tank::tank_visible(sx3, sy3));
    const i16 px = tank::pmg_x_from_screen(sx3), py = tank::pmg_y_from_screen(sy3);
    CHECK(px >= 0 && px <= 255 && py >= 0 && py <= 255);
}

int main() {
    test_headings();
    test_vectors();
    test_reverse();
    test_silhouettes();
    test_input();
    test_accumulation();
    test_clamp();
    test_center_conversion();
    test_offscreen();
    if (g_failures == 0) printf("ALL TESTS PASSED\n");
    else                 printf("%u FAILURES\n", g_failures);
    return g_failures != 0;
}
