#include <stdint.h>

#include <engine/core.h>
#include <engine/display.h>
#include <engine/net.h>
#include <engine/platform/atari/platform.h>
#include <engine/screen.h>

using engine::u8;

namespace M = atari;

// Stage 7 dual-lane API usage demo.
//
// This sample demonstrates intended game-side flow only. The Atari Fujinet HAL
// seam is still stubbed (no real transport wiring yet), so calls below are API
// shape examples and compile-time integration checks.

using Platform = M::Platform<
    M::Machine::XL,
    M::RAM::Baseline,
    M::gfx::Baseline,
    M::Sound::Mono,
    M::TV::NTSC,
    M::Network::Fujinet>;

struct DemoScreen {
    using display = engine::DisplayLayout<engine::TextRegion<M::Mode::MODE_2, 24>>;
};

struct DemoConfig {
    using screens = engine::ScreenSet<DemoScreen>;
    static constexpr u8 max_sprites = 1;
    static constexpr u8 sound_channels = 1;
};

using Game = engine::Core<Platform, DemoConfig>;

static u8 g_tick = 0;

static void init_network() {
    // Realtime lane intent: UDP-seq gameplay packets.
    Game::net.realtime.open_udp_seq("192.168.1.100", 9001);

    // Session lane intent: reliable TCP-like control/chat/lobby messages.
    Game::net.session.connect_tcp("192.168.1.100", 9000);
}

static void update_network_frame() {
    Game::net.realtime.poll();

    engine::net::RealtimePacket16 pkt{};
    while (Game::net.realtime.recv(pkt)) {
        // Apply remote player state from pkt.bytes[16].
    }

    engine::net::RealtimePacket16 local{};
    local.bytes[0] = g_tick++;
    Game::net.realtime.send(local);

    Game::net.session.poll();

    engine::net::SessionMessageView msg{};
    while (Game::net.session.recv(msg)) {
        // Handle reliable session/control data from msg.
        (void)msg;
    }

    if ((g_tick & 0x3F) == 0) {
        static constexpr uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
        Game::net.session.send_bytes(hello, sizeof(hello), 0);
    }
}

int main() {
    Game::init();
    init_network();

    // Real FujiNet transport calls are deferred to a later stage; this demo
    // intentionally runs with the current stub seam and no server dependency.
    Game::run([](const auto&) {
        update_network_frame();
    });
}