// demo/fujinet_session_validate/fujinet_session_validate.cpp
//
// Minimal Stage 8H runtime validation tool for the optional fujinet-lib-backed
// session lane. This is not a gameplay demo.
//
// What it does:
// 1) Game::net.session.connect_tcp(host, port)
// 2) One explicit small send: "ping"
// 3) Bounded poll/recv loop for a short window
// 4) On-screen status codes for connect/send/read + last_error status/detail
//
// Constraints:
// - Session lane only (no realtime lane usage).
// - No heap, no std::string/vector, no exceptions/RTTI/virtual dispatch.
// - Send is one-shot, not in a tight frame loop.
//
// IMPORTANT: session send currently calls fujinet-lib network_write underneath.
// network_write may block for a full FujiNet/CIO transaction. Keep writes small
// and avoid calling this from timing-critical gameplay frames.

#include <stdint.h>

#include <engine/core.h>
#include <engine/display.h>
#include <engine/net.h>
#include <engine/platform/atari/platform.h>
#include <engine/screen.h>

using engine::u8;
using engine::u16;
using engine::i16;
using engine::net::NetError;
using engine::net::NetStatus;

namespace M = atari;

//#ifndef EDGE_FUJINET_VALIDATE_HOST
#define EDGE_FUJINET_VALIDATE_HOST "127.0.0.1"
//#endif

#ifndef EDGE_FUJINET_VALIDATE_PORT
#define EDGE_FUJINET_VALIDATE_PORT 9000
#endif

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

static constexpr const char* kHost = EDGE_FUJINET_VALIDATE_HOST;
static constexpr u16 kPort = static_cast<u16>(EDGE_FUJINET_VALIDATE_PORT);
static constexpr u8 kMaxPollFrames = 120;

static NetStatus g_connect_status = NetStatus::WouldBlock;
static NetStatus g_send_status = NetStatus::WouldBlock;
static NetStatus g_read_status = NetStatus::WouldBlock;
static NetError g_connect_error{};
static NetError g_send_error{};
static NetError g_last_error{};
static u8 g_fn_err = 0;
static u8 g_dev_err = 0;
static u8 g_fn_conn = 0;
static u16 g_fn_bw = 0;

static u8 g_frame = 0;
static u8 g_rx_size = 0;
static u8 g_rx_kind = 0;

static u16 as_u16_status(NetStatus st) {
    return static_cast<u16>(static_cast<u8>(st));
}

static void snapshot_adapter_diag() {
    g_fn_err = Platform::hal::session_diag_fn_error();
    g_dev_err = Platform::hal::session_diag_device_error();
    g_fn_conn = Platform::hal::session_diag_conn();
    g_fn_bw = Platform::hal::session_diag_bw();
}

static void render_status() {
    Game::print(0, 0, "FUJINET SESSION VALIDATE");
    Game::print(0, 1, "HOST:");
    Game::print(5, 1, kHost);
    Game::print(0, 2, "PORT:");
    Game::print_num(5, 2, kPort, 5);

    Game::print(0, 4, "CONN:");
    Game::print_num(6, 4, as_u16_status(g_connect_status), 2);
    Game::print(10, 4, "DET:");
    Game::print_num(14, 4, static_cast<u16>(g_connect_error.detail), 5);

    Game::print(0, 5, "SEND:");
    Game::print_num(6, 5, as_u16_status(g_send_status), 2);
    Game::print(10, 5, "DET:");
    Game::print_num(14, 5, static_cast<u16>(g_send_error.detail), 5);

    Game::print(0, 6, "READ:");
    Game::print_num(6, 6, as_u16_status(g_read_status), 2);

    Game::print(0, 7, "LAST:");
    Game::print_num(6, 7, as_u16_status(g_last_error.status), 2);
    Game::print(10, 7, "DET:");
    Game::print_num(14, 7, static_cast<u16>(g_last_error.detail), 5);

    Game::print(0, 9, "RXK:");
    Game::print_num(4, 9, g_rx_kind, 3);
    Game::print(9, 9, "RXS:");
    Game::print_num(13, 9, g_rx_size, 3);

    Game::print(0, 10, "FN_ERR:");
    Game::print_num(7, 10, g_fn_err, 3);
    Game::print(0, 11, "DEV_ERR:");
    Game::print_num(8, 11, g_dev_err, 3);
    Game::print(0, 12, "FN_CONN:");
    Game::print_num(8, 12, g_fn_conn, 3);
    Game::print(0, 13, "FN_BW:");
    Game::print_num(6, 13, g_fn_bw, 5);

    Game::print(0, 15, "FR:");
    Game::print_num(3, 15, g_frame, 3);
    Game::print(8, 15, "MAX:");
    Game::print_num(12, 15, kMaxPollFrames, 3);

    Game::print(0, 17, "PING SENT ONCE");
    Game::print(0, 18, "READ LOOP IS BOUNDED");
    Game::print(0, 20, "FIRE TO EXIT EARLY");
}

static bool step(const engine::Input& in) {
    if (g_frame < 255) {
        ++g_frame;
    }

    if (Game::net.session.connected()) {
        const NetStatus poll_st = Game::net.session.poll();
        if (poll_st == NetStatus::Ok || poll_st == NetStatus::WouldBlock) {
            engine::net::SessionMessageView view{};
            if (Game::net.session.recv(view)) {
                g_read_status = NetStatus::Ok;
                g_rx_kind = view.kind;
                g_rx_size = (view.size > 255) ? 255 : static_cast<u8>(view.size);
            } else {
                g_read_status = Game::net.session.last_error().status;
            }
        } else {
            g_read_status = poll_st;
        }
    } else {
        g_read_status = NetStatus::Closed;
    }

    g_last_error = Game::net.session.last_error();
    snapshot_adapter_diag();
    render_status();

    return in.fire() || g_frame >= kMaxPollFrames;
}

int main() {
    Game::init();

    g_connect_status = Game::net.session.connect_tcp(kHost, kPort);
    g_connect_error = Game::net.session.last_error();

    if (g_connect_status == NetStatus::Ok) {
        // One small explicit send for validation only. This call path may block
        // because session_send_nb uses fujinet-lib network_write underneath.
        static constexpr uint8_t kPing[] = {'p', 'i', 'n', 'g'};
        g_send_status = Game::net.session.send_bytes(kPing, sizeof(kPing), 1);
        g_send_error = Game::net.session.last_error();
    } else {
        g_send_status = NetStatus::Closed;
        g_send_error = Game::net.session.last_error();
    }

    g_last_error = Game::net.session.last_error();
    snapshot_adapter_diag();
    render_status();

    Game::run_until(step);
    Game::net.session.close();

    Game::print(0, 22, "DONE - SESSION CLOSED");
    for (;;) {}
}
