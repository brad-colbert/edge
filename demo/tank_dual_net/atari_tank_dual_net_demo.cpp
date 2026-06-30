// demo/tank_dual_net/atari_tank_dual_net_demo.cpp — EDGE dual-lane tank demo.
//
// Demonstrates BOTH EDGE networking lanes in ONE program, used SEQUENTIALLY:
//
//   PHASE 1 — TCP / session lane (reliable).  On boot the playfield map + tileset
//             are downloaded over the reliable session lane with a loading screen,
//             exactly like demo/tank's LiveSession mode (asset_loader.h +
//             net_session_loader.h + asset_protocol.h).
//   HANDOFF — the TCP session is CLOSED, the downloaded tileset/map are installed,
//             then the UDP realtime lane is OPENED.
//   PHASE 2 — UDP / realtime lane (unframed netstream).  Three server-driven
//             adversary tanks are streamed at ~10 Hz while the local player drives,
//             exactly like demo/tank_net (adversary_net.h).
//
// The two lanes are used one-after-the-other, never concurrently: the realtime lane
// reprograms POKEY serial for continuous streaming and cannot coexist with normal
// SIO command/response fujinet-lib traffic. "Download, then switch to streaming."
//
// Any keypress during Phase 2 closes the realtime lane cleanly and stops the demo.
//
// Reuses demo/tank + demo/tank_net headers verbatim (added to the include path by
// CMake); only the two-phase sequencing + the TCP->close->UDP handoff are new.
//
// ── Phase-1 asset source (EDGE_TANK_DUAL_ASSET_SOURCE) ──────────────────────────
//   SimulatedNetwork (default) — the Stage-5A protocol is fed from embedded assets
//                                through the real AssetLoader over several frames
//                                (no connection, no server). Builds with NO
//                                fujinet-lib; proves the whole download->handoff
//                                sequence host-side / in Altirra.
//   Embedded                   — skip Phase 1; fill the map from compiled-in assets
//                                and go straight to the handoff (test Phase 2 alone).
//   LiveSession                — real TCP download over Game::net.session; requires
//                                -DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON + the
//                                llvm-mos libfujinet.a, plus the asset server.
//
// Phase 2 needs -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON for real streaming;
// otherwise the realtime lane is a stub (open_udp_seq -> Unsupported), the
// adversaries stay hidden and the border turns red ("NO NET"). Phase 1 still runs.
//
// REQUIRES fujinet-firmware v1.6.2+ for the realtime lane (whole-frame-aligned
// drop-oldest); older firmware desyncs the unframed deframer. See demo/tank_net.

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

#include "playfield_geometry.h"
#include "tank_camera.h"
#include "tank_motion.h"
#include "tank_palette.h"
#include "tank_shapes.h"

#include "adversary_net.h"           // Phase 2: demo-local packet + dead-reckoning

// ── Phase-1 asset-source selection ──────────────────────────────────────────
#define DUAL_ASSET_EMBEDDED   0
#define DUAL_ASSET_SIMULATED  1
#define DUAL_ASSET_LIVE       2
#ifndef EDGE_TANK_DUAL_ASSET_SOURCE
#define EDGE_TANK_DUAL_ASSET_SOURCE DUAL_ASSET_SIMULATED
#endif
// Net mode == Phase 1 loads into a game-owned tileset buffer (Simulated/Live);
// Embedded installs the compiled-in tileset directly via Game::init(tileset).
#define DUAL_NET_MODE (EDGE_TANK_DUAL_ASSET_SOURCE != DUAL_ASSET_EMBEDDED)

#if EDGE_TANK_DUAL_ASSET_SOURCE != DUAL_ASSET_LIVE
#include "playfield_assets.h"        // embedded tileset + chunks (NOT in live mode)
#endif
#if DUAL_NET_MODE
#include "asset_loader.h"
#include "asset_protocol.h"
#endif
#if EDGE_TANK_DUAL_ASSET_SOURCE == DUAL_ASSET_LIVE
#include "net_session_loader.h"
#endif

