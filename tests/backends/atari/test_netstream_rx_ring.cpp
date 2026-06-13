// tests/backends/atari/test_netstream_rx_ring.cpp
// Stage 9H: RX ring behavioral self-test, executed under mos-sim.
//
// Setup uses the real hardware-free _ns_begin_stream (via the shim); RX bytes are
// injected through the PRIVATE, test-only producer hook ns_test_rx_push (gated by
// EDGE_NETSTREAM_TEST_HOOKS, mimics the deferred input IRQ). The
// production byte path (_ns_recv_byte / _ns_bytes_avail) is exercised via the
// EDGE ABI shim (_edge_ns_recv_byte_packed / _edge_ns_bytes_avail), so this also
// re-validates the carry->packed-status mapping.
//
// Coverage: empty, single, FIFO order, bytes_avail count, wrap across the 128
// boundary, drain-to-empty, capacity/full.

#include <stdint.h>
#include <stdio.h>

extern "C" {
    // Production byte path via the ABI shim.
    // _edge_ns_recv_byte_packed: low byte = data, high byte = status (0=ok,1=empty).
    uint16_t _edge_ns_recv_byte_packed(void);
    uint16_t _edge_ns_bytes_avail(void);
    void     _edge_ns_begin_stream(void);   // Stage 9J: real hardware-free init
    uint8_t  _edge_ns_get_status(void);      // Stage 9M.3: serialErrors[0] read+clear

    // Private test-only RX hook (handler, EDGE_NETSTREAM_TEST_HOOKS): mimics the
    // deferred input-IRQ producer.
    uint8_t ns_test_rx_push(uint8_t byte);  // 0 = pushed, 1 = full
}

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static inline uint8_t recv_status(uint16_t packed) { return (uint8_t)(packed >> 8); }
static inline uint8_t recv_data(uint16_t packed)   { return (uint8_t)(packed & 0xFF); }

// A distinct, order-sensitive value for index i.
static inline uint8_t pat(unsigned i) { return (uint8_t)(i * 7u + 3u); }

int main() {
    const unsigned CAP = 128;  // NS_INPUT_BUFSIZE

    // ---- empty after init ----
    _edge_ns_begin_stream();
    CHECK(_edge_ns_bytes_avail() == 0);
    {
        uint16_t r = _edge_ns_recv_byte_packed();
        CHECK(recv_status(r) == 1);  // empty
    }

    // ---- single push/recv ----
    CHECK(ns_test_rx_push(0xA5) == 0);
    CHECK(_edge_ns_bytes_avail() == 1);
    {
        uint16_t r = _edge_ns_recv_byte_packed();
        CHECK(recv_status(r) == 0);
        CHECK(recv_data(r) == 0xA5);
    }
    CHECK(_edge_ns_bytes_avail() == 0);
    CHECK(recv_status(_edge_ns_recv_byte_packed()) == 1);  // empty again

    // ---- FIFO order over a batch ----
    for (unsigned i = 0; i < 32; ++i) CHECK(ns_test_rx_push(pat(i)) == 0);
    CHECK(_edge_ns_bytes_avail() == 32);
    for (unsigned i = 0; i < 32; ++i) {
        uint16_t r = _edge_ns_recv_byte_packed();
        CHECK(recv_status(r) == 0);
        CHECK(recv_data(r) == pat(i));
    }
    CHECK(_edge_ns_bytes_avail() == 0);

    // ---- wrap across the 128 boundary ----
    // Advance both pointers near the end, then push past it so inPtr/inReadPtr wrap.
    _edge_ns_begin_stream();
    for (unsigned i = 0; i < 100; ++i) CHECK(ns_test_rx_push(pat(i)) == 0);
    for (unsigned i = 0; i < 100; ++i) {           // drain; pointers now at offset 100
        uint16_t r = _edge_ns_recv_byte_packed();
        CHECK(recv_status(r) == 0);
        CHECK(recv_data(r) == pat(i));
    }
    CHECK(_edge_ns_bytes_avail() == 0);
    for (unsigned i = 0; i < 50; ++i)              // 100+50 = 150 > 128 -> wrap at 28
        CHECK(ns_test_rx_push(pat(1000 + i)) == 0);
    CHECK(_edge_ns_bytes_avail() == 50);
    for (unsigned i = 0; i < 50; ++i) {
        uint16_t r = _edge_ns_recv_byte_packed();
        CHECK(recv_status(r) == 0);
        CHECK(recv_data(r) == pat(1000 + i));      // order preserved across wrap
    }
    CHECK(_edge_ns_bytes_avail() == 0);

    // ---- capacity / full + overflow status ----
    _edge_ns_begin_stream();
    CHECK(_edge_ns_get_status() == 0);             // begin cleared status
    for (unsigned i = 0; i < CAP; ++i) CHECK(ns_test_rx_push(pat(i)) == 0);
    CHECK(_edge_ns_bytes_avail() == CAP);
    CHECK(ns_test_rx_push(0xFF) == 1);             // 129th push: full -> overflow
    CHECK(_edge_ns_bytes_avail() == CAP);          // unchanged after a full push
    // Overflow set the status bit (0x10); get_status clears it on read.
    CHECK(_edge_ns_get_status() == 0x10);
    CHECK(_edge_ns_get_status() == 0);             // cleared on prior read
    // Drain returns exactly the 128 accepted bytes, in order (overflow byte absent).
    for (unsigned i = 0; i < CAP; ++i) {
        uint16_t r = _edge_ns_recv_byte_packed();
        CHECK(recv_status(r) == 0);
        CHECK(recv_data(r) == pat(i));
    }
    CHECK(_edge_ns_bytes_avail() == 0);
    CHECK(recv_status(_edge_ns_recv_byte_packed()) == 1);  // empty

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream RX ring behavioral)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
