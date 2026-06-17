// tests/backends/atari/test_netstream_lifecycle.cpp
// Netstream HARDWARE-FREE lifecycle state self-test, executed under mos-sim.
//
// As of Stage 9O.1 this validates the hardware-free lifecycle STATE PATH via the
// internal soft bodies ns_test_begin_soft / ns_test_end_soft (the part begin/end
// perform in RAM): begin-soft initializes the RX/TX rings + status to empty-ready
// and re-initializes a dirty ring; end-soft leaves a safe initialized-empty state.
// It does NOT exercise full _ns_begin_stream/_ns_end_stream, which from 9O.2 on also
// install OS vectors / program POKEY -- that hardware path writes the OS-vector page
// (the sim program image) and is validated on Altirra/hardware, not mos-sim. RX/TX
// are driven via the private test-only IRQ-side hooks (ns_test_rx_push /
// ns_test_tx_drain, gated by EDGE_NETSTREAM_TEST_HOOKS). No hardware registers touched.

#include <stdint.h>
#include <stdio.h>

extern "C" {
    void     ns_test_begin_soft(void);
    void     ns_test_end_soft(void);
    uint16_t _edge_ns_bytes_avail(void);
    uint16_t _edge_ns_recv_byte_packed(void);  // low=data, high=status (0=ok,1=empty)
    uint8_t  _edge_ns_send_byte(uint8_t byte);  // 0=enqueued, 1=full
    uint8_t  _edge_ns_get_status(void);

    // Private test-only IRQ-side hooks.
    uint8_t  ns_test_rx_push(uint8_t byte);     // 0=pushed, 1=full
    uint16_t ns_test_tx_drain(void);            // low=data, high=status (0=ok,1=empty)
}

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static inline uint8_t hi(uint16_t p) { return (uint8_t)(p >> 8); }

// Assert RX and TX both read empty.
static void expect_empty() {
    CHECK(_edge_ns_bytes_avail() == 0);
    CHECK(hi(_edge_ns_recv_byte_packed()) == 1);  // RX empty
    CHECK(hi(ns_test_tx_drain()) == 1);           // TX empty
}

int main() {
    // ---- begin initializes to empty-ready (RX cap 128, both rings empty) ----
    ns_test_begin_soft();
    expect_empty();
    CHECK(_edge_ns_get_status() == 0);            // status cleared

    // RX accepts up to capacity 128 after begin (proves capacity initialized).
    for (unsigned i = 0; i < 128; ++i) CHECK(ns_test_rx_push((uint8_t)i) == 0);
    CHECK(_edge_ns_bytes_avail() == 128);
    CHECK(ns_test_rx_push(0xFF) == 1);            // full

    // ---- begin re-initializes a dirty ring ----
    // Dirty both rings, then begin() must reset everything to empty-ready.
    for (unsigned i = 0; i < 10; ++i) (void)_edge_ns_send_byte((uint8_t)i);  // dirty TX
    // RX already full from above.
    ns_test_begin_soft();
    expect_empty();
    CHECK(_edge_ns_get_status() == 0);
    // Rings are usable again after re-init.
    CHECK(ns_test_rx_push(0x5A) == 0);
    CHECK(_edge_ns_bytes_avail() == 1);
    CHECK(_edge_ns_send_byte(0x3C) == 0);

    // ---- end leaves a safe initialized-empty state ----
    // (RX has 1 byte buffered, TX has 1 byte queued from above.)
    ns_test_end_soft();
    expect_empty();
    CHECK(_edge_ns_get_status() == 0);

    // After end the rings are still usable as an empty ring (byte API not gated).
    CHECK(ns_test_rx_push(0x11) == 0);
    CHECK(hi(_edge_ns_recv_byte_packed()) == 0);  // returns the pushed byte
    CHECK(_edge_ns_send_byte(0x22) == 0);
    CHECK(hi(ns_test_tx_drain()) == 0);

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream lifecycle begin/end)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