using engine::u8;
using engine::u16;
using engine::i16;
namespace M = atari;
namespace net = engine::net;
using G = tank::PlayfieldGeometry;

// ── Phase-2 peer endpoint (UDP realtime lane), build-time overridable ──────────
//   -DEDGE_NET_PEER_HOST='"172.30.0.2"' -DEDGE_NET_PEER_PORT=9000
#ifndef EDGE_NET_PEER_HOST
#define EDGE_NET_PEER_HOST "192.168.1.205"
#endif
#ifndef EDGE_NET_PEER_PORT
#define EDGE_NET_PEER_PORT 9000
#endif
static constexpr const char* kPeerHost  = EDGE_NET_PEER_HOST;
static constexpr u16         kPeerPort   = EDGE_NET_PEER_PORT;
static constexpr u16         kLocalPort  = 0;

// ── Platform + game configuration ────────────────────────────────────────────
// Network::Fujinet exposes BOTH lanes (Game::net.session + Game::net.realtime),
// regardless of the Phase-1 source (Phase 2 always needs the realtime lane).
using Platform = atari::Platform<atari::Machine::XL, atari::RAM::Baseline,
                                 atari::gfx::Baseline, atari::Sound::Mono,
                                 atari::TV::NTSC, atari::Network::Fujinet>;

struct PlayScreen {
    using display = engine::DisplayLayout<
        engine::ScrollRegion<engine::TextRegion<M::Mode::MODE_4, 24>,
                             G::physical_width, G::physical_height>>;
};
// 1 local player + kAdvCount adversaries must stay <= the 4 hardware players for
// direct-bind; 3 fills it exactly (single zone, no multiplexer).
static constexpr u8 kAdvCount = 3;

