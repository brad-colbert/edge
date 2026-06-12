// demo/fujinet_net_test/fujinet_net_test.cpp — Edge engine Fujinet UDP smoke test.
//
// A minimal end-to-end check that the engine can talk to a LAN service over
// Fujinet using the C library fujinetlib. A Python UDP server on the LAN
// (port 2222) waits for any inbound packet ("hello"), then streams 2-byte
// [x, y] position datagrams at 20Hz. This app connects, reads positions, and
// drives a single hardware P/M sprite to those coordinates — proving the
// fujinetlib link, the N:UDP:// device path, and the read path on hardware.
//
// First consumer of the Network::Fujinet platform axis. The engine treats
// has_network as a pure capability flag and does nothing with it at runtime,
// so this demo calls fujinetlib directly (the correct integration today).
//
//   Build: cmake --build build --target fujinet_net_test   # -> fujinet_net_test.xex
//   Run  : needs a Fujinet device or Fujisan (cannot be host-tested).

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

// fujinetlib is a C library WITHOUT an extern "C" guard in its header, so the
// include MUST be wrapped — otherwise the C++ name mangling produces unresolved
// network_* symbols at link time.
extern "C" {
#include <fujinet-network.h>
}

using engine::u8;
using engine::u16;
namespace M = atari;

// ── Server / device spec ────────────────────────────────────────────────────
// CHANGE SERVER_IP to the UDP server's actual LAN address before flashing.
#define SERVER_IP "192.168.1.205"
#define NET_SPEC  "N:UDP://" SERVER_IP ":2222/"

// ── Platform + game configuration ───────────────────────────────────────────
//
// StockXL_NTSC is Network::None; spell out the same config with Network::Fujinet
// rather than mutating the shared alias.
using Platform = atari::Platform<
    M::Machine::XL,
    M::RAM::Baseline,
    M::gfx::Baseline,
    M::Sound::Mono,
    M::TV::NTSC,
    M::Network::Fujinet>;

struct DemoScreen {
    // ANTIC Mode 2 text, 24 rows — text is only a status display; the sprite is
    // the point of the demo. sprites_active enables the P/M DMA path.
    using display = engine::DisplayLayout<
        engine::TextRegion<M::Mode::MODE_2, 24>>;
    static constexpr bool sprites_active = true;
    static constexpr auto pm_resolution =
        engine::SpriteVerticalResolution::SingleLine;
    static constexpr bool scroll_active = false;
    static constexpr bool use_row_table = false;
};

struct GameConfig {
    using screens        = engine::ScreenSet<DemoScreen>;
    using initial_screen = DemoScreen;
    static constexpr u8 max_sprites = 1;
    // The brief asked for 0, but SoundManager static_asserts MaxChannels >= 1
    // (engine/sound.h). This demo plays no sound, so the single channel is
    // simply unused.
    static constexpr u8 sound_channels   = 1;
    // 1 sprite => single multiplex zone => no zone-boundary DLIs; 1 is plenty.
    static constexpr u8 max_raster_hooks = 1;
};

using Game = engine::Core<Platform, GameConfig>;

// ── Sprite shape (constexpr, ROM-resident) ──────────────────────────────────

constexpr auto sprite_shape = engine::make_sprite<8, 8>({
    0b00011000,
    0b00111100,
    0b01111110,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000,
});

// ── Coordinate mapping ───────────────────────────────────────────────────────
//
// The server streams each axis as one byte (0..255 logical space). Written
// straight to the P/M hardware, x=0 lands at HPOSP 0 — off the left edge — and a
// full-range value makes the motion span the whole color-clock range (oversized
// steps). Map the byte into the *visible* P/M window instead: HPOSP shows the
// playfield over roughly [48, 208], and the vertical strip is visible over
// roughly [32, 212]. The (v * span) >> 8 form compresses 0..255 into the window
// (max 255*180 = 45900 fits a u16) and the offset keeps it on screen. Tune the
// four constants to match the server's actual coordinate range.
static constexpr u8 kXMin = 48, kXSpan = 160;   // visible HPOS window  -> [48, 207]
static constexpr u8 kYMin = 32, kYSpan = 180;   // visible vertical band -> [32, 211]

static u8 map_axis(u8 v, u8 lo, u8 span) {
    return static_cast<u8>(lo + ((static_cast<u16>(v) * span) >> 8));
}

// ── Game state (static; no heap) ────────────────────────────────────────────
// Raw bytes last received from the server (shown on the status row). Seeded mid-
// range so the sprite sits on-screen before the first packet arrives.
static u8 g_x = 128;
static u8 g_y = 128;

