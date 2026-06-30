// test_adversary_net.cpp — networked-adversary packet + dead-reckoning invariants
// (demo/tank_net/adversary_net.h). Built for mos-sim, run under CTest (0 = pass).
// Pure logic; no Atari hardware, no transport.
//
// The lane now carries ONE combined 32-byte packet per tick holding up to kMaxAdv
// adversary records (status bit i = rec[i] live); stale/dup detection is per-PACKET.

#include <stdint.h>
#include <stdio.h>

#include "demo/tank_net/adversary_net.h"

using engine::u8;
using engine::u16;
using engine::i16;

static unsigned g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// One adversary record (x,y nominal px, heading, speed).
static tanknet::AdvRecord rec_of(u16 x, u16 y, u8 h, u8 sp) {
    tanknet::AdvRecord r{};
    tanknet::wr16(r.x_lo, r.x_hi, x);
    tanknet::wr16(r.y_lo, r.y_hi, y);
    r.heading = h;
    r.speed   = sp;
    return r;
}

// Build a combined authoritative adversary packet (type 0): n records, status bit i set.
static tanknet::TankPacket32 make_adv_packet(u16 seq, const tanknet::AdvRecord* recs, u8 n) {
    tanknet::TankPacket32 p{};
    p.magic   = tanknet::kMagic;
    p.version = tanknet::kVersion;
    p.type    = tanknet::kTypeAdversary;
    tanknet::wr16(p.seq_lo, p.seq_hi, seq);
    for (u8 i = 0; i < n && i < tanknet::kMaxAdv; ++i) {
        p.rec[i] = recs[i];
        p.status = static_cast<u8>(p.status | (1u << i));
    }
    p.pattern = tanknet::kPattern;
    return p;
}

// One-adversary convenience packet (only rec[0] live).
static tanknet::TankPacket32 make_adv1(u16 seq, u16 x, u16 y, u8 h, u8 sp) {
    const tanknet::AdvRecord r = rec_of(x, y, h, sp);
    return make_adv_packet(seq, &r, 1);
}

// ── Packet layout + player-packet build/round-trip ─────────────────────────
static void test_packet_roundtrip() {
    CHECK(sizeof(tanknet::TankPacket32) == 32);
    CHECK(sizeof(tanknet::AdvRecord) == 6);

    tank::TankState s{};
    s.world_x_q4 = static_cast<i16>(300 << 4);
    s.world_y_q4 = static_cast<i16>(200 << 4);
    s.heading    = 6;
    const tanknet::TankPacket32 p =
        tanknet::make_player_packet(s, 1, 42, /*echo_adv_seq=*/1234, /*echo_flags=*/7);

    CHECK(p.magic == tanknet::kMagic);
    CHECK(p.version == tanknet::kVersion);
    CHECK(p.type == tanknet::kTypePlayer);
    CHECK(p.pattern == tanknet::kPattern);
    CHECK(tanknet::rd16(p.seq_lo, p.seq_hi) == 42);
    // Player state lives in rec[0].
    CHECK(tanknet::rd16(p.rec[0].x_lo, p.rec[0].x_hi) == 300);
    CHECK(tanknet::rd16(p.rec[0].y_lo, p.rec[0].y_hi) == 200);
    CHECK(p.rec[0].heading == 6);
    CHECK(p.rec[0].speed == 1);
    // Timing echo round-trips in the dedicated fields.
    CHECK(tanknet::rd16(p.echo_seq_lo, p.echo_seq_hi) == 1234);
    CHECK(p.echo_flags == 7);
    // No hit_count argument (default nullptr) => all counters zero.
    for (engine::u8 i = 0; i < tanknet::kMaxAdv; ++i) CHECK(p.hit_count[i] == 0);

    // With per-adversary hit counters supplied, they round-trip into hit_count[].
    const engine::u8 hits[tanknet::kMaxAdv] = {5, 0, 200};
    const tanknet::TankPacket32 ph =
        tanknet::make_player_packet(s, 1, 43, 1234, 7, hits);
    CHECK(ph.hit_count[0] == 5);
    CHECK(ph.hit_count[1] == 0);
    CHECK(ph.hit_count[2] == 200);
}

// ── apply: validate, snap, reject foreign/stale ────────────────────────────
static void test_apply_snap_and_validation() {
    tanknet::Adversary adv[1] = {};
    tanknet::AdvRxState rx{};

    // First valid snapshot: snaps position, adopts heading/speed, marks have_state.
    CHECK(tanknet::adv_apply_packet(adv, 1, rx, make_adv1(10, 250, 150, 4, 1)));
    CHECK(adv[0].have_state == 1);
    CHECK(rx.have_state == 1);
    CHECK(tank::world_x_nominal(adv[0].s) == 250);
    CHECK(tank::world_y_nominal(adv[0].s) == 150);
    CHECK(adv[0].s.heading == 4);
    CHECK(adv[0].speed == 1);
    CHECK(rx.last_seq == 10);

    // Foreign magic / wrong direction (player type) are rejected, state unchanged.
    tanknet::TankPacket32 bad = make_adv1(11, 1, 1, 0, 0);
    bad.magic = 0x00;
    CHECK(!tanknet::adv_apply_packet(adv, 1, rx, bad));
    tanknet::TankPacket32 wrong_dir = make_adv1(11, 1, 1, 0, 0);
    wrong_dir.type = tanknet::kTypePlayer;
    CHECK(!tanknet::adv_apply_packet(adv, 1, rx, wrong_dir));
    CHECK(tank::world_x_nominal(adv[0].s) == 250);   // untouched
    CHECK(rx.last_seq == 10);

    // Stale / duplicate sequence dropped; strictly-newer accepted.
    CHECK(!tanknet::adv_apply_packet(adv, 1, rx, make_adv1(10, 999, 999, 0, 0)));  // dup
    CHECK(!tanknet::adv_apply_packet(adv, 1, rx, make_adv1(3,  999, 999, 0, 0)));  // older
    CHECK(tank::world_x_nominal(adv[0].s) == 250);
    CHECK(tanknet::adv_apply_packet(adv, 1, rx, make_adv1(11, 260, 150, 4, 1)));   // newer
    CHECK(tank::world_x_nominal(adv[0].s) == 260);
}

