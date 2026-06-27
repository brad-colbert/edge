// demo/tank_net/atari_tank_net_demo.cpp — EDGE networked multi-tank demo.
//
// A "bigger tank demo": the Stage-4 joystick tank (local player) PLUS three
// "adversary" tanks whose authoritative state (world position, heading, speed) is
// streamed from a Python UDP server at ~10 Hz over the EDGE realtime lane. The
// link is bidirectional — the Atari streams its own tank state back so the
// server's adversary AI can react (chase/avoid/patrol). Between the 10 Hz snapshots
// the client dead-reckons each adversary forward and snaps on its new packets.
//
// Reuses demo/tank's motion/camera/shapes/assets verbatim (added to the include
// path by CMake); only the network glue + the extra sprites are new.
//
// SPRITES: four tanks on DEDICATED hardware players via the engine direct-bind mode
// (GameConfig::sprite_binding = Direct) — slot 0 -> player 0 (local), slots 1..3 ->
// players 1..3 (adversaries 0..2), fixed for the whole frame. 1 player + 3
// adversaries == the 4 hardware players, so the direct-bind path stays a single zone.
// NOT the multiplexer: chasing adversaries cross the player in Y constantly, which
// would make the multiplexer's per-frame Y-sort swap players/colours for a frame.
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
// Number of network-driven adversaries. 1 local player + kAdvCount adversaries must
// stay <= the 4 hardware players for direct-bind (kPlayers); 3 fills it exactly.
static constexpr u8 kAdvCount = 3;

struct GameConfig {
    using screens = engine::ScreenSet<PlayScreen>;
    static constexpr u8 max_sprites    = 1 + kAdvCount;   // local player + adversaries
    static constexpr u8 sound_channels = 1;
    // No multiplexer: pin slot i -> player i for the whole frame so the tanks never
    // swap players/colours when they cross in Y.
    static constexpr engine::SpriteBinding sprite_binding = engine::SpriteBinding::Direct;
    // Widen the realtime frame so ONE packet carries all kAdvCount adversaries per tick
    // (10 pkt/s) instead of one packet each (30 pkt/s) — the downstream-overload
    // mitigation. 32 bytes holds the header + kMaxAdv 6-byte records + the timing echo.
    static constexpr u16 realtime_packet_bytes = sizeof(tanknet::TankPacket32);
};
static_assert(tanknet::kMaxAdv >= kAdvCount, "combined packet must hold every adversary");
using Game = engine::Core<Platform, GameConfig>;

static tank::PhysicalMap g_map;
static_assert(Platform::capabilities::screen_buffer_alignment == tank::kScanWrapBoundary,
              "kScanWrapBoundary must match the platform scan-wrap granularity");

static constexpr u8 kFps        = Platform::capabilities::frames_per_second;
static constexpr u8 kTurnPeriod = (kFps >= 60) ? 7 : 6;
static constexpr u8 kPlayerColor = 0x1E;   // brassy (matches the original tank)
// One vivid colour per adversary, each a hue NOT used by the playfield palette
// (grey 0x0e, brick-red 0x32, green 0xc6, blue 0x84) or the gold player (0x1e), so
// no adversary is camouflaged against a wall/tile: pink, purple, cyan.
static constexpr u8 kAdvColors[kAdvCount] = { 0x48, 0x68, 0x98 };

// Send the local player's snapshot at ~10 Hz (every ~6 frames at 60 fps / 5 at 50).
static constexpr u8 kTxPeriod = (kFps >= 60) ? 6 : 5;

// ── Tank state ─────────────────────────────────────────────────────────────
#ifndef EDGE_TANK_HEADING
#define EDGE_TANK_HEADING 0
#endif
// Local player starts in the UPPER-RIGHT corner of the world — at (624,16), the
// upper-right area, with a few cells of margin from the top/right border walls so
// the 16x16 sprite does not spawn already touching them (which would wall-collide on
// frame 1). The adversary starts LOWER-LEFT (server --start).
static tank::TankState g_tank = {
    static_cast<i16>(600 << 4), static_cast<i16>(32 << 4),
    static_cast<u8>(EDGE_TANK_HEADING), 0,
};

// GTIA player→playfield collision: P0PF bit 0 = COLPF0, the white wall colour
// (tank_palette colpf0). A set bit means player 0's hull overlapped a wall.
static constexpr u8 kWallColpfMask = 0x01;