// ── Receive-timeliness diagnostics ───────────────────────────────────────────
//
// net_step runs once per VBI (kFps Hz). The server streams at 20Hz, so a timely
// stream should land ~20 packets/sec — one packet roughly every kFps/20 frames,
// in bursts of 1. We surface:
//   CNT   the server's 16-bit per-packet counter, last value received.
//   FPS   EFFECTIVE callback rate: net_step calls per REAL second. This is the
//         headline number — if it's < the stream rate, the loop can't consume
//         the stream and the FIFO backlog grows (sprite lags real position).
//   PPS   packets consumed in the last REAL second (true rate).
//   GAP   iterations since the last packet arrived.
//   MAX   worst inter-arrival iteration gap seen.
//   BST   packets read in one iteration (≤1 with a single read/frame).
//   DRP   cumulative packets MISSING (gaps in the counter — lost in transit).
//   OOO   cumulative duplicate / out-of-order packets (counter not advancing).
static constexpr u8 kFps = Platform::capabilities::frames_per_second;

static u16 g_win_pkts  = 0;     // packets seen in the current (real-time) window
static u8  g_pps       = 0;     // packets in the previous real second (true rate)
static u8  g_gap       = 0;     // iterations since the last packet (live)
static u8  g_gap_max   = 0;     // worst inter-packet gap (iterations)
static u8  g_burst_max = 0;     // most packets read in one iteration
// Real-time window via the OS jiffy counter, for honest PPS and effective FPS.
static u8  g_last_tick   = 0;   // RTCLOK $14 at the previous net_step
static u16 g_real_frames = 0;   // true VBIs elapsed this window
static u16 g_calls       = 0;   // net_step calls this window
static u8  g_fps         = 0;   // effective callback rate (calls / real second)

// ── Read-path diagnostics (why are reads empty?) ──────────────────────────────
//   RT  last network_read_nb() return value (bytes, or a negative FN error).
//   BW  bytes waiting in the FujiNet channel (from network_status). 0 => the
//       device isn't receiving the UDP at all (routing/firewall/wrong addr);
//       >0 with RT=0 => data is buffered but the read isn't returning it.
//   CN  connection status (1 = open/reading, 0 = closed).
//   ER  network error code from network_status.
static int16_t g_last_ret = 0;
static u16 g_bw       = 0;
static u8  g_conn     = 0;
static u8  g_nerr     = 0;
// Sequence tracking off the server's 16-bit per-packet counter.
static u16 g_count     = 0;     // last counter value received
static u16 g_drops     = 0;     // cumulative MISSING packets (lost)
static u16 g_ooo       = 0;     // cumulative duplicate / out-of-order packets
static bool g_have_first = false; // first packet seen yet?

// ── Per-frame logic (runs once per VBI via Game::run_until) ──────────────────

