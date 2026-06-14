// tests/backends/atari/test_netstream_tx_prime.cpp
// Stage 9P: SEROUT idle-prime behavioral self-test, executed under mos-sim.
//
// 9P adds an idle-prime to _ns_send_byte: on the idle->active edge (hardwareActivated
// && serialOutIdle bit7 set) it writes the FIRST queued byte directly to POKEY SEROUT
// to kick the output-ready IRQ chain, consumes that byte, and clears serialOutIdle.
//
// $D20D is a SPLIT POKEY register -- a CPU read returns SERIN, NOT the SEROUT latch --
// so this test never reads $D20D. Instead it observes the prime through a test-only
// RAM mirror written right next to the sta SEROUT: ns_test_last_prime_byte holds the
// primed byte and ns_test_prime_count counts primes. Both, plus the TX/gate state
// (serialOutIdle/outLevel/outIndex/hardwareActivated), are exposed under
// EDGE_NETSTREAM_TEST_HOOKS.
//
// ns_test_set_hw_activated arms/disarms the prime gate WITHOUT touching POKMSK/IRQEN
// (ns_enable_serial_irqs is unsafe under mos-sim). ns_test_begin_soft re-idles the
// ring (serialOutIdle=0xff, ring empty) while deliberately leaving hardwareActivated
// intact, which is how the test re-idles between phases.

#include <stdint.h>
#include <stdio.h>

extern "C" {
    // Production TX path via the ABI shim: 0 = enqueued, 1 = full/not sent.
    uint8_t _edge_ns_send_byte(uint8_t byte);
    void    ns_test_begin_soft(void);          // hardware-free begin body (ring setup/re-idle)
    void    ns_test_set_hw_activated(uint8_t);  // RAM-only arm/disarm of the prime gate

    // Private TX consumer hook: mimics the output-IRQ consumer minus the SEROUT write.
    // Low byte = data, high byte = status (0=ok, 1=empty). Does NOT re-idle the ring.
    uint16_t ns_test_tx_drain(void);

    // 9P RAM mirror + exposed TX/gate state (observe the prime via RAM, never $D20D).
    extern volatile uint8_t ns_test_last_prime_byte;
    extern volatile uint8_t ns_test_prime_count;
    extern volatile uint8_t serialOutIdle;
    extern volatile uint8_t outLevel;
    extern volatile uint8_t outIndex;
    extern volatile uint8_t hardwareActivated;
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

int main() {
    // ---- Phase A: gate off (not armed) -> NO prime ----
    ns_test_begin_soft();                  // ring empty, serialOutIdle=0xff
    ns_test_set_hw_activated(0);           // disarmed
    CHECK(ns_test_prime_count == 0);
    CHECK(_edge_ns_send_byte(0x5A) == 0);  // enqueued (buffered only)
    CHECK(ns_test_prime_count == 0);       // not armed -> no prime
    CHECK(serialOutIdle == 0xFF);          // transmitter stays idle
    CHECK(outLevel == 1);                  // byte is buffered, not consumed

    // ---- reset between phases so Phase B's FIFO is unambiguous ----
    ns_test_begin_soft();                  // clears the ring, re-idles (keeps hw gate)

    // ---- Phase B: idle -> active prime ----
    ns_test_set_hw_activated(1);           // armed
    CHECK(_edge_ns_send_byte(0xA5) == 0);  // enqueued, then primed
    CHECK(ns_test_prime_count == 1);       // exactly one prime
    CHECK(ns_test_last_prime_byte == 0xA5);// the primed byte reached SEROUT
    CHECK(serialOutIdle == 0x00);          // transmitter now active
    CHECK(outLevel == 0);                  // primed byte consumed -> ring empty

    // ---- Phase C: already active -> buffer only, no re-prime, FIFO preserved ----
    CHECK(_edge_ns_send_byte(0x11) == 0);
    CHECK(_edge_ns_send_byte(0x22) == 0);
    CHECK(_edge_ns_send_byte(0x33) == 0);
    CHECK(ns_test_prime_count == 1);       // no second prime while active
    CHECK(outLevel == 3);                  // all three buffered
    {
        uint16_t d = ns_test_tx_drain();   CHECK(drain_status(d) == 0); CHECK(drain_data(d) == 0x11);
        d = ns_test_tx_drain();            CHECK(drain_status(d) == 0); CHECK(drain_data(d) == 0x22);
        d = ns_test_tx_drain();            CHECK(drain_status(d) == 0); CHECK(drain_data(d) == 0x33);
        CHECK(drain_status(ns_test_tx_drain()) == 1);  // empty
    }

    // ---- Phase D: re-idle (begin_soft keeps the hw gate) -> prime again ----
    ns_test_begin_soft();                  // serialOutIdle=0xff, ring empty, hw still armed
    CHECK(hardwareActivated == 1);         // begin_soft must NOT disarm the gate
    CHECK(_edge_ns_send_byte(0xBB) == 0);
    CHECK(ns_test_prime_count == 2);       // primed again
    CHECK(ns_test_last_prime_byte == 0xBB);
    CHECK(serialOutIdle == 0x00);

    // ---- Optional backlog coverage: oldest byte primes first (FIFO) ----
    ns_test_begin_soft();
    ns_test_set_hw_activated(0);           // inactive: this byte just backlogs
    CHECK(_edge_ns_send_byte(0x71) == 0);  // queued while not armed
    CHECK(ns_test_prime_count == 2);       // unchanged (no prime)
    ns_test_set_hw_activated(1);           // arm: transmitter still idle, backlog present
    CHECK(_edge_ns_send_byte(0xA5) == 0);  // enqueues 0xA5, primes the OLDEST byte
    CHECK(ns_test_prime_count == 3);
    CHECK(ns_test_last_prime_byte == 0x71);// FIFO: 0x71 primed, not 0xA5
    CHECK(outLevel == 1);                  // 0xA5 remains queued
    {
        uint16_t d = ns_test_tx_drain();   CHECK(drain_status(d) == 0); CHECK(drain_data(d) == 0xA5);
        CHECK(drain_status(ns_test_tx_drain()) == 1);
    }

    // ---- Phase E: carry-return semantics preserved with the gate ARMED ----
    // The capacity check (cmp #0x80 -> .Lsend_full) is taken BEFORE any idle-prime
    // code, so the prime branch cannot corrupt the full-path carry. Prove both edges
    // through the safe shim: success returns 0 (incl. the priming send), full returns 1.
    ns_test_begin_soft();
    ns_test_set_hw_activated(1);
    {
        uint8_t primes_before = ns_test_prime_count;
        CHECK(_edge_ns_send_byte(0x01) == 0);          // armed+idle: primes & consumes one -> success
        CHECK(ns_test_prime_count == (uint8_t)(primes_before + 1));
        for (unsigned i = 0; i < 128; ++i)
            CHECK(_edge_ns_send_byte((uint8_t)i) == 0);// buffer to capacity (no re-prime) -> success
        CHECK(ns_test_prime_count == (uint8_t)(primes_before + 1));  // no further prime fired
        CHECK(_edge_ns_send_byte(0xFF) == 1);          // FULL -> failure carry, even with gate armed
    }

    ns_test_set_hw_activated(0);           // cleanup: leave the gate disarmed

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream TX idle-prime)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
