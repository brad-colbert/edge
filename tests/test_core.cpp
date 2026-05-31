// test_core.cpp — unit tests for engine/core.h (the integration layer).
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// Verifies that engine::Core wires the subsystems together: the compile-time
// queries, the sub-object accessors, init(), the sprite/text delegators, and the
// per-frame vbi_service() sequence (input capture + collision latch). The loop
// (run/run_until) needs real VBI hardware, so it is only checked for compilation,
// never invoked — same convention as the live ANTIC/interrupt paths.

#include <stdint.h>
#include <stdio.h>

#include <engine/core.h>
#include <engine/display.h>

using engine::u8;
using engine::u16;

using engine::Core;
using engine::DisplayLayout;
using engine::TextRegion;
using engine::ScreenSet;
using engine::make_sprite;

namespace M = atari;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records the HAL calls Core makes and lets the test inject input/collision
// reads. All-static, mirroring the other engine test mocks.
struct MockHal {
    // Display / DMA.
    static const u8* last_dl;
    static u8        dma_state;
    static u8        chbase;
    static unsigned  chbase_writes;
    // P/M.
    static u8        pm_base_page;
    static bool      pm_base_set;
    static u8        gractl;
    // VBI install.
    static void    (*vbi_handler)();
    // Injected input.
    static u8        joy_state[2];
    static u8        key_state;
    // Injected collisions.
    static u8        in_p_pf[4], in_p_pl[4], in_m_pf[4], in_m_pl[4];
    static unsigned  clear_calls;

    static constexpr uint16_t pm_area_bytes = 2048;

    // Display list / DMA.
    static void antic_dma_disable()            { dma_state = 0; }
    static void antic_dma_enable(u8 v = 0x22)  { dma_state = v; }
    static void set_display_list(const u8* dl) { last_dl = dl; }

    // Char set.
    static void write_chbase(u8 page) { chbase = page; ++chbase_writes; }

    // Sound (commit/tick may touch these).
    static void write_audf(u8, u8) {}
    static void write_audc(u8, u8) {}

    // P/M positions + layout.
    static void     write_hposp(u8, u8) {}
    static void     write_hposm(u8, u8) {}
    static uint16_t pm_player_offset(u8, u8 player) {
        return static_cast<uint16_t>(player * 256);
    }
    static uint16_t pm_strip_size(u8) { return 256; }

    // DLI dispatcher addresses (prepare_chain reads these).
    static uint16_t dli_dispatch_addr() { return 0xD15A; }
    static uint16_t dli_terminal_addr() { return 0xD160; }

    // Input capture (injected).
    static u8 read_joystick(u8 port) { return joy_state[port]; }
    static u8 read_keyboard()        { return key_state; }

    // Collision reads (injected) + clear.
    static u8 coll_player_playfield(u8 p)  { return in_p_pf[p]; }
    static u8 coll_player_player(u8 p)     { return in_p_pl[p]; }
    static u8 coll_missile_playfield(u8 m) { return in_m_pf[m]; }
    static u8 coll_missile_player(u8 m)    { return in_m_pl[m]; }
    static void clear_collisions()         { ++clear_calls; }

    // P/M DMA setup.
    static void set_pm_base(u8 page) { pm_base_page = page; pm_base_set = true; }
    static void pm_dma_enable()      { gractl = 0x03; }
    static void pm_dma_disable()     { gractl = 0x00; }

    // VBI install.
    static void install_vbi(void (*h)()) { vbi_handler = h; }

    static void reset() {
        last_dl = nullptr; dma_state = 0; chbase = 0; chbase_writes = 0;
        pm_base_page = 0; pm_base_set = false; gractl = 0;
        vbi_handler = nullptr; key_state = 0; clear_calls = 0;
        for (u8 i = 0; i < 2; ++i) joy_state[i] = 0;
        for (u8 i = 0; i < 4; ++i)
            in_p_pf[i] = in_p_pl[i] = in_m_pf[i] = in_m_pl[i] = 0;
    }
};
const u8* MockHal::last_dl       = nullptr;
u8        MockHal::dma_state     = 0;
u8        MockHal::chbase        = 0;
unsigned  MockHal::chbase_writes = 0;
u8        MockHal::pm_base_page  = 0;
bool      MockHal::pm_base_set   = false;
u8        MockHal::gractl        = 0;
void    (*MockHal::vbi_handler)() = nullptr;
u8        MockHal::joy_state[2]  = {};
u8        MockHal::key_state     = 0;
u8        MockHal::in_p_pf[4]    = {};
u8        MockHal::in_p_pl[4]    = {};
u8        MockHal::in_m_pf[4]    = {};
u8        MockHal::in_m_pl[4]    = {};
unsigned  MockHal::clear_calls   = 0;

