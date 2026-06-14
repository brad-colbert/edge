// tests/backends/atari/test_netstream_init_marshal.cpp
// Stage 9Q.2: marshal-only Netstream init wrapper self-test (mos-sim, no SIOV).
//
// Exercises edge_ns_init_marshal() -- the C half of the safe 4-arg init wrapper that
// writes the staged init inputs (host pointer lo/hi, flags, nominal baud lo/hi, port
// lo/hi) into the handler .bss. This test calls ONLY edge_ns_init_marshal, never
// edge_ns_init_netstream / _edge_ns_init_run / raw _ns_init_netstream, so it does NOT
// execute SIOV or touch any hardware -- it runs deterministically under mos-sim.
//
// Gated by EDGE_ATARI_FUJINET_REALTIME_NETSTREAM (+ EDGE_NETSTREAM_TEST_HOOKS for parity
// with the other netstream sim builds); run with mos-sim, same pattern as the other
// netstream behavioral tests.

#include <stdint.h>
#include <stdio.h>

#include <engine/platform/atari/fujinet_netstream_realtime_abi.h>

extern "C" {
    // Staged init inputs (handler .bss; exported in production by the handler).
    extern uint8_t nsHostPtrLo, nsHostPtrHi;
    extern uint8_t nsInitFlags;
    extern uint8_t nsNominalBaudLo, nsNominalBaudHi;
    extern uint8_t nsPortLo, nsPortHi;
}

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

int main() {
    static const char host[] = "edge";
    uintptr_t hp = (uintptr_t)host;

    // ----- marshal splits all four args into the staging .bss correctly -----
    edge_ns_init_marshal(host, /*flags=*/0x21, /*nominal_baud=*/9600,
                         /*port_swapped=*/0xBEEF);
    CHECK(nsHostPtrLo == (uint8_t)(hp & 0xff));
    CHECK(nsHostPtrHi == (uint8_t)((hp >> 8) & 0xff));
    CHECK(nsInitFlags == 0x21);
    CHECK(nsNominalBaudLo == (uint8_t)(9600 & 0xff));   // 0x80
    CHECK(nsNominalBaudHi == (uint8_t)((9600 >> 8) & 0xff)); // 0x25
    CHECK(nsPortLo == 0xEF);                             // port lo  -> daux1
    CHECK(nsPortHi == 0xBE);                             // port hi  -> daux2

    // ----- a second marshal fully overwrites the previous staging -----
    static const char host2[] = "h";
    uintptr_t hp2 = (uintptr_t)host2;
    edge_ns_init_marshal(host2, /*flags=*/0x00, /*nominal_baud=*/1200,
                         /*port_swapped=*/0x0102);
    CHECK(nsHostPtrLo == (uint8_t)(hp2 & 0xff));
    CHECK(nsHostPtrHi == (uint8_t)((hp2 >> 8) & 0xff));
    CHECK(nsInitFlags == 0x00);
    CHECK(nsNominalBaudLo == (uint8_t)(1200 & 0xff));   // 0xB0
    CHECK(nsNominalBaudHi == (uint8_t)((1200 >> 8) & 0xff)); // 0x04
    CHECK(nsPortLo == 0x02);
    CHECK(nsPortHi == 0x01);

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream init-marshal 9Q.2)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
