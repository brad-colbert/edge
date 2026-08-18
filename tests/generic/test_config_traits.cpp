// test_config_traits.cpp — pay-for-what-you-use capacity and lane traits.
//
// Covers the GameConfig knobs whose whole purpose is that unused capacity costs
// nothing: the raster/frame hook capacities, uses_hw_collisions, the per-direction
// session lane sizes, and net_lanes. The size claims themselves are link-time
// facts measured on a real image (docs/PROPOSAL_size_diet.md); what is asserted
// here is the behaviour those savings must not break — defaults unchanged, a
// specialized build still correct at its declared capacity, and narrowing that
// never widens past the platform.
//
// Built for the llvm-mos `mos-sim` platform; main()'s return value is the CTest
// exit code (0 = pass).

#include <stdint.h>
#include <stdio.h>

#include <engine/interrupt.h>
#include <engine/screen.h>
#include <engine/core.h>
#include <engine/net_api.h>
#include <engine/net_types.h>

using engine::u8;
using engine::u16;

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Mock platform ─────────────────────────────────────────────────────

struct MockHal {
    static constexpr u16 DISPATCH = 0xD15A;
    static constexpr u16 TERMINAL = 0xD160;
    static u16 raster_dispatch_addr() { return DISPATCH; }
    static u16 raster_terminal_addr() { return TERMINAL; }
    static void program_raster_lines(u8*, u16, const u8*, u8) {}
    static void set_raster_vector(u16) {}
    static void enable_raster()  {}
    static void disable_raster() {}
    static void install_raster_dispatch(u16, u16, u16, u16, u16, u16) {}
};

// Both lanes present in hardware — so any narrowing observed below comes from
// GameConfig, never from the capability profile.
struct BothLanesCaps : engine::Capabilities {
    static constexpr bool has_network          = true;
    static constexpr bool has_network_realtime = true;
    static constexpr bool has_network_session  = true;
};

struct MockPlatform {
    using hal = MockHal;
    using capabilities = BothLanesCaps;
};

// Realtime in hardware, no session lane at all.
struct RealtimeOnlyCaps : engine::Capabilities {
    static constexpr bool has_network          = true;
    static constexpr bool has_network_realtime = true;
    static constexpr bool has_network_session  = false;
};
struct RealtimeOnlyPlatform {
    using hal = MockHal;
    using capabilities = RealtimeOnlyCaps;
};

// ── Game configs ──────────────────────────────────────────────────────

struct DefaultConfig { };

struct LeanConfig {
    static constexpr u8   max_raster_hooks   = 1;
    static constexpr u8   max_frame_hooks    = 0;
    static constexpr bool uses_hw_collisions = false;
    static constexpr u16  session_rx_bytes    = 64;
    static constexpr u16  session_tx_bytes    = 32;
    static constexpr u16  session_max_message = 48;
};

struct RealtimeOnlyConfig {
    static constexpr engine::net::NetLanes net_lanes = engine::net::NetLanes::Realtime;
};
struct SessionOnlyConfig {
    static constexpr engine::net::NetLanes net_lanes = engine::net::NetLanes::Session;
};

// Asks for a session lane the hardware does not have — narrowing must not widen.
struct SessionOnRealtimeOnlyHw {
    static constexpr engine::net::NetLanes net_lanes = engine::net::NetLanes::Session;
};

// Member-detection helper: does NetManager<...> expose `.session` / `.realtime`?
template <typename...> using void_t = void;
namespace { struct yes { static constexpr bool value = true; };
            struct no  { static constexpr bool value = false; }; }
template <typename T, typename = void> struct HasSession : no { };
template <typename T> struct HasSession<T, void_t<decltype(T::session)>> : yes { };
template <typename T, typename = void> struct HasRealtime : no { };
template <typename T> struct HasRealtime<T, void_t<decltype(T::realtime)>> : yes { };

// ── Tests ─────────────────────────────────────────────────────────────

// Defaults must be exactly what they were before the capacity work: a game that
// declares nothing keeps 12/4, hardware collisions, and 256/256/128.
static void test_defaults_unchanged() {
    using IM = engine::InterruptManager<MockPlatform>;
    CHECK(IM::capacity() == 12);

    namespace nd = engine::net::ndetail;
    CHECK(nd::session_rx_bytes_or_default<DefaultConfig>::value == 256);
    CHECK(nd::session_tx_bytes_or_default<DefaultConfig>::value == 256);
    CHECK(nd::session_max_message_or_default<DefaultConfig>::value == 128);
    CHECK(nd::net_lanes_or_default<DefaultConfig>::value == engine::net::NetLanes::Both);
    CHECK(nd::wants_realtime_lane<DefaultConfig>);
    CHECK(nd::wants_session_lane<DefaultConfig>);

    using Net = engine::net::NetManager<MockPlatform, DefaultConfig>;
    CHECK(HasRealtime<Net>::value);
    CHECK(HasSession<Net>::value);
}

