// demo/edge_net_realtime_meter.cpp — EDGE Stage 9S.3
//
// Realtime-networking DIAGNOSTIC demo (not a game, not a protocol layer).
//
// Opens the public realtime lane (Game::net.realtime), sends fixed 16-byte
// "meter" packets at a steady cadence to a host peer, drains the 16-byte
// replies each frame, and renders a text-mode HUD plus sparkline history of the
// link's timing: TX/RX sequence, RX count, RX age, round-trip delay, clock
// offset, jitter, stale state, and the public RX drop/overflow indicators.
//
// Scope / boundaries (Stage 9S.3):
//   * Uses ONLY the public Game::net.realtime API (open_udp_seq / poll / send /
//     recv / active / rx_count / rx_capacity / rx_dropped / consume_rx_overflowed
//     / last_error). No session lane, no fujinet-lib, no HAL/internal access.
//   * Fixed 16-byte all-or-nothing packets only. No wire framing, checksum,
//     resync, retransmit, reliability, or variable sizes.
//   * The Netstream hardware settle is internal to open_udp_seq() and blocks
//     until it returns, so this demo does NOT fake a live settle countdown:
//     it shows "OPENING / NETSTREAM SETTLE..." before the call and
//     "ACTIVE"/"OPEN FAILED" after. Any post-open metric warmup is labelled
//     POST-OPEN WARMUP — explicitly NOT the hardware settle.
//   * API-gap rule: if a metric would need internal access it is NOT shown.
//     (TX queue depth is not public, so TX health is inferred from send()/poll()
//     return codes only; internal Netstream flags are never displayed.)
//
// Pair with: tools/net/edge_realtime_peer.py  (--mode echo, default).
//
// BUILD NOTE: real transport needs the EDGE Netstream adapter. Build with
//   EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON (the CMake target links the two .S
//   handlers automatically). WITHOUT it the realtime HAL is the stub: open/send
//   return Ok and the HUD shows ACTIVE/LAST OK, but NO real SIO happens and the
//   peer receives nothing. (Validated end-to-end over fujinet-pc + NetSIO +
//   Altirra + Docker peer, Mode B; not on physical FujiNet hardware.)
//
// NOTE: peer endpoint is configured by the constants directly below — the .xex
// has no command line. EDIT THESE to point at the host running the peer.

#include <stdint.h>

#include <engine/core.h>
#include <engine/net.h>
#include <engine/platform/atari/platform.h>

#include "PREPPIEPC_FNT.h"   // 8x8 PC/ASCII-ordered font asset (remapped below)

using engine::u8;
using engine::u16;
using engine::i16;

namespace M = atari;
namespace net = engine::net;

// ── Peer configuration (single edit point — the .xex has no CLI args) ──────────
// Edit these constants, or override at build time with
//   -DEDGE_NET_PEER_HOST='"172.30.0.2"' -DEDGE_NET_PEER_PORT=9000
#ifndef EDGE_NET_PEER_HOST
#define EDGE_NET_PEER_HOST "192.168.1.205"   // EDIT: host running the peer
#endif
#ifndef EDGE_NET_PEER_PORT
#define EDGE_NET_PEER_PORT 9000             // EDIT: peer UDP port
#endif
static constexpr const char* kPeerHost  = EDGE_NET_PEER_HOST;
static constexpr u16         kPeerPort  = EDGE_NET_PEER_PORT;
static constexpr u16         kLocalPort = 0;               // 0 = any/ephemeral local port

// ── Demo tunables ─────────────────────────────────────────────────────────────
static constexpr u8  kSendPeriod   = 4;    // send one meter packet every N frames
static constexpr u8  kWarmupFrames = 30;   // POST-OPEN WARMUP: ignore early RTT samples
static constexpr u8  kSparkW       = 24;   // sparkline width in characters
static constexpr u16 kStaleFrames  = 30;   // RX age above this => STALE
static constexpr u8  kWinSecs      = 4;    // link-quality averaging window (seconds)

