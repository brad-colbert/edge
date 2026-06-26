// test_sprites.cpp — unit tests for engine/sprites.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// The SpriteManager is exercised against a MOCK platform whose HAL records sprite
// position writes and reports a standard single-line strip layout, so commit() can
// be driven into a plain buffer (not real sprite RAM) and the zone/dirty bookkeeping
// asserted exactly without backend hardware. The live sprite + boundary-hook path is
// verified separately on Altirra/Fujisan.

#include <stdint.h>
#include <stdio.h>

#include <engine/interrupt.h>
#include <engine/sprites.h>

using engine::u8;
using engine::u16;

using engine::SpriteShape;
using engine::ZoneInfo;

// ── Mock platform ─────────────────────────────────────────────────────
//
// sprite_strip_offset returns the standard single-line layout (sprite N at
// 1024 + N*256); set_sprite_x / set_projectile_x record the last value written per index.
struct MockHal {
    static u8 hposp[4];
    static u8 hposm[4];
    static u8 colpm[4];

    static void set_sprite_x(u8 player,  u8 x) { hposp[player]  = x; }
    static void set_projectile_x(u8 missile, u8 x) { hposm[missile] = x; }
    static void set_sprite_color(u8 player,  u8 c) { colpm[player]  = c; }
    // Zone-0 colour now goes through the PCOLR shadow; the mock treats it as the
    // player's effective colour (no OS copy to model), so the color-follows test
    // keeps checking colpm[].
    static void set_color_pm(u8 player, u8 c) { colpm[player] = c; }

    static u16 sprite_strip_offset(u8 res, u8 player) {
        const bool single = (res == 0);
        const u16 base   = single ? 1024 : 512;
        const u16 stride = single ? 256  : 128;
        return static_cast<u16>(base + player * stride);
    }
    static u16 sprite_strip_size(u8 res) { return (res == 0) ? 256 : 128; }

    // Single shared missile strip below the player strips (768 single-line).
    static u16 missile_strip_offset(u8 res) { return (res == 0) ? 768 : 384; }

    // The raw multiplex DLI is hardware-only; build_raster_hooks just needs a
    // valid handler pointer for the slot (never entered under the simulator).
    static void mux_noop() {}
    static void (*multiplex_dli())() { return &mux_noop; }
};
u8 MockHal::hposp[4] = {};
u8 MockHal::hposm[4] = {};
u8 MockHal::colpm[4] = {};

struct MockPlatform {
    using hal = MockHal;
};

using SM = engine::SpriteManager<MockPlatform, 9, 4>;
// Direct-bind manager: 2 slots pinned 1:1 to hardware players (no multiplexer).
using SMD = engine::SpriteManager<MockPlatform, 2, 4, engine::SpriteBinding::Direct>;
using IM = engine::InterruptManager<MockPlatform>;

// A shape whose rows are all 0xFF, so committed bytes are easy to spot.
static const SpriteShape<8, 8> g_shape = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

// ── Harness ────────────────────────────────────────────────────────────

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// Player layout base used by the tests (matches MockHal single-line).
static u16 pbase(u8 player) { return static_cast<u16>(1024 + player * 256); }

// ── 5 sprites → 2 zones, sorted by Y, boundary in the gap ──────────────

static void test_five_sprites_two_zones() {
    static SM mgr;
    // Deliberately out of Y order: indices 0..4 at Y 50,10,90,30,70.
    mgr.sprite(0, g_shape, 100, 50);
    mgr.sprite(1, g_shape, 100, 10);
    mgr.sprite(2, g_shape, 100, 90);
    mgr.sprite(3, g_shape, 100, 30);
    mgr.sprite(4, g_shape, 100, 70);
    mgr.update_zones();

    CHECK(mgr.active_count() == 5);
    CHECK(mgr.zone_count() == 2);          // 5 sprites need 2 zones (4 + 1)

    // Zone 0's four players are the four lowest-Y sprites, in ascending Y.
    const ZoneInfo& z0 = mgr.zone(0);
    CHECK(mgr.logical(z0.player_assignment[0]).y == 10);
    CHECK(mgr.logical(z0.player_assignment[1]).y == 30);
    CHECK(mgr.logical(z0.player_assignment[2]).y == 50);
    CHECK(mgr.logical(z0.player_assignment[3]).y == 70);

    // Zone 1 holds the remaining (highest-Y) sprite on player 0.
    const ZoneInfo& z1 = mgr.zone(1);
    CHECK(mgr.logical(z1.player_assignment[0]).y == 90);
    CHECK(z1.player_assignment[1] == ZoneInfo::UNUSED);

    // The boundary falls between zone 0's last sprite (70) and zone 1's (90),
    // biased up by one mode line (kBoundaryBias = 8) so the DLI — which fires at
    // the end of its mode line — switches players in the gap, not mid-sprite.
    CHECK(z1.boundary_scanline >= 70 && z1.boundary_scanline < 90);
    CHECK(z1.boundary_scanline == 72);     // midpoint 80 - kBoundaryBias 8
    CHECK(z0.boundary_scanline == 0);      // zone 0 is active from the top
}

