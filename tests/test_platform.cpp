// test_platform.cpp — unit tests for the Atari platform capability profiles.
//
// Capabilities are compile-time traits, so the primary checks are
// `static_assert` (a failure stops the build). The same checks are mirrored as
// runtime CHECKs so the test harness sees a running, passing executable under
// mos-sim (main()'s return value is the CTest pass/fail signal).

#include <stdio.h>

#include <engine/platform/atari/platform.h>

namespace a = atari;

using StockXLCaps      = a::StockXL_NTSC::capabilities;
using StockXLPALCaps   = a::StockXL_PAL::capabilities;
using ExpandedXECaps   = a::ExpandedXE::capabilities;
using FullUpgradeCaps  = a::FullUpgrade::capabilities;

// A custom platform with stereo sound (XL, baseline RAM/graphics, Stereo, NTSC).
using StereoCaps = a::Platform<
    a::Machine::XL, a::RAM::Baseline, a::Graphics::Baseline,
    a::Sound::Stereo, a::TV::NTSC>::capabilities;

// ── Compile-time checks ──────────────────────────────────────────────

// 1. StockXL: baseline hardware sprites, 4 of them, 4 POKEY voices, no network.
static_assert(StockXLCaps::has_hardware_sprites == true,  "StockXL has P/M sprites");
static_assert(StockXLCaps::max_hardware_sprites == 4,     "StockXL has 4 players");
static_assert(StockXLCaps::sound_voices == 4,             "StockXL has 4 voices");
static_assert(StockXLCaps::has_network == false,          "StockXL has no network");

// 2. ExpandedXE: bank-switched RAM present.
static_assert(ExpandedXECaps::has_extended_ram == true,   "ExpandedXE has extended RAM");
static_assert(ExpandedXECaps::extended_ram_bytes > 0,     "ExpandedXE extended RAM > 0");

// 3. FullUpgrade: VBXE blitter, PokeyMax extended sound, and network.
static_assert(FullUpgradeCaps::has_blitter == true,       "FullUpgrade has VBXE blitter");
static_assert(FullUpgradeCaps::has_extended_sound == true, "FullUpgrade has PokeyMax");
static_assert(FullUpgradeCaps::has_network == true,       "FullUpgrade has network");

// 4. Custom Stereo platform reports 8 voices.
static_assert(StereoCaps::sound_voices == 8,              "Stereo has 8 voices");

// 5. TV axis drives timing capabilities (ADR-018).
static_assert(StockXLCaps::frames_per_second == 60,      "NTSC is 60 Hz");
static_assert(StockXLCaps::cycles_per_frame == 29780,    "NTSC is 29780 cycles/frame");
static_assert(StockXLPALCaps::frames_per_second == 50,   "PAL is 50 Hz");
static_assert(StockXLPALCaps::cycles_per_frame == 35280, "PAL is 35280 cycles/frame");

// ── Runtime mirror (for the harness) ─────────────────────────────────

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static void test_stock_xl() {
    CHECK(StockXLCaps::has_hardware_sprites == true);
    CHECK(StockXLCaps::max_hardware_sprites == 4);
    CHECK(StockXLCaps::sound_voices == 4);
    CHECK(StockXLCaps::has_network == false);
}

static void test_expanded_xe() {
    CHECK(ExpandedXECaps::has_extended_ram == true);
    CHECK(ExpandedXECaps::extended_ram_bytes > 0);
}

static void test_full_upgrade() {
    CHECK(FullUpgradeCaps::has_blitter == true);
    CHECK(FullUpgradeCaps::has_extended_sound == true);
    CHECK(FullUpgradeCaps::has_network == true);
}

static void test_custom_stereo() {
    CHECK(StereoCaps::sound_voices == 8);
    CHECK(StereoCaps::has_stereo == true);
}

static void test_tv_timing() {
    CHECK(StockXLCaps::frames_per_second == 60);
    CHECK(StockXLCaps::cycles_per_frame == 29780);
    CHECK(StockXLPALCaps::frames_per_second == 50);
    CHECK(StockXLPALCaps::cycles_per_frame == 35280);
}

int main() {
    test_stock_xl();
    test_expanded_xe();
    test_full_upgrade();
    test_custom_stereo();
    test_tv_timing();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