// HUD redraw cadence — draw the full text HUD every N frames, NOT every frame.
// The dashboard redraw (3 sparklines * kSparkW cells with 32-bit math + ~30
// print_num fields) is by far the heaviest per-frame work here. Because the game
// loop and the VBI frame service run lockstep, an every-frame redraw throttles the
// whole loop (and the frame clock with it), capping the realtime send/recv rate far
// below what the transport can actually carry. Measured on fujinet-pc + NetSIO +
// Altirra against a Docker echo peer: every-frame HUD held the link to ~5 pkt/s;
// redrawing every 8th frame (~7.5 Hz — still perfectly readable) lifted it to
// ~12 pkt/s and halved the observed DELAY. Sparkline history is pushed in the RX
// drain (independent of when we draw), so throttling the draw loses no samples.
// Override at build time with -DEDGE_METER_HUD_PERIOD=N (1 = old every-frame draw).
#ifndef EDGE_METER_HUD_PERIOD
#define EDGE_METER_HUD_PERIOD 8
#endif
static constexpr u8  kHudPeriod    = EDGE_METER_HUD_PERIOD;   // redraw HUD every N frames

// MeterPacket16 full-scale ranges (frames) for the sparkline level mapping.
static constexpr i16 kDelayFull  = 24;     // 0..24 frames
static constexpr i16 kJitterFull = 12;     // 0..12 frames
static constexpr i16 kOffsetSpan = 24;     // offset clamped to +/- this; mid = zero

// ── Platform / screen / game ──────────────────────────────────────────────────
using Platform = M::Platform<
    M::Machine::XL,
    M::RAM::Baseline,
    M::gfx::Baseline,
    M::Sound::Mono,
    M::TV::NTSC,
    M::Network::Fujinet>;

struct MeterScreen {
    using display = engine::DisplayLayout<engine::TextRegion<M::Mode::MODE_2, 24>>;
};

struct MeterConfig {
    using screens = engine::ScreenSet<MeterScreen>;
    static constexpr u8 max_sprites = 1;   // engine requires >= 1; this demo draws none
    static constexpr u8 sound_channels = 1;
};

using Game = engine::Core<Platform, MeterConfig>;

// Authoritative frame rate for this build (NTSC=60 / PAL=50). Used only for the
// approximate ms readout; all timing diagnostics are frame/jiffy-first.
static constexpr u8 kFps = Platform::capabilities::frames_per_second;

// ── MeterPacket16 — DEMO DIAGNOSTIC PAYLOAD ONLY ──────────────────────────────
//
// This is the demo's private interpretation of the lane's opaque fixed 16-byte
// unit. It is NOT the EDGE Netstream wire format. All multi-byte fields are
// little-endian byte pairs (explicit u8 fields => no padding, no alignment or
// endianness assumptions). T4 (client receive jiffy) is captured locally and is
// NOT in the packet.
struct MeterPacket16 {
    u8 magic;          //  0  demo id (0xE7) — reject foreign UDP
    u8 version;        //  1  payload version (0x01)
    u8 type;           //  2  0=meter/echo-request, 1=peer-reply, 2=ticker
    u8 client_status;  //  3  bit0 active, bit1 rx_overflowed, bit2 warmup-done
    u8 cseq_lo, cseq_hi;  // 4..5  client_seq (LE)
    u8 pseq_lo, pseq_hi;  // 6..7  peer_seq   (LE)
    u8 t1_lo, t1_hi;      // 8..9  T1 client send jiffy   (LE, echoed unchanged)
    u8 t2_lo, t2_hi;      // 10..11 T2 peer receive virtual jiffy (LE)
    u8 t3_lo, t3_hi;      // 12..13 T3 peer transmit virtual jiffy (LE)
    u8 peer_status;       // 14    peer flags
    u8 pattern;           // 15    reserved/sanity pattern (0x5A)
};
static_assert(sizeof(MeterPacket16) == 16, "MeterPacket16 must be exactly 16 bytes");

static constexpr u8 kMagic    = 0xE7;
static constexpr u8 kVersion  = 0x01;
static constexpr u8 kPattern  = 0x5A;
static constexpr u8 kTypeReq  = 0;
static constexpr u8 kTypeReply= 1;
[[maybe_unused]] static constexpr u8 kTypeTick = 2;  // peer ticker; client treats as plain RX

