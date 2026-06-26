// demo/tank_net/atari_tank_net_demo.cpp — EDGE networked two-tank demo.
//
// A "bigger tank demo": the Stage-4 joystick tank (local player) PLUS a second
// "adversary" tank whose authoritative state (world position, heading, speed) is
// streamed from a Python UDP server at ~10 Hz over the EDGE realtime lane. The
// link is bidirectional — the Atari streams its own tank state back so the
// server's adversary AI can react (chase/avoid). Between the 10 Hz snapshots the
// client dead-reckons the adversary forward and snaps on each new packet.
//
// Reuses demo/tank's motion/camera/shapes/assets verbatim (added to the include
// path by CMake); only the network glue + the second sprite are new.
//
// SPRITES: two tanks on DEDICATED hardware players via the engine direct-bind mode
// (GameConfig::sprite_binding = Direct) — slot 0 -> player 0 (local), slot 1 ->
// player 1 (adversary), fixed for the whole frame. NOT the multiplexer: a chasing
// adversary crosses the player in Y constantly, which would make the multiplexer's
// per-frame Y-sort swap players/colours for a frame.
//
// REALTIME LANE: this lane is EMULATOR-validated only (Altirra + NetSIO + Docker
// peer), not yet physical-FujiNet validated. This demo is a prototype for that
// environment. Build the real adapter with -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON;
// otherwise the realtime HAL is a stub (open_udp_seq -> Unsupported) and the
// adversary never appears — the border turns red ("NO NET") to make that obvious.

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

#include "playfield_geometry.h"
#include "tank_camera.h"
#include "tank_motion.h"
#include "tank_palette.h"
#include "tank_shapes.h"
#include "playfield_assets.h"        // embedded tileset + chunks (Stage 2)

#include "adversary_net.h"           // demo-local: packet + dead-reckoning

using engine::u8;
using engine::u16;
using engine::i16;
namespace M = atari;
namespace net = engine::net;
using G = tank::PlayfieldGeometry;

// ── Peer endpoint (build-time overridable, mirrors edge_net_realtime_meter) ──
//   -DEDGE_NET_PEER_HOST='"172.30.0.2"' -DEDGE_NET_PEER_PORT=9000
#ifndef EDGE_NET_PEER_HOST
#define EDGE_NET_PEER_HOST "192.168.1.205"
#endif
#ifndef EDGE_NET_PEER_PORT
#define EDGE_NET_PEER_PORT 9000
#endif
static constexpr const char* kPeerHost = EDGE_NET_PEER_HOST;
static constexpr u16         kPeerPort = EDGE_NET_PEER_PORT;
static constexpr u16         kLocalPort = 0;

// ── Platform + game configuration ────────────────────────────────────────
using Platform = atari::Platform<atari::Machine::XL, atari::RAM::Baseline,
                                 atari::gfx::Baseline, atari::Sound::Mono,
                                 atari::TV::NTSC, atari::Network::Fujinet>;

struct PlayScreen {
    using display = engine::DisplayLayout<
        engine::ScrollRegion<engine::TextRegion<M::Mode::MODE_4, 24>,
                             G::physical_width, G::physical_height>>;
};
struct GameConfig {
    using screens = engine::ScreenSet<PlayScreen>;
    static constexpr u8 max_sprites    = 2;     // local player + adversary
    static constexpr u8 sound_channels = 1;
    // No multiplexer: pin slot 0 -> player 0, slot 1 -> player 1 for the whole
    // frame so the two tanks never swap players/colours when they cross in Y.
    static constexpr engine::SpriteBinding sprite_binding = engine::SpriteBinding::Direct;
};
using Game = engine::Core<Platform, GameConfig>;

static tank::PhysicalMap g_map;
static_assert(Platform::capabilities::screen_buffer_alignment == tank::kScanWrapBoundary,
              "kScanWrapBoundary must match the platform scan-wrap granularity");

static constexpr u8 kFps        = Platform::capabilities::frames_per_second;
static constexpr u8 kTurnPeriod = (kFps >= 60) ? 7 : 6;
static constexpr u8 kPlayerColor = 0x1E;   // brassy (matches the original tank)
static constexpr u8 kAdvColor    = 0x46;   // red — visually distinct adversary

// Send the local player's snapshot at ~10 Hz (every ~6 frames at 60 fps / 5 at 50).
static constexpr u8 kTxPeriod = (kFps >= 60) ? 6 : 5;

// ── Tank state ─────────────────────────────────────────────────────────────
#ifndef EDGE_TANK_HEADING
#define EDGE_TANK_HEADING 0
#endif
// Local player starts in the UPPER-RIGHT corner of the world (centre clamp box is
// [8,632] x [8,376] nominal px); the adversary starts LOWER-LEFT (server --start).
static tank::TankState g_tank = {
    static_cast<i16>(632 << 4), static_cast<i16>(8 << 4),
    static_cast<u8>(EDGE_TANK_HEADING), 0,
};
static tanknet::Adversary g_adv = {};
static u8  g_player_speed = 0;       // 1 while the player is moving (for the TX packet)
static u16 g_tx_seq       = 0;
static u8  g_tx_frame     = 0;

// Playfield background. Darker than the shared tank palette's 0x04 dark-grey
// (same hue 0, lower luminance) but not black — local to this demo so the original
// tank demo's palette is unchanged.
static constexpr u8 kPlayfieldBg = 0x02;

static void set_palette() {
    using P = tank::Palette;
    Platform::hal::set_color_pf(0, P::colpf0);
    Platform::hal::set_color_pf(1, P::colpf1);
    Platform::hal::set_color_pf(2, P::colpf2);
    Platform::hal::set_color_pf(3, P::colpf3);
    Platform::hal::set_color_pf(4, kPlayfieldBg);
}

