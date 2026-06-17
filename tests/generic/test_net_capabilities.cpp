// test_net_capabilities.cpp — compile-time capability gating tests for Game::net.

#include <stdio.h>

#include <engine/core.h>
#include <engine/display.h>
#include <engine/screen.h>
#include <engine/net.h>
#include <engine/platform/atari/platform.h>

using engine::u8;

namespace td {
template <typename...> using void_t = void;

template <typename T, typename = void>
struct has_net { static constexpr bool value = false; };
template <typename T>
struct has_net<T, void_t<decltype(T::net)>> { static constexpr bool value = true; };

template <typename T, typename = void>
struct has_realtime { static constexpr bool value = false; };
template <typename T>
struct has_realtime<T, void_t<decltype(T::realtime)>> { static constexpr bool value = true; };

template <typename T, typename = void>
struct has_session { static constexpr bool value = false; };
template <typename T>
struct has_session<T, void_t<decltype(T::session)>> { static constexpr bool value = true; };
} // namespace td

struct NetScreen {
    using display = engine::DisplayLayout<engine::TextRegion<atari::Mode::MODE_2, 24>>;
};
struct NetConfig {
    using screens = engine::ScreenSet<NetScreen>;
    static constexpr u8 max_sprites = 1;
    static constexpr u8 sound_channels = 1;
};

// Real Atari axes: None has no net; Fujinet has both lanes.
using NonePlatform = atari::StockXL_NTSC;
using FujiPlatform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::gfx::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC,
    atari::Network::Fujinet>;

using NoneGame = engine::Core<NonePlatform, NetConfig>;
using FujiGame = engine::Core<FujiPlatform, NetConfig>;

// Lane-selective mock platforms for per-lane compile-time gating tests.
struct CapsRealtimeOnly : engine::Capabilities {
    static constexpr bool has_network_realtime = true;
    static constexpr bool has_network_session = false;
    static constexpr bool has_network = has_network_realtime || has_network_session;
};
struct CapsSessionOnly : engine::Capabilities {
    static constexpr bool has_network_realtime = false;
    static constexpr bool has_network_session = true;
    static constexpr bool has_network = has_network_realtime || has_network_session;
};

struct MockRtPlatform { using capabilities = CapsRealtimeOnly; };
struct MockSessPlatform { using capabilities = CapsSessionOnly; };

using RtOnlyNet = engine::net::NetManager<MockRtPlatform, NetConfig>;
using SessOnlyNet = engine::net::NetManager<MockSessPlatform, NetConfig>;

static_assert(!NonePlatform::capabilities::has_network, "None platform has no network");
static_assert(FujiPlatform::capabilities::has_network, "Fujinet platform has network");
static_assert(FujiPlatform::capabilities::has_network_realtime,
              "Fujinet platform enables realtime lane");
static_assert(FujiPlatform::capabilities::has_network_session,
              "Fujinet platform enables session lane");

static_assert(!td::has_net<NoneGame>::value,
              "Game::net must not exist when has_network is false");
static_assert(td::has_net<FujiGame>::value,
              "Game::net must exist when has_network is true");

static_assert(td::has_realtime<decltype(FujiGame::net)>::value,
              "Game::net.realtime must exist when realtime is enabled");
static_assert(td::has_session<decltype(FujiGame::net)>::value,
              "Game::net.session must exist when session is enabled");

static_assert(td::has_realtime<RtOnlyNet>::value,
              "Net manager should expose realtime lane when enabled");
static_assert(!td::has_session<RtOnlyNet>::value,
              "Net manager should hide session lane when disabled");
static_assert(!td::has_realtime<SessOnlyNet>::value,
              "Net manager should hide realtime lane when disabled");
static_assert(td::has_session<SessOnlyNet>::value,
              "Net manager should expose session lane when enabled");

int main() {
    printf("ALL TESTS PASSED\n");
    return 0;
}