static inline u16 rd16(u8 lo, u8 hi) { return static_cast<u16>(lo | (hi << 8)); }
static inline void wr16(u8& lo, u8& hi, u16 v) { lo = static_cast<u8>(v); hi = static_cast<u8>(v >> 8); }

// ── Atari OS frame clock (RTCLOK) — torn-read-safe 16-bit jiffy read ───────────
//
// Reading the Atari OS frame clock inside this Atari-specific demo is fine — it is
// NOT HAL/internal *networking* access, and it is kept in exactly one place. The
// 3-byte counter at $12/$13/$14 is bumped by the OS VBI, so a naive two-byte read
// can tear across a tick; read high/low/high and retry if the high byte changed.
static inline u16 read_jiffy16() {
    volatile const u8* const rtc = reinterpret_cast<volatile const u8*>(0x0012); // $12,$13,$14
    for (;;) {
        const u8 hi = rtc[1];                       // $13
        const u8 lo = rtc[2];                       // $14
        if (rtc[1] == hi) return static_cast<u16>((hi << 8) | lo);  // no tick crossed
    }
}

// Signed shortest-path delta on a u16 jiffy ring (handles 16-bit wrap).
static inline i16 sub16(u16 a, u16 b) { return static_cast<i16>(static_cast<u16>(a - b)); }

// ── Graph glyph policy (demo-local; platform-definable) ───────────────────────
//
// The sparkline renderer asks the policy for a tile per quantised level, so the
// graph glyphs are pluggable per platform. This Atari policy returns authored
// custom-charset tiles (see make_meter_charset) rather than guessed ROM glyphs.
static constexpr u8 kGlyphBarBase = 0x40;   // internal codes 0x40..0x47 = 8 bar tiles
struct AtariGlyphPolicy {
    static constexpr u8 kLevels = 8;
    static constexpr u8 blank()        { return 0x00; }                 // space tile
    static constexpr u8 bar(u8 level)  { return static_cast<u8>(kGlyphBarBase + (level & 0x07)); }
};

// ── Custom Mode-2 charset: remapped font + authored graph tiles ───────────────
//
// PREPPIEPC_FONT is PC/ASCII-ordered (index N == ASCII N), but the engine writes
// Atari INTERNAL screen codes (print() maps ASCII->internal via ascii_to_internal,
// where internal ic == ASCII ic+0x20 for ic in 0x00..0x5F). So remap the font into
// internal order, then overlay 8 vertical-bar graph tiles at the unused (lowercase)
// internal codes 0x40..0x47 — the HUD never prints lowercase.
constexpr engine::Charset1K make_meter_charset() {
    engine::Charset1K cs{};
    for (u16 ic = 0; ic <= 0x5F; ++ic)
        for (u8 r = 0; r < 8; ++r)
            cs.data[ic * 8 + r] = edge::assets::PREPPIEPC_FONT[ic + 0x20][r];
    for (u8 lvl = 0; lvl < 8; ++lvl) {                 // bar level 0..7 (rises from bottom)
        const u16 o = static_cast<u16>(kGlyphBarBase + lvl) * 8;
        for (u8 r = 0; r < 8; ++r)
            cs.data[o + r] = (r >= (7 - lvl)) ? 0xFF : 0x00;
    }
    return cs;
}
static constexpr engine::Charset1K g_charset = make_meter_charset();

// ── Demo state ────────────────────────────────────────────────────────────────
static u16 g_tx_seq   = 0;     // next client_seq to send
static u16 g_rx_seq   = 0;     // last seen peer/client seq from a reply
static u16 g_rx_count = 0;     // total replies received
static u16 g_last_rx_jiffy = 0;
static bool g_have_rx = false;
static u8  g_frame    = 0;     // send-cadence counter
static u8  g_hud_frame = static_cast<u8>(kHudPeriod - 1);  // HUD-cadence counter (draw on 1st frame)
static u8  g_warmup   = kWarmupFrames;
static u8  g_last_send_status = 0;  // last NetStatus from send()
static bool g_overflow_sticky = false;

