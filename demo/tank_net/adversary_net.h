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

// ── Wire packet — fixed 16 bytes, the realtime lane's opaque unit ──────────────
//
// The demo's private interpretation of the lane's fixed 16-byte packet (NOT the
// EDGE Netstream wire format). Explicit u8 fields => no padding, no alignment or
// endianness assumptions; multi-byte fields are little-endian byte pairs. ONE
// layout, two directions distinguished by `type`:
//   type 0  server -> client : adversary state snapshot
//   type 1  client -> server : local-player state (so the server's adversary AI
//                              can react — chase/avoid — and learn our address)
struct TankPacket16 {
    u8 magic;            //  0  0xAD — reject foreign UDP
    u8 version;          //  1  0x01
    u8 type;             //  2  0 = adversary (S->C), 1 = player (C->S)
    u8 status;           //  3  bit0 have_state / alive
    u8 seq_lo, seq_hi;   //  4..5  u16 LE sequence (loss / reorder / stale detect)
    u8 x_lo, x_hi;       //  6..7  world_x nominal px, u16 LE (0..639)
    u8 y_lo, y_hi;       //  8..9  world_y nominal px, u16 LE (0..383)
    u8 heading;          // 10     0..15 (drives silhouette + motion vector)
    u8 speed;            // 11     dead-reckon move steps per frame (0 = stopped)
    u8 reserved[3];      // 12..14
    u8 pattern;          // 15     0x5A sanity
};
static_assert(sizeof(TankPacket16) == 16, "TankPacket16 must be exactly 16 bytes");

inline constexpr u8 kMagic   = 0xAD;
inline constexpr u8 kVersion = 0x01;
inline constexpr u8 kPattern = 0x5A;
inline constexpr u8 kTypeAdversary = 0;   // server -> client
inline constexpr u8 kTypePlayer    = 1;   // client -> server
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
    u16 last_seq;           // last accepted snapshot sequence
    u8  have_state;         // 0 until the first valid snapshot
};

// Apply an authoritative snapshot: validate, drop stale/foreign, else SNAP the
// adversary onto the server's position and adopt its heading + speed.
//
// SNAP rationale (prototype): cheap and deterministic (no 6502 divide); if the
// server's speed/heading match this client's motion_vector magnitudes, the snap
// only corrects accumulated rounding, not a visible teleport.
// TODO(interp): to hide snap jumps under packet loss, instead of writing world_*_q4
// directly here, store a correction TARGET and ease world_*_q4 toward it over the
// next ~6 frames in adv_dead_reckon (needs a per-frame fractional delta = a divide).
inline bool adv_apply_packet(Adversary& adv, const TankPacket16& p) {
    if (p.magic != kMagic || p.type != kTypeAdversary) return false;
    const u16 seq = rd16(p.seq_lo, p.seq_hi);
    if (adv.have_state && seq_delta(seq, adv.last_seq) <= 0) return false;  // stale/dup

    const u16 x = rd16(p.x_lo, p.x_hi);
    const u16 y = rd16(p.y_lo, p.y_hi);
    adv.s.world_x_q4 = static_cast<i16>(static_cast<i16>(x) << 4);   // SNAP (interp seam)
    adv.s.world_y_q4 = static_cast<i16>(static_cast<i16>(y) << 4);
    adv.s.heading    = static_cast<u8>(p.heading & 15);
    adv.speed        = p.speed;
    tank::clamp_world(adv.s);
    adv.last_seq   = seq;
    adv.have_state = 1;
    return true;
}

// Advance the adversary one frame between snapshots: step `speed` times along the
// last-known heading via the shared ROM motion table + world clamp (same cadence
// as the local tank, so DR closely tracks the authoritative path).
inline void adv_dead_reckon(Adversary& adv) {
    if (!adv.have_state) return;
    for (u8 i = 0; i < adv.speed; ++i) tank::move_tank(adv.s, static_cast<engine::i8>(1));
}

// Build the local-player snapshot the client streams back to the server (type 1).
inline TankPacket16 make_player_packet(const tank::TankState& s, u8 speed, u16 seq) {
    TankPacket16 p{};
    p.magic   = kMagic;
    p.version = kVersion;
    p.type    = kTypePlayer;
    p.status  = kStatusHaveState;
    wr16(p.seq_lo, p.seq_hi, seq);
    wr16(p.x_lo, p.x_hi, static_cast<u16>(tank::world_x_nominal(s)));
    wr16(p.y_lo, p.y_hi, static_cast<u16>(tank::world_y_nominal(s)));
    p.heading = static_cast<u8>(s.heading & 15);
    p.speed   = speed;
    p.pattern = kPattern;
    return p;
}

}  // namespace tanknet

#endif  // DEMO_TANK_NET_ADVERSARY_NET_H
