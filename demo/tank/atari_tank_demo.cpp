// demo/tank/atari_tank_demo.cpp — EDGE tank demo, Stage 3: PMG tank with
// sixteen-heading tank-style steering over the static four-chunk playfield.
//
// Stage 2 built the playfield (40x24 Mode 4 viewport onto an 80x48 logical tile
// map from a 2x2 chunk grid in an 88x48 physical map). Stage 3 adds one
// normal-width GTIA player tank: 16 movement headings, 8 displayed silhouettes,
// Q12.4 world position, forward/reverse movement, world-boundary clamping.
//
// The CAMERA IS FIXED at the world centre this stage (it shows the central
// 320x192 of the 640x384 world); the Stage 2 joystick free-camera is removed so
// the joystick steers the tank. Camera FOLLOWING, collision, bullets, and
// networking are NOT implemented yet (Stage 4+).
//
// Geometry/placement live in playfield_geometry.h; assets in playfield_assets.h;
// tank steering/movement math in tank_motion.h (shared with test_tank_motion);
// silhouettes in tank_shapes.h. All hardware access is via the public EDGE API.

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

#include "playfield_assets.h"
#include "playfield_geometry.h"
#include "tank_motion.h"
#include "tank_shapes.h"

using engine::u8;
using engine::u16;
using engine::i16;
namespace M = atari;
using G = tank::PlayfieldGeometry;

// ── Platform + game configuration ────────────────────────────────────────

using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::gfx::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC>;

struct PlayScreen {
    using display = engine::DisplayLayout<
        engine::ScrollRegion<engine::TextRegion<M::Mode::MODE_4, 24>,
                             G::physical_width, G::physical_height>>;
};

struct GameConfig {
    using screens = engine::ScreenSet<PlayScreen>;
    static constexpr u8 max_sprites    = 1;   // the tank (one GTIA player)
    static constexpr u8 sound_channels = 1;
};

using Game = engine::Core<Platform, GameConfig>;

// ── The single writable physical tile map (RAM; ANTIC DMA-reads it) ───────
static tank::PhysicalMap g_map;

// One 22.5-degree heading step every kTurnPeriod frames (~8.6 steps/s NTSC,
// full turn ~1.86 s). Scaled for a comparable PAL period via the frame rate.
static constexpr u8 kTurnPeriod = (Platform::capabilities::frames_per_second >= 60) ? 7 : 6;

// Bright tank colour against the dark ATank arena floor.
static constexpr u8 kTankColor = 0x1E;

// ── Tank state ─────────────────────────────────────────────────────────────
#ifndef EDGE_TANK_HEADING
#define EDGE_TANK_HEADING 0
#endif

// Deterministic validation positions (hull-centre, nominal px). Camera is fixed
// at (160,96) so it shows world X[160,480] x Y[96,288]; positions 5-8 are inside
// the viewport, 1-4 are world corners (clamped + offscreen -> hidden).
struct WPos { i16 x_nom, y_nom; };
[[maybe_unused]] static constexpr WPos kValidationPositions[] = {
    {320, 192},   // 0 world centre = viewport centre
    {  8,   8},   // 1 world top-left corner   (min clamp; offscreen -> hidden)
    {632,   8},   // 2 world top-right corner  (max-X clamp)
    {  8, 376},   // 3 world bottom-left corner(max-Y clamp)
    {632, 376},   // 4 world bottom-right corner
    {168, 104},   // 5 near viewport top-left     (screen ~8,8)
    {472, 104},   // 6 near viewport top-right    (screen ~312,8)
    {168, 280},   // 7 near viewport bottom-left  (screen ~8,184)
    {472, 280},   // 8 near viewport bottom-right (screen ~312,184)
};

#ifdef EDGE_TANK_POSITION
static constexpr WPos kStartPos = kValidationPositions[EDGE_TANK_POSITION];
#else
static constexpr WPos kStartPos = {320, 192};   // interactive default: centre
#endif

static tank::TankState g_tank = {
    static_cast<i16>(kStartPos.x_nom << 4),
    static_cast<i16>(kStartPos.y_nom << 4),
    static_cast<u8>(EDGE_TANK_HEADING),
    0,
};