static i16 g_delay = 0, g_offset = 0, g_jitter = 0;
static i16 g_last_delay = 0;
static bool g_have_delay = false;

// ── Link-quality window (reliability + true throughput, recomputed per ~1s RTCLOK
// window). peer_seq is the peer's reply counter = forward survivors, so window
// deltas split loss by direction without any packet change. See the README. ──
static u16 g_peer_seq = 0;          // last peer_seq seen (forward-survivor counter)
static bool g_win_init = false;
static u16 g_win_jiffy0 = 0;        // window start: jiffy + counter snapshots
static u16 g_win_tx0 = 0, g_win_got0 = 0, g_win_pseq0 = 0;
static u16 g_tx_hz = 0,  g_rx_hz = 0;     // packets/sec (measured)
static u16 g_tx_bps = 0, g_rx_bps = 0;    // bytes/sec (= pkts/sec * 16)
static u8  g_loss_rt = 0, g_loss_fwd = 0, g_loss_rev = 0;   // percent 0..99
static u8  g_loss_worst = 0, g_loss_best = 99;              // round-trip loss range

// Sparkline rings (oldest..newest, left-to-right). Shift-on-push; tiny at -O2.
static i16 g_spark_delay[kSparkW]  = {};
static i16 g_spark_offset[kSparkW] = {};
static i16 g_spark_jitter[kSparkW] = {};

static void spark_push(i16* ring, i16 v) {
    for (u8 i = 0; i < kSparkW - 1; ++i) ring[i] = ring[i + 1];
    ring[kSparkW - 1] = v;
}

// ── Rendering helpers ─────────────────────────────────────────────────────────
static const char* status_label(u8 s) {
    switch (static_cast<net::NetStatus>(s)) {
        case net::NetStatus::Ok:              return "OK ";
        case net::NetStatus::WouldBlock:      return "WB ";
        case net::NetStatus::Closed:          return "CLO";
        case net::NetStatus::Overflow:        return "OVF";
        case net::NetStatus::InvalidArgument: return "ARG";
        case net::NetStatus::BadConfig:       return "CFG";
        case net::NetStatus::Unsupported:     return "UNS";
        case net::NetStatus::TransportError:  return "TXE";
    }
    return "???";
}

// Print a signed value right-aligned in `digits`, with a leading sign column.
static void print_signed(u8 col, u8 row, i16 v, u8 digits) {
    const bool neg = v < 0;
    Game::put_char(col, row, neg ? 0x0D /*'-' internal*/ : 0x0B /*'+' internal*/);
    const u16 mag = static_cast<u16>(neg ? -v : v);
    Game::print_num(static_cast<u8>(col + 1), row, mag, digits);
}

// Print a 2-digit percent followed by '%' (ASCII 0x25 -> internal screen code 0x05).
static void print_pct(u8 col, u8 row, u8 v) {
    Game::print_num(col, row, v, 2);
    Game::put_char(static_cast<u8>(col + 2), row, 0x05 /*'%' internal*/);
}

// Percent of `sent` not accounted for by `recv`, clamped to 0..99.
static u8 pct_loss(u16 sent, u16 recv) {
    if (sent == 0 || recv >= sent) return 0;
    const u16 p = static_cast<u16>(
        (static_cast<uint32_t>(sent - recv) * 100u) / sent);
    return p > 99 ? 99 : static_cast<u8>(p);
}

