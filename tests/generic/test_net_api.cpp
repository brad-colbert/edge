// test_net_api.cpp — Stage-6.5 tests for HAL-wired Game::net lane stubs.

#include <stdio.h>

#include <engine/core.h>
#include <engine/display.h>
#include <engine/net.h>
#include <engine/screen.h>
#include <engine/platform/atari/platform.h>

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

namespace n = engine::net;

struct MockHal {
    static bool realtime_open;
    static bool session_open;

    static u16 realtime_open_calls;
    static u16 session_connect_calls;
    static u16 realtime_send_calls;
    static u16 session_send_calls;

    static n::NetError rt_err;
    static n::NetError sess_err;

    static u8 rt_rx[4][16];
    static u8 rt_rx_head;
    static u8 rt_rx_tail;
    static u8 rt_rx_count;

    static u8 sess_rx[512];
    static u16 sess_rx_head;
    static u16 sess_rx_tail;
    static u16 sess_rx_count;

    static void reset() {
        realtime_open = false;
        session_open = false;
        realtime_open_calls = 0;
        session_connect_calls = 0;
        realtime_send_calls = 0;
        session_send_calls = 0;
        rt_err = {n::NetStatus::WouldBlock, 0};
        sess_err = {n::NetStatus::WouldBlock, 0};
        rt_rx_head = rt_rx_tail = rt_rx_count = 0;
        sess_rx_head = sess_rx_tail = sess_rx_count = 0;
    }

    static void inject_realtime_packet(u8 tag) {
        if (rt_rx_count >= 4) return;
        rt_rx[rt_rx_head][0] = tag;
        for (u8 i = 1; i < 16; ++i) rt_rx[rt_rx_head][i] = 0;
        rt_rx_head = static_cast<u8>((rt_rx_head + 1) & 0x03);
        ++rt_rx_count;
    }

    static void inject_session_frame(u8 kind, const u8* data, u16 size) {
        if (sess_rx_count + static_cast<u16>(3 + size) > 512) return;
        sess_rx[sess_rx_head] = kind;
        sess_rx_head = static_cast<u16>((sess_rx_head + 1) & 0x01FF);
        ++sess_rx_count;
        sess_rx[sess_rx_head] = static_cast<u8>(size & 0xFF);
        sess_rx_head = static_cast<u16>((sess_rx_head + 1) & 0x01FF);
        ++sess_rx_count;
        sess_rx[sess_rx_head] = static_cast<u8>(size >> 8);
        sess_rx_head = static_cast<u16>((sess_rx_head + 1) & 0x01FF);
        ++sess_rx_count;
        for (u16 i = 0; i < size; ++i) {
            sess_rx[sess_rx_head] = data[i];
            sess_rx_head = static_cast<u16>((sess_rx_head + 1) & 0x01FF);
            ++sess_rx_count;
        }
    }

    static n::NetStatus realtime_open_udp_seq(const char* host, u16 remote_port, u16) {
        ++realtime_open_calls;
        if (host == nullptr || host[0] == '\0' || remote_port == 0)
            return rt_err.status = n::NetStatus::InvalidArgument;
        realtime_open = true;
        rt_err.status = n::NetStatus::Ok;
        return n::NetStatus::Ok;
    }
    static n::NetStatus realtime_bind_udp_seq(u16) {
        rt_err.status = n::NetStatus::Unsupported;
        return n::NetStatus::Unsupported;
    }
    static void realtime_close() { realtime_open = false; rt_err.status = n::NetStatus::Closed; }
    static bool realtime_active() { return realtime_open; }
    static n::NetStatus realtime_poll() {
        if (!realtime_open) return rt_err.status = n::NetStatus::Closed;
        return rt_err.status = n::NetStatus::Ok;
    }
    static n::NetStatus realtime_send_nb(const void* bytes, u16 size) {
        if (bytes == nullptr && size > 0) return rt_err.status = n::NetStatus::InvalidArgument;
        if (!realtime_open) return rt_err.status = n::NetStatus::Closed;
        ++realtime_send_calls;
        return rt_err.status = n::NetStatus::Ok;
    }
    static n::NetStatus realtime_recv_nb(void* bytes, u16 size) {
        if (bytes == nullptr && size > 0) return rt_err.status = n::NetStatus::InvalidArgument;
        if (!realtime_open) return rt_err.status = n::NetStatus::Closed;
        if (rt_rx_count == 0) return rt_err.status = n::NetStatus::WouldBlock;
        u8* out = static_cast<u8*>(bytes);
        for (u16 i = 0; i < size; ++i) out[i] = rt_rx[rt_rx_tail][i];
        rt_rx_tail = static_cast<u8>((rt_rx_tail + 1) & 0x03);
        --rt_rx_count;
        return rt_err.status = n::NetStatus::Ok;
    }
    static n::NetError realtime_last_error() { return rt_err; }