struct MockPlatform {
    using hal = MockHal;
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

// init() brings up the initial screen, enables P/M, and installs the VBI.
static void test_init() {
    MockHal::reset();
    Game::init();

    CHECK(MockHal::last_dl != nullptr);      // display list installed
    CHECK(MockHal::dma_state != 0);          // ANTIC DMA re-enabled
    CHECK(MockHal::pm_base_set);             // PMBASE programmed
    CHECK(MockHal::gractl == 0x03);          // P/M DMA latched
    CHECK(MockHal::vbi_handler == &Game::vbi_service);
}

// init(charset) additionally loads the char set and points CHBASE at it.
static void test_init_charset() {
    MockHal::reset();
    static const u8 cs_bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto cs = engine::make_charset(cs_bytes);
    Game::init(cs);
    CHECK(MockHal::chbase_writes == 1);
    CHECK(MockHal::vbi_handler == &Game::vbi_service);
}

// The sub-object accessors resolve to the right subsystems, and multiplex is an
// alias of the sprite manager.
static void test_subsystem_access() {
    CHECK(!Game::sound.active(0));                 // SoundManager
    CHECK(Game::interrupts.dli_count() == 0 ||
          Game::interrupts.dli_count() > 0);       // InterruptManager (compiles)
    CHECK(static_cast<const void*>(&Game::multiplex) ==
          static_cast<const void*>(&Game::sprites));
}

// Game::sprite() delegates to the sprite manager's logical buffer.
static void test_sprite_delegation() {
    auto shp = make_sprite<8, 2>({0xFF, 0x0F});
    Game::sprite_hide_all();
    Game::sprite(0, shp, 10, 20);

    const auto& ls = Game::sprites.logical(0);
    CHECK(ls.x == 10);
    CHECK(ls.y == 20);
    CHECK(ls.height == 2);
    CHECK((ls.flags & engine::LogicalSprite::FLAG_ACTIVE) != 0);
    CHECK((ls.flags & engine::LogicalSprite::FLAG_VISIBLE) != 0);
}

// Game::print() writes screen codes into the region buffer.
static void test_print() {
    MockHal::reset();
    Game::init();                       // binds region views to the buffer
    Game::print(0, 0, "AB");
    auto& region = Game::region<0>();
    CHECK(region.get_char(0, 0) == M::ascii_to_internal('A'));
    CHECK(region.get_char(1, 0) == M::ascii_to_internal('B'));
}

// vbi_service() captures input (incl. console keys packed into port 0) and
// latches + clears the collision registers.
static void test_vbi_service() {
    MockHal::reset();
    Game::init();

    MockHal::joy_state[0] = 0x10 | 0x20;   // FIRE + START on port 0
    MockHal::joy_state[1] = 0x04;          // LEFT on port 1
    MockHal::key_state    = 0x15;
    MockHal::in_p_pl[0]   = 0x04;          // player 0 hit player 2
    MockHal::in_m_pf[1]   = 0x02;          // missile 1 hit playfield 1

    Game::vbi_service();

    CHECK(Game::input.fire(0));
    CHECK(Game::input.start());
    CHECK(Game::input.left(1));
    CHECK(Game::input.key() == 0x15);

    CHECK(Game::pm_collisions().player_to_player(0) == 0x04);
    CHECK(Game::pm_collisions().missile_to_playfield(1) == 0x02);
    CHECK(MockHal::clear_calls == 1);

    CHECK(Game::frame_ready_);             // loop frame released
}

// The loop entry points must compile (they are never run without real VBI).
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
    test_vbi_service();
    compile_only_loop_check();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