static bool net_step(const engine::Input& in) {
    // ONE non-blocking read per frame — not a drain. Every network_read_nb is a
    // full SIO transaction (~ms); issuing several per frame overran the 16.6ms
    // budget and dropped the loop to ~7Hz. At 60Hz a single read easily keeps up
    // with a 20Hz stream (the FujiNet buffer stays drained), so one read/frame is
    // both cheap and current. Only a full 4-byte read counts as a packet
    // ([x, y, count_lo, count_hi]); 0 = nothing waiting, negative = FN error.
    const bool had_first = g_have_first;   // seen a packet before this frame?
    uint8_t buf[4];
    u8 got = 0;
    const int16_t r = network_read_nb(NET_SPEC, buf, 4);
    g_last_ret = r;
    if (r == 4) {
        g_x = buf[0];
        g_y = buf[1];
        const u16 cnt = static_cast<u16>(buf[2] | (buf[3] << 8));
        got = 1;
        // Sequence check against the previous counter. diff is the unsigned
        // distance (wraps at 65536): 1 = in order, >1 = some were lost in
        // between, 0 = duplicate, >=0x8000 = arrived out of order (backwards).
        if (g_have_first) {
            const u16 diff = static_cast<u16>(cnt - g_count);
            if (diff == 0 || diff >= 0x8000) ++g_ooo;
            else g_drops = static_cast<u16>(g_drops + (diff - 1));
        }
        g_have_first = true;
        g_count = cnt;
    }

    // Timeliness accounting.
    g_win_pkts = static_cast<u16>(g_win_pkts + got);
    if (got > 0) {
        // Gap that just closed (skip the initial connect wait so it doesn't
        // dominate MAX).
        if (had_first && g_gap > g_gap_max) g_gap_max = g_gap;
        g_gap = 0;
        if (got > g_burst_max) g_burst_max = got;
    } else if (g_gap < 255) {
        ++g_gap;
    }
    // Advance the REAL-time window from the OS jiffy counter (RTCLOK low byte
    // $14, read inline — no static-init pointer). It ticks at kFps Hz however
    // slow the loop runs, so g_calls per window = the effective FPS and
    // g_win_pkts per window = the true PPS.
    const u8 tick = *reinterpret_cast<volatile u8*>(0x14);
    g_real_frames = static_cast<u16>(g_real_frames +
                                     static_cast<u8>(tick - g_last_tick));
    g_last_tick = tick;
    ++g_calls;

    // Live every frame: the sprite (the point of the demo) and the X/Y readout.
    Game::sprite(0, sprite_shape, map_axis(g_x, kXMin, kXSpan),
                                  map_axis(g_y, kYMin, kYSpan));
    Game::print(0, 0, "X:");
    Game::print_num(2, 0, g_x, 3);
    Game::print(7, 0, "Y:");
    Game::print_num(9, 0, g_y, 3);

    // Heavy stats only once per REAL second — kept off the hot path so the ~12
    // text writes and the SIO status poll don't shrink the per-frame budget.
    if (g_real_frames >= kFps) {
        g_pps = (g_win_pkts > 255) ? 255 : static_cast<u8>(g_win_pkts);
        g_fps = (g_calls    > 255) ? 255 : static_cast<u8>(g_calls);
        g_win_pkts    = 0;
        g_calls       = 0;
        g_real_frames = 0;
        network_status(NET_SPEC, &g_bw, &g_conn, &g_nerr);

        Game::print(14, 0, "CNT:");
        Game::print_num(18, 0, g_count, 5);
        Game::print(25, 0, "FPS:");
        Game::print_num(29, 0, g_fps, 2);
        Game::print(0, 2, "PPS:");
        Game::print_num(4, 2, g_pps, 3);
        Game::print(9, 2, "DRP:");
        Game::print_num(13, 2, g_drops, 5);
        Game::print(20, 2, "OOO:");
        Game::print_num(24, 2, g_ooo, 5);
        Game::print(0, 3, "GAP:");
        Game::print_num(4, 3, g_gap, 3);
        Game::print(9, 3, "MAX:");
        Game::print_num(13, 3, g_gap_max, 3);
        Game::print(18, 3, "BST:");
        Game::print_num(22, 3, g_burst_max, 3);
        // Read-path status: RT (cast to u16; >32767 = negative FN error),
        // BW bytes waiting, CN connection, ER error.
        Game::print(0, 5, "RT:");
        Game::print_num(3, 5, static_cast<u16>(g_last_ret), 5);
        Game::print(10, 5, "BW:");
        Game::print_num(13, 5, g_bw, 5);
        Game::print(20, 5, "CN:");
        Game::print_num(23, 5, g_conn, 3);
        Game::print(28, 5, "ER:");
        Game::print_num(31, 5, g_nerr, 3);
    }

    // Exit on fire (level read — robust to dropped frames; ADR on edge loss).
    return in.fire();
}

// ── Entry point ──────────────────────────────────────────────────────────────

int main() {
    // Build the display list, set up P/M (single-line DMA), arm the DLI
    // dispatcher, install the deferred-VBI frame service. No charset arg => the
    // OS ROM font is used for the text field. init() also set_screen<DemoScreen>.
    Game::init();

    // Single-width player object, so the sprite is 8 px wide on screen.
    Platform::hal::set_player_size(0, M::sizep::NORMAL);

    // Readable text: white luminance on a black field/border (via the OS colour
    // shadows the deferred VBI copies to hardware each frame).
    Platform::hal::set_color_pf(4, 0x00);   // COLBK  : background / border
    Platform::hal::set_color_pf(2, 0x00);   // COLPF2 : text background
    Platform::hal::set_color_pf(1, 0x0E);   // COLPF1 : text luminance
    Game::sprite_color(0, 0x28);            // orange sprite

    // Status: announce, then init/open the network device.
    Game::print(10, 12, "INIT NETWORK...");
    if (network_init() != FN_ERR_OK) {
        Game::print(10, 14, "NET INIT FAILED");
        for (;;) {}
    }
    if (network_open(NET_SPEC, OPEN_MODE_RW, OPEN_TRANS_NONE) != FN_ERR_OK) {
        Game::print(10, 14, "NET OPEN FAILED");
        for (;;) {}
    }

    // Send the 1-byte hello that triggers the server's position stream.
    uint8_t hello = 'H';
    network_write(NET_SPEC, &hello, 1);

    // Overwrite the init line (pad to clear the longer prior text).
    Game::print(10, 12, "CONNECTED - WAITING");

    // Stream positions until fire is pressed. Seed the real-time reference so the
    // first FPS window isn't skewed by the connect delay.
    g_last_tick = *reinterpret_cast<volatile u8*>(0x14);
    Game::run_until(net_step);

    // Clean shutdown.
    network_close(NET_SPEC);
    Game::print(10, 12, "CLOSED             ");
    for (;;) {}
}