    static n::NetStatus session_connect_tcp(const char* host, u16 port) {
        ++session_connect_calls;
        if (host == nullptr || host[0] == '\0' || port == 0)
            return sess_err.status = n::NetStatus::InvalidArgument;
        session_open = true;
        sess_err.status = n::NetStatus::Ok;
        return n::NetStatus::Ok;
    }
    static void session_close() { session_open = false; sess_err.status = n::NetStatus::Closed; }
    static bool session_connected() { return session_open; }
    static n::NetStatus session_poll() {
        if (!session_open) return sess_err.status = n::NetStatus::Closed;
        return sess_err.status = n::NetStatus::Ok;
    }
    static n::NetStatus session_send_nb(const void* bytes, u16 size) {
        if (bytes == nullptr && size > 0) return sess_err.status = n::NetStatus::InvalidArgument;
        if (!session_open) return sess_err.status = n::NetStatus::Closed;
        ++session_send_calls;
        return sess_err.status = n::NetStatus::Ok;
    }
    static n::NetStatus session_recv_nb(void* bytes, u16 capacity) {
        if (bytes == nullptr && capacity > 0) return sess_err.status = n::NetStatus::InvalidArgument;
        if (!session_open) return sess_err.status = n::NetStatus::Closed;
        if (sess_rx_count == 0) return sess_err.status = n::NetStatus::WouldBlock;
        // Lane asks one byte at a time.
        static_cast<u8*>(bytes)[0] = sess_rx[sess_rx_tail];
        sess_rx_tail = static_cast<u16>((sess_rx_tail + 1) & 0x01FF);
        --sess_rx_count;
        return sess_err.status = n::NetStatus::Ok;
    }
    static n::NetError session_last_error() { return sess_err; }
};

bool MockHal::realtime_open = false;
bool MockHal::session_open = false;
u16 MockHal::realtime_open_calls = 0;
u16 MockHal::session_connect_calls = 0;
u16 MockHal::realtime_send_calls = 0;
u16 MockHal::session_send_calls = 0;
n::NetError MockHal::rt_err = {n::NetStatus::WouldBlock, 0};
n::NetError MockHal::sess_err = {n::NetStatus::WouldBlock, 0};
u8 MockHal::rt_rx[4][16] = {};
u8 MockHal::rt_rx_head = 0;
u8 MockHal::rt_rx_tail = 0;
u8 MockHal::rt_rx_count = 0;
u8 MockHal::sess_rx[512] = {};
u16 MockHal::sess_rx_head = 0;
u16 MockHal::sess_rx_tail = 0;
u16 MockHal::sess_rx_count = 0;

struct MockCaps : engine::Capabilities {
    static constexpr bool has_network_realtime = true;
    static constexpr bool has_network_session = true;
    static constexpr bool has_network = true;
};

struct MockPlatform {
    using capabilities = MockCaps;
    using hal = MockHal;
};

// Minimal Core alias to verify Game::net API shape compiles.
struct NetScreen {
    using display = engine::DisplayLayout<engine::TextRegion<atari::Mode::MODE_2, 24>>;
};
struct NetConfig {
    using screens = engine::ScreenSet<NetScreen>;
    static constexpr u8 max_sprites = 1;
    static constexpr u8 sound_channels = 1;
};
using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::gfx::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC,
    atari::Network::Fujinet>;
using Game = engine::Core<Platform, NetConfig>;

struct Packet16 {
    u8 b[16];
};

static void test_game_net_shape() {
    // Shape test only: ensure facade/lane members are available.
    (void)&Game::net;
    (void)&Game::net.realtime;
    (void)&Game::net.session;
}

static void test_open_and_connect_delegate_to_hal() {
    MockHal::reset();
    n::RealtimeLane<MockPlatform> rt;
    n::SessionLane<MockPlatform> s;

    CHECK(rt.open_udp_seq("192.168.0.2", 2222) == n::NetStatus::Ok);
    CHECK(MockHal::realtime_open_calls == 1);
    CHECK(rt.active());

    CHECK(s.connect_tcp("example.local", 8080) == n::NetStatus::Ok);
    CHECK(MockHal::session_connect_calls == 1);
    CHECK(s.connected());
}

static void test_realtime_send_recv_and_queueing() {
    MockHal::reset();
    n::RealtimeLane<MockPlatform> rt;
    CHECK(rt.open_udp_seq("192.168.0.2", 2222) == n::NetStatus::Ok);

    Packet16 a{{1}};
    Packet16 b{{2}};

    CHECK(rt.send(a) == n::NetStatus::Ok);
    CHECK(rt.send(b) == n::NetStatus::Ok);

    // Poll flushes TX through HAL; no fake inbound packets are created.
    CHECK(rt.poll() == n::NetStatus::Ok);
    CHECK(MockHal::realtime_send_calls == 2);
    CHECK(rt.rx_count() == 0);

    Packet16 out{};
    CHECK(!rt.recv(out));
    CHECK(rt.last_error().status == n::NetStatus::WouldBlock);
}

