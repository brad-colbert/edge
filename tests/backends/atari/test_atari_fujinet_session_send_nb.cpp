// test_atari_fujinet_session_send_nb.cpp — session_send_nb tests for Stage 8F.
//
// OFF mode: verifies stub returns Unsupported for nonzero sends.
// ON mode: verifies pre-connection guards; connected path compiles against
//          network_write (no hardware needed for the guard tests).

#include <stdio.h>

#include <engine/platform/atari/fujinet_session_fujinetlib.h>

using engine::u16;
using engine::net::NetStatus;

namespace fs = atari::fujinet_session;

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)

// Test zero-length send always succeeds (no-op).
static void test_send_nb_zero_length_ok() {
    char buf[10] = "test";
    NetStatus st = fs::FujinetLibSessionAdapter::session_send_nb(buf, 0);
    CHECK(st == NetStatus::Ok);
}

// Test nonzero send with null buffer returns InvalidArgument.
static void test_send_nb_null_nonzero_invalid() {
    NetStatus st = fs::FujinetLibSessionAdapter::session_send_nb(nullptr, 10);
    CHECK(st == NetStatus::InvalidArgument);
}

// Test nonzero send when not connected returns Closed.
static void test_send_nb_not_connected_closed() {
    char buf[10] = "test";
    NetStatus st = fs::FujinetLibSessionAdapter::session_send_nb(buf, 4);
    CHECK(st == NetStatus::Closed);
}

// Test nonzero send with valid buffer when not connected still returns Closed.
static void test_send_nb_valid_not_connected_closed() {
    char buf[10] = "hello";
    NetStatus st = fs::FujinetLibSessionAdapter::session_send_nb(buf, 5);
    CHECK(st == NetStatus::Closed);
}

// Test nonzero send when connected calls network_write (Stage 8F wired).
// Without real hardware, we cannot reach the connected state at runtime here.
// This function exists to validate compile-time reachability: the call site
// references network_write and the compiler must resolve the symbol.
// Runtime behavior with hardware is tested via the mos-toolchain link smoke.
static void test_send_nb_when_connected_network_write_compile_path() {
    // network_write is declared inside the atari::fujinet_session namespace
    // (included via the adapter header's extern "C" block inside that namespace).
    // Verify the symbol resolves through the expected qualified path.
    using atari::fujinet_session::network_write;
    uint8_t (*p_write)(const char*, const uint8_t*, uint16_t) = &network_write;
    (void)p_write;  // compile-only check; not reachable without hardware
}

#else

// OFF-mode tests: verify OFF-mode behavior is unchanged.

static void test_send_nb_off_mode_unsupported() {
    char buf[10] = "test";
    NetStatus st = fs::FujinetLibSessionAdapter::session_send_nb(buf, 10);
    CHECK(st == NetStatus::Unsupported);
}

#endif

int main() {
    printf("Testing FujinetLibSessionAdapter::session_send_nb...\n");

#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
    test_send_nb_zero_length_ok();
    test_send_nb_null_nonzero_invalid();
    test_send_nb_not_connected_closed();
    test_send_nb_valid_not_connected_closed();
    test_send_nb_when_connected_network_write_compile_path();
    printf("ON-mode tests completed.\n");
#else
    test_send_nb_off_mode_unsupported();
    printf("OFF-mode tests completed.\n");
#endif

    if (g_failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        printf("%u tests failed.\n", g_failures);
        return 1;
    }
}