// ── apply: sequence wrap is handled by the signed delta ────────────────────
static void test_apply_seq_wrap() {
    tanknet::Adversary adv[1] = {};
    tanknet::AdvRxState rx{};
    CHECK(tanknet::adv_apply_packet(adv, 1, rx, make_adv1(65530, 100, 100, 0, 0)));
    // 2 is "newer" than 65530 across the wrap (delta = +8) -> accepted.
    CHECK(tanknet::adv_apply_packet(adv, 1, rx, make_adv1(2, 110, 100, 0, 0)));
    CHECK(rx.last_seq == 2);
    // 65530 is now "older" than 2 (delta = -8) -> rejected.
    CHECK(!tanknet::adv_apply_packet(adv, 1, rx, make_adv1(65530, 120, 100, 0, 0)));
}

// ── apply: out-of-range position is clamped to the world box ───────────────
static void test_apply_clamps_world() {
    tanknet::Adversary adv[1] = {};
    tanknet::AdvRxState rx{};
    CHECK(tanknet::adv_apply_packet(adv, 1, rx, make_adv1(1, 1000, 1000, 0, 0)));
    CHECK(tank::world_x_nominal(adv[0].s) == 632);   // kMaxCenterX
    CHECK(tank::world_y_nominal(adv[0].s) == 376);   // kMaxCenterY
}

// ── dead-reckon: advances exactly like the player's move_tank ──────────────
static void test_dead_reckon_matches_motion() {
    tanknet::Adversary adv[1] = {};
    tanknet::AdvRxState rx{};
    // East heading (4), speed 1. With kSpeedScale=3 the per-frame vector is +24 Q12.4
    // (1.5 px), so 6 frames advance x by 9 px and leave y unchanged.
    CHECK(tanknet::adv_apply_packet(adv, 1, rx, make_adv1(1, 100, 100, 4, 1)));
    for (u8 i = 0; i < 6; ++i) tanknet::adv_dead_reckon(adv[0]);
    CHECK(tank::world_x_nominal(adv[0].s) == static_cast<i16>(100 + 6 * tank::kSpeedScale * 8 / 16));
    CHECK(tank::world_y_nominal(adv[0].s) == 100);

    // speed 0 => stationary between snapshots.
    CHECK(tanknet::adv_apply_packet(adv, 1, rx, make_adv1(2, 100, 100, 4, 0)));
    for (u8 i = 0; i < 6; ++i) tanknet::adv_dead_reckon(adv[0]);
    CHECK(tank::world_x_nominal(adv[0].s) == 100);

    // No state yet => dead-reckon is a no-op (adversary stays hidden upstream).
    tanknet::Adversary fresh{};
    tanknet::adv_dead_reckon(fresh);
    CHECK(fresh.have_state == 0);
}

// ── combined packet: one packet snaps all live adversaries; per-PACKET seq gate ──
static void test_combined_packet() {
    tanknet::Adversary adv[3] = {};
    tanknet::AdvRxState rx{};

    // One packet with 3 records updates all three; status bits 0..2 set.
    const tanknet::AdvRecord recs[3] = {
        rec_of(100, 100, 0, 0), rec_of(200, 150, 4, 0), rec_of(300, 250, 8, 0),
    };
    CHECK(tanknet::adv_apply_packet(adv, 3, rx, make_adv_packet(1, recs, 3)));
    CHECK(tank::world_x_nominal(adv[0].s) == 100);
    CHECK(tank::world_x_nominal(adv[1].s) == 200);
    CHECK(tank::world_y_nominal(adv[2].s) == 250);
    CHECK(adv[0].have_state && adv[1].have_state && adv[2].have_state);

    // A stale combined packet is dropped WHOLESALE (no record applied).
    const tanknet::AdvRecord stale[3] = {
        rec_of(999, 999, 0, 0), rec_of(999, 999, 0, 0), rec_of(999, 999, 0, 0),
    };
    CHECK(!tanknet::adv_apply_packet(adv, 3, rx, make_adv_packet(1, stale, 3)));  // dup seq
    CHECK(tank::world_x_nominal(adv[1].s) == 200);                               // untouched

    // status bit clear => that record is skipped even in a newer packet.
    tanknet::TankPacket32 p = make_adv_packet(2, recs, 3);
    p.rec[1] = rec_of(640, 384, 0, 0);   // would clamp to (632,376) if applied
    p.status = static_cast<u8>(p.status & ~0x02);  // clear bit 1 (adv 1 not live)
    CHECK(tanknet::adv_apply_packet(adv, 3, rx, p));
    CHECK(tank::world_x_nominal(adv[1].s) == 200);   // adv 1 unchanged (bit clear)
    CHECK(rx.last_seq == 2);
}

int main() {
    test_packet_roundtrip();
    test_apply_snap_and_validation();
    test_apply_seq_wrap();
    test_apply_clamps_world();
    test_dead_reckon_matches_motion();
    test_combined_packet();

    if (g_failures == 0) printf("ALL TESTS PASSED\n");
    else                 printf("%u FAILURES\n", g_failures);
    return g_failures != 0;
}