// ── Documented RAM state block (Altirra memory inspector: see the .map) ───
struct DebugState {
    u8  heading;
    u8  silhouette;
    u16 world_x_nominal;
    u16 world_y_nominal;
    u8  pmg_x;
    u8  pmg_y;
    u8  visible;
};
static volatile DebugState g_dbg = {};

// ── Per-frame logic ────────────────────────────────────────────────────────

static void frame_step(const engine::Input& in) {
    const tank::Intent intent =
        tank::resolve_input(in.left(), in.right(), in.up(), in.down());

    // Rotation with a repeat counter: step on press, then once every kTurnPeriod
    // frames while held; releasing resets so the next press turns immediately.
    if (intent.rotate != 0) {
        if (g_tank.turn_counter == 0) {
            g_tank.heading = (intent.rotate > 0) ? tank::heading_cw(g_tank.heading)
                                                 : tank::heading_ccw(g_tank.heading);
            g_tank.turn_counter = kTurnPeriod;
        } else {
            --g_tank.turn_counter;
        }
    } else {
        g_tank.turn_counter = 0;
    }

    // Forward / reverse along the current heading (clamped to the world).
    tank::move_tank(g_tank, intent.move);

    // World -> screen (fixed camera) -> PMG, with offscreen handling. Convert to
    // u8 only when the footprint is safely on-screen.
    const i16 sx = tank::screen_x_nominal(tank::world_x_nominal(g_tank));
    const i16 sy = tank::screen_y_nominal(tank::world_y_nominal(g_tank));
    const u8  sil = tank::silhouette_for(g_tank.heading);
    u8 dbg_px = 0, dbg_py = 0, dbg_vis = 0;
    if (tank::tank_visible(sx, sy)) {
        const i16 px = tank::pmg_x_from_screen(sx);
        const i16 py = tank::pmg_y_from_screen(sy);
        Game::sprite(0, tank::shape_for(sil), static_cast<u8>(px), static_cast<u8>(py));
        dbg_px = static_cast<u8>(px);
        dbg_py = static_cast<u8>(py);
        dbg_vis = 1;
    } else {
        Game::sprite_hide(0);
    }

    g_dbg.heading         = g_tank.heading;
    g_dbg.silhouette      = sil;
    g_dbg.world_x_nominal = static_cast<u16>(tank::world_x_nominal(g_tank));
    g_dbg.world_y_nominal = static_cast<u16>(tank::world_y_nominal(g_tank));
    g_dbg.pmg_x           = dbg_px;
    g_dbg.pmg_y           = dbg_py;
    g_dbg.visible         = dbg_vis;
}

// ── Entry point ────────────────────────────────────────────────────────────

int main() {
    Game::init(tank::tileset);

    using P = tank::Palette;
    Platform::hal::set_color_pf(0, P::colpf0);
    Platform::hal::set_color_pf(1, P::colpf1);
    Platform::hal::set_color_pf(2, P::colpf2);
    Platform::hal::set_color_pf(3, P::colpf3);
    Platform::hal::set_color_pf(4, P::colbk);

    // Normal-width tank player + its (sticky) colour.
    Platform::hal::set_player_size(0, M::sizep::NORMAL);
    Game::sprite_color(0, kTankColor);

    // Build the playfield: clear (padding + logical) to the neutral tile, then
    // copy each ROM chunk directly into its final physical position.
    tank::clear_physical_map(g_map);
    for (u8 cy = 0; cy < G::chunk_rows; ++cy)
        for (u8 cx = 0; cx < G::chunk_columns; ++cx)
            tank::copy_chunk_to_map(g_map, tank::chunk_payload(cx, cy), cx, cy);

    Game::scroll_map(g_map);

    // Fixed camera at the world centre: scroll_x = 160>>1 = 80 cc, scroll_y = 96
    // sl. Set once (the frame service re-applies the stored position each frame).
    Game::scroll.set(tank::scroll_color_clocks_x(tank::kCameraNominalX),
                     tank::scroll_scanlines_y(tank::kCameraNominalY));

    Game::run(frame_step);
}
