// tests/backends/atari/test_netstream_status.cpp
// Stage 9L: status/error model behavioral self-test (mos-sim).
//
// Validates the serialErrors[4] status model behind _ns_get_status (via the shim):
// begin clears status; a seeded status is returned once then cleared on read; end
// clears status. The status byte is seeded via the PRIVATE test-only hook
// ns_test_set_status (gated by EDGE_NETSTREAM_TEST_HOOKS) in place of the still-
// deferred input-IRQ overflow path. Not part of the edge_ns_* ABI or Game::net.

#include <stdint.h>
#include <stdio.h>

extern "C" {
    void    ns_test_begin_soft(void);
    void    ns_test_end_soft(void);
    uint8_t _edge_ns_get_status(void);          // read + clear serialErrors[0]

    // Private test-only hook: seed serialErrors[0].
    void    ns_test_set_status(uint8_t value);
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
    // begin clears status.
    ns_test_set_status(0x99);
    ns_test_begin_soft();
    CHECK(_edge_ns_get_status() == 0);

    // Setting status makes get_status return that value...
    ns_test_set_status(0x10);
    CHECK(_edge_ns_get_status() == 0x10);
    // ...and clears on read: a second read returns 0.
    CHECK(_edge_ns_get_status() == 0);

    // Distinct value, re-confirm read+clear.
    ns_test_set_status(0xA5);
    CHECK(_edge_ns_get_status() == 0xA5);
    CHECK(_edge_ns_get_status() == 0);

    // end clears status.
    ns_test_set_status(0x7F);
    ns_test_end_soft();
    CHECK(_edge_ns_get_status() == 0);

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream status model)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
