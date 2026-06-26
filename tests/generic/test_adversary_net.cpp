// test_adversary_net.cpp — networked-adversary packet + dead-reckoning invariants
// (demo/tank_net/adversary_net.h). Built for mos-sim, run under CTest (0 = pass).
// Pure logic; no Atari hardware, no transport.

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

// Build an authoritative adversary snapshot (type 0) for the tests.
static tanknet::TankPacket16 make_adv(u16 seq, u16 x, u16 y, u8 h, u8 sp) {
    tanknet::TankPacket16 p{};
    p.magic   = tanknet::kMagic;
    p.version = tanknet::kVersion;
    p.type    = tanknet::kTypeAdversary;
    tanknet::wr16(p.seq_lo, p.seq_hi, seq);
    tanknet::wr16(p.x_lo, p.x_hi, x);
    tanknet::wr16(p.y_lo, p.y_hi, y);
    p.heading = h;
    p.speed   = sp;
    p.pattern = tanknet::kPattern;
    return p;
}

// ── Packet layout + player-packet build/round-trip ─────────────────────────
static void test_packet_roundtrip() {
    CHECK(sizeof(tanknet::TankPacket16) == 16);

    tank::TankState s{};
    s.world_x_q4 = static_cast<i16>(300 << 4);
    s.world_y_q4 = static_cast<i16>(200 << 4);
    s.heading    = 6;
    const tanknet::TankPacket16 p = tanknet::make_player_packet(s, 1, 42);

    CHECK(p.magic == tanknet::kMagic);
    CHECK(p.version == tanknet::kVersion);
    CHECK(p.type == tanknet::kTypePlayer);
    CHECK(p.pattern == tanknet::kPattern);
    CHECK(tanknet::rd16(p.seq_lo, p.seq_hi) == 42);
    CHECK(tanknet::rd16(p.x_lo, p.x_hi) == 300);
    CHECK(tanknet::rd16(p.y_lo, p.y_hi) == 200);
    CHECK(p.heading == 6);
    CHECK(p.speed == 1);
}

// ── apply: validate, snap, reject foreign/stale ────────────────────────────
static void test_apply_snap_and_validation() {
    tanknet::Adversary adv{};

    // First valid snapshot: snaps position, adopts heading/speed, marks have_state.
    CHECK(tanknet::adv_apply_packet(adv, make_adv(10, 250, 150, 4, 1)));
    CHECK(adv.have_state == 1);
    CHECK(tank::world_x_nominal(adv.s) == 250);
    CHECK(tank::world_y_nominal(adv.s) == 150);
    CHECK(adv.s.heading == 4);
    CHECK(adv.speed == 1);
    CHECK(adv.last_seq == 10);

    // Foreign magic / wrong direction (player type) are rejected, state unchanged.
    tanknet::TankPacket16 bad = make_adv(11, 1, 1, 0, 0);
    bad.magic = 0x00;
    CHECK(!tanknet::adv_apply_packet(adv, bad));
    tanknet::TankPacket16 wrong_dir = make_adv(11, 1, 1, 0, 0);
    wrong_dir.type = tanknet::kTypePlayer;
    CHECK(!tanknet::adv_apply_packet(adv, wrong_dir));
    CHECK(tank::world_x_nominal(adv.s) == 250);   // untouched
    CHECK(adv.last_seq == 10);

    // Stale / duplicate sequence dropped; strictly-newer accepted.
    CHECK(!tanknet::adv_apply_packet(adv, make_adv(10, 999, 999, 0, 0)));  // dup
    CHECK(!tanknet::adv_apply_packet(adv, make_adv(3,  999, 999, 0, 0)));  // older
    CHECK(tank::world_x_nominal(adv.s) == 250);
    CHECK(tanknet::adv_apply_packet(adv, make_adv(11, 260, 150, 4, 1)));   // newer
    CHECK(tank::world_x_nominal(adv.s) == 260);
}

// ── apply: sequence wrap is handled by the signed delta ────────────────────
static void test_apply_seq_wrap() {
    tanknet::Adversary adv{};
    CHECK(tanknet::adv_apply_packet(adv, make_adv(65530, 100, 100, 0, 0)));
    // 2 is "newer" than 65530 across the wrap (delta = +8) -> accepted.
    CHECK(tanknet::adv_apply_packet(adv, make_adv(2, 110, 100, 0, 0)));
    CHECK(adv.last_seq == 2);
    // 65530 is now "older" than 2 (delta = -8) -> rejected.
    CHECK(!tanknet::adv_apply_packet(adv, make_adv(65530, 120, 100, 0, 0)));
}

// ── apply: out-of-range position is clamped to the world box ───────────────
static void test_apply_clamps_world() {
    tanknet::Adversary adv{};
    CHECK(tanknet::adv_apply_packet(adv, make_adv(1, 1000, 1000, 0, 0)));
    CHECK(tank::world_x_nominal(adv.s) == 632);   // kMaxCenterX
    CHECK(tank::world_y_nominal(adv.s) == 376);   // kMaxCenterY
}

// ── dead-reckon: advances exactly like the player's move_tank ──────────────
static void test_dead_reckon_matches_motion() {
    tanknet::Adversary adv{};
    // East heading (4), speed 1. With kSpeedScale=3 the per-frame vector is +24 Q12.4
    // (1.5 px), so 6 frames advance x by 9 px and leave y unchanged.
    CHECK(tanknet::adv_apply_packet(adv, make_adv(1, 100, 100, 4, 1)));
    for (u8 i = 0; i < 6; ++i) tanknet::adv_dead_reckon(adv);
    CHECK(tank::world_x_nominal(adv.s) == static_cast<i16>(100 + 6 * tank::kSpeedScale * 8 / 16));
    CHECK(tank::world_y_nominal(adv.s) == 100);

    // speed 0 => stationary between snapshots.
    CHECK(tanknet::adv_apply_packet(adv, make_adv(2, 100, 100, 4, 0)));
    for (u8 i = 0; i < 6; ++i) tanknet::adv_dead_reckon(adv);
    CHECK(tank::world_x_nominal(adv.s) == 100);

    // No state yet => dead-reckon is a no-op (adversary stays hidden upstream).
    tanknet::Adversary fresh{};
    tanknet::adv_dead_reckon(fresh);
    CHECK(fresh.have_state == 0);
}

int main() {
    test_packet_roundtrip();
    test_apply_snap_and_validation();
    test_apply_seq_wrap();
    test_apply_clamps_world();
    test_dead_reckon_matches_motion();

    if (g_failures == 0) printf("ALL TESTS PASSED\n");
    else                 printf("%u FAILURES\n", g_failures);
    return g_failures != 0;
}