// A single-slot chain is the case the insertion sort is compiled away for. It
// must still build a correct one-entry chain and refuse a second hook.
static void test_single_slot_chain() {
    using IM = engine::InterruptManager<MockPlatform, 1, 0>;
    CHECK(IM::capacity() == 1);

    IM im;
    im.add_raster_hook(64, +[]() {});
    CHECK(im.raster_hook_count() == 1);
    CHECK(im.slot(0).scanline == 64);

    // Over capacity: dropped, not written out of bounds.
    im.add_raster_hook(96, +[]() {});
    CHECK(im.raster_hook_count() == 1);
    CHECK(im.slot(0).scanline == 64);

    im.prepare_chain(nullptr, 0);
    CHECK(im.raster_hook_count() == 1);
    // One C++ hook enters through the dispatcher and leaves to the terminal.
    CHECK(im.first_handler_addr() == MockHal::DISPATCH);
    CHECK((static_cast<u16>(im.next_lo(0)) | (static_cast<u16>(im.next_hi(0)) << 8))
          == MockHal::TERMINAL);
}

// Regression guard on the `if constexpr (MaxRasterHooks > 1)` sort gate: a
// multi-slot chain must still be sorted by scanline.
static void test_multi_slot_still_sorts() {
    using IM = engine::InterruptManager<MockPlatform, 4, 2>;
    IM im;
    im.add_raster_hook(120, +[]() {});
    im.add_raster_hook(32,  +[]() {});
    im.add_raster_hook(200, +[]() {});
    im.add_raster_hook(80,  +[]() {});
    im.prepare_chain(nullptr, 0);

    CHECK(im.raster_hook_count() == 4);
    CHECK(im.slot(0).scanline == 32);
    CHECK(im.slot(1).scanline == 80);
    CHECK(im.slot(2).scanline == 120);
    CHECK(im.slot(3).scanline == 200);
}

// Zero frame hooks: registration is a no-op and dispatch runs nothing. The
// storage is one spare pointer (a zero-length array is a compiler extension).
static void test_zero_frame_hooks() {
    using IM = engine::InterruptManager<MockPlatform, 1, 0>;
    static bool fired;
    fired = false;

    IM im;
    CHECK(im.frame_hook_count() == 0);
    im.add_frame_hook(+[]() { fired = true; });
    CHECK(im.frame_hook_count() == 0);

    im.run_frame_hooks();
    CHECK(!fired);

    // Removal on an empty table stays well-defined.
    im.remove_frame_hook(+[]() { fired = true; });
    CHECK(im.frame_hook_count() == 0);
}

// Frame hooks still dispatch, in registration order, when the game asks for them.
static void test_frame_hooks_still_run() {
    using IM = engine::InterruptManager<MockPlatform, 2, 2>;
    static u8 order;
    static u8 first_seen, second_seen;
    order = 0; first_seen = second_seen = 0xFF;

    IM im;
    im.add_frame_hook(+[]() { first_seen  = order++; });
    im.add_frame_hook(+[]() { second_seen = order++; });
    CHECK(im.frame_hook_count() == 2);

    im.run_frame_hooks();
    CHECK(first_seen == 0);
    CHECK(second_seen == 1);
}

// Per-direction session sizing must reach the lane, and shrink its storage.
static void test_session_sizing() {
    namespace nd = engine::net::ndetail;
    CHECK(nd::session_rx_bytes_or_default<LeanConfig>::value == 64);
    CHECK(nd::session_tx_bytes_or_default<LeanConfig>::value == 32);
    CHECK(nd::session_max_message_or_default<LeanConfig>::value == 48);

    using DefaultNet = engine::net::NetManager<MockPlatform, DefaultConfig>;
    using LeanNet    = engine::net::NetManager<MockPlatform, LeanConfig>;
    CHECK(sizeof(LeanNet) < sizeof(DefaultNet));

    LeanNet net;
    CHECK(net.session.max_message_bytes() == 48);
}

// net_lanes narrows the facade to one lane; the other's storage disappears.
static void test_lane_narrowing() {
    using BothNet = engine::net::NetManager<MockPlatform, DefaultConfig>;
    using RtNet   = engine::net::NetManager<MockPlatform, RealtimeOnlyConfig>;
    using SessNet = engine::net::NetManager<MockPlatform, SessionOnlyConfig>;

    CHECK(HasRealtime<RtNet>::value);
    CHECK(!HasSession<RtNet>::value);

    CHECK(HasSession<SessNet>::value);
    CHECK(!HasRealtime<SessNet>::value);

    // Dropping a lane must actually drop its storage.
    CHECK(sizeof(RtNet)   < sizeof(BothNet));
    CHECK(sizeof(SessNet) < sizeof(BothNet));
}

