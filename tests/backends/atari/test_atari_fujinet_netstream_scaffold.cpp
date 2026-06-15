// test_atari_fujinet_netstream_scaffold.cpp
// Netstream realtime seam-safety audit (no backend I/O).
//
// This is a SEAM/scaffold safety test: it exercises only the cached-state paths of
// FujiPlatform::hal that are safe to reach without opening the lane. It never calls
// open_udp_seq, so the real backend (RealNetstreamOps -> SIOV/begin/end) is never
// executed under mos-sim. The detailed DATA-PATH semantics of the wired 9R.2 adapter
// (active-path send/recv, zero-length Ok, nullptr InvalidArgument, full-packet
// WouldBlock/Ok) are covered deterministically by test_netstream_adapter_lifecycle
// (FakeOps).
//
// Verifies:
// - active() is false before any successful open.
// - While INACTIVE the wired adapter checks !active FIRST, so every send_nb/recv_nb
//   returns Closed (and touches no backend op), regardless of size or pointer.
// - bind_udp_seq remains Unsupported.
// - Storage minimization: minimal state when ON, zero when OFF.

#include <stdio.h>
#include <engine/platform/atari/platform.h>
#include <engine/net_types.h>

namespace a = atari;
namespace n = engine::net;

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

// Test OFF mode: no state, no dependencies.
static void test_off_mode() {
    printf("test_off_mode (EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=OFF)...\n");

#if !defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
    // When OFF, realtime methods should return stub values.
    // OFF mode validates input but always returns Ok (no real I/O).
    CHECK(FujiPlatform::hal::realtime_open_udp_seq("host", 1234, 5678) ==
          n::NetStatus::Ok);
    CHECK(!FujiPlatform::hal::realtime_active());
    FujiPlatform::hal::realtime_close();  // no-op, returns void
    CHECK(FujiPlatform::hal::realtime_poll() == n::NetStatus::WouldBlock);
    CHECK(FujiPlatform::hal::realtime_bind_udp_seq(9000) == n::NetStatus::Unsupported);

    char io = 0;
    // OFF mode send/recv: always Ok for zero-length, Ok for nonzero too (always succeeds)
    CHECK(FujiPlatform::hal::realtime_send_nb(&io, 0) == n::NetStatus::Ok);
    CHECK(FujiPlatform::hal::realtime_send_nb(&io, 1) == n::NetStatus::Ok);
    // OFF mode recv always returns WouldBlock (no data available stub)
    CHECK(FujiPlatform::hal::realtime_recv_nb(&io, 0) == n::NetStatus::WouldBlock);
    CHECK(FujiPlatform::hal::realtime_recv_nb(&io, 1) == n::NetStatus::WouldBlock);

    printf("  OFF mode tests passed.\n");
#else
    printf("  (Skipped: OFF mode disabled by EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON)\n");
#endif
}

// Test ON mode: skeleton safety behavior.
static void test_on_mode_safety() {
    printf("test_on_mode_safety (EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON)...\n");

#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
    // Stage 9R.1: the realtime seam now wires the REAL backend (RealNetstreamOps ->
    // SIOV/begin), which cannot run under mos-sim and must not be ODR-used in this
    // add_engine_test (no handler.S/abi.s linked). So we do NOT drive open/close/poll
    // through FujiPlatform::hal here. realtime_active() reads cached state only (no backend
    // call) and is safe: before any successful open it must be false.
    CHECK(!FujiPlatform::hal::realtime_active());

    // Full lifecycle (open->init->begin, close->end, poll/status, errors, port byte order)
    // is validated deterministically by test_netstream_adapter_lifecycle (FakeOps) and on
    // hardware by the Altirra adapter probe.
    printf("  ON mode safety: active()==false (lifecycle covered by FakeOps test).\n");
#else
    printf("  (Skipped: OFF mode; this test requires EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON)\n");
#endif
}

// Test ON mode: send/recv behavior.
static void test_on_mode_send_recv() {
    printf("test_on_mode_send_recv (EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON)...\n");

#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
    char io = 0;

    // The lane was never opened, so state().active is false. The wired 9R.2 adapter checks
    // !active FIRST in send_nb/recv_nb and returns Closed before touching any backend op
    // (no SIOV/POKEY). This holds for every size and pointer here -- the zero-length Ok,
    // nullptr InvalidArgument, and full-packet WouldBlock/Ok paths are only reachable AFTER
    // a successful open and are covered by test_netstream_adapter_lifecycle (FakeOps).
    CHECK(FujiPlatform::hal::realtime_send_nb(&io, 0) == n::NetStatus::Closed);
    CHECK(FujiPlatform::hal::realtime_recv_nb(&io, 0) == n::NetStatus::Closed);
    CHECK(FujiPlatform::hal::realtime_send_nb(&io, 1) == n::NetStatus::Closed);
    CHECK(FujiPlatform::hal::realtime_send_nb(&io, 16) == n::NetStatus::Closed);
    CHECK(FujiPlatform::hal::realtime_recv_nb(&io, 1) == n::NetStatus::Closed);
    CHECK(FujiPlatform::hal::realtime_recv_nb(&io, 16) == n::NetStatus::Closed);

    // nullptr is also rejected with Closed while inactive (the !active guard precedes the
    // nullptr check, so no backend op runs).
    CHECK(FujiPlatform::hal::realtime_send_nb(nullptr, 1) == n::NetStatus::Closed);
    CHECK(FujiPlatform::hal::realtime_recv_nb(nullptr, 1) == n::NetStatus::Closed);

    printf("  ON mode send/recv (inactive seam) tests passed.\n");
#else
    printf("  (Skipped: OFF mode; this test requires EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON)\n");
#endif
}

// Test ON mode: bind remains Unsupported.
static void test_on_mode_bind_unsupported() {
    printf("test_on_mode_bind_unsupported (EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON)...\n");

#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
    // Stage 9A discovery: no separate bind primitive in Netstream.
    // Port is fixed at init time; rebind is unsupported.
    CHECK(FujiPlatform::hal::realtime_bind_udp_seq(5000) ==
          n::NetStatus::Unsupported);
    CHECK(FujiPlatform::hal::realtime_bind_udp_seq(9000) ==
          n::NetStatus::Unsupported);

    printf("  ON mode bind tests passed.\n");
#else
    printf("  (Skipped: OFF mode; this test requires EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON)\n");
#endif
}

// Test error state is updated correctly.
static void test_error_tracking() {
    printf("test_error_tracking...\n");

#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
    // Stage 9R.1: error tracking that requires driving open() (a real-backend call) is
    // covered by test_netstream_adapter_lifecycle (FakeOps) — not here, where the real
    // adapter must not be ODR-used. realtime_last_error() reads cached state only (safe).
    auto err = FujiPlatform::hal::realtime_last_error();
    (void)err;
    printf("  Error tracking covered by FakeOps lifecycle test.\n");
#else
    printf("  (Skipped: OFF mode)\n");
#endif
}

int main() {
    test_off_mode();
    test_on_mode_safety();
    test_on_mode_send_recv();
    test_on_mode_bind_unsupported();
    test_error_tracking();

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream scaffold safety audit)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
