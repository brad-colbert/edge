// test_core.cpp — generic unit tests for engine/core.h (the integration layer).
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// Verifies that engine::Core wires the subsystems together with backend-neutral
// behaviour: the compile-time queries, the sub-object accessors, init(), the
// sprite/text delegators, and the per-frame frame_service() sequence (input
// capture + collision latch). The MOCK HAL records calls in neutral terms — the
// exact Atari register/DMA bit composition is asserted in
// tests/backends/atari/test_atari_core.cpp. The loop (run/run_until) needs a real
// frame interrupt, so it is only checked for compilation, never invoked.

#include <stdint.h>
#include <stdio.h>

#include <engine/core.h>
#include <engine/platform/atari/platform.h>   // backend mode trait + display-program builder

using engine::u8;
using engine::u16;

using engine::Core;
using engine::DisplayLayout;
using engine::TextRegion;
using engine::ScreenSet;
using engine::make_sprite;
using engine::audio::Waveform;

namespace M = atari;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records the HAL calls Core makes (in backend-neutral terms) and lets the test
// inject input/collision reads. All-static, mirroring the other engine test mocks.
struct MockHal {
    // Display.
    static const u8* last_program;
    static bool      dma_enabled;
    static u8        charset_base;
    static unsigned  charset_writes;
    // Sprites.
    static u8        sprite_base_page;
    static bool      sprite_base_set;
    static bool      sprite_dma_on;
    // System.
    static unsigned  idle_dim_suppressions;
    // Frame service install.
    static void    (*frame_handler)();
    // Injected input.
    static u8        joy_state[2];
    static u8        key_state;
    // Injected collisions.
    static u8        in_s_bg[4], in_s_s[4], in_p_bg[4], in_p_s[4];
    static unsigned  clear_calls;

    static constexpr uint16_t sprite_area_bytes = 2048;

    // Display.
    static void display_dma_disable() { dma_enabled = false; }
    static void display_dma_enable()  { dma_enabled = true; }
    static void set_display_program(const u8* p) { last_program = p; }
    static void set_charset_base(u8 page) { charset_base = page; ++charset_writes; }

    // Sound (commit/tick may touch these).
    static void set_voice_freq(u8, u8) {}
    static void set_voice_control(u8, Waveform, u8) {}
    static void silence_voice(u8) {}

    // Sprite positions + layout.
    static void     set_sprite_x(u8, u8) {}
    static void     set_projectile_x(u8, u8) {}
    static void     set_sprite_color(u8, u8) {}
    static uint16_t sprite_strip_offset(u8, u8 sprite) {
        return static_cast<uint16_t>(sprite * 256);
    }
    static uint16_t sprite_strip_size(u8) { return 256; }
    static uint16_t missile_strip_offset(u8) { return 768; }

    // Raster dispatcher addresses + delivery (prepare_chain drives these).
    static uint16_t raster_dispatch_addr() { return 0xD15A; }
    static uint16_t raster_terminal_addr() { return 0xD160; }
    static void program_raster_lines(u8*, u16, const u8*, u8) {}
    static void set_raster_vector(u16) {}
    static void enable_raster()  {}
    static void disable_raster() {}
    static void install_raster_dispatch(u16, u16, u16, u16, u16) {}
    static void mux_noop() {}
    static void (*multiplex_dli())() { return &mux_noop; }
    static void install_multiplex_dli(u16, u16) {}

    // Input capture (injected).
    static u8 read_joystick(u8 port) { return joy_state[port]; }
    static u8 read_keyboard()        { return key_state; }

    // Collision reads (injected) + clear.
    static u8 coll_player_playfield(u8 p)  { return in_s_bg[p]; }
    static u8 coll_player_player(u8 p)     { return in_s_s[p]; }
    static u8 coll_missile_playfield(u8 m) { return in_p_bg[m]; }
    static u8 coll_missile_player(u8 m)    { return in_p_s[m]; }
    static void clear_collisions()         { ++clear_calls; }

    // Sprite DMA setup.
    static void set_sprite_base(u8 page) { sprite_base_page = page; sprite_base_set = true; }
    static void sprite_dma_enable(bool)  { sprite_dma_on = true; }
    static void sprite_dma_disable()     { sprite_dma_on = false; }

    // OS shadow colours + idle-dim suppression.
    static void set_color_pm(u8, u8) {}
    static void set_color_pf(u8, u8) {}
    static void suppress_idle_dim() { ++idle_dim_suppressions; }

    // Fine-scroll registers (frame_service instantiates apply_scroll; it is a
    // runtime no-op here since no scroll map is bound).
    static void set_fine_scroll_x(u8) {}
    static void set_fine_scroll_y(u8) {}

    // Frame-service install.
    static void install_frame_isr(void (*h)()) { frame_handler = h; }

    static void reset() {
        last_program = nullptr; dma_enabled = false; charset_base = 0; charset_writes = 0;
        sprite_base_page = 0; sprite_base_set = false; sprite_dma_on = false;
        idle_dim_suppressions = 0;
        frame_handler = nullptr; key_state = 0; clear_calls = 0;
        for (u8 i = 0; i < 2; ++i) joy_state[i] = 0;
        for (u8 i = 0; i < 4; ++i)
            in_s_bg[i] = in_s_s[i] = in_p_bg[i] = in_p_s[i] = 0;
    }
};
const u8* MockHal::last_program  = nullptr;
bool      MockHal::dma_enabled   = false;
u8        MockHal::charset_base   = 0;
unsigned  MockHal::charset_writes = 0;
u8        MockHal::sprite_base_page = 0;
bool      MockHal::sprite_base_set  = false;
bool      MockHal::sprite_dma_on    = false;
unsigned  MockHal::idle_dim_suppressions = 0;
void    (*MockHal::frame_handler)() = nullptr;
u8        MockHal::joy_state[2]  = {};
u8        MockHal::key_state     = 0;
u8        MockHal::in_s_bg[4]    = {};
u8        MockHal::in_s_s[4]     = {};
u8        MockHal::in_p_bg[4]    = {};
u8        MockHal::in_p_s[4]     = {};
unsigned  MockHal::clear_calls   = 0;