// Project a world point into the PLAYER's camera frame. tank_camera.h's screen_*
// helpers re-derive a self-centred camera from the coordinate you pass them — that
// is only correct for the player (which the camera follows). The adversary must be
// placed relative to the SAME camera scroll the player produced, so we compute its
// screen position from the submitted camera here.
static inline i16 screen_cc_in_cam(i16 world_x_nom, u16 cam_cc) {
    return static_cast<i16>((world_x_nom >> 1) - static_cast<i16>(cam_cc));
}
static inline i16 screen_sl_in_cam(i16 world_y_nom, u16 cam_sl) {
    return static_cast<i16>(world_y_nom - static_cast<i16>(cam_sl));
}

// Submit one tank into a sprite slot, given its world position + the player's camera
// scroll. Hidden when its footprint leaves the viewport.
static void submit_tank(u8 slot, const tank::TankState& s, u16 cam_cc, u16 cam_sl) {
    const i16 wx  = tank::world_x_nominal(s);
    const i16 wy  = tank::world_y_nominal(s);
    const i16 scc = screen_cc_in_cam(wx, cam_cc);
    const i16 ssl = screen_sl_in_cam(wy, cam_sl);
    if (tank::tank_visible(scc, ssl)) {
        const i16 px = static_cast<i16>(tank::kPmgOriginX + scc - tank::kHalfTankCC);
        const i16 py = static_cast<i16>(tank::kPmgOriginY + ssl - tank::kHalfTankSL);
        Game::sprite(slot, tank::shape_for(tank::silhouette_for(s.heading)),
                     static_cast<u8>(px), static_cast<u8>(py));
    } else {
        Game::sprite_hide(slot);
    }
}

// ── Sprite submission: local player (slot 0) + adversary (slot 1) ───────────
//
// The camera follows the LOCAL player; the adversary is drawn in that same camera
// frame (so it scrolls correctly and hides when it leaves the viewport).
static void submit_two_sprites() {
    const i16 pwx = tank::world_x_nominal(g_tank);
    const i16 pwy = tank::world_y_nominal(g_tank);
    const u16 cam_cc = tank::camera_scroll_x_for_world_x(pwx);
    const u16 cam_sl = tank::camera_scroll_y_for_world_y(pwy);
    Game::scroll.set(cam_cc, cam_sl);

    submit_tank(0, g_tank, cam_cc, cam_sl);     // local player (always on-screen)
    if (g_adv.have_state) submit_tank(1, g_adv.s, cam_cc, cam_sl);
    else                  Game::sprite_hide(1);
}

// ── Per-frame step ──────────────────────────────────────────────────────────
// Order matters (ADR-016: poll the network ONCE per frame here, in the main
// callback — never in the VBI; FujiNet SIO is 1-3 ms).
static void frame_step(const engine::Input& in) {
    if (Game::net.realtime.active()) {
        Game::net.realtime.poll();
        // 1. Drain authoritative adversary snapshots (last accepted wins; snaps state).
        tanknet::TankPacket16 pkt{};
        while (Game::net.realtime.recv(pkt)) tanknet::adv_apply_packet(g_adv, pkt);
        (void)Game::net.realtime.consume_rx_overflowed();
        // 2. Dead-reckon the adversary forward between snapshots.
        tanknet::adv_dead_reckon(g_adv);
    }

    // 3. Local tank input + movement (unchanged from the original tank demo).
    const tank::Intent intent = tank::resolve_input(in.left(), in.right(), in.up(), in.down());
    if (intent.rotate != 0) {
        if (g_tank.turn_counter == 0) {
            g_tank.heading = (intent.rotate > 0) ? tank::heading_cw(g_tank.heading)
                                                 : tank::heading_ccw(g_tank.heading);
            g_tank.turn_counter = kTurnPeriod;
        } else { --g_tank.turn_counter; }
    } else { g_tank.turn_counter = 0; }
    tank::move_tank(g_tank, intent.move);
    g_player_speed = (intent.move != 0) ? 1 : 0;

    // 4. Stream our own state back to the server (bidirectional; all-or-nothing TX).
    if (Game::net.realtime.active() && ++g_tx_frame >= kTxPeriod) {
        g_tx_frame = 0;
        Game::net.realtime.send(
            tanknet::make_player_packet(g_tank, g_player_speed, g_tx_seq++));
    }

    // 5. Draw both tanks.
    submit_two_sprites();
}

// ── Entry point ────────────────────────────────────────────────────────────
int main() {
    Platform::hal::set_player_size(0, M::sizep::NORMAL);
    Platform::hal::set_player_size(1, M::sizep::NORMAL);
    Game::sprite_color(0, kPlayerColor);
    Game::sprite_color(1, kAdvColor);

    Game::init(tank::tileset);
    set_palette();
    tank::clear_physical_map(g_map);
    for (u8 cy = 0; cy < G::chunk_rows; ++cy)
        for (u8 cx = 0; cx < G::chunk_columns; ++cx)
            tank::copy_chunk_to_map(g_map, tank::chunk_payload(cx, cy), cx, cy);
    Game::scroll_map(g_map);

    // Open the realtime lane (Netstream settle blocks inside open_udp_seq). Tolerate
    // failure (stub HAL or no peer): run player-only with the adversary hidden, and
    // turn the border red so a missing/failed link is visible at a glance.
    const net::NetStatus st = Game::net.realtime.open_udp_seq(kPeerHost, kPeerPort, kLocalPort);
    if (st != net::NetStatus::Ok || !Game::net.realtime.active()) {
        Platform::hal::set_color_pf(4, 0x34);    // "NO NET" — red border/background
    }

    submit_two_sprites();
    Game::run(frame_step);
}
