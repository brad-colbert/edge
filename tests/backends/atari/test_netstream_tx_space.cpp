// tests/backends/atari/test_netstream_tx_space.cpp
// Stage 9R.2a: low-level TX free-space query (_ns_tx_space) behavioral self-test, mos-sim.
//
// Exercises _edge_ns_tx_space() (wrapper over _ns_tx_space; returns 128 - outLevel, range
// 0..128 as an UNSIGNED uint8_t) against the real TX producer _ns_send_byte via the ABI
// shim. To make outLevel move deterministically the idle-prime is disabled by holding
// hardwareActivated = 0 (so _ns_send_byte only enqueues; no SEROUT prime); a separate case
// pins the hardware-active idle-prime interaction. Test hooks are gated by
// EDGE_NETSTREAM_TEST_HOOKS and are not part of the edge_ns_* ABI or Game::net.

#include <stdint.h>
#include <stdio.h>

extern "C" {
    uint8_t _edge_ns_tx_space(void);    // 9R.2: TX free bytes (0..128, unsigned)
    uint8_t _edge_ns_send_byte(uint8_t byte);  // 0 = enqueued, 1 = full
    void    ns_test_begin_soft(void);          // hardware-free begin body (resets TX, outLevel=0)
    void    ns_test_set_hw_activated(uint8_t); // arm/disarm the 9P idle-prime gate (RAM only)
    uint16_t ns_test_tx_drain(void);           // mimic the output-IRQ consumer (no SEROUT)
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
    const unsigned CAP = 128;  // TX capacity (cmp #0x80 in _ns_send_byte)

    // ---- after soft begin: ring empty -> full space ----
    ns_test_begin_soft();
    ns_test_set_hw_activated(0);   // disable idle-prime: pure enqueue, outLevel deterministic
    CHECK(_edge_ns_tx_space() == 128);   // unsigned 128, not a signed -128

    // ---- monotone decrease per enqueue (no prime) ----
    for (unsigned k = 1; k <= 10; ++k) {
        CHECK(_edge_ns_send_byte((uint8_t)k) == 0);   // enqueued
        CHECK(_edge_ns_tx_space() == (uint8_t)(128 - k));
    }

    // ---- fill to capacity: free == 0, 129th send reports full, free stays 0 ----
    ns_test_begin_soft();
    ns_test_set_hw_activated(0);
    for (unsigned k = 0; k < CAP; ++k) {
        CHECK(_edge_ns_send_byte((uint8_t)k) == 0);   // all 128 accepted
    }
    CHECK(_edge_ns_tx_space() == 0);
    CHECK(_edge_ns_send_byte(0xFF) == 1);             // 129th -> full
    CHECK(_edge_ns_tx_space() == 0);                  // unchanged on a rejected send

    // ---- draining frees space back up (consumer increments free) ----
    CHECK(ns_test_tx_drain() != 0 || true);  // drain one (value/status ignored here)
    CHECK(_edge_ns_tx_space() == 1);
    for (unsigned k = 0; k < 5; ++k) (void)ns_test_tx_drain();
    CHECK(_edge_ns_tx_space() == 6);

    // ---- hardware-active idle-prime: first send is primed straight out, so it leaves
    //      outLevel unchanged net -> free reflects BUFFERED bytes only (pinned behavior) ----
    ns_test_begin_soft();              // serialOutIdle = idle (0xff)
    ns_test_set_hw_activated(1);       // arm the prime gate
    CHECK(_edge_ns_tx_space() == 128);
    CHECK(_edge_ns_send_byte(0xAB) == 0);   // enqueue then prime-consume the same byte
    CHECK(_edge_ns_tx_space() == 128);      // primed out -> nothing left buffered
    ns_test_set_hw_activated(0);

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream TX free-space 9R.2a)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