// ── 4 sprites → 1 zone, each on a unique player ────────────────────────

static void test_four_sprites_one_zone() {
    static SM mgr;
    mgr.sprite(0, g_shape, 100, 40);
    mgr.sprite(1, g_shape, 100, 10);
    mgr.sprite(2, g_shape, 100, 30);
    mgr.sprite(3, g_shape, 100, 20);
    mgr.update_zones();

    CHECK(mgr.zone_count() == 1);

    // Each of the four logical sprites maps to a distinct hardware player 0-3.
    u8 seen = 0;
    for (u8 i = 0; i < 4; ++i) {
        const u8 p = mgr.player_for_sprite(i);
        CHECK(p < 4);
        CHECK((seen & (1u << p)) == 0);    // not previously assigned
        seen |= static_cast<u8>(1u << p);
    }
    CHECK(seen == 0x0F);                   // all four players used exactly once
}

// ── 9 sprites → ≤4 zones; bitmask / mapping consistency ────────────────

static void test_nine_sprites_mapping() {
    static SM mgr;
    // Y = 10,20,...,90, so the sorted order is the natural index order.
    for (u8 i = 0; i < 9; ++i) mgr.sprite(i, g_shape, 100, (u8)((i + 1) * 10));
    mgr.update_zones();

    CHECK(mgr.active_count() == 9);
    CHECK(mgr.zone_count() == 3);          // ceil(9/4)
    CHECK(mgr.zone_count() <= 4);

    // Zones: [0..3] / [4..7] / [8]. Player p collects sorted[p], sorted[4+p],
    // sorted[8] (only player 0 in zone 2).
    CHECK(mgr.sprites_on_player(0) ==
          static_cast<u16>((1u << 0) | (1u << 4) | (1u << 8)));
    CHECK(mgr.sprites_on_player(1) ==
          static_cast<u16>((1u << 1) | (1u << 5)));
    CHECK(mgr.sprites_on_player(2) ==
          static_cast<u16>((1u << 2) | (1u << 6)));
    CHECK(mgr.sprites_on_player(3) ==
          static_cast<u16>((1u << 3) | (1u << 7)));

    // player_for_sprite / zone_for_sprite agree with the assignment.
    CHECK(mgr.player_for_sprite(0) == 0 && mgr.zone_for_sprite(0) == 0);
    CHECK(mgr.player_for_sprite(4) == 0 && mgr.zone_for_sprite(4) == 1);
    CHECK(mgr.player_for_sprite(8) == 0 && mgr.zone_for_sprite(8) == 2);
    CHECK(mgr.player_for_sprite(7) == 3 && mgr.zone_for_sprite(7) == 1);
}

// ── Tracked-range commit: only dirty Y ranges written / cleared ────────

static void test_commit_dirty_tracking() {
    static SM mgr;
    static u8 buf[2048] = {};

    // Two sprites on different players (single zone): idx0 Y=50, idx1 Y=60.
    mgr.sprite(0, g_shape, 100, 50);
    mgr.sprite(1, g_shape, 110, 60);
    mgr.update_zones();
    mgr.commit(buf);

    // Sorted by Y: player 0 = idx0 (Y50), player 1 = idx1 (Y60).
    for (u8 r = 0; r < 8; ++r) {
        CHECK(buf[pbase(0) + 50 + r] == 0xFF);
        CHECK(buf[pbase(1) + 60 + r] == 0xFF);
    }
    // Bytes just outside each sprite's range are untouched.
    CHECK(buf[pbase(0) + 49] == 0);
    CHECK(buf[pbase(0) + 58] == 0);

    // Zone-0 horizontal positions reached the HAL.
    CHECK(MockHal::hposp[0] == 100);
    CHECK(MockHal::hposp[1] == 110);

    // Move both sprites; old ranges must be cleared, new ranges written.
    mgr.sprite(0, g_shape, 100, 100);
    mgr.sprite(1, g_shape, 110, 120);
    mgr.update_zones();
    mgr.commit(buf);

    for (u8 r = 0; r < 8; ++r) {
        CHECK(buf[pbase(0) + 50 + r] == 0);    // old range cleared
        CHECK(buf[pbase(1) + 60 + r] == 0);
        CHECK(buf[pbase(0) + 100 + r] == 0xFF);
        CHECK(buf[pbase(1) + 120 + r] == 0xFF);
    }
}

