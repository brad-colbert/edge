// test_atari_fujinet_session_spec.cpp — devicespec helper tests for Stage 8C.

#include <stdio.h>

#include <engine/platform/atari/fujinet_session_fujinetlib.h>

using engine::u16;
using engine::net::NetStatus;

namespace fs = atari::fujinet_session;

#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
static_assert(fs::FujinetLibSessionAdapter::state_size_bytes() > 0,
              "ON mode must carry fujinet session adapter state");
#else
static_assert(fs::FujinetLibSessionAdapter::state_size_bytes() == 0,
              "OFF mode must add zero fujinet session adapter state");
#endif

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static bool str_equal(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static void test_valid_devicespec() {
    char dst[fs::kTcpDeviceSpecCapacity] = {};
    u16 len = 0;
    CHECK(fs::build_tcp_devicespec("example.local", 8080,
                                   dst, fs::kTcpDeviceSpecCapacity, len) == NetStatus::Ok);
    CHECK(str_equal(dst, "N:TCP://example.local:8080/"));
    CHECK(len > 0);
}

static void test_too_long_host_fails() {
    char host[100] = {};
    for (u16 i = 0; i < 99; ++i) host[i] = 'a';
    host[99] = '\0';

    char dst[fs::kTcpDeviceSpecCapacity] = {};
    u16 len = 0;
    CHECK(fs::build_tcp_devicespec(host, 8080,
                                   dst, fs::kTcpDeviceSpecCapacity, len) == NetStatus::InvalidArgument);
    CHECK(dst[0] == '\0');
    CHECK(len == 0);
}

static void test_null_host_fails() {
    char dst[fs::kTcpDeviceSpecCapacity] = {};
    u16 len = 0;
    CHECK(fs::build_tcp_devicespec(nullptr, 8080,
                                   dst, fs::kTcpDeviceSpecCapacity, len) == NetStatus::InvalidArgument);
}

static void test_empty_host_fails() {
    char dst[fs::kTcpDeviceSpecCapacity] = {};
    u16 len = 0;
    CHECK(fs::build_tcp_devicespec("", 8080,
                                   dst, fs::kTcpDeviceSpecCapacity, len) == NetStatus::InvalidArgument);
}

static void test_port_zero_fails() {
    char dst[fs::kTcpDeviceSpecCapacity] = {};
    u16 len = 0;
    CHECK(fs::build_tcp_devicespec("host", 0,
                                   dst, fs::kTcpDeviceSpecCapacity, len) == NetStatus::InvalidArgument);
}

static void test_host_mutation_does_not_change_copy() {
    char host[] = "abc.local";
    char dst[fs::kTcpDeviceSpecCapacity] = {};
    u16 len = 0;

    CHECK(fs::build_tcp_devicespec(host, 1234,
                                   dst, fs::kTcpDeviceSpecCapacity, len) == NetStatus::Ok);
    CHECK(str_equal(dst, "N:TCP://abc.local:1234/"));

    host[0] = 'X';
    CHECK(str_equal(dst, "N:TCP://abc.local:1234/"));
}

static void test_pack_fujinet_detail_packs_high_low_bytes() {
    const u16 d = fs::pack_fujinet_detail(0x12, 0x34, 0xABCD);
    CHECK(d == 0x1234);
}

static void test_pack_fujinet_detail_fallback_when_globals_zero() {
    const u16 d = fs::pack_fujinet_detail(0x00, 0x00, 0x00E7);
    CHECK(d == 0x00E7);
}

int main() {
    test_valid_devicespec();
    test_too_long_host_fails();
    test_null_host_fails();
    test_empty_host_fails();
    test_port_zero_fails();
    test_host_mutation_does_not_change_copy();
    test_pack_fujinet_detail_packs_high_low_bytes();
    test_pack_fujinet_detail_fallback_when_globals_zero();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
