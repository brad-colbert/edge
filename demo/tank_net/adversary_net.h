#ifndef DEMO_TANK_NET_ADVERSARY_NET_H
#define DEMO_TANK_NET_ADVERSARY_NET_H

// adversary_net.h — networked-adversary packet + client-side motion (demo-local,
// pure). Shared by atari_tank_net_demo.cpp and the host test_adversary_net.
//
// The "bigger tank demo" adds a second tank whose authoritative position /
// heading / speed is streamed from a Python UDP server at ~10 Hz over the EDGE
// realtime lane (ADR-015: the host sends STATE snapshots, not input). At 60 fps a
// snapshot lands only every ~6 frames, so between snapshots the client DEAD-RECKONS
// the adversary forward with the last-known heading + speed using the SAME motion
// machinery as the local tank, then SNAPS to each new authoritative position.
//
// Pure: no engine API, no floating point, no runtime multiply/divide/trig — it
// reuses tank_motion.h (move_tank / motion_vector / clamp_world / silhouette_for).

#include <engine/types.h>

#include "tank_motion.h"   // tank::TankState, move_tank, world_*_nominal

namespace tanknet {

using engine::u8;
using engine::u16;
using engine::i16;

// ── Wire packet — fixed 32 bytes, the realtime lane's opaque unit ──────────────
//
// The demo's private interpretation of the lane's fixed 32-byte packet (NOT the
// EDGE Netstream wire format; the demo widens the lane via
// GameConfig::realtime_packet_bytes). Explicit u8 fields => no padding, no alignment
// or endianness assumptions; multi-byte fields are little-endian byte pairs.
//
// ONE combined packet per tick carries ALL adversaries — this packs 3 adversaries
// into a single S->C packet (10 pkt/s) instead of one packet each (30 pkt/s), the
// downstream-overload mitigation. ONE layout, two directions by `type`:
//   type 0  server -> client : up to kMaxAdv adversary records; `status` bit i = rec[i] live.
//   type 1  client -> server : local-player state in rec[0] (status bit0 = have_state),
//                              plus the timing-echo fields (see make_player_packet).
struct AdvRecord {       // 6 bytes — one tank's authoritative state
    u8 x_lo, x_hi;       //  world_x nominal px, u16 LE (0..639)
    u8 y_lo, y_hi;       //  world_y nominal px, u16 LE (0..383)
    u8 heading;          //  0..15 (drives silhouette + motion vector)
    u8 speed;            //  dead-reckon move steps per frame (0 = stopped)
};
static_assert(sizeof(AdvRecord) == 6, "AdvRecord must be exactly 6 bytes");

inline constexpr u8 kMaxAdv = 3;          // adversary records per combined packet

struct TankPacket32 {
    u8 magic;                  //  0      0xAD — reject foreign UDP
    u8 version;                //  1      0x02 (combined-packet format)
    u8 type;                   //  2      0 = adversary (S->C), 1 = player (C->S)
    u8 status;                 //  3      S->C: bit i = rec[i] live; C->S: bit0 have_state
    u8 seq_lo, seq_hi;         //  4..5   u16 LE sequence (loss / reorder / stale detect)
    AdvRecord rec[kMaxAdv];    //  6..23  adversary records (C->S: rec[0] = player)
    u8 echo_seq_lo, echo_seq_hi;  // 24..25  C->S: adv[0] last-applied seq (timing echo)
    u8 echo_flags;             // 26     C->S: client health byte (RX-overflow events)
    u8 reserved[4];            // 27..30
    u8 pattern;                // 31     0x5A sanity
};
static_assert(sizeof(TankPacket32) == 32, "TankPacket32 must be exactly 32 bytes");

inline constexpr u8 kMagic   = 0xAD;
inline constexpr u8 kVersion = 0x02;
inline constexpr u8 kPattern = 0x5A;
inline constexpr u8 kTypeAdversary = 0;   // server -> client
inline constexpr u8 kTypePlayer    = 1;   // client -> server
inline constexpr u8 kTypeBye       = 2;   // client -> server: leaving; stop streaming
inline constexpr u8 kStatusHaveState = 0x01;

inline constexpr u16 rd16(u8 lo, u8 hi) { return static_cast<u16>(lo | (hi << 8)); }
inline void wr16(u8& lo, u8& hi, u16 v) {
    lo = static_cast<u8>(v & 0xFF);
    hi = static_cast<u8>(v >> 8);
}

// Signed shortest-path distance new-old on the u16 sequence ring (handles wrap):
// > 0 ⇒ `a` is newer than `b`, <= 0 ⇒ same or older (stale). Mirrors the Python
// peer's seq_delta so both ends agree on ordering.
inline i16 seq_delta(u16 a, u16 b) {
    return static_cast<i16>(static_cast<u16>(a - b));
}

// ── Adversary state ────────────────────────────────────────────────────────────
struct Adversary {
    tank::TankState s;      // reuse hull-centre Q12.4 world pos + heading
    u8  speed;              // last-known dead-reckon speed (steps/frame)
    u8  have_state;         // 0 until the first valid snapshot
};

// Shared packet-level sequence gate. ONE combined packet now carries all adversaries,
// so stale/dup detection is per-PACKET (not per-adversary): the whole snapshot is
// accepted or dropped together.
struct AdvRxState {
    u16 last_seq;           // last accepted combined-packet sequence
    u8  have_state;         // 0 until the first valid packet
};

// Apply a combined authoritative snapshot: validate, drop stale/foreign, else SNAP
// each LIVE adversary (status bit i) onto the server's position and adopt heading +
// speed. Returns true if the packet was applied (seq strictly newer).
//
// SNAP rationale (prototype): cheap and deterministic (no 6502 divide); if the
// server's speed/heading match this client's motion_vector magnitudes, the snap
// only corrects accumulated rounding, not a visible teleport.
inline bool adv_apply_packet(Adversary* adv, u8 count, AdvRxState& rx,
                             const TankPacket32& p) {
    if (p.magic != kMagic || p.type != kTypeAdversary) return false;
    const u16 seq = rd16(p.seq_lo, p.seq_hi);
    if (rx.have_state && seq_delta(seq, rx.last_seq) <= 0) return false;  // stale/dup

    const u8 n = (count < kMaxAdv) ? count : kMaxAdv;
    for (u8 i = 0; i < n; ++i) {
        if (!(p.status & (1u << i))) continue;             // adversary not live this packet
        const AdvRecord& r = p.rec[i];
        const u16 x = rd16(r.x_lo, r.x_hi);
        const u16 y = rd16(r.y_lo, r.y_hi);
        adv[i].s.world_x_q4 = static_cast<i16>(static_cast<i16>(x) << 4);   // SNAP
        adv[i].s.world_y_q4 = static_cast<i16>(static_cast<i16>(y) << 4);
        adv[i].s.heading    = static_cast<u8>(r.heading & 15);
        adv[i].speed        = r.speed;
        tank::clamp_world(adv[i].s);
        adv[i].have_state = 1;
    }
    rx.last_seq   = seq;
    rx.have_state = 1;
    return true;
}

// Advance one adversary one frame between snapshots: step `speed` times along the
// last-known heading via the shared ROM motion table + world clamp (same cadence
// as the local tank, so DR closely tracks the authoritative path).
inline void adv_dead_reckon(Adversary& adv) {
    if (!adv.have_state) return;
    for (u8 i = 0; i < adv.speed; ++i) tank::move_tank(adv.s, static_cast<engine::i8>(1));
}

// Build the local-player snapshot the client streams back to the server (type 1):
// player state in rec[0], status bit0 = have_state.
//
// TIMING ECHO (diagnostic): the C->S packet piggybacks what the client has actually
// applied — `echo_adv_seq` is adv[0]'s last accepted snapshot sequence and
// `echo_flags` is a small client-side health byte (a saturating RX-overflow-event
// count). The server compares echo_adv_seq against the seq it most recently SENT to
// measure end-to-end lag in packets (≈ ms via the known send rate) and whether it
// grows over time.
inline TankPacket32 make_player_packet(const tank::TankState& s, u8 speed, u16 seq,
                                       u16 echo_adv_seq, u8 echo_flags) {
    TankPacket32 p{};
    p.magic   = kMagic;
    p.version = kVersion;
    p.type    = kTypePlayer;
    p.status  = kStatusHaveState;
    wr16(p.seq_lo, p.seq_hi, seq);
    wr16(p.rec[0].x_lo, p.rec[0].x_hi, static_cast<u16>(tank::world_x_nominal(s)));
    wr16(p.rec[0].y_lo, p.rec[0].y_hi, static_cast<u16>(tank::world_y_nominal(s)));
    p.rec[0].heading = static_cast<u8>(s.heading & 15);
    p.rec[0].speed   = speed;
    wr16(p.echo_seq_lo, p.echo_seq_hi, echo_adv_seq);  // adv[0] last-applied seq
    p.echo_flags = echo_flags;                         // client health byte
    p.pattern = kPattern;
    return p;
}

// Build the "leaving" packet the client streams just before it closes the realtime
// lane (type 2). The server stops streaming this client and returns to waiting for a
// new asset-transfer request. Carries no state — just magic/version/type + seq.
inline TankPacket32 make_bye_packet(u16 seq) {
    TankPacket32 p{};
    p.magic   = kMagic;
    p.version = kVersion;
    p.type    = kTypeBye;
    wr16(p.seq_lo, p.seq_hi, seq);
    p.pattern = kPattern;
    return p;
}

}  // namespace tanknet

#endif  // DEMO_TANK_NET_ADVERSARY_NET_H
