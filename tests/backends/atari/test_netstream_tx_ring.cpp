// tests/backends/atari/test_netstream_tx_ring.cpp
// Stage 9I: TX ring behavioral self-test, executed under mos-sim.
//
// Exercises _ns_send_byte (TX producer) via the EDGE ABI shim (_edge_ns_send_byte:
// 0 = enqueued, 1 = full) and drains via a PRIVATE, test-only consumer hook
// (ns_test_tx_drain) that mimics the future output-IRQ consumer minus the SEROUT
// write. The handler's _ns_send_byte does NOT touch POKEY/SEROUT in this stage --
// it enqueues into RAM only. Test hooks are gated by EDGE_NETSTREAM_TEST_HOOKS and
// are not part of the edge_ns_* ABI or Game::net.
//
// Coverage: initial empty, single enqueue+drain, FIFO order, capacity (128),
// 129th send full, wraparound, drain-to-empty.

#include <stdint.h>
#include <stdio.h>

extern "C" {
    // Production TX path via the ABI shim: 0 = enqueued, 1 = full/not sent.
    uint8_t _edge_ns_send_byte(uint8_t byte);

    // Private test-only TX hooks (handler, EDGE_NETSTREAM_TEST_HOOKS).
    void     ns_test_init_tx(void);
    // ns_test_tx_drain: low byte = data, high byte = status (0=ok, 1=empty).
    uint16_t ns_test_tx_drain(void);
}

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static inline uint8_t drain_status(uint16_t packed) { return (uint8_t)(packed >> 8); }
static inline uint8_t drain_data(uint16_t packed)   { return (uint8_t)(packed & 0xFF); }

// A distinct, order-sensitive value for index i.
static inline uint8_t pat(unsigned i) { return (uint8_t)(i * 7u + 3u); }

int main() {
    const unsigned CAP = 128;  // capacity, hardcoded as cmp #0x80

    // ---- initial TX empty ----
    ns_test_init_tx();
    CHECK(drain_status(ns_test_tx_drain()) == 1);  // empty

    // ---- single enqueue / drain ----
    CHECK(_edge_ns_send_byte(0xA5) == 0);          // enqueued
    {
        uint16_t d = ns_test_tx_drain();
        CHECK(drain_status(d) == 0);
        CHECK(drain_data(d) == 0xA5);
    }
    CHECK(drain_status(ns_test_tx_drain()) == 1);  // empty again

    // ---- FIFO order over a batch ----
    for (unsigned i = 0; i < 32; ++i) CHECK(_edge_ns_send_byte(pat(i)) == 0);
    for (unsigned i = 0; i < 32; ++i) {
        uint16_t d = ns_test_tx_drain();
        CHECK(drain_status(d) == 0);
        CHECK(drain_data(d) == pat(i));
    }
    CHECK(drain_status(ns_test_tx_drain()) == 1);

    // ---- wraparound across the 0x7f index boundary ----
    ns_test_init_tx();
    for (unsigned i = 0; i < 100; ++i) CHECK(_edge_ns_send_byte(pat(i)) == 0);
    for (unsigned i = 0; i < 100; ++i) {           // drain; indices now at 100
        uint16_t d = ns_test_tx_drain();
        CHECK(drain_status(d) == 0);
        CHECK(drain_data(d) == pat(i));
    }
    for (unsigned i = 0; i < 50; ++i)              // 100+50 = 150 > 128 -> wrap at 28
        CHECK(_edge_ns_send_byte(pat(1000 + i)) == 0);
    for (unsigned i = 0; i < 50; ++i) {
        uint16_t d = ns_test_tx_drain();
        CHECK(drain_status(d) == 0);
        CHECK(drain_data(d) == pat(1000 + i));     // order preserved across wrap
    }
    CHECK(drain_status(ns_test_tx_drain()) == 1);

    // ---- capacity / full ----
    ns_test_init_tx();
    for (unsigned i = 0; i < CAP; ++i) CHECK(_edge_ns_send_byte(pat(i)) == 0);
    CHECK(_edge_ns_send_byte(0xFF) == 1);          // 129th send: full/not sent
    // Drain fully, in order; the rejected byte must NOT appear.
    for (unsigned i = 0; i < CAP; ++i) {
        uint16_t d = ns_test_tx_drain();
        CHECK(drain_status(d) == 0);
        CHECK(drain_data(d) == pat(i));
    }
    CHECK(drain_status(ns_test_tx_drain()) == 1);  // empty

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream TX ring behavioral)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
