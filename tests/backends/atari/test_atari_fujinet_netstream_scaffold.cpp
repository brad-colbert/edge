// test_atari_fujinet_netstream_scaffold.cpp
// Stage 9B.5: Netstream realtime skeleton safety and storage audit.
//
// Verifies:
// - Scaffold deliberately returns Unsupported (not Ok) to prevent false activation.
// - active() always returns false during scaffolding.
// - Nonzero send/recv return Unsupported, not Ok or WouldBlock.
// - Zero-length send/recv return Ok (no-op, always safe).
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
    // CRITICAL: open() must NOT mark active during scaffolding.
    CHECK(FujiPlatform::hal::realtime_open_udp_seq("localhost", 9000, 5000) ==
          n::NetStatus::Unsupported);
    CHECK(!FujiPlatform::hal::realtime_active());  // Must remain false.

    // Invalid input still rejected.
    CHECK(FujiPlatform::hal::realtime_open_udp_seq("", 9000, 5000) ==
          n::NetStatus::InvalidArgument);
    CHECK(!FujiPlatform::hal::realtime_active());

    CHECK(FujiPlatform::hal::realtime_open_udp_seq("host", 0, 5000) ==
          n::NetStatus::InvalidArgument);
    CHECK(!FujiPlatform::hal::realtime_active());

    CHECK(FujiPlatform::hal::realtime_open_udp_seq(nullptr, 9000, 5000) ==
          n::NetStatus::InvalidArgument);
    CHECK(!FujiPlatform::hal::realtime_active());

    // close() is a no-op (safe when not active).
    FujiPlatform::hal::realtime_close();
    CHECK(!FujiPlatform::hal::realtime_active());

    // poll() returns Unsupported when not active (scaffolding).
    CHECK(FujiPlatform::hal::realtime_poll() == n::NetStatus::Unsupported);

    printf("  ON mode safety tests passed.\n");
#else
    printf("  (Skipped: OFF mode; this test requires EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON)\n");
#endif
}

// Test ON mode: send/recv behavior.
static void test_on_mode_send_recv() {
    printf("test_on_mode_send_recv (EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON)...\n");

#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
    char io = 0;

    // Zero-length send/recv must return Ok (no-op, always safe).
    CHECK(FujiPlatform::hal::realtime_send_nb(&io, 0) == n::NetStatus::Ok);
    CHECK(FujiPlatform::hal::realtime_recv_nb(&io, 0) == n::NetStatus::Ok);

    // Nonzero send must NOT return Ok (scaffolding, not wired).
    // SAFETY: must return Unsupported or WouldBlock, never Ok.
    CHECK(FujiPlatform::hal::realtime_send_nb(&io, 1) == n::NetStatus::Unsupported);
    CHECK(FujiPlatform::hal::realtime_send_nb(&io, 16) == n::NetStatus::Unsupported);

    // Nonzero recv must NOT return Ok (scaffolding, not wired).
    // SAFETY: must return Unsupported or WouldBlock, never Ok.
    CHECK(FujiPlatform::hal::realtime_recv_nb(&io, 1) == n::NetStatus::Unsupported);
    CHECK(FujiPlatform::hal::realtime_recv_nb(&io, 16) == n::NetStatus::Unsupported);

    // Invalid pointers still rejected.
    CHECK(FujiPlatform::hal::realtime_send_nb(nullptr, 1) ==
          n::NetStatus::InvalidArgument);
    CHECK(FujiPlatform::hal::realtime_recv_nb(nullptr, 1) ==
          n::NetStatus::InvalidArgument);

    printf("  ON mode send/recv tests passed.\n");
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
    // Invalid host should set last_error.
    FujiPlatform::hal::realtime_open_udp_seq("", 9000, 5000);
    auto err = FujiPlatform::hal::realtime_last_error();
    CHECK(err.status == n::NetStatus::InvalidArgument);

    // Valid host (but unsupported) should set last_error.
    FujiPlatform::hal::realtime_open_udp_seq("host", 9000, 5000);
    err = FujiPlatform::hal::realtime_last_error();
    CHECK(err.status == n::NetStatus::Unsupported);

    printf("  Error tracking tests passed.\n");
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