struct GameConfig {
    using screens = engine::ScreenSet<PlayScreen>;
    static constexpr u8 max_sprites    = 1 + kAdvCount;   // local player + adversaries
    static constexpr u8 sound_channels = 1;
    // No multiplexer: pin slot i -> player i for the whole frame so the tanks never
    // swap players/colours when they cross in Y (chasing adversaries cross constantly).
    static constexpr engine::SpriteBinding sprite_binding = engine::SpriteBinding::Direct;
    // Widen the realtime frame so ONE packet carries all kAdvCount adversaries per tick
    // (10 pkt/s) instead of one each (30 pkt/s) — the downstream-overload mitigation.
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
// One vivid colour per adversary, each a hue NOT used by the playfield palette so no
// adversary is camouflaged against a wall/tile: pink, purple, cyan.
static constexpr u8 kAdvColors[kAdvCount] = { 0x48, 0x68, 0x98 };

// Send the local player's snapshot at ~10 Hz (every ~6 frames at 60 fps / 5 at 50).
static constexpr u8 kTxPeriod = (kFps >= 60) ? 6 : 5;

// ── Local tank state ──────────────────────────────────────────────────────────
#ifndef EDGE_TANK_HEADING
#define EDGE_TANK_HEADING 0
#endif
// Spawn upper-right with a margin from the border walls (so the 16x16 sprite does
// not start already wall-touching). Adversaries start in the other corners (server).
static tank::TankState g_tank = {
    static_cast<i16>(600 << 4), static_cast<i16>(32 << 4),
    static_cast<u8>(EDGE_TANK_HEADING), 0,
};

// GTIA player->playfield collision: P0PF bit 0 = COLPF0, the white wall colour.
// Depot icons (fuel/ammo) draw their white letters inside a coloured box
// (COLPF1/COLPF2), so overlapping a depot also sets a colour bit; a plain wall is
// pure white. Block only on pure-white contact so the tank drives over depots while
// walls still stop it — derived from the live register, no depot tile codes hardcoded.
static constexpr u8 kWallColpfMask     = 0x01;          // COLPF0 = walls (white)
static constexpr u8 kDepotSurroundMask = 0x02 | 0x04;   // COLPF1|COLPF2 = depot box

// Mutable game state bundled into ONE struct so it lands in main RAM (.bss), not the
// near-full zero page — small separate statics get zp-promoted and overflow it.
struct GameState {
    tanknet::Adversary  adv[kAdvCount];   // network-driven adversaries
    tanknet::AdvRxState adv_rx;           // shared packet-level seq gate (combined snapshot)
    tank::TankState     tank_safe;        // last position player 0 was NOT touching a wall
    u8  player_speed;                     // 1 while the player is moving (for the TX packet)
    u16 tx_seq;
    u8  tx_frame;
    u8  rx_overflow_count;                // saturating RX-overflow events since last TX (echoed)
    u8  bye_frames;                       // quit: frames left to send/drain the "bye" packet
    u8  bss_anchor[24];                   // force .bss placement (main RAM is plentiful)
};
static GameState g_st = {};

// Playfield background. Darker than the shared palette's 0x04 (same hue, lower
// luminance) but not black — local to this demo.
static constexpr u8 kPlayfieldBg = 0x02;

#if DUAL_NET_MODE
// One page-aligned, game-owned 1024-byte tileset buffer (the accepted duplicate),
// filled by the Phase-1 loader and installed at the handoff via the public charset
// API. Declared here (before enter_streaming) so the handoff can reference it.
alignas(256) static engine::TilesetData<1024> g_net_tileset;

static void show_progress(bool failed, bool complete) {
    Platform::hal::set_color_pf(4, failed ? 0x34 : (complete ? 0xC6 : 0x24));
}
#endif

static void set_palette() {
    using P = tank::Palette;
    Platform::hal::set_color_pf(0, P::colpf0);
    Platform::hal::set_color_pf(1, P::colpf1);
    Platform::hal::set_color_pf(2, P::colpf2);
    Platform::hal::set_color_pf(3, P::colpf3);
    Platform::hal::set_color_pf(4, kPlayfieldBg);
}

// ── Phase 2: sprite submission (copied from demo/tank_net) ─────────────────────
// Project a world point into the PLAYER's camera frame (the camera follows the
// player, so the adversaries must be placed relative to the same submitted scroll).
static inline i16 screen_cc_in_cam(i16 world_x_nom, u16 cam_cc) {
    return static_cast<i16>((world_x_nom >> 1) - static_cast<i16>(cam_cc));
}
static inline i16 screen_sl_in_cam(i16 world_y_nom, u16 cam_sl) {
    return static_cast<i16>(world_y_nom - static_cast<i16>(cam_sl));
}

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

// Local player (slot 0) + adversaries (slots 1..kAdvCount), all in the player camera.
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

// ── Phase 2: per-frame step (copied from demo/tank_net) ────────────────────────
// Order matters (ADR-016: poll the network ONCE per frame here, never in the VBI).
static void streaming_frame_step(const engine::Input& in) {
    if (Game::net.realtime.active()) {
        Game::net.realtime.poll();
        // 1. Drain authoritative snapshots. ONE combined packet carries all adversaries;
        //    the seq gate keeps the newest (older/dup packets are dropped wholesale).
        tanknet::TankPacket32 pkt{};
        while (Game::net.realtime.recv(pkt)) {
            tanknet::adv_apply_packet(g_st.adv, kAdvCount, g_st.adv_rx, pkt);
        }
        if (Game::net.realtime.consume_rx_overflowed() && g_st.rx_overflow_count < 255)
            ++g_st.rx_overflow_count;
        // 2. Dead-reckon every adversary forward between snapshots.
        for (u8 i = 0; i < kAdvCount; ++i) tanknet::adv_dead_reckon(g_st.adv[i]);
    }

    // 2b. GTIA wall collision for player 0 (direct-bind: slot 0 IS hardware player 0).
    // Pure-white contact (COLPF0, no depot-box colour) is a wall → snap back; a depot
    // letter sets a colour bit too, so the tank drives over it.
    const u8 hit = Game::sprite_collisions().sprite_to_background(0);
    if ((hit & kWallColpfMask) && !(hit & kDepotSurroundMask)) {
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
        Game::net.realtime.send(
            tanknet::make_player_packet(g_tank, g_st.player_speed, g_st.tx_seq++,
                                        g_st.adv_rx.last_seq, g_st.rx_overflow_count));
        g_st.rx_overflow_count = 0;
    }

    // 5. Draw all tanks.
    submit_sprites();
}

// Per-frame streaming + quit handling (one callback to keep the demo's near-full zero
// page in budget — a separate quit callback tips it over). A keypress starts the quit:
// we notify the server we are leaving so it returns to Phase 1 (waiting for a new asset
// transfer) instead of streaming to a client that is gone. The realtime lane is
// unframed/unreliable UDP, so send the "bye" over several frames — early frames enqueue
// it (re-sending to survive loss), later frames only poll so the serial-out IRQ clocks
// the ring onto the wire BEFORE realtime.close() resets the TX ring. The loop then
// exits (screen briefly frozen ~0.2 s during the drain).
static constexpr u8 kByeFrames    = 12;
static constexpr u8 kByeSendUntil = 4;    // send while bye_frames >= this, then drain
static bool streaming_step(const engine::Input& in) {
    if (g_st.bye_frames != 0) {           // quitting: drain the bye, then end the loop
        --g_st.bye_frames;
        if (g_st.bye_frames >= kByeSendUntil)
            Game::net.realtime.send(tanknet::make_bye_packet(g_st.tx_seq++));
        Game::net.realtime.poll();
        return g_st.bye_frames == 0;
    }
    if (in.key_pressed()) {
        if (!Game::net.realtime.active()) return true;   // no lane: nothing to notify
        g_st.bye_frames = kByeFrames;     // begin the bye drain; exit once it completes
        return false;
    }
    streaming_frame_step(in);
    return false;
}

[[noreturn]] static void halt_loop() { Game::run([](const engine::Input&) {}); }

// Clean program exit: hand control back to the caller via DOSVEC ($0A) — DOS if the
// demo was launched from DOS/FujiNet, or the OS self-test/Memo Pad if booted bare.
// The realtime lane has already restored the OS vectors + disabled serial IRQs, so
// the machine is in a normal OS state; this is the idiomatic Atari "program done".
[[noreturn]] static void exit_to_dos() {
    __asm__ volatile("jmp ($000A)");
    __builtin_unreachable();
}

// ── HANDOFF + Phase 2 ─────────────────────────────────────────────────────────
// Close the TCP/session lane, install the downloaded assets, open the UDP/realtime
// lane, then stream until a keypress, then close the realtime lane cleanly.
//
// HANDOFF DISCIPLINE: the asset transfer is complete and fully drained before we get
// here (loader.complete()/client.ready() only fire after the COMPLETE message was
// consumed), so the session lane is idle. Close it FIRST — that releases the SIO/N:
// device — then do only RAM/charset writes before open_udp_seq() reprograms POKEY.
// No session SIO op may run between session.close() and open_udp_seq() returning.
[[noreturn]] static void enter_streaming() {
    Game::net.session.close();               // release SIO before realtime takes POKEY

#if DUAL_NET_MODE
    // Install the received tileset (page-aligned game-owned buffer, public API) and
    // bind the freshly-filled map. Embedded mode already did this via Game::init().
    Game::tiles.bind_charset_page(
        static_cast<u8>(reinterpret_cast<uintptr_t>(&g_net_tileset) >> 8));
#endif
    set_palette();
    Game::scroll_map(g_map);
    g_st.tank_safe = g_tank;                 // seed the wall-free fallback to spawn

    // Open the realtime lane (Netstream settle blocks for ~30 frames inside
    // open_udp_seq — a brief screen freeze, NOT a hang). Tolerate failure (stub HAL /
    // no peer): run player-only with adversaries hidden and a red "NO NET" border.
    const net::NetStatus st = Game::net.realtime.open_udp_seq(kPeerHost, kPeerPort, kLocalPort);
    if (st != net::NetStatus::Ok || !Game::net.realtime.active()) {
        Platform::hal::set_color_pf(4, 0x34);    // "NO NET" — red background
    }

    submit_sprites();
    // Streams until a keypress; on keypress it sends the "bye" to the server over a few
    // frames (so it returns to Phase 1) and then returns.
    Game::run_until(streaming_step);

    // Quiesce every subsystem (closes both net lanes — disarming the netstream serial
    // IRQs/vectors — and tears down the VBI/DLI, P/M graphics, sound, …) so nothing
    // keeps running or references this program's RAM once we leave it.
    Game::shutdown();
    exit_to_dos();                           // hand control back to DOS
}

// ── Phase 1: SimulatedNetwork ──────────────────────────────────────────────────
#if EDGE_TANK_DUAL_ASSET_SOURCE == DUAL_ASSET_SIMULATED
namespace P = tank::proto;
// Bundle the loader + scratch + index in one large struct so it lands in main RAM
// (.bss), not the near-full zero page.
struct SimFeed {
    tank::AssetLoader loader;
    u8 scratch[128];
    u8 index;
};
static SimFeed g_sim;
static constexpr u8 kXfer = 0x42;
static constexpr u8 kSimCount = 66;

static u16 build_sim_message(u8 index, u8* out) {
    if (index == 0) return P::build_manifest(out, kXfer);
    if (index <= 16) { const u16 blk = static_cast<u16>(index - 1);
        return P::build_tileset_block(out, kXfer, static_cast<u16>(blk * 64),
                                      &tank::tileset.data[blk * 64], 64); }
    if (index <= 64) { const u8 j = static_cast<u8>(index - 17);
        const u8 chunk = static_cast<u8>(j / 12), pair = static_cast<u8>(j % 12);
        const u8 cx = static_cast<u8>(chunk & 1), cy = static_cast<u8>(chunk >> 1);
        const u8 start = static_cast<u8>(pair * 2);
        return P::build_chunk_rows(out, kXfer, cx, cy, start, 2,
                                   &tank::chunk_payload(cx, cy)[start * 40]); }
    return P::build_complete(out, kXfer);
}
static bool loading_step(const engine::Input&) {
    for (u8 i = 0; i < 6 && g_sim.index < kSimCount && !g_sim.loader.failed(); ++i) {
        const u8 idx = g_sim.index++;
        const u16 sz = build_sim_message(idx, g_sim.scratch);
        g_sim.loader.consume(g_sim.scratch, sz);
    }
    show_progress(g_sim.loader.failed(), g_sim.loader.complete());
    return g_sim.loader.complete() || g_sim.loader.failed();
}
static bool phase1_ready() { return g_sim.loader.complete(); }
#endif  // SIMULATED

// ── Phase 1: LiveSession ───────────────────────────────────────────────────────
#if EDGE_TANK_DUAL_ASSET_SOURCE == DUAL_ASSET_LIVE
#ifndef EDGE_TANK_NET_HOST
#define EDGE_TANK_NET_HOST "127.0.0.1"
#endif
#ifndef EDGE_TANK_NET_PORT
#define EDGE_TANK_NET_PORT 9000
#endif
static const char kNetHost[] = EDGE_TANK_NET_HOST;     // ROM-resident endpoint
static constexpr u16 kNetPort = EDGE_TANK_NET_PORT;
static constexpr u8  kXfer = 0x42;

// Build-embedded live-backend marker: a stub LiveSession build must never be mistaken
// for a real network-capable one. This string lands in the .xex (and linker map).
#ifndef EDGE_TANK_LIVE_BACKEND_REAL
#define EDGE_TANK_LIVE_BACKEND_REAL 0
#endif
#if EDGE_TANK_LIVE_BACKEND_REAL
[[gnu::used, gnu::retain]] static const char kLiveBackendMarker[] = "EDGE-LIVE-BACKEND:RealFujinetLib";
#else
[[gnu::used, gnu::retain]] static const char kLiveBackendMarker[] = "EDGE-LIVE-BACKEND:Stub";
#endif
// Loading-lane timeouts (frames), sized for REAL FujiNet SIO throughput. They reset
// on each accepted message / successful connect, so they bound a genuine stall.
static constexpr u16 kFpsW = Platform::capabilities::frames_per_second;
static constexpr u16 kConnectTimeoutFrames    = static_cast<u16>(30 * kFpsW);
static constexpr u16 kInactivityTimeoutFrames = static_cast<u16>(60 * kFpsW);

using SessionT = decltype(Game::net.session);
static tank::NetAssetClient<SessionT> g_client;

struct NetDebug {
    u8 state; u8 net_error; u8 loader_error; u8 messages; u16 bytes;
    u8 last_kind; u8 outstanding; u8 overflow; u8 loader_tiles; u8 loader_rows;
};
// Bundle the debug record + diagnostic scratch into one larger struct so it lands in
// main RAM, not the near-full zero page.
struct LiveState {
    NetDebug dbg;
    u8 diag_scratch[24];
};
static volatile LiveState g_live = {};
#define g_netdbg g_live.dbg

static bool loading_step(const engine::Input&) {
    g_client.update();
    const tank::LoadProgress pr = g_client.loader().progress();
    g_netdbg.state        = static_cast<u8>(g_client.state());
    g_netdbg.net_error    = static_cast<u8>(g_client.net_error());
    g_netdbg.loader_error = static_cast<u8>(g_client.loader_error());
    g_netdbg.messages     = g_client.messages_received();
    g_netdbg.bytes        = g_client.bytes_received();
    g_netdbg.last_kind    = g_client.last_kind();
    g_netdbg.outstanding  = g_client.outstanding();
    g_netdbg.overflow     = g_client.overflow_seen() ? 1 : 0;
    g_netdbg.loader_tiles = pr.tileset_blocks;
    g_netdbg.loader_rows  = pr.chunk_rows;
    show_progress(g_client.failed(), g_client.ready());
    return g_client.ready() || g_client.failed();
}
static bool phase1_ready() { return g_client.ready(); }

// Optional FujiNet transfer diagnostic (-DEDGE_TANK_NET_DIAG): on completion/failure,
// self-dump the final client + session + fujinet-lib state to H:TANKDBG.BIN via CIO,
// so a stalled TCP->UDP handoff is observable headless. Needs Altirra /hdpathrw.
#ifdef EDGE_TANK_NET_DIAG
static u8 tank_diag_cio(u8 cmd, const void* buf, u16 len) {
    volatile u8* const ICCMD = (u8*)0x0352; volatile u8* const ICBAL = (u8*)0x0354;
    volatile u8* const ICBAH = (u8*)0x0355; volatile u8* const ICBLL = (u8*)0x0358;
    volatile u8* const ICBLH = (u8*)0x0359; volatile u8* const ICAX1 = (u8*)0x035A;
    volatile u8* const ICAX2 = (u8*)0x035B;
    const uintptr_t p = (uintptr_t)buf; u8 st = 0;
    *ICBAL = (u8)(p & 0xff); *ICBAH = (u8)((p >> 8) & 0xff);
    *ICBLL = (u8)(len & 0xff); *ICBLH = (u8)((len >> 8) & 0xff);
    if (cmd == 3) { *ICAX1 = 8; *ICAX2 = 0; }
    *ICCMD = cmd;
    __asm__ volatile("ldx #$10\n\tjsr $E456\n\tsty %0" : "=r"(st) : : "a", "x", "y");
    return st;
}
static void tank_net_diag_dump() {
    static char fn[] = "H1:TANKDBG.BIN\x9b";
    namespace FS = atari::fujinet_session;
    const engine::net::NetError se = Game::net.session.last_error();
    volatile u8* d = g_live.diag_scratch;
    d[0]  = g_netdbg.state;        d[1]  = g_netdbg.net_error;
    d[2]  = g_netdbg.loader_error; d[3]  = g_netdbg.messages;
    d[4]  = g_netdbg.last_kind;    d[5]  = g_netdbg.outstanding;
    d[6]  = g_netdbg.overflow;     d[7]  = g_netdbg.loader_tiles;
    d[8]  = g_netdbg.loader_rows;  d[9]  = static_cast<u8>(se.status);
    d[10] = static_cast<u8>(se.detail & 0xFF); d[11] = static_cast<u8>((se.detail >> 8) & 0xFF);
    d[12] = FS::FujinetLibSessionAdapter::session_diag_fn_error();
    d[13] = FS::FujinetLibSessionAdapter::session_diag_device_error();
    d[14] = FS::FujinetLibSessionAdapter::session_diag_conn();
    d[15] = static_cast<u8>(g_netdbg.bytes & 0xFF);
    tank_diag_cio(3, fn, 0);                          // OPEN write
    tank_diag_cio(11, (const void*)d, 16);            // PUT BINARY (16 diag bytes)
    tank_diag_cio(12, nullptr, 0);                    // CLOSE
}
#endif
#endif  // LIVE

// ── Entry point ────────────────────────────────────────────────────────────────
int main() {
    // Sprite sizes + colours for all four players (local + adversaries).
    Platform::hal::set_player_size(0, M::sizep::NORMAL);
    Game::sprite_color(0, kPlayerColor);
    for (u8 i = 0; i < kAdvCount; ++i) {
        const u8 slot = static_cast<u8>(i + 1);
        Platform::hal::set_player_size(slot, M::sizep::NORMAL);
        Game::sprite_color(slot, kAdvColors[i]);
    }

#if EDGE_TANK_DUAL_ASSET_SOURCE == DUAL_ASSET_EMBEDDED
    // No Phase 1: install compiled-in assets and go straight to the handoff.
    Game::init(tank::tileset);
    set_palette();
    tank::clear_physical_map(g_map);
    for (u8 cy = 0; cy < G::chunk_rows; ++cy)
        for (u8 cx = 0; cx < G::chunk_columns; ++cx)
            tank::copy_chunk_to_map(g_map, tank::chunk_payload(cx, cy), cx, cy);
    enter_streaming();

#elif EDGE_TANK_DUAL_ASSET_SOURCE == DUAL_ASSET_SIMULATED
    // Phase 1: feed embedded assets through the real AssetLoader (no connection).
    Game::init();
    set_palette();
    tank::clear_physical_map(g_map);
    g_sim.loader.begin(&g_map, g_net_tileset.data);
    Game::run_until(loading_step);
    if (!phase1_ready()) halt_loop();
    enter_streaming();

#else  // DUAL_ASSET_LIVE
    // Phase 1: real TCP download over the reliable session lane.
    Game::init();
    set_palette();
    tank::clear_physical_map(g_map);
    g_client.begin(&Game::net.session, &g_map, g_net_tileset.data,
                   kNetHost, kNetPort, kXfer,
                   kConnectTimeoutFrames, kInactivityTimeoutFrames);
    Game::run_until(loading_step);
#ifdef EDGE_TANK_NET_DIAG
    tank_net_diag_dump();
#endif
    if (!phase1_ready()) halt_loop();
    enter_streaming();
#endif
}
