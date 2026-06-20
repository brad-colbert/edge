#ifndef DEMO_TANK_TANK_MOTION_H
#define DEMO_TANK_TANK_MOTION_H

// tank_motion.h — Stage 3 tank steering/movement math (demo-local, pure). Shared
// by atari_tank_demo.cpp and the host test_tank_motion. No engine API changes,
// no floating point, no runtime multiply/divide/trig.
//
//   * 16 movement headings (N=0, clockwise, 22.5 deg apart) with wrap.
//   * 16-entry ROM motion table in Q12.4 nominal px/frame (unit ~|8|; scaled by
//     kSpeedScale at compile time — default 3 ≈ 1.5 px/frame).
//   * 8 displayed silhouettes; intermediate (odd) headings round to the next
//     clockwise silhouette.
//   * Q12.4 hull-centre world position, forward/reverse (add/subtract the same
//     vector — no reverse table), world-boundary clamp.
//   * fixed camera + world->screen->PMG conversion and an offscreen/visibility
//     test (signed throughout; convert to u8 only when safely on-screen).

#include <engine/types.h>

#include "playfield_geometry.h"   // tank::clamp_i16

namespace tank {

using engine::u8;
using engine::i8;
using engine::i16;

// ── 16 movement headings (clockwise from North) ────────────────────────────
enum Heading : u8 {
    H_N = 0, H_NNE, H_NE, H_ENE, H_E, H_ESE, H_SE, H_SSE,
    H_S,     H_SSW, H_SW, H_WSW, H_W, H_WNW, H_NW, H_NNW,
    H_COUNT = 16,
};

inline constexpr u8 heading_cw (u8 h) { return static_cast<u8>((h + 1) & 15); }       // 15 -> 0
inline constexpr u8 heading_ccw(u8 h) { return static_cast<u8>((h + 15) & 15); }      // 0 -> 15

// ── Motion vectors (Q12.4 nominal px/frame) ────────────────────────────────
// dy is negative for "up" (north). The table is point-symmetric, so reverse is
// just subtraction of the forward vector (no separate reverse table).
//
// kSpeedScale sets the tank's translation speed. The base direction table is
// unit-speed (magnitude ~8 Q12.4 ≈ 0.5 px/frame); the scale multiplies it at
// COMPILE time (constexpr — no runtime multiply, honouring the no-mul rule). So
//   1 → ~0.5 px/frame (original)   2 → ~1.0   3 → ~1.5 px/frame, etc.
// Raise/lower this one constant to retune speed. Keep it small enough that the
// scaled components stay in i8 range (≤127): 7*kSpeedScale ≤ 127 ⇒ scale ≤ 18.
struct MotionVector { i8 dx_q4; i8 dy_q4; };

#ifndef EDGE_TANK_SPEED_SCALE
#define EDGE_TANK_SPEED_SCALE 3
#endif
inline constexpr i8 kSpeedScale = EDGE_TANK_SPEED_SCALE;

inline const MotionVector& motion_vector(u8 heading) {
    constexpr int S = kSpeedScale;
    static constexpr MotionVector kTable[16] = {
        { i8(0*S), i8(-8*S)}, { i8(3*S), i8(-7*S)}, { i8(6*S), i8(-6*S)}, { i8(7*S), i8(-3*S)},  // N  NNE NE  ENE
        { i8(8*S), i8( 0*S)}, { i8(7*S), i8( 3*S)}, { i8(6*S), i8( 6*S)}, { i8(3*S), i8( 7*S)},  // E  ESE SE  SSE
        { i8(0*S), i8( 8*S)}, { i8(-3*S),i8( 7*S)}, { i8(-6*S),i8( 6*S)}, { i8(-7*S),i8( 3*S)},  // S  SSW SW  WSW
        { i8(-8*S),i8( 0*S)}, { i8(-7*S),i8(-3*S)}, { i8(-6*S),i8(-6*S)}, { i8(-3*S),i8(-7*S)},  // W  WNW NW  NNW
    };
    return kTable[heading & 15];
}

// ── 16 headings -> 8 displayed silhouettes (N,NE,E,SE,S,SW,W,NW = 0..7) ─────
// Tie policy: even headings map to their own silhouette; odd (intermediate)
// headings round to the NEXT CLOCKWISE silhouette. silhouette = ((h+1)/2) % 8.
// NOTE: 8 silhouettes are not true 22.5-deg artwork; adjacent headings share art.
inline constexpr u8 silhouette_for(u8 heading) {
    return static_cast<u8>(((heading + 1) >> 1) & 7);
}

// ── Joystick intent (cancellation + simultaneous turn/move) ────────────────
struct Intent { i8 rotate; i8 move; };   // rotate: -1 ccw / +1 cw / 0 ; move: +1 fwd / -1 rev / 0
inline Intent resolve_input(bool left, bool right, bool up, bool down) {
    Intent in{0, 0};
    if (left != right) in.rotate = right ? static_cast<i8>(1) : static_cast<i8>(-1); // both => cancel
    if (up != down)    in.move   = up    ? static_cast<i8>(1) : static_cast<i8>(-1); // both => cancel
    return in;
}

// ── Tank state (hull centre in Q12.4 nominal px) ───────────────────────────
struct TankState {
    i16 world_x_q4;
    i16 world_y_q4;
    u8  heading;
    u8  turn_counter;
};

// World-boundary clamp: keep the whole 16x16 (anchor = centre) inside the 640x384
// logical world -> centre in [8,632] x [8,376] nominal px (Q12.4 below). NOT
// derived from the physical 88-cell map width.
inline constexpr i16 kMinCenterXq4 =   8 * 16;   // 128
inline constexpr i16 kMaxCenterXq4 = 632 * 16;   // 10112
inline constexpr i16 kMinCenterYq4 =   8 * 16;   // 128
inline constexpr i16 kMaxCenterYq4 = 376 * 16;   // 6016

inline void clamp_world(TankState& s) {
    s.world_x_q4 = clamp_i16(s.world_x_q4, kMinCenterXq4, kMaxCenterXq4);
    s.world_y_q4 = clamp_i16(s.world_y_q4, kMinCenterYq4, kMaxCenterYq4);
}

// Forward (dir +1) adds the heading vector; reverse (dir -1) subtracts it; 0 = no
// move. No multiply — sign-branch add/subtract. Clamps after moving.
inline void move_tank(TankState& s, i8 dir) {
    if (dir == 0) return;
    const MotionVector v = motion_vector(s.heading);
    if (dir > 0) { s.world_x_q4 = static_cast<i16>(s.world_x_q4 + v.dx_q4);
                   s.world_y_q4 = static_cast<i16>(s.world_y_q4 + v.dy_q4); }
    else         { s.world_x_q4 = static_cast<i16>(s.world_x_q4 - v.dx_q4);
                   s.world_y_q4 = static_cast<i16>(s.world_y_q4 - v.dy_q4); }
    clamp_world(s);
}

inline constexpr i16 world_x_nominal(const TankState& s) { return static_cast<i16>(s.world_x_q4 >> 4); }
inline constexpr i16 world_y_nominal(const TankState& s) { return static_cast<i16>(s.world_y_q4 >> 4); }

// The world->screen->PMG conversion now lives in tank_camera.h (Stage 4 uses a
// following camera, color-clock-coherent). tank_motion.h owns only heading,
// movement, and world clamping.

}  // namespace tank

#endif  // DEMO_TANK_TANK_MOTION_H
