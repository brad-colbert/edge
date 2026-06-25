// test_atari_core.cpp — Atari-backend tests for engine::Core's DMA composition.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// The portable Core wiring is tested in tests/generic/test_core.cpp. This file
// asserts the Atari-specific shared-register composition that init() drives: the
// SDMCTL shadow must carry BOTH subsystems' bits — the screen manager's DL-enable
// + normal playfield AND the sprite system's P/M DMA + single-line resolution —
// and GRACTL must latch the P/M graphics, with neither write clobbering the other.

#include <stdint.h>
#include <stdio.h>

#include <engine/core.h>
#include <engine/platform/atari/platform.h>
#include <engine/platform/atari/registers.h>   // atari::dmactl / gractl bit constants

using engine::u8;
using engine::u16;

using engine::Core;
using engine::DisplayLayout;
using engine::TextRegion;
using engine::ScreenSet;
using engine::audio::Waveform;

namespace M = atari;

// ── Mock platform: the real Atari SDMCTL/GRACTL RMW, everything else stubbed ──

struct MockHal {
    static u8 sdmctl;     // SDMCTL shadow, RMW'd as the real HAL does
    static u8 gractl;

    static constexpr uint16_t sprite_area_bytes = 2048;

    // Display DMA — DL/playfield bits, preserving the P/M bits.
    static void display_dma_disable() { sdmctl &= M::dmactl::PM_DMA_MASK; }
    static void display_dma_enable(u8 mode_bits = M::dmactl::PLAYFIELD_NORMAL) {
        sdmctl = static_cast<u8>((sdmctl & M::dmactl::PM_DMA_MASK) |
                                 M::dmactl::DL_ENABLE | mode_bits);
    }
    static void set_display_program(const u8*) {}
    static void set_charset_base(u8) {}

    // Sprite DMA — GRACTL latch (chip) + the P/M bits OR'd into SDMCTL.
    static void set_sprite_base(u8) {}
    static void sprite_dma_enable(bool single_line) {
        gractl = M::gractl::PLAYER_LATCH | M::gractl::MISSILE_LATCH;
        u8 bits = M::dmactl::PLAYER_DMA | M::dmactl::MISSILE_DMA;
        if (single_line) bits |= M::dmactl::PM_SINGLE_LINE;
        sdmctl = static_cast<u8>(sdmctl | bits);
    }
    static void sprite_dma_disable() {
        gractl = 0x00;
        sdmctl &= static_cast<u8>(~M::dmactl::PM_DMA_MASK);
    }

    // The rest of the HAL surface init()/frame_service() touch — stubbed.
    static void set_voice_freq(u8, u8) {}
    static void set_voice_control(u8, Waveform, u8) {}
    static void silence_voice(u8) {}
    static void set_sprite_x(u8, u8) {}
    static void set_projectile_x(u8, u8) {}
    static void set_sprite_color(u8, u8) {}
    static void set_color_pm(u8, u8) {}
    static uint16_t sprite_strip_offset(u8, u8 s) { return static_cast<uint16_t>(s * 256); }
    static uint16_t sprite_strip_size(u8) { return 256; }
    static uint16_t missile_strip_offset(u8) { return 768; }
    static uint16_t raster_dispatch_addr() { return 0xD15A; }
    static uint16_t raster_terminal_addr() { return 0xD160; }
    static void program_raster_lines(u8*, u16, const u8*, u8) {}
    static void set_raster_vector(u16) {}
    static void enable_raster()  {}
    static void disable_raster() {}
    static void install_raster_dispatch(u16, u16, u16, u16, u16, u16) {}
    static void mux_noop() {}
    static void (*multiplex_dli())() { return &mux_noop; }
    static void install_multiplex_dli(u16, u16) {}
    static u8   read_joystick(u8) { return 0; }
    static u8   read_keyboard()   { return 0; }
    static u8   coll_player_playfield(u8) { return 0; }
    static u8   coll_player_player(u8)    { return 0; }
    static u8   coll_missile_playfield(u8){ return 0; }
    static u8   coll_missile_player(u8)   { return 0; }
    static void clear_collisions() {}
    static void suppress_idle_dim() {}
    static void set_fine_scroll_x(u8) {}
    static void set_fine_scroll_y(u8) {}
    static void install_frame_isr(void (*)()) {}

    static void reset() { sdmctl = 0; gractl = 0; }
};
u8 MockHal::sdmctl = 0;
u8 MockHal::gractl = 0;

struct MockPlatform {
    using hal = MockHal;
    template <typename Layout>
    using display_program = atari::DisplayProgram<Layout>;
};

struct ScreenText {
    using display = DisplayLayout<TextRegion<M::Mode::MODE_2, 24>>;
};
struct GameConfig {
    using screens = ScreenSet<ScreenText>;
    static constexpr u8 max_sprites    = 4;
    static constexpr u8 sound_channels = 2;
};
using Game = Core<MockPlatform, GameConfig>;

// ── Harness ────────────────────────────────────────────────────────────

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// After init() the SDMCTL shadow must carry BOTH subsystems' bits: the screen
// manager's DL-enable + normal playfield AND the sprite system's P/M DMA +
// single-line resolution (the default). This proves the RMW accumulation in the
// HAL — neither write clobbers the other's bits — and GRACTL latches the P/M DMA.
static void test_sdmctl_composition() {
    MockHal::reset();
    Game::init();

    const u8 expected = M::dmactl::DL_ENABLE | M::dmactl::PLAYFIELD_NORMAL |
                        M::dmactl::PLAYER_DMA | M::dmactl::MISSILE_DMA |
                        M::dmactl::PM_SINGLE_LINE;   // 0x3E
    CHECK(MockHal::sdmctl == expected);
    CHECK(MockHal::gractl == 0x03);                  // player + missile latch
}

int main() {
    test_sdmctl_composition();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