// ── sprite_hide removes a sprite from zone assignment ──────────────────

static void test_sprite_hide() {
    static SM mgr;
    for (u8 i = 0; i < 5; ++i) mgr.sprite(i, g_shape, 100, (u8)((i + 1) * 10));
    mgr.sprite_hide(2);
    mgr.update_zones();

    CHECK(mgr.active_count() == 4);
    CHECK(mgr.zone_count() == 1);

    // The hidden sprite is in no zone / on no player.
    CHECK(mgr.player_for_sprite(2) == 0xFF);
    CHECK(mgr.zone_for_sprite(2) == 0xFF);
    const ZoneInfo& z0 = mgr.zone(0);
    for (u8 p = 0; p < 4; ++p) CHECK(z0.player_assignment[p] != 2);
}

// ── Per-sprite colour follows the sprite across a Y crossing ───────────

static void test_sprite_color_follows_sprite() {
    static SM mgr;
    static u8 buf[2048] = {};

    mgr.sprite_color(0, 0xAA);            // sprite 0's colour
    mgr.sprite_color(1, 0xBB);            // sprite 1's colour

    // Sprite 0 above sprite 1: sorted player 0 = sprite0, player 1 = sprite1.
    mgr.sprite(0, g_shape, 100, 50);
    mgr.sprite(1, g_shape, 110, 60);
    mgr.update_zones();
    mgr.commit(buf);
    CHECK(MockHal::colpm[0] == 0xAA);     // slot 0 shows sprite 0's colour
    CHECK(MockHal::colpm[1] == 0xBB);

    // Move sprite 0 below sprite 1: the slots swap (player 0 = sprite1,
    // player 1 = sprite0) but each sprite keeps its colour — the colour follows it.
    mgr.sprite(0, g_shape, 100, 70);
    mgr.update_zones();
    mgr.commit(buf);
    CHECK(mgr.player_for_sprite(0) == 1);  // sprite 0 now on slot 1
    CHECK(mgr.player_for_sprite(1) == 0);
    CHECK(MockHal::colpm[0] == 0xBB);      // slot 0 now shows sprite 1's colour
    CHECK(MockHal::colpm[1] == 0xAA);      // slot 1 shows sprite 0's colour (no swap)

    // sprite() preserves the colour set earlier (sticky across position updates).
    CHECK(mgr.logical(0).color == 0xAA);
    CHECK(mgr.logical(1).color == 0xBB);
}

// ── Direct binding: fixed slot->player, no swap on a Y crossing ────────
//
// The regression guard for the multiplexer's Y-cross player/colour swap: with
// Direct binding, slot i always drives hardware player i regardless of relative Y,
// so two sprites that cross never trade players or colours.

static void test_direct_bind_no_swap() {
    static SMD mgr;
    static u8 buf[2048] = {};

    mgr.sprite_color(0, 0xAA);
    mgr.sprite_color(1, 0xBB);

    // Slot 0 above slot 1.
    mgr.sprite(0, g_shape, 100, 50);
    mgr.sprite(1, g_shape, 110, 60);
    mgr.update_zones();
    CHECK(mgr.zone_count() == 1);          // always a single zone, no boundary hooks
    CHECK(mgr.active_count() == 2);
    mgr.commit(buf);

    CHECK(mgr.player_for_sprite(0) == 0);  // fixed 1:1 binding
    CHECK(mgr.player_for_sprite(1) == 1);
    CHECK(MockHal::hposp[0] == 100);
    CHECK(MockHal::hposp[1] == 110);
    CHECK(MockHal::colpm[0] == 0xAA);
    CHECK(MockHal::colpm[1] == 0xBB);
    for (u8 r = 0; r < 8; ++r) {
        CHECK(buf[pbase(0) + 50 + r] == 0xFF);
        CHECK(buf[pbase(1) + 60 + r] == 0xFF);
    }

    // Cross in Y: slot 0 drops below slot 1. The multiplexer would swap players and
    // colours here (see test_sprite_color_follows_sprite); Direct binding does not.
    mgr.sprite(0, g_shape, 100, 70);
    mgr.update_zones();
    mgr.commit(buf);

    CHECK(mgr.player_for_sprite(0) == 0);  // NOT swapped
    CHECK(mgr.player_for_sprite(1) == 1);
    CHECK(MockHal::hposp[0] == 100);
    CHECK(MockHal::hposp[1] == 110);
    CHECK(MockHal::colpm[0] == 0xAA);      // colour stays on its player (no flip)
    CHECK(MockHal::colpm[1] == 0xBB);
    for (u8 r = 0; r < 8; ++r) {
        CHECK(buf[pbase(0) + 50 + r] == 0);     // slot 0's old range cleared
        CHECK(buf[pbase(0) + 70 + r] == 0xFF);  // slot 0's new range written
        CHECK(buf[pbase(1) + 60 + r] == 0xFF);  // slot 1 unchanged
    }
}