// Mutable game state bundled into ONE struct so it lands in main RAM (.bss), not
// the near-full zero page — small separate statics get zp-promoted and overflow it
// (volatile alone does not prevent promotion; see the tank demo's SimFeed/LiveState
// bundling). The padding keeps the struct comfortably above the zp-promotion size.
struct GameState {
    tanknet::Adversary adv[kAdvCount];   // network-driven adversaries
    tanknet::AdvRxState adv_rx;      // shared packet-level seq gate (combined snapshot)
    tank::TankState    tank_safe;    // last position player 0 was NOT touching a wall
    u8  player_speed;                // 1 while the player is moving (for the TX packet)
    u16 tx_seq;
    u8  tx_frame;
    u8  rx_overflow_count;           // saturating RX-overflow events since last TX (echoed)
    u8  bss_anchor[24];              // force .bss placement (main RAM is plentiful)
};
static GameState g_st = {};

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

// ── Sprite submission: local player (slot 0) + adversaries (slots 1..kAdvCount) ─
//
// The camera follows the LOCAL player; each adversary is drawn in that same camera
// frame (so it scrolls correctly and hides when it leaves the viewport).
static void submit_sprites() {
    const i16 pwx = tank::world_x_nominal(g_tank);
    const i16 pwy = tank::world_y_nominal(g_tank);
    const u16 cam_cc = tank::camera_scroll_x_for_world_x(pwx);
    const u16 cam_sl = tank::camera_scroll_y_for_world_y(pwy);
    Game::scroll.set(cam_cc, cam_sl);

    submit_tank(0, g_tank, cam_cc, cam_sl);     // local player (always on-screen)
    for (u8 i = 0; i < kAdvCount; ++i) {
        const u8 slot = static_cast<u8>(i + 1);
        if (g_st.adv[i].have_state) submit_tank(slot, g_st.adv[i].s, cam_cc, cam_sl);
        else                        Game::sprite_hide(slot);
    }
}

// ── Per-frame step ──────────────────────────────────────────────────────────
// Order matters (ADR-016: poll the network ONCE per frame here, in the main
// callback — never in the VBI; FujiNet SIO is 1-3 ms).
static void frame_step(const engine::Input& in) {
    if (Game::net.realtime.active()) {
        Game::net.realtime.poll();
        // 1. Drain authoritative snapshots. ONE combined packet carries all adversaries;
        //    the seq gate keeps the newest (older/dup packets are dropped wholesale).
        tanknet::TankPacket32 pkt{};
        while (Game::net.realtime.recv(pkt)) {
            tanknet::adv_apply_packet(g_st.adv, kAdvCount, g_st.adv_rx, pkt);
        }
        // Track RX-ring overflow events (oldest-dropped) so the server can see the
        // client falling behind; saturate the echo byte at 255.
        if (Game::net.realtime.consume_rx_overflowed() && g_st.rx_overflow_count < 255)
            ++g_st.rx_overflow_count;
        // 2. Dead-reckon every adversary forward between snapshots.
        for (u8 i = 0; i < kAdvCount; ++i) tanknet::adv_dead_reckon(g_st.adv[i]);
    }

    // 2b. GTIA wall collision for player 0. The engine latched P0PF from last
    // frame's render; direct-bind means logical slot 0 IS hardware player 0, so the
    // mask is reliable (no multiplexer reassignment). If the hull drove into a white
    // wall (COLPF0), snap back to the last wall-free position; otherwise remember
    // this position as wall-free.
    if (Game::sprite_collisions().sprite_to_background(0) & kWallColpfMask) {
        g_tank = g_st.tank_safe;
    } else {
        g_st.tank_safe = g_tank;
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
    g_st.player_speed = (intent.move != 0) ? 1 : 0;

    // 4. Stream our own state back to the server (bidirectional; all-or-nothing TX).
    if (Game::net.realtime.active() && ++g_st.tx_frame >= kTxPeriod) {
        g_st.tx_frame = 0;
        // Echo the last-applied combined-packet seq + the overflow accumulator so the
        // server can measure end-to-end lag; reset the accumulator once it is reported.
        Game::net.realtime.send(
            tanknet::make_player_packet(g_tank, g_st.player_speed, g_st.tx_seq++,
                                        g_st.adv_rx.last_seq, g_st.rx_overflow_count));
        g_st.rx_overflow_count = 0;
    }

    // 5. Draw all tanks.
    submit_sprites();
}

// ── Entry point ────────────────────────────────────────────────────────────
int main() {
    g_st.tank_safe = g_tank;    // seed the wall-free fallback to the spawn position
    Platform::hal::set_player_size(0, M::sizep::NORMAL);
    Game::sprite_color(0, kPlayerColor);
    for (u8 i = 0; i < kAdvCount; ++i) {
        const u8 slot = static_cast<u8>(i + 1);
        Platform::hal::set_player_size(slot, M::sizep::NORMAL);
        Game::sprite_color(slot, kAdvColors[i]);
    }

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

    submit_sprites();
    Game::run(frame_step);
}
