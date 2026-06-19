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

// Stage 5A: optional network-assets mode (default OFF). When ON, a transport-
// neutral loader is fed the protocol messages a server would send (built here
// from the embedded assets — Stage 5A simulates delivery; no real connection).
#ifndef EDGE_TANK_NETWORK_ASSETS
#define EDGE_TANK_NETWORK_ASSETS 0
#endif
#if EDGE_TANK_NETWORK_ASSETS
#include "asset_loader.h"
#include "asset_protocol.h"
#endif

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

static void set_palette() {
    using P = tank::Palette;
    Platform::hal::set_color_pf(0, P::colpf0);
    Platform::hal::set_color_pf(1, P::colpf1);
    Platform::hal::set_color_pf(2, P::colpf2);
    Platform::hal::set_color_pf(3, P::colpf3);
    Platform::hal::set_color_pf(4, P::colbk);
}

#if EDGE_TANK_NETWORK_ASSETS
// ── Stage 5A simulated network-asset loading ────────────────────────────────
//
// The tileset arrives into one game-owned, page-aligned 1024-byte buffer (the
// accepted network-mode duplicate); after COMPLETE it is "installed" purely via
// the public charset API (bind_charset_page) — no access to private engine
// charset storage, no copy. Map rows land directly in g_map via the loader.

#ifndef EDGE_TANK_NET_FAULT
#define EDGE_TANK_NET_FAULT 0   // 0 success, 1 bad manifest, 2 missing last row, 3 premature COMPLETE
#endif

alignas(256) static engine::TilesetData<1024> g_net_tileset;
static tank::AssetLoader g_loader;
static u8 g_scratch[128];
static u8 g_sim_index = 0;
static constexpr u8 kXfer = 0x42;

namespace P = tank::proto;

// Build the Nth message of the simulated transfer from the embedded assets.
//   0          : manifest
//   1..16      : tileset blocks (block 0..15)
//   17..64     : chunk rows (48 = 4 chunks x 12 two-row pairs)
//   65         : COMPLETE
static constexpr u8 kSimCount = 66;
static u16 build_sim_message(u8 index, u8* out) {
    if (index == 0) return P::build_manifest(out, kXfer);
    if (index <= 16) {
        const u16 blk = static_cast<u16>(index - 1);
        return P::build_tileset_block(out, kXfer, static_cast<u16>(blk * 64),
                                      &tank::tileset.data[blk * 64], 64);
    }
    if (index <= 64) {
        const u8 j = static_cast<u8>(index - 17);   // 0..47
        const u8 chunk = static_cast<u8>(j / 12);
        const u8 pair  = static_cast<u8>(j % 12);
        const u8 cx = static_cast<u8>(chunk & 1);
        const u8 cy = static_cast<u8>(chunk >> 1);
        const u8 start = static_cast<u8>(pair * 2);
        return P::build_chunk_rows(out, kXfer, cx, cy, start, 2,
                                   &tank::chunk_payload(cx, cy)[start * 40]);
    }
    return P::build_complete(out, kXfer);
}

// Border colour as a coarse progress/health indicator (no text HUD).
static void show_progress() {
    u8 c;
    if (g_loader.failed())        c = 0x34;   // red  = failed
    else if (g_loader.complete()) c = 0xC6;   // green= complete
    else                          c = 0x24;   // amber= loading
    Platform::hal::set_color_pf(4, c);
}

// One loading frame: feed a few messages, show progress. Returns true (stop the
// loading loop) on completion or failure.
static bool loading_step(const engine::Input&) {
    for (u8 i = 0; i < 6 && g_sim_index < kSimCount && !g_loader.failed(); ++i) {
        u8 idx = g_sim_index++;
#if EDGE_TANK_NET_FAULT == 2
        if (idx == 64) idx = 65;            // skip the last chunk-row pair -> premature COMPLETE
#endif
#if EDGE_TANK_NET_FAULT == 3
        if (idx == 1) { g_sim_index = kSimCount; idx = 65; }  // jump straight to COMPLETE
#endif
        u16 sz = build_sim_message(idx, g_scratch);
#if EDGE_TANK_NET_FAULT == 1
        if (idx == 0) g_scratch[7] = 39;    // corrupt manifest chunk_width -> ManifestMismatch
#endif
        g_loader.consume(g_scratch, sz);
    }
    show_progress();
    return g_loader.complete() || g_loader.failed();
}

[[noreturn]] static void halt_loop() {
    Game::run([](const engine::Input&) {});   // failed: stay put, do NOT start gameplay
}
#endif  // EDGE_TANK_NETWORK_ASSETS

// ── Entry point ────────────────────────────────────────────────────────────

int main() {
    Platform::hal::set_player_size(0, M::sizep::NORMAL);
    Game::sprite_color(0, kTankColor);

#if EDGE_TANK_NETWORK_ASSETS
    // Network mode: bring up the system WITHOUT a charset, load assets over many
    // frames, then install the received tileset + bind the map and play.
    Game::init();
    set_palette();
    tank::clear_physical_map(g_map);                    // neutral map during loading
    g_loader.begin(&g_map, g_net_tileset.data);
    Game::run_until(loading_step);                      // multi-frame protocol feed
    if (!g_loader.complete()) halt_loop();              // failure mode: never reaches gameplay
    set_palette();                                       // restore COLBK after the progress indicator
    // Install the received tileset via the public charset API (page-aligned buffer).
    Game::tiles.bind_charset_page(
        static_cast<u8>(reinterpret_cast<uintptr_t>(&g_net_tileset) >> 8));
    Game::scroll_map(g_map);
    submit_camera_and_sprite();
    Game::run(frame_step);
#else
    // Embedded mode (default): unchanged from Stage 4.
    Game::init(tank::tileset);
    set_palette();
    tank::clear_physical_map(g_map);
    for (u8 cy = 0; cy < G::chunk_rows; ++cy)
        for (u8 cx = 0; cx < G::chunk_columns; ++cx)
            tank::copy_chunk_to_map(g_map, tank::chunk_payload(cx, cy), cx, cy);
    Game::scroll_map(g_map);
    submit_camera_and_sprite();
    Game::run(frame_step);
#endif
}