// net_lanes only ever narrows: asking for a lane the platform lacks yields none.
static void test_narrowing_cannot_widen() {
    using Net = engine::net::NetManager<RealtimeOnlyPlatform, SessionOnRealtimeOnlyHw>;
    CHECK(!HasSession<Net>::value);    // hardware says no
    CHECK(!HasRealtime<Net>::value);   // config says no
}

// ── Display-program arena (stage C) ───────────────────────────────────

// Mock layouts/screens of differing display-program size, so the "size the block
// to the biggest screen" rule is observable.
struct LayoutSmall { static constexpr u16 total_ram = 100; static constexpr u16 kBytes = 64; };
struct LayoutBig   { static constexpr u16 total_ram = 200; static constexpr u16 kBytes = 200; };
struct ScreenSmall { using display = LayoutSmall; };
struct ScreenBig   { using display = LayoutBig; };

template <typename Layout> struct MockProgram { u8 bytes[Layout::kBytes]; };
struct MockDisplayPlatform {
    using hal = MockHal;
    template <typename Layout> using display_program = MockProgram<Layout>;
};

static u8 g_test_arena[256];

struct NoArenaConfig {
    using screens = engine::ScreenSet<ScreenSmall, ScreenBig>;
};
struct ArenaConfig {
    using screens = engine::ScreenSet<ScreenSmall, ScreenBig>;
    static u8* display_program_arena() { return g_test_arena; }
    static constexpr u16 display_program_arena_bytes = sizeof(g_test_arena);
};

// Absent members mean engine-owned statics — the default must not shift.
static void test_arena_detection() {
    CHECK(!engine::detail::has_dl_arena<NoArenaConfig>::value);
    CHECK(engine::detail::has_dl_arena<ArenaConfig>::value);
    CHECK(!engine::detail::has_dl_arena<DefaultConfig>::value);
}

// Screens sharing one block must size it to the largest of them, not the first.
static void test_arena_sizing() {
    using Screens = engine::ScreenSet<ScreenSmall, ScreenBig>;
    constexpr u16 need = engine::display_program_bytes<MockDisplayPlatform, Screens>;
    CHECK(need == sizeof(MockProgram<LayoutBig>));
    CHECK(need >= sizeof(MockProgram<LayoutSmall>));

    // Order must not matter — the max is over the whole set.
    using Reversed = engine::ScreenSet<ScreenBig, ScreenSmall>;
    constexpr u16 reversed_need =
        engine::display_program_bytes<MockDisplayPlatform, Reversed>;
    CHECK(reversed_need == need);

    // Single-screen set: exactly that screen's program.
    using One = engine::ScreenSet<ScreenSmall>;
    constexpr u16 one_need = engine::display_program_bytes<MockDisplayPlatform, One>;
    CHECK(one_need == sizeof(MockProgram<LayoutSmall>));
}

// ── Sprite-memory arena (ask 6) ───────────────────────────────────────

static u8 g_test_pm[2048];

struct NoSpriteArenaConfig { };
struct SpriteArenaConfig {
    static u8* sprite_memory() { return g_test_pm; }
    static constexpr u16 sprite_memory_bytes = sizeof(g_test_pm);
};

// A HAL that declares no dead head region: the query must stay total and answer 0
// rather than failing to compile.
struct HeadlessHal { };
struct HeadlessPlatform { using hal = HeadlessHal; };
// A HAL that does declare one.
struct HeadedHal { static constexpr u16 sprite_area_head_bytes = 768; };
struct HeadedPlatform { using hal = HeadedHal; };

static void test_sprite_arena_detection() {
    CHECK(!engine::cdetail::has_sprite_arena<NoSpriteArenaConfig>::value);
    CHECK(engine::cdetail::has_sprite_arena<SpriteArenaConfig>::value);
    // The display-program arena and the sprite arena are independent opt-ins.
    CHECK(!engine::cdetail::has_sprite_arena<DefaultConfig>::value);
}

// The head region is a platform fact, so a game must query it rather than assume
// a value — and a backend without the concept must still compile.
static void test_sprite_head_bytes_is_total() {
    CHECK(engine::cdetail::sprite_head_bytes<HeadlessPlatform>::value == 0);
    CHECK(engine::cdetail::sprite_head_bytes<HeadedPlatform>::value == 768);
}

int main() {
    test_sprite_arena_detection();
    test_sprite_head_bytes_is_total();
    test_arena_detection();
    test_arena_sizing();
    test_defaults_unchanged();
    test_single_slot_chain();
    test_multi_slot_still_sorts();
    test_zero_frame_hooks();
    test_frame_hooks_still_run();
    test_session_sizing();
    test_lane_narrowing();
    test_narrowing_cannot_widen();

    if (g_failures == 0) printf("test_config_traits: all tests passed\n");
    else                 printf("test_config_traits: %u FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
