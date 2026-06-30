// demo/tank/atari_tank_demo.cpp — EDGE tank demo.
//
// Stages 2-4 build the four-chunk ANTIC Mode 4 playfield, the PMG tank with
// sixteen-heading steering, and a centered+clamped following camera. Stage 5
// adds optional network asset loading with three asset sources selected at
// compile time (EDGE_TANK_ASSET_SOURCE):
//
//   Embedded        (default) — tileset + four chunks compiled in (Stage 2).
//   SimulatedNetwork          — the Stage 5A protocol is fed from the embedded
//                               assets through the loader over several frames
//                               (no real connection).
//   LiveSession               — the tileset + four chunks are loaded from a
//                               server over the reliable session lane (Stage 5B);
//                               NO embedded tileset/chunks are linked.
//
// After a valid load every mode enters the identical Stage 4 gameplay. There is
// no collision/terrain/bullets/realtime-lane/gameplay networking.

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

#include "playfield_geometry.h"
#include "tank_camera.h"
#include "tank_motion.h"
#include "tank_palette.h"
#include "tank_shapes.h"

// ── Asset-source selection ──────────────────────────────────────────────────
#define TANK_ASSET_EMBEDDED   0
#define TANK_ASSET_SIMULATED  1
#define TANK_ASSET_LIVE       2
#ifndef EDGE_TANK_ASSET_SOURCE
#define EDGE_TANK_ASSET_SOURCE TANK_ASSET_EMBEDDED
#endif
#define TANK_NET_MODE (EDGE_TANK_ASSET_SOURCE != TANK_ASSET_EMBEDDED)

#if EDGE_TANK_ASSET_SOURCE != TANK_ASSET_LIVE
#include "playfield_assets.h"            // embedded tileset + chunks (NOT in live mode)
#endif
#if TANK_NET_MODE
#include "asset_loader.h"
#include "asset_protocol.h"
#endif
#if EDGE_TANK_ASSET_SOURCE == TANK_ASSET_LIVE
#include "net_session_loader.h"
#endif

using engine::u8;
using engine::u16;
using engine::i16;
namespace M = atari;
using G = tank::PlayfieldGeometry;

// ── Platform + game configuration ────────────────────────────────────────
#if EDGE_TANK_ASSET_SOURCE == TANK_ASSET_LIVE
using Platform = atari::Platform<atari::Machine::XL, atari::RAM::Baseline,
                                 atari::gfx::Baseline, atari::Sound::Mono,
                                 atari::TV::NTSC, atari::Network::Fujinet>;
#else
using Platform = atari::Platform<atari::Machine::XL, atari::RAM::Baseline,
                                 atari::gfx::Baseline, atari::Sound::Mono,
                                 atari::TV::NTSC>;
#endif

struct PlayScreen {
    using display = engine::DisplayLayout<
        engine::ScrollRegion<engine::TextRegion<M::Mode::MODE_4, 24>,
                             G::physical_width, G::physical_height>>;
};
struct GameConfig {
    using screens = engine::ScreenSet<PlayScreen>;
    static constexpr u8 max_sprites    = 1;
    static constexpr u8 sound_channels = 1;
};
using Game = engine::Core<Platform, GameConfig>;

EDGE_SCROLL_TILE_MAP(tank::PhysicalMap, g_map);
// The scroll-map placement guard (ScrollTileMap head-pad/alignment) is sized for the
// platform's scan-wrap boundary; keep the demo-local constant honest against caps.
static_assert(Platform::capabilities::screen_buffer_alignment == tank::kScanWrapBoundary,
              "kScanWrapBoundary must match the platform scan-wrap granularity");

static constexpr u8 kTurnPeriod = (Platform::capabilities::frames_per_second >= 60) ? 7 : 6;
static constexpr u8 kTankColor  = 0x1E;

// ── Tank state ─────────────────────────────────────────────────────────────
#ifndef EDGE_TANK_HEADING
#define EDGE_TANK_HEADING 0
#endif
struct WPos { i16 x_nom, y_nom; };
[[maybe_unused]] static constexpr WPos kValidationPositions[] = {
    {  8,   8}, {320,   8}, {632,   8}, {  8, 192}, {320, 192}, {632, 192},
    {  8, 376}, {320, 376}, {632, 376}, {400, 250}, {160, 192}, {480, 192},
    {320,  96}, {320, 288}, {321, 192},
};
#ifdef EDGE_TANK_POSITION
static constexpr WPos kStartPos = kValidationPositions[EDGE_TANK_POSITION];
#else
static constexpr WPos kStartPos = {320, 192};
#endif
static tank::TankState g_tank = {
    static_cast<i16>(kStartPos.x_nom << 4), static_cast<i16>(kStartPos.y_nom << 4),
    static_cast<u8>(EDGE_TANK_HEADING), 0,
};

struct DebugState {
    u8 heading; u8 silhouette; u16 world_x_nominal; u16 world_y_nominal;
    u16 camera_cc_x; u16 camera_sl_y; u8 pmg_x; u8 pmg_y; u8 visible;
};
static volatile DebugState g_dbg = {};