// Once per ~1s RTCLOK window, turn the raw counters into measured rates and
// directional loss. Δtx sent, Δpseq forward survivors (peer replied), Δgot replies
// received -> forward = 1-Δpseq/Δtx, reverse = 1-Δgot/Δpseq, round-trip = 1-Δgot/Δtx.
static void update_link_window(u16 now) {
    if (!g_win_init) {
        g_win_init = true;
        g_win_jiffy0 = now;
        g_win_tx0 = g_tx_seq; g_win_got0 = g_rx_count; g_win_pseq0 = g_peer_seq;
        return;
    }
    // Average over kWinSecs (more packets per window => far less loss-% noise at low
    // rates); rates are still reported per second.
    constexpr u16 kWinFrames = static_cast<u16>(kFps) * kWinSecs;
    if (static_cast<u16>(sub16(now, g_win_jiffy0)) < kWinFrames) return;

    const u16 d_tx  = static_cast<u16>(sub16(g_tx_seq,   g_win_tx0));
    const u16 d_got = static_cast<u16>(sub16(g_rx_count, g_win_got0));
    const u16 d_ps  = static_cast<u16>(sub16(g_peer_seq, g_win_pseq0));
    g_tx_hz = static_cast<u16>(d_tx  / kWinSecs);   // packets/sec
    g_rx_hz = static_cast<u16>(d_got / kWinSecs);
    g_tx_bps = static_cast<u16>(g_tx_hz * 16u);
    g_rx_bps = static_cast<u16>(g_rx_hz * 16u);
    g_loss_rt  = pct_loss(d_tx, d_got);   // round-trip
    g_loss_fwd = pct_loss(d_tx, d_ps);    // Atari -> peer
    g_loss_rev = pct_loss(d_ps, d_got);   // peer  -> Atari
    if (d_tx > 0) {
        if (g_loss_rt > g_loss_worst) g_loss_worst = g_loss_rt;
        if (g_loss_rt < g_loss_best)  g_loss_best  = g_loss_rt;
    }
    g_win_jiffy0 = now;
    g_win_tx0 = g_tx_seq; g_win_got0 = g_rx_count; g_win_pseq0 = g_peer_seq;
}

template <typename Policy>
static void draw_sparkline(u8 col, u8 row, const i16* ring, i16 lo, i16 hi) {
    const i16 span = (hi > lo) ? static_cast<i16>(hi - lo) : 1;
    for (u8 i = 0; i < kSparkW; ++i) {
        i16 c = ring[i];
        if (c < lo) c = lo;
        if (c > hi) c = hi;
        const u8 level = static_cast<u8>(
            (static_cast<int32_t>(c - lo) * (Policy::kLevels - 1)) / span);
        Game::put_char(static_cast<u8>(col + i), row, Policy::bar(level));
    }
}

static void draw_static_labels() {
    Game::print(0, 0,  "EDGE NET REALTIME METER");
    Game::print(0, 1,  "HOST");
    Game::print(0, 3,  "TX");
    Game::print(11, 3, "RX");
    Game::print(22, 3, "RXC");
    Game::print(32, 3, "GOT");
    Game::print(0, 4,  "AGE");
    Game::print(12, 4, "LAST");
    Game::print(24, 4, "DRP");
    Game::print(33, 4, "OVF");
    Game::print(0, 6,  "DELAY");
    Game::print(24, 6, "RTT");
    Game::print(0, 7,  "OFFSET");
    Game::print(24, 7, "JIT");
    Game::print(0, 8,  "STALE");
    Game::print(20, 8, "WARMUP");
    Game::print(0, 10, "DELAY");
    Game::print(0, 11, "OFFS");
    Game::print(0, 12, "JITTER");
    // Link-quality block (reliability + true throughput).
    Game::print(0, 14, "---- LINK QUALITY (4S) ----");
    Game::print(0, 15, "TXHZ");
    Game::print(11, 15, "RXHZ");
    Game::print(0, 16, "TXBPS");
    Game::print(12, 16, "RXBPS");
    Game::print(0, 17, "LOSS RT");
    Game::print(12, 17, "FWD");
    Game::print(20, 17, "REV");
    Game::print(0, 18, "WORST");
    Game::print(12, 18, "BEST");
    // Engine frame-overrun diagnostic: frames the game loop failed to consume in
    // time (dropped) out of total frames serviced. Reads Game::frames_dropped()/
    // frames_serviced() — a nonzero DROP means the per-frame path overran 60Hz.
    Game::print(0, 20, "FRM DROP");
    Game::print(15, 20, "OF");
    Game::print(0, 23, "FIRE = QUIT");
    // Peer endpoint + TV/fps line (host string then ":port  ECHO  NTSC <fps>").
    Game::print(5, 1, kPeerHost);
    Game::print(28, 1, "NTSC");
    Game::print_num(33, 1, kFps, 2);
}