// ── Direct binding: hiding a slot frees its player, leaves the other ───

static void test_direct_bind_hide() {
    static SMD mgr;
    mgr.sprite(0, g_shape, 100, 40);
    mgr.sprite(1, g_shape, 110, 50);
    mgr.sprite_hide(0);
    mgr.update_zones();

    CHECK(mgr.zone_count() == 1);
    CHECK(mgr.active_count() == 1);
    CHECK(mgr.player_for_sprite(0) == 0xFF);   // hidden slot is on no player
    CHECK(mgr.player_for_sprite(1) == 1);      // slot 1 keeps its own player
    const ZoneInfo& z0 = mgr.zone(0);
    CHECK(z0.player_assignment[0] == ZoneInfo::UNUSED);
    CHECK(z0.player_assignment[1] == 1);
}

// ── build_raster_hooks registers one boundary hook per extra zone ───────

static void test_raster_hook_registration() {
    static SM mgr;
    static IM im;
    for (u8 i = 0; i < 5; ++i) mgr.sprite(i, g_shape, 100, (u8)((i + 1) * 10));
    mgr.update_zones();                    // 2 zones
    mgr.build_raster_hooks(im);
    // Zone 0 is armed by the frame-service commit; only zone 1's boundary needs a hook.
    CHECK(im.raster_hook_count() == 1);
    CHECK(im.slot(0).scanline == mgr.zone(1).boundary_scanline);
}

// ── Missiles: shared strip render, 2-bit field per missile, hide clears ────

static void test_missile_render_and_hide() {
    static SM mgr;
    static u8 buf[2048] = {};
    constexpr u16 mb = 768;                // single-line missile strip base

    // Missile 1 at Y=40, height 4: sets bits 2..3 (0x0C) across [40,44).
    mgr.missile(1, 100, 40, 4);
    mgr.update_zones();
    mgr.commit(buf);
    for (u8 r = 0; r < 4; ++r) CHECK(buf[mb + 40 + r] == 0x0C);
    CHECK(buf[mb + 39] == 0);              // just outside the range
    CHECK(buf[mb + 44] == 0);
    CHECK(MockHal::hposm[1] == 100);       // horizontal position reached the HAL

    // Missile 2 shares scanline 41 (bits 4..5 = 0x30): both fields coexist in the
    // same byte, neither stomps the other.
    mgr.missile(2, 120, 41, 2);
    mgr.commit(buf);
    CHECK(buf[mb + 41] == (0x0C | 0x30));  // missile 1 + missile 2 in one byte
    CHECK(buf[mb + 40] == 0x0C);           // missile 1 only

    // Hide missile 1: its 2 bits clear everywhere, missile 2's remain.
    mgr.missile_hide(1);
    mgr.commit(buf);
    for (u8 r = 0; r < 4; ++r) CHECK((buf[mb + 40 + r] & 0x0C) == 0);
    CHECK(buf[mb + 41] == 0x30);           // missile 2 untouched
    CHECK(buf[mb + 42] == 0x30);
}

int main() {
    test_five_sprites_two_zones();
    test_four_sprites_one_zone();
    test_nine_sprites_mapping();
    test_commit_dirty_tracking();
    test_sprite_hide();
    test_sprite_color_follows_sprite();
    test_direct_bind_no_swap();
    test_direct_bind_hide();
    test_raster_hook_registration();
    test_missile_render_and_hide();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
