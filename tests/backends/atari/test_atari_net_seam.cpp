// test_atari_net_seam.cpp — Atari network seam availability and stub behavior.

#include <stdio.h>

#include <engine/platform/atari/platform.h>
#include <engine/net_types.h>

namespace a = atari;
namespace n = engine::net;

using NonePlatform = a::StockXL_NTSC;
using FujiPlatform = a::Platform<
    a::Machine::XL,
    a::RAM::Baseline,
    a::gfx::Baseline,
    a::Sound::Mono,
    a::TV::NTSC,
    a::Network::Fujinet>;

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static_assert(sizeof(NonePlatform::hal) == 1,
              "None HAL seam should have no instance storage");
static_assert(sizeof(FujiPlatform::hal) == 1,
              "Fujinet HAL seam should have no instance storage");

static void test_none_seam_surface() {
    CHECK(NonePlatform::hal::realtime_open_udp_seq("host", 1234, 0) ==
          n::NetStatus::Unsupported);
    CHECK(NonePlatform::hal::realtime_poll() == n::NetStatus::WouldBlock);
    CHECK(NonePlatform::hal::session_connect_tcp("host", 5000) ==
          n::NetStatus::Unsupported);
    CHECK(NonePlatform::hal::session_poll() == n::NetStatus::WouldBlock);
}

static void test_fujinet_seam_surface() {
    char io = 0;

#if !defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
    // OFF-mode realtime seam stubs. In ON mode the seam delegates to the real
    // NetstreamRealtimeAdapter (RealNetstreamOps -> SIOV/begin), which cannot run here and
    // must not be ODR-used in this add_engine_test (no handler.S/abi.s linked). ON-mode
    // adapter lifecycle is covered by test_netstream_adapter_lifecycle (FakeOps) + the
    // Altirra adapter probe.
    CHECK(FujiPlatform::hal::realtime_open_udp_seq("host", 1234, 0) ==
          n::NetStatus::Ok);
        CHECK(!FujiPlatform::hal::realtime_active());
    CHECK(FujiPlatform::hal::realtime_open_udp_seq("", 1234, 0) ==
          n::NetStatus::InvalidArgument);

    CHECK(FujiPlatform::hal::realtime_send_nb(nullptr, 1) ==
          n::NetStatus::InvalidArgument);
    CHECK(FujiPlatform::hal::realtime_send_nb(&io, 1) == n::NetStatus::Ok);
    CHECK(FujiPlatform::hal::realtime_recv_nb(&io, 1) == n::NetStatus::WouldBlock);
    CHECK(FujiPlatform::hal::realtime_last_error().status == n::NetStatus::WouldBlock);
#endif

#if !defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
    // OFF-mode session seam stubs. Independent of the netstream realtime wiring
    // (both realtime modes). In ON mode the seam delegates to the real
    // fujinet-lib (network_init/network_open -> SIO), which cannot run here; the
    // ON-mode session lane is covered by test_atari_fujinet_session_send_nb's
    // pre-connection guards and validated on hardware by fujinet_session_validate.
    CHECK(FujiPlatform::hal::session_connect_tcp("host", 5000) ==
          n::NetStatus::Ok);
        CHECK(!FujiPlatform::hal::session_connected());
    CHECK(FujiPlatform::hal::session_connect_tcp(nullptr, 5000) ==
          n::NetStatus::InvalidArgument);

    CHECK(FujiPlatform::hal::session_recv_nb(nullptr, 1) ==
          n::NetStatus::InvalidArgument);
    CHECK(FujiPlatform::hal::session_send_nb(&io, 1) == n::NetStatus::Ok);
    CHECK(FujiPlatform::hal::session_recv_nb(&io, 1) == n::NetStatus::WouldBlock);

        CHECK(FujiPlatform::hal::session_last_error().status == n::NetStatus::WouldBlock);
#else
    // ON mode: nullptr host is rejected before any hardware access, so this one
    // guard is still safe to exercise under the simulator.
    CHECK(FujiPlatform::hal::session_connect_tcp(nullptr, 5000) ==
          n::NetStatus::InvalidArgument);
    CHECK(FujiPlatform::hal::session_recv_nb(nullptr, 1) ==
          n::NetStatus::InvalidArgument);
    (void)io;
#endif
}

int main() {
    test_none_seam_surface();
    test_fujinet_seam_surface();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