static void draw_dynamic(bool active, u16 now) {
    Game::print(28, 0, active ? "ACTIVE " : "FAILED ");  // trailing space clears "OPENING"'s G

    Game::print_num(3, 3, g_tx_seq, 5);
    Game::print_num(14, 3, g_rx_seq, 5);
    Game::print_num(26, 3, Game::net.realtime.rx_count(), 2);
    Game::put_char(28, 3, 0x1A /*':'*/);
    Game::print_num(29, 3, Game::net.realtime.rx_capacity(), 2);
    Game::print_num(35, 3, g_rx_count, 5);

    const u16 age = g_have_rx ? static_cast<u16>(sub16(now, g_last_rx_jiffy)) : 0;
    Game::print_num(4, 4, g_have_rx ? age : 999, 3);
    Game::print(17, 4, status_label(g_last_send_status));
    Game::print_num(28, 4, Game::net.realtime.rx_dropped(), 4);
    Game::print(37, 4, g_overflow_sticky ? "Y" : "-");

    if (g_have_delay) {
        Game::print_num(6, 6, static_cast<u16>(g_delay < 0 ? 0 : g_delay), 3);
        Game::print(9, 6, "F ~");
        // Approximate ms only (frames are authoritative): d * 1000 / fps.
        const u16 ms = static_cast<u16>(
            (static_cast<uint32_t>(g_delay < 0 ? 0 : g_delay) * 1000u) / (kFps ? kFps : 60));
        Game::print_num(12, 6, ms, 4);
        Game::print(16, 6, "MS");
        Game::print_num(28, 6, static_cast<u16>(g_delay < 0 ? 0 : g_delay), 3);
        print_signed(7, 7, g_offset, 3);
        Game::print_num(28, 7, static_cast<u16>(g_jitter < 0 ? 0 : g_jitter), 3);
    }

    const bool stale = !g_have_rx || age > kStaleFrames;
    Game::print(6, 8, stale ? "YES" : "NO ");
    Game::print_num(27, 8, g_warmup, 2);

    draw_sparkline<AtariGlyphPolicy>(8, 10, g_spark_delay, 0, kDelayFull);
    draw_sparkline<AtariGlyphPolicy>(8, 11, g_spark_offset, static_cast<i16>(-kOffsetSpan), kOffsetSpan);
    draw_sparkline<AtariGlyphPolicy>(8, 12, g_spark_jitter, 0, kJitterFull);

    // Link quality (measured per ~1s window).
    Game::print_num(5, 15, g_tx_hz, 3);
    Game::print_num(16, 15, g_rx_hz, 3);
    Game::print_num(6, 16, g_tx_bps, 5);
    Game::print_num(18, 16, g_rx_bps, 5);
    print_pct(8, 17, g_loss_rt);
    print_pct(16, 17, g_loss_fwd);
    print_pct(24, 17, g_loss_rev);
    print_pct(6, 18, g_loss_worst);
    print_pct(17, 18, g_loss_best);

    // Frame-overrun diagnostic (engine counter): dropped / serviced.
    Game::print_num(9, 20, Game::frames_dropped(), 5);
    Game::print_num(18, 20, Game::frames_serviced(), 5);
}

