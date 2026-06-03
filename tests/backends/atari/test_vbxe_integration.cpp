// test_vbxe_integration.cpp — Phase 4a compile-validation: the portable engine
// (engine::Core) must build for BOTH a baseline (gfx::Baseline) and a VBXE
// (gfx::VBXE<>) platform, against the real Atari HAL bundle.
//
// There is no runtime here: Core::run() loops forever and the VBXE overlay seams
// drive real $D6xx MMIO. Instead we force instantiation of both Cores'
// init()/frame_service()/flip() by taking their addresses — this compiles the
// VBXE `overlay_*` path and the baseline P/M path without executing either. A
// clean build is the test; main() returns 0 so the mos-sim harness sees a pass.

#include <stdint.h>
#include <stdio.h>

#include <engine/core.h>
#include <engine/platform/atari/platform.h>

using engine::u8;
using engine::Core;
using engine::DisplayLayout;
using engine::TextRegion;
using engine::ScreenSet;

namespace M = atari;

// Shared minimal game configuration (one text screen, a few sprite slots).
struct ScreenText {
    using display = DisplayLayout<TextRegion<M::Mode::MODE_2, 24>>;
};
struct GameConfig {
    using screens = ScreenSet<ScreenText>;
    static constexpr u8 max_sprites    = 4;
    static constexpr u8 sound_channels = 2;
};

// Real platforms (real HAL): baseline ANTIC/GTIA, and VBXE with default config.
using BaselinePlatform = M::Platform<
    M::Machine::XL, M::RAM::Baseline, M::gfx::Baseline, M::Sound::Mono, M::TV::NTSC>;
using VbxePlatform = M::Platform<
    M::Machine::XL, M::RAM::U1MB, M::gfx::VBXE<>, M::Sound::Mono, M::TV::NTSC>;

// VBXE in 80-column text mode (exercises the overlay-text seam path).
using VbxeTextPlatform = M::Platform<
    M::Machine::XL, M::RAM::U1MB,
    M::gfx::VBXE<atari::vbxe::Config<atari::vbxe::Mode::Text_80>>,
    M::Sound::Mono, M::TV::NTSC>;

// VBXE in sprites-over-bitmap mode (double-buffered, Background::Bitmap):
// exercises the master-canvas compositor + publish seam path.
using VbxeBmpPlatform = M::Platform<
    M::Machine::XL, M::RAM::U1MB,
    M::gfx::VBXE<atari::vbxe::Config<
        atari::vbxe::Mode::SR_320, atari::vbxe::Buffers::Double,
        atari::vbxe::RegBase::D640, atari::vbxe::MEMAC_A, 0x00000,
        atari::vbxe::Background::Bitmap>>,
    M::Sound::Mono, M::TV::NTSC>;

using BaselineGame = Core<BaselinePlatform, GameConfig>;
using VbxeGame     = Core<VbxePlatform, GameConfig>;
using VbxeTextGame = Core<VbxeTextPlatform, GameConfig>;
using VbxeBmpGame  = Core<VbxeBmpPlatform, GameConfig>;

// The capability axis must drive the two paths apart at compile time.
static_assert(VbxePlatform::capabilities::has_blitter,          "VBXE has the blitter path");
static_assert(VbxePlatform::capabilities::has_vram,             "VBXE has VRAM (flip path)");
static_assert(VbxePlatform::capabilities::has_overlay_collision,"VBXE has overlay collision");
static_assert(!BaselinePlatform::capabilities::has_blitter,     "baseline uses the P/M path");

// ── Force instantiation of both Cores' entry points (compiles, never runs) ──
// Volatile sinks (volatile on the pointer object) so the optimiser can't drop
// the odr-use that triggers instantiation.
using fn_t   = void (*)();
using fn_u8t = void (*)(engine::u8);
static fn_t   volatile g_sink_vbxe_init  = static_cast<fn_t>(&VbxeGame::init);
static fn_t   volatile g_sink_vbxe_frame = &VbxeGame::frame_service;
static fn_u8t volatile g_sink_vbxe_bg    = static_cast<fn_u8t>(&VbxeGame::set_overlay_background);
static fn_t   volatile g_sink_base_init  = static_cast<fn_t>(&BaselineGame::init);
static fn_t   volatile g_sink_base_frame = &BaselineGame::frame_service;
// sprites-over-bitmap: instantiate the Bitmap-mode Core + the publish seam.
static fn_t   volatile g_sink_vbxe_bmp_init    = static_cast<fn_t>(&VbxeBmpGame::init);
static fn_t   volatile g_sink_vbxe_bmp_frame   = &VbxeBmpGame::frame_service;
static fn_t   volatile g_sink_vbxe_bmp_publish = &VbxeBmpGame::overlay_publish_background;

// Overlay text-mode forwarders (compile the Text_80 seam path; never run — MMIO).
static void touch_vbxe_text() {
    static const engine::u8 font[8] = { 0 };
    VbxeTextGame::overlay_text_font(font, 8);
    VbxeTextGame::overlay_text_clear(0x20, 0x81);
    VbxeTextGame::overlay_put_char(0, 0, static_cast<engine::u8>('A'), 0x81);
    VbxeTextGame::overlay_print(0, 1, "HI", 0x81);
}
static fn_t volatile g_sink_vbxe_text = &touch_vbxe_text;

static unsigned g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

int main() {
    // Touch the sinks so the entry points are emitted and linked.
    CHECK(g_sink_vbxe_init  != nullptr);
    CHECK(g_sink_vbxe_frame != nullptr);
    CHECK(g_sink_vbxe_bg    != nullptr);
    CHECK(g_sink_vbxe_text  != nullptr);
    CHECK(g_sink_base_init  != nullptr);
    CHECK(g_sink_base_frame != nullptr);
    CHECK(g_sink_vbxe_bmp_init    != nullptr);
    CHECK(g_sink_vbxe_bmp_frame   != nullptr);
    CHECK(g_sink_vbxe_bmp_publish != nullptr);

    // Overlay accessors default to zero before any frame service runs.
    CHECK(VbxeGame::overlay_collision() == 0);
    CHECK(VbxeGame::overlay_blit_collision() == 0);

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