static void test_realtime_overflow_diagnostics_via_lane() {
    MockHal::reset();
    n::RealtimeLane<MockPlatform, 16, 2, 4> rt;
    CHECK(rt.open_udp_seq("10.0.0.5", 1234) == n::NetStatus::Ok);

    // Inject three inbound packets through HAL seam; RX capacity is 2.
    MockHal::inject_realtime_packet(10);
    MockHal::inject_realtime_packet(20);
    MockHal::inject_realtime_packet(30);

    CHECK(rt.poll() == n::NetStatus::Ok);
    CHECK(rt.rx_count() == 2);
    CHECK(rt.rx_dropped() == 1);
    CHECK(rt.consume_rx_overflowed());
    CHECK(!rt.consume_rx_overflowed());

    Packet16 out{};
    // Oldest dropped => expect 20, then 30.
    CHECK(rt.recv(out)); CHECK(out.b[0] == 20);
    CHECK(rt.recv(out)); CHECK(out.b[0] == 30);
}

static void test_realtime_send_attempts_hal_send_without_fake_rx() {
    MockHal::reset();
    n::RealtimeLane<MockPlatform, 16, 8, 2> rt;
    CHECK(rt.open_udp_seq("10.0.0.5", 1234) == n::NetStatus::Ok);

    Packet16 a{{10}}, b{{20}}, c{{30}};
    CHECK(rt.send(a) == n::NetStatus::Ok);
    CHECK(rt.send(b) == n::NetStatus::Ok);
    CHECK(rt.send(c) == n::NetStatus::Ok);

    CHECK(MockHal::realtime_send_calls >= 2);
    CHECK(rt.rx_count() == 0);
}

static void test_session_connect_host_lifetime_stub() {
    MockHal::reset();
    n::SessionLane<MockPlatform> s;
    char host[] = "example.local";
    CHECK(s.connect_tcp(host, 8080) == n::NetStatus::Ok);

    // Mutate caller buffer after connect; lane must not depend on borrowed pointer.
    host[0] = 'X';
    CHECK(s.connected());

    const u8 payload[2] = {1, 2};
    CHECK(s.send_bytes(payload, 2) == n::NetStatus::Ok);
}

static void test_session_recv_wouldblock_when_empty() {
    MockHal::reset();
    n::SessionLane<MockPlatform> s;
    CHECK(s.connect_tcp("example.local", 9000) == n::NetStatus::Ok);

    n::SessionMessageView v{};
    CHECK(!s.recv(v));
    CHECK(s.last_error().status == n::NetStatus::WouldBlock);
    CHECK(v.data == nullptr);
    CHECK(v.size == 0);
}

static void test_session_send_bytes_size_validation() {
    MockHal::reset();
    n::SessionLane<MockPlatform, 64, 64, 8> s;
    CHECK(s.connect_tcp("example.local", 7777) == n::NetStatus::Ok);

    u8 ok[8] = {};
    u8 too_big[9] = {};

    CHECK(s.send_bytes(ok, 8) == n::NetStatus::Ok);
    CHECK(MockHal::session_send_calls >= 1);
    CHECK(s.send_bytes(too_big, 9) == n::NetStatus::InvalidArgument);
    CHECK(s.send_bytes(nullptr, 1) == n::NetStatus::InvalidArgument);
}

static void test_session_recv_parses_inbound_frame_from_mock_hal() {
    MockHal::reset();
    n::SessionLane<MockPlatform> s;
    CHECK(s.connect_tcp("example.local", 7000) == n::NetStatus::Ok);

    const u8 payload[3] = {7, 8, 9};
    MockHal::inject_session_frame(42, payload, 3);
    CHECK(s.poll() == n::NetStatus::Ok);

    n::SessionMessageView view{};
    CHECK(s.recv(view));
    CHECK(view.kind == 42);
    CHECK(view.size == 3);
    CHECK(view.data != nullptr);
    CHECK(view.data[0] == 7 && view.data[1] == 8 && view.data[2] == 9);
}

int main() {
    test_game_net_shape();
    test_open_and_connect_delegate_to_hal();
    test_realtime_send_recv_and_queueing();
    test_realtime_overflow_diagnostics_via_lane();
    test_realtime_send_attempts_hal_send_without_fake_rx();
    test_session_connect_host_lifetime_stub();
    test_session_recv_wouldblock_when_empty();
    test_session_send_bytes_size_validation();
    test_session_recv_parses_inbound_frame_from_mock_hal();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