static void set_palette() {
    using P = tank::Palette;
    Platform::hal::set_color_pf(0, P::colpf0);
    Platform::hal::set_color_pf(1, P::colpf1);
    Platform::hal::set_color_pf(2, P::colpf2);
    Platform::hal::set_color_pf(3, P::colpf3);
    Platform::hal::set_color_pf(4, P::colbk);
}

static void submit_camera_and_sprite() {
    const i16 wx = tank::world_x_nominal(g_tank);
    const i16 wy = tank::world_y_nominal(g_tank);
    const u16 cam_cc = tank::camera_scroll_x_for_world_x(wx);
    const u16 cam_sl = tank::camera_scroll_y_for_world_y(wy);
    Game::scroll.set(cam_cc, cam_sl);
    const i16 scc = tank::screen_color_clock_x(wx);
    const i16 ssl = tank::screen_scanline_y(wy);
    const u8  sil = tank::silhouette_for(g_tank.heading);
    u8 px = 0, py = 0, vis = 0;
    if (tank::tank_visible(scc, ssl)) {
        const i16 pmg_x = tank::pmg_x_for_world_x(wx);
        const i16 pmg_y = tank::pmg_y_for_world_y(wy);
        Game::sprite(0, tank::shape_for(sil), static_cast<u8>(pmg_x), static_cast<u8>(pmg_y));
        px = static_cast<u8>(pmg_x); py = static_cast<u8>(pmg_y); vis = 1;
    } else {
        Game::sprite_hide(0);
    }
    g_dbg.heading = g_tank.heading; g_dbg.silhouette = sil;
    g_dbg.world_x_nominal = static_cast<u16>(wx); g_dbg.world_y_nominal = static_cast<u16>(wy);
    g_dbg.camera_cc_x = cam_cc; g_dbg.camera_sl_y = cam_sl;
    g_dbg.pmg_x = px; g_dbg.pmg_y = py; g_dbg.visible = vis;
}

static void frame_step(const engine::Input& in) {
    const tank::Intent intent = tank::resolve_input(in.left(), in.right(), in.up(), in.down());
    if (intent.rotate != 0) {
        if (g_tank.turn_counter == 0) {
            g_tank.heading = (intent.rotate > 0) ? tank::heading_cw(g_tank.heading)
                                                 : tank::heading_ccw(g_tank.heading);
            g_tank.turn_counter = kTurnPeriod;
        } else { --g_tank.turn_counter; }
    } else { g_tank.turn_counter = 0; }
    tank::move_tank(g_tank, intent.move);
    submit_camera_and_sprite();
}

#if TANK_NET_MODE
// One page-aligned, game-owned 1024-byte network tileset buffer (the accepted
// duplicate). Filled by the loader; installed via the public charset API.
alignas(256) static engine::TilesetData<1024> g_net_tileset;

static void show_progress(bool failed, bool complete) {
    Platform::hal::set_color_pf(4, failed ? 0x34 : (complete ? 0xC6 : 0x24));
}

// After a valid load: install the received tileset (page-aligned buffer, public
// API), bind the map, restore the palette, and run the unchanged gameplay loop.
[[noreturn]] static void enter_gameplay() {
    set_palette();
    Game::tiles.bind_charset_page(
        static_cast<u8>(reinterpret_cast<uintptr_t>(&g_net_tileset) >> 8));
    Game::scroll_map(g_map);
    submit_camera_and_sprite();
    Game::run(frame_step);
}
[[noreturn]] static void halt_loop() { Game::run([](const engine::Input&) {}); }
#endif

// ── SimulatedNetwork mode ───────────────────────────────────────────────────
#if EDGE_TANK_ASSET_SOURCE == TANK_ASSET_SIMULATED
#ifndef EDGE_TANK_NET_FAULT
#define EDGE_TANK_NET_FAULT 0
#endif
namespace P = tank::proto;
// Bundle the loader + message scratch + index in one large struct so it lands in
// main RAM (.bss), not the nearly-full zero page (small separate statics would be
// zp-promoted and overflow it).
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
        u8 idx = g_sim.index++;
#if EDGE_TANK_NET_FAULT == 2
        if (idx == 64) idx = 65;
#endif
#if EDGE_TANK_NET_FAULT == 3
        if (idx == 1) { g_sim.index = kSimCount; idx = 65; }
#endif
        u16 sz = build_sim_message(idx, g_sim.scratch);
#if EDGE_TANK_NET_FAULT == 1
        if (idx == 0) g_sim.scratch[7] = 39;
#endif
        g_sim.loader.consume(g_sim.scratch, sz);
    }
    show_progress(g_sim.loader.failed(), g_sim.loader.complete());
    return g_sim.loader.complete() || g_sim.loader.failed();
}
#endif  // SIMULATED

