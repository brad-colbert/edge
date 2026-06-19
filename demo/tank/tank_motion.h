#ifndef DEMO_TANK_TANK_MOTION_H
#define DEMO_TANK_TANK_MOTION_H

// tank_motion.h — Stage 3 tank steering/movement math (demo-local, pure). Shared
// by atari_tank_demo.cpp and the host test_tank_motion. No engine API changes,
// no floating point, no runtime multiply/divide/trig.
//
//   * 16 movement headings (N=0, clockwise, 22.5 deg apart) with wrap.
//   * 16-entry ROM motion table in Q12.4 nominal px/frame (~|8| each ≈ 0.5 px).
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

// ── Motion vectors (Q12.4 nominal px/frame), magnitude ~8 (|v|^2 ≈ 58..72) ──
// dy is negative for "up" (north). The table is point-symmetric, so reverse is
// just subtraction of the forward vector (no separate reverse table).
struct MotionVector { i8 dx_q4; i8 dy_q4; };

inline const MotionVector& motion_vector(u8 heading) {
    static constexpr MotionVector kTable[16] = {
        { 0, -8}, { 3, -7}, { 6, -6}, { 7, -3},   // N  NNE NE  ENE
        { 8,  0}, { 7,  3}, { 6,  6}, { 3,  7},   // E  ESE SE  SSE
        { 0,  8}, {-3,  7}, {-6,  6}, {-7,  3},   // S  SSW SW  WSW
        {-8,  0}, {-7, -3}, {-6, -6}, {-3, -7},   // W  WNW NW  NNW
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

// ── Fixed camera + world->screen->PMG conversion (Stage 3 keeps camera fixed) ─
inline constexpr i16 kCameraNominalX = 160;   // shows central 320x192 of the 640x384 world
inline constexpr i16 kCameraNominalY = 96;
inline constexpr i16 kPmgOriginX = 48;        // HPOSP at visible playfield X=0 (Stage 1.1)
inline constexpr i16 kPmgOriginY = 32;        // strip Y at visible playfield Y=0
inline constexpr i16 kAnchorCC = 4;           // hull-centre horizontal anchor (color clocks)
inline constexpr i16 kAnchorSL = 8;           // hull-centre vertical anchor (scanlines)

inline constexpr i16 screen_x_nominal(i16 world_x_nom) { return static_cast<i16>(world_x_nom - kCameraNominalX); }
inline constexpr i16 screen_y_nominal(i16 world_y_nom) { return static_cast<i16>(world_y_nom - kCameraNominalY); }

// PMG top-left. Horizontal is quantized to 2 nominal px because HPOSP is in
// color clocks (screen_x >> 1). Signed result — caller checks tank_visible()
// before narrowing to u8.
inline constexpr i16 pmg_x_from_screen(i16 screen_x_nom) {
    return static_cast<i16>(kPmgOriginX + (screen_x_nom >> 1) - kAnchorCC);
}
inline constexpr i16 pmg_y_from_screen(i16 screen_y_nom) {
    return static_cast<i16>(kPmgOriginY + screen_y_nom - kAnchorSL);
}

// Visible if any of the 16x16 footprint (centre = screen pos) overlaps the
// 320x192 viewport. When true, the derived pmg_x/pmg_y are in safe u8 range
// (no strip overflow, no HPOSP wrap); when false, the demo hides the player.
inline constexpr bool tank_visible(i16 screen_x_nom, i16 screen_y_nom) {
    return screen_x_nom + 8 > 0 && screen_x_nom - 8 < 320 &&
           screen_y_nom + 8 > 0 && screen_y_nom - 8 < 192;
}

}  // namespace tank

#endif  // DEMO_TANK_TANK_MOTION_H
