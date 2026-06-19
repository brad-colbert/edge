// demo/tank/atari_tank_demo.cpp — EDGE tank demo, Stage 4: centered + clamped
// following camera over the four-chunk ANTIC Mode 4 playfield.
//
// Stage 2 built the playfield; Stage 3 added the PMG tank with sixteen-heading
// tank-style steering. Stage 4 replaces the fixed camera with one that FOLLOWS
// the tank: it keeps the tank centred while scrolling room remains and clamps at
// the four logical-world edges, after which the tank slides toward the
// corresponding viewport edge. The camera and sprite are computed from the SAME
// frame's tank state, and the horizontal camera/sprite share one color-clock
// quantization so there is no one-pixel wobble at odd world X.
//
// NOT implemented yet: collision, terrain response, bullets, networking, runtime
// map streaming. No generic engine camera API is added.
//
// Geometry/placement: playfield_geometry.h; assets: playfield_assets.h; steering/
// movement: tank_motion.h; following camera: tank_camera.h; silhouettes:
// tank_shapes.h. All hardware access is via the public EDGE API.

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

#include "playfield_assets.h"
#include "playfield_geometry.h"
#include "tank_camera.h"
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

static tank::PhysicalMap g_map;

// One 22.5-degree heading step every kTurnPeriod frames (~7 NTSC / ~6 PAL).
static constexpr u8 kTurnPeriod = (Platform::capabilities::frames_per_second >= 60) ? 7 : 6;
static constexpr u8 kTankColor  = 0x1E;   // bright tank vs. the dark ATank floor

// ── Tank state ─────────────────────────────────────────────────────────────
#ifndef EDGE_TANK_HEADING
#define EDGE_TANK_HEADING 0
#endif

// Deterministic validation positions (hull-centre, nominal px). The camera now
// follows, so these exercise centring, the four clamps, the centre<->edge
// transitions, and odd-X color-clock quantization.
struct WPos { i16 x_nom, y_nom; };
[[maybe_unused]] static constexpr WPos kValidationPositions[] = {
    {  8,   8},   // 0  world top-left clamp
    {320,   8},   // 1  world top edge, horizontally centred
    {632,   8},   // 2  world top-right clamp
    {  8, 192},   // 3  world left edge, vertically centred
    {320, 192},   // 4  world centre
    {632, 192},   // 5  world right edge, vertically centred
    {  8, 376},   // 6  world bottom-left clamp
    {320, 376},   // 7  world bottom edge, horizontally centred
    {632, 376},   // 8  world bottom-right clamp
    {400, 250},   // 9  interior: camera centres on both axes
    {160, 192},   // 10 left follow transition (~world X 160)
    {480, 192},   // 11 right follow transition (~world X 480)
    {320,  96},   // 12 top follow transition (~world Y 96)
    {320, 288},   // 13 bottom follow transition (~world Y 288)
    {321, 192},   // 14 odd nominal X — color-clock quantization check
};

#ifdef EDGE_TANK_POSITION
static constexpr WPos kStartPos = kValidationPositions[EDGE_TANK_POSITION];
#else
static constexpr WPos kStartPos = {320, 192};   // interactive default: world centre
#endif

static tank::TankState g_tank = {
    static_cast<i16>(kStartPos.x_nom << 4),
    static_cast<i16>(kStartPos.y_nom << 4),
    static_cast<u8>(EDGE_TANK_HEADING),
    0,
};

// ── Documented RAM state block (Altirra: see symbol g_dbg in the .map) ────
struct DebugState {
    u8  heading;
    u8  silhouette;
    u16 world_x_nominal;
    u16 world_y_nominal;
    u16 camera_cc_x;        // submitted scroll X (color clocks)
    u16 camera_sl_y;        // submitted scroll Y (scanlines)
    u16 eff_cam_nominal_x;  // camera_cc_x << 1
    u16 eff_cam_nominal_y;  // = camera_sl_y
    i16 screen_cc_x;        // tank screen X (color clocks)
    i16 screen_sl_y;        // tank screen Y (scanlines)
    u8  pmg_x;
    u8  pmg_y;
    u8  visible;
};
static volatile DebugState g_dbg = {};

// Derive the following camera + PMG placement from the current tank state and
// submit both for the same frame. Used by main() (initial) and frame_step().
static void submit_camera_and_sprite() {
    const i16 wx = tank::world_x_nominal(g_tank);
    const i16 wy = tank::world_y_nominal(g_tank);

    const u16 cam_cc = tank::camera_scroll_x_for_world_x(wx);
    const u16 cam_sl = tank::camera_scroll_y_for_world_y(wy);
    Game::scroll.set(cam_cc, cam_sl);   // applied by the frame service next VBI

    const i16 scc = tank::screen_color_clock_x(wx);
    const i16 ssl = tank::screen_scanline_y(wy);
    const u8  sil = tank::silhouette_for(g_tank.heading);

    u8 px = 0, py = 0, vis = 0;
    if (tank::tank_visible(scc, ssl)) {   // defensive; legal positions are fully visible
        const i16 pmg_x = tank::pmg_x_for_world_x(wx);
        const i16 pmg_y = tank::pmg_y_for_world_y(wy);
        Game::sprite(0, tank::shape_for(sil), static_cast<u8>(pmg_x), static_cast<u8>(pmg_y));
        px = static_cast<u8>(pmg_x); py = static_cast<u8>(pmg_y); vis = 1;
    } else {
        Game::sprite_hide(0);
    }

    g_dbg.heading          = g_tank.heading;
    g_dbg.silhouette       = sil;
    g_dbg.world_x_nominal  = static_cast<u16>(wx);
    g_dbg.world_y_nominal  = static_cast<u16>(wy);
    g_dbg.camera_cc_x      = cam_cc;
    g_dbg.camera_sl_y      = cam_sl;
    g_dbg.eff_cam_nominal_x = static_cast<u16>(tank::effective_camera_nominal_x(cam_cc));
    g_dbg.eff_cam_nominal_y = static_cast<u16>(tank::effective_camera_nominal_y(cam_sl));
    g_dbg.screen_cc_x      = scc;
    g_dbg.screen_sl_y      = ssl;
    g_dbg.pmg_x            = px;
    g_dbg.pmg_y            = py;
    g_dbg.visible          = vis;
}

// ── Per-frame logic (the order matters: heading -> move -> clamp -> camera) ──

static void frame_step(const engine::Input& in) {
    const tank::Intent intent =
        tank::resolve_input(in.left(), in.right(), in.up(), in.down());

    // Rotate (repeat-timed): step on press, then once every kTurnPeriod frames;
    // release resets so the next press turns immediately.
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

    // Forward / reverse along the heading, clamped to the logical world (inside
    // move_tank). The camera + sprite are then derived from this updated state.
    tank::move_tank(g_tank, intent.move);
    submit_camera_and_sprite();
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

    Platform::hal::set_player_size(0, M::sizep::NORMAL);
    Game::sprite_color(0, kTankColor);

    tank::clear_physical_map(g_map);
    for (u8 cy = 0; cy < G::chunk_rows; ++cy)
        for (u8 cx = 0; cx < G::chunk_columns; ++cx)
            tank::copy_chunk_to_map(g_map, tank::chunk_payload(cx, cy), cx, cy);

    Game::scroll_map(g_map);

    // Seed the camera + sprite from the initial tank state so frame 0 is correct
    // (no fixed camera anymore — it is recomputed every frame in frame_step).
    submit_camera_and_sprite();

    Game::run(frame_step);
}