// ── LiveSession mode ────────────────────────────────────────────────────────
#if EDGE_TANK_ASSET_SOURCE == TANK_ASSET_LIVE
#ifndef EDGE_TANK_NET_HOST
#define EDGE_TANK_NET_HOST "127.0.0.1"
#endif
#ifndef EDGE_TANK_NET_PORT
#define EDGE_TANK_NET_PORT 9000
#endif
static const char kNetHost[] = EDGE_TANK_NET_HOST;     // ROM-resident endpoint
static constexpr u16 kNetPort = EDGE_TANK_NET_PORT;
static constexpr u8 kXfer = 0x42;

// Build-embedded live-backend marker (Stage 5C). A stub LiveSession build must
// never be mistaken for a real network-capable one: this string lands in the
// .xex (and linker map) so the backend can be confirmed from the binary itself.
#ifndef EDGE_TANK_LIVE_BACKEND_REAL
#define EDGE_TANK_LIVE_BACKEND_REAL 0
#endif
// [[gnu::used, gnu::retain]] keeps the marker in the .xex (rodata) without a
// volatile zero-page static — the demo's zero page is nearly full, so the marker
// must cost nothing there.
#if EDGE_TANK_LIVE_BACKEND_REAL
[[gnu::used, gnu::retain]] static const char kLiveBackendMarker[] = "EDGE-LIVE-BACKEND:RealFujinetLib";
#else
[[gnu::used, gnu::retain]] static const char kLiveBackendMarker[] = "EDGE-LIVE-BACKEND:Stub";
#endif
// Loading-lane timeouts, in frames. Sized for REAL FujiNet SIO throughput
// (Stage 5C): an N:TCP read delivers a ~74 B asset frame roughly every ~3.5 s
// over NetSIO, so a 3 s inactivity window (the original value) tripped a false
// failure partway through the 66-message transfer. The timers reset on each
// accepted message / successful connect, so these bound a genuine stall, not the
// whole transfer — generous margins are cheap on a loading screen.
static constexpr u16 kFps = Platform::capabilities::frames_per_second;
static constexpr u16 kConnectTimeoutFrames    = static_cast<u16>(30 * kFps);
static constexpr u16 kInactivityTimeoutFrames = static_cast<u16>(60 * kFps);

using SessionT = decltype(Game::net.session);
static tank::NetAssetClient<SessionT> g_client;

struct NetDebug {
    u8 state; u8 net_error; u8 loader_error; u8 messages; u16 bytes;
    u8 last_kind; u8 outstanding; u8 overflow; u8 loader_tiles; u8 loader_rows;
};
// Bundle the debug record (and the diagnostic scratch) into one larger struct so
// it lands in main RAM, NOT the near-full zero page (a bare `volatile NetDebug`
// gets zp-promoted and tips the demo over the 256-byte zp boundary at link).
struct LiveState {
    NetDebug dbg;
    u8 diag_scratch[24];
};
static volatile LiveState g_live = {};
#define g_netdbg g_live.dbg

static bool live_step(const engine::Input&) {
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

// Optional FujiNet transfer diagnostic (-DEDGE_TANK_NET_DIAG): on
// completion/failure, self-dump the final client + session + fujinet-lib state to
// a host file via Altirra's H: device (CIO IOCB #1), so the exact failure cause is
// observable headless. Off by default; needs an Altirra run with /hdpathrw mounted.
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

// ── Entry point ────────────────────────────────────────────────────────────
int main() {
    Platform::hal::set_player_size(0, M::sizep::NORMAL);
    Game::sprite_color(0, kTankColor);

#if EDGE_TANK_ASSET_SOURCE == TANK_ASSET_EMBEDDED
    Game::init(tank::tileset);
    set_palette();
    tank::clear_physical_map(g_map);
    for (u8 cy = 0; cy < G::chunk_rows; ++cy)
        for (u8 cx = 0; cx < G::chunk_columns; ++cx)
            tank::copy_chunk_to_map(g_map, tank::chunk_payload(cx, cy), cx, cy);
    Game::scroll_map(g_map);
    submit_camera_and_sprite();
    Game::run(frame_step);

#elif EDGE_TANK_ASSET_SOURCE == TANK_ASSET_SIMULATED
    Game::init();
    set_palette();
    tank::clear_physical_map(g_map);
    g_sim.loader.begin(&g_map, g_net_tileset.data);
    Game::run_until(loading_step);
    if (!g_sim.loader.complete()) halt_loop();
    enter_gameplay();

#else  // TANK_ASSET_LIVE
    Game::init();
    set_palette();
    tank::clear_physical_map(g_map);
    g_client.begin(&Game::net.session, &g_map, g_net_tileset.data,
                   kNetHost, kNetPort, kXfer,
                   kConnectTimeoutFrames, kInactivityTimeoutFrames);
    Game::run_until(live_step);
#ifdef EDGE_TANK_NET_DIAG
    tank_net_diag_dump();
#endif
    if (!g_client.ready()) halt_loop();
    enter_gameplay();
#endif
}
