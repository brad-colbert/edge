#ifndef DEMO_TANK_TANK_CAMERA_H
#define DEMO_TANK_TANK_CAMERA_H

// tank_camera.h — Stage 4 centered+clamped following camera (demo-local, pure).
// Shared by atari_tank_demo.cpp and the host test_tank_camera.
//
// The camera keeps the tank centred while scrolling room remains and clamps at
// the four logical-playfield edges; once clamped, the tank slides from screen
// centre toward the corresponding viewport edge. The behaviour falls out of
// clamping the desired camera origin — no camera-mode state machine.
//
// CRITICAL horizontal coherence rule: ANTIC scrolling and PMG HPOSP both use
// COLOR CLOCKS (1 cc = 2 nominal px). The player world X is quantized to color
// clocks ONCE (>>1); the SAME quantized value drives both the scroll position
// and the sprite X, so they can never disagree by a nominal pixel at odd world
// X (no horizontal wobble). Vertical uses scanlines (1 sl = 1 nominal px), so no
// such quantization is needed.
//
// Only shifts, subtracts, additions, and clamps — no float/multiply/divide/trig.

#include <engine/types.h>

#include "playfield_geometry.h"   // tank::clamp_i16

namespace tank {

using engine::u16;
using engine::i16;

// Geometry constants (color clocks / scanlines).
inline constexpr i16 kHalfViewportCC  = 80;   // half of the 160-cc viewport width
inline constexpr i16 kHalfViewportSL  = 96;   // half of the 192-scanline viewport height
inline constexpr i16 kHalfTankCC      = 4;    // half the 8-cc tank width
inline constexpr i16 kHalfTankSL      = 8;    // half the 16-scanline tank height
inline constexpr i16 kMaxScrollCC     = 160;  // EDGE horizontal scroll range (color clocks)
inline constexpr i16 kMaxScrollSL     = 192;  // EDGE vertical scroll range (scanlines)
inline constexpr i16 kPmgOriginX      = 48;   // HPOSP at visible playfield X=0 (Stage 1.1)
inline constexpr i16 kPmgOriginY      = 32;   // strip Y at visible playfield Y=0
inline constexpr i16 kViewportCC      = 160;  // visible width  in color clocks
inline constexpr i16 kViewportSL      = 192;  // visible height in scanlines

// Player world X quantized to color clocks (the single source of horizontal
// truth; both the camera and the sprite derive from this).
inline constexpr u16 player_color_clock_x(i16 world_x_nominal) {
    return static_cast<u16>(world_x_nominal >> 1);
}

// ── Horizontal (color-clock space) ──────────────────────────────────────────
// Desired camera = player_cc - halfViewport, clamped to [0,160].
inline constexpr u16 camera_scroll_x_for_world_x(i16 world_x_nominal) {
    const i16 pcc = static_cast<i16>(world_x_nominal >> 1);
    return static_cast<u16>(clamp_i16(static_cast<i16>(pcc - kHalfViewportCC), 0, kMaxScrollCC));
}
// Tank screen X in color clocks (player_cc - camera_cc), from the same pcc.
inline constexpr i16 screen_color_clock_x(i16 world_x_nominal) {
    return static_cast<i16>(static_cast<i16>(world_x_nominal >> 1) -
                            static_cast<i16>(camera_scroll_x_for_world_x(world_x_nominal)));
}
// PMG top-left X: origin + screen_cc - half-tank-width. Signed; narrow to u8 only
// when on-screen (see tank_visible).
inline constexpr i16 pmg_x_for_world_x(i16 world_x_nominal) {
    return static_cast<i16>(kPmgOriginX + screen_color_clock_x(world_x_nominal) - kHalfTankCC);
}

// ── Vertical (scanline space) ───────────────────────────────────────────────
inline constexpr u16 camera_scroll_y_for_world_y(i16 world_y_nominal) {
    return static_cast<u16>(clamp_i16(static_cast<i16>(world_y_nominal - kHalfViewportSL),
                                      0, kMaxScrollSL));
}
inline constexpr i16 screen_scanline_y(i16 world_y_nominal) {
    return static_cast<i16>(world_y_nominal -
                            static_cast<i16>(camera_scroll_y_for_world_y(world_y_nominal)));
}
inline constexpr i16 pmg_y_for_world_y(i16 world_y_nominal) {
    return static_cast<i16>(kPmgOriginY + screen_scanline_y(world_y_nominal) - kHalfTankSL);
}

// Effective camera nominal coordinates derived from the SUBMITTED scroll (for
// diagnostics only): horizontal is camera_cc << 1, vertical is the scanline value.
inline constexpr i16 effective_camera_nominal_x(u16 camera_scroll_cc_x) {
    return static_cast<i16>(camera_scroll_cc_x << 1);
}
inline constexpr i16 effective_camera_nominal_y(u16 camera_scroll_sl_y) {
    return static_cast<i16>(camera_scroll_sl_y);
}

// ── Visibility ──────────────────────────────────────────────────────────────
// Defensive partial-overlap test (footprint centre = screen pos): 8 cc x 16 sl
// footprint vs the 160 cc x 192 sl viewport. With the following camera + world
// clamp, every legal tank position is FULLY visible; this only matters for
// debug/deterministic abnormal coordinates.
inline constexpr bool tank_visible(i16 screen_cc_x, i16 screen_sl_y) {
    return screen_cc_x + kHalfTankCC > 0 && screen_cc_x - kHalfTankCC < kViewportCC &&
           screen_sl_y + kHalfTankSL > 0 && screen_sl_y - kHalfTankSL < kViewportSL;
}
// Footprint lies entirely inside the viewport (used to prove legal positions).
inline constexpr bool tank_fully_visible(i16 screen_cc_x, i16 screen_sl_y) {
    return screen_cc_x - kHalfTankCC >= 0 && screen_cc_x + kHalfTankCC <= kViewportCC &&
           screen_sl_y - kHalfTankSL >= 0 && screen_sl_y + kHalfTankSL <= kViewportSL;
}

}  // namespace tank

#endif  // DEMO_TANK_TANK_CAMERA_H