struct MockPlatform {
    using hal = MockHal;
    template <typename Layout>
    using display_program = atari::DisplayProgram<Layout>;
};

// ── Game configuration: single-screen text, 4 sprites, 2 sound channels ──

struct ScreenText {
    using display = DisplayLayout<TextRegion<M::Mode::MODE_2, 24>>;
};

struct GameConfig {
    using screens = ScreenSet<ScreenText>;
    static constexpr u8 max_sprites     = 4;
    static constexpr u8 sound_channels  = 2;
};

using Game = Core<MockPlatform, GameConfig>;

// ── Compile-time query checks ──────────────────────────────────────────

static_assert(Game::max_display_ram == 960, "40x24 text = 960 bytes");
static_assert(Game::ram_usage > 0,          "ram_usage is nonzero");
static_assert(Game::ram_usage < 32768,      "ram_usage fits well under 32K");
static_assert(Game::user_zp_base >= 0x80,   "user ZP is in the user page");
static_assert(Game::zp_remaining > 0,       "some ZP is left for the game");

// ── Runtime harness ────────────────────────────────────────────────────

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Tests ───────────────────────────────────────────────────────────────

// init() brings up the initial screen, enables sprites, and installs the frame
// service.
static void test_init() {
    MockHal::reset();
    Game::init();

    CHECK(MockHal::last_program != nullptr);  // display program installed
    CHECK(MockHal::dma_enabled);              // display DMA re-enabled
    CHECK(MockHal::sprite_base_set);          // sprite base programmed
    CHECK(MockHal::sprite_dma_on);            // sprite DMA latched
    CHECK(MockHal::frame_handler == &Game::frame_service);
}

// init(tileset) additionally loads the tileset and binds the charset base to it.
static void test_init_charset() {
    MockHal::reset();
    static const u8 cs_bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto cs = engine::make_tileset(cs_bytes);
    Game::init(cs);
    CHECK(MockHal::charset_writes == 1);
    CHECK(MockHal::frame_handler == &Game::frame_service);
}

// The sub-object accessors resolve to the right subsystems, and multiplex is an
// alias of the sprite manager.
static void test_subsystem_access() {
    CHECK(!Game::sound.active(0));                          // SoundManager
    CHECK(Game::interrupts.raster_hook_count() == 0 ||
          Game::interrupts.raster_hook_count() > 0);        // InterruptManager (compiles)
    CHECK(static_cast<const void*>(&Game::multiplex) ==
          static_cast<const void*>(&Game::sprites));
}

// Game::sprite() delegates to the sprite manager's logical buffer.
static void test_sprite_delegation() {
    auto shp = make_sprite<8, 2>({0xFF, 0x0F});
    Game::sprite_hide_all();
    Game::sprite_color(0, 0x3A);
    Game::sprite(0, shp, 10, 20);

    const auto& ls = Game::sprites.logical(0);
    CHECK(ls.x == 10);
    CHECK(ls.y == 20);
    CHECK(ls.height == 2);
    CHECK(ls.color == 0x3A);             // sprite_color set it; sprite() kept it
    CHECK((ls.flags & engine::LogicalSprite::FLAG_ACTIVE) != 0);
    CHECK((ls.flags & engine::LogicalSprite::FLAG_VISIBLE) != 0);
}

// Game::print() writes screen codes into the region buffer.
static void test_print() {
    MockHal::reset();
    Game::init();                       // binds region views to the buffer
    Game::print(0, 0, "AB");
    auto& region = Game::region<0>();
    using tr = engine::display::traits<M::Mode>;
    CHECK(region.get_char(0, 0) == tr::to_screen_code('A'));
    CHECK(region.get_char(1, 0) == tr::to_screen_code('B'));
}

// frame_service() captures input (incl. system keys packed into port 0) and
// latches + clears the collision registers.
static void test_frame_service() {
    MockHal::reset();
    Game::init();

    MockHal::joy_state[0] = 0x10 | 0x20;   // FIRE + system PRIMARY on port 0
    MockHal::joy_state[1] = 0x04;          // LEFT on port 1
    MockHal::key_state    = 0x15;
    MockHal::in_s_s[0]    = 0x04;          // sprite 0 hit sprite 2
    MockHal::in_p_bg[1]   = 0x02;          // projectile 1 hit background 1

    Game::frame_service();

    CHECK(Game::input.fire(0));
    CHECK(Game::input.system_primary());
    CHECK(Game::input.left(1));
    CHECK(Game::input.key() == 0x15);

    CHECK(Game::sprite_collisions().sprite_to_sprite(0) == 0x04);
    CHECK(Game::sprite_collisions().projectile_to_background(1) == 0x02);
    CHECK(MockHal::clear_calls == 1);
    CHECK(MockHal::idle_dim_suppressions == 1);   // idle-dim timer zeroed this frame

    CHECK(Game::frame_ready_);             // loop frame released
}

// The loop entry points must compile (they are never run without a real frame ISR).
static void compile_only_loop_check() {
    volatile bool never = false;
    if (never) { Game::run([](auto&) {}); }
    if (never) { Game::run_until([](auto&) -> bool { return true; }); }
    (void)Game::frame_overrun();
}

int main() {
    test_init();
    test_init_charset();
    test_subsystem_access();
    test_sprite_delegation();
    test_print();
    test_frame_service();
    compile_only_loop_check();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