// ── Per-frame step ────────────────────────────────────────────────────────────
static bool step(const engine::InputState<>& in) {
    const bool active = Game::net.realtime.active();

    if (active) {
        Game::net.realtime.poll();

        // Drain every available reply; T4 captured locally on receive.
        MeterPacket16 pkt{};
        while (Game::net.realtime.recv(pkt)) {
            if (pkt.magic != kMagic) continue;          // ignore foreign payloads
            const u16 t4 = read_jiffy16();
            g_have_rx = true;
            g_last_rx_jiffy = t4;
            ++g_rx_count;
            g_rx_seq = rd16(pkt.cseq_lo, pkt.cseq_hi);
            g_peer_seq = rd16(pkt.pseq_lo, pkt.pseq_hi);   // forward-survivor counter

            if (pkt.type == kTypeReply) {
                const u16 t1 = rd16(pkt.t1_lo, pkt.t1_hi);
                const u16 t2 = rd16(pkt.t2_lo, pkt.t2_hi);
                const u16 t3 = rd16(pkt.t3_lo, pkt.t3_hi);
                const i16 delay  = static_cast<i16>(sub16(t4, t1) - sub16(t3, t2));
                const i16 offset = static_cast<i16>(
                    (static_cast<int32_t>(sub16(t2, t1)) + sub16(t3, t4)) / 2);

                if (g_warmup == 0) {                    // ignore POST-OPEN WARMUP samples
                    g_delay = delay;
                    g_offset = offset;
                    const i16 d = static_cast<i16>(delay - g_last_delay);
                    const i16 inst = static_cast<i16>(d < 0 ? -d : d);
                    // Integer EWMA jitter (RFC3550-style): J += (|d| - J) / 4.
                    g_jitter = g_have_delay
                        ? static_cast<i16>(g_jitter + ((inst - g_jitter) >> 2))
                        : inst;
                    g_last_delay = delay;
                    g_have_delay = true;
                    spark_push(g_spark_delay, g_delay);
                    spark_push(g_spark_offset, g_offset);
                    spark_push(g_spark_jitter, g_jitter);
                }
            }
        }

        if (Game::net.realtime.consume_rx_overflowed()) g_overflow_sticky = true;

        // Steady send cadence: one meter request every kSendPeriod frames.
        if (++g_frame >= kSendPeriod) {
            g_frame = 0;
            MeterPacket16 req{};
            req.magic = kMagic;
            req.version = kVersion;
            req.type = kTypeReq;
            req.client_status = static_cast<u8>(
                (active ? 0x01 : 0) | (g_overflow_sticky ? 0x02 : 0) | (g_warmup == 0 ? 0x04 : 0));
            wr16(req.cseq_lo, req.cseq_hi, g_tx_seq);
            wr16(req.t1_lo, req.t1_hi, read_jiffy16());
            req.peer_status = 0;
            req.pattern = kPattern;
            const net::NetStatus st = Game::net.realtime.send(req);
            g_last_send_status = static_cast<u8>(st);
            ++g_tx_seq;
        }

        if (g_warmup > 0) --g_warmup;
    }

    const u16 now = read_jiffy16();
    if (active) update_link_window(now);
    // Redraw the HUD every kHudPeriod frames, not every frame: the full redraw is the
    // loop's dominant cost, so throttling it lets the per-frame net path run near the
    // kSendPeriod cadence (see kHudPeriod). All metric state is updated above every
    // frame; only the rendering is throttled.
    if (++g_hud_frame >= kHudPeriod) {
        g_hud_frame = 0;
        draw_dynamic(active, now);
    }
    return in.fire();
}

int main() {
    Game::init(g_charset);
    draw_static_labels();

    // The Netstream hardware settle runs INSIDE open_udp_seq() and blocks until it
    // returns — we cannot poll it. Show intent before the (blocking) call, result
    // after. This is the real settle; the POST-OPEN WARMUP below is demo-only.
    Game::print(28, 0, "OPENING");
    Game::print(0, 2, "NETSTREAM SETTLE...");
    const net::NetStatus st = Game::net.realtime.open_udp_seq(kPeerHost, kPeerPort, kLocalPort);
    Game::print(0, 2, "                   ");   // clear the settle banner

    if (st != net::NetStatus::Ok || !Game::net.realtime.active()) {
        Game::print(28, 0, "FAILED");
        const net::NetError e = Game::net.realtime.last_error();
        Game::print(0, 2, "ERR ST");
        Game::print_num(7, 2, static_cast<u16>(e.status), 2);
        Game::print(10, 2, "DET");
        print_signed(14, 2, e.detail, 5);
    } else {
        Game::print(28, 0, "ACTIVE");
    }

    // Zero the frame-overrun counters after all one-shot setup (charset load,
    // screen bring-up, blocking netstream settle) so the on-screen DROP/SVC
    // readout measures only the steady-state run loop, not boot-time stalls.
    Game::reset_frame_stats();
    Game::run_until(step);
    return 0;
}
