// tests/backends/atari/test_netstream_swap_vector.cpp
// Stage 9M.1: SwapIrqVector involution self-test (mos-sim).
//
// SwapIrqVector exchanges the 2-byte VIMIRQ OS vector ($0216) with the handler's
// chain_addr SMC operand. It touches no hardware registers (VIMIRQ is RAM under
// mos-sim), so we can verify the install/restore symmetry directly: two swaps must
// return VIMIRQ to its original value. SwapIrqVector itself stays internal; we call
// it through the PRIVATE test-only wrapper ns_test_swap_irq_vector (gated by
// EDGE_NETSTREAM_TEST_HOOKS). IrqHandler is NOT called here (it touches IRQ hardware).

#include <stdint.h>
#include <stdio.h>

extern "C" {
    void ns_test_swap_irq_vector(void);   // -> internal SwapIrqVector
}

// VIMIRQ immediate-IRQ vector (OS DB $0216). Plain RAM under mos-sim.
static volatile uint8_t* const VIMIRQ = (volatile uint8_t*)0x0216;

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static inline uint16_t vimirq() { return (uint16_t)(VIMIRQ[0] | (VIMIRQ[1] << 8)); }

int main() {
    // Save the original VIMIRQ so the test leaves it untouched.
    const uint8_t o_lo = VIMIRQ[0];
    const uint8_t o_hi = VIMIRQ[1];

    // Seed a known vector value.
    VIMIRQ[0] = 0xCD;
    VIMIRQ[1] = 0xAB;                       // 0xABCD
    CHECK(vimirq() == 0xABCD);

    // One swap: VIMIRQ takes chain_addr's prior value (initial dummy 0xFFFF), so it
    // must change away from 0xABCD.
    ns_test_swap_irq_vector();
    CHECK(vimirq() != 0xABCD);

    // Second swap restores VIMIRQ (involution): install-then-restore symmetry.
    ns_test_swap_irq_vector();
    CHECK(vimirq() == 0xABCD);

    // Restore the original VIMIRQ.
    VIMIRQ[0] = o_lo;
    VIMIRQ[1] = o_hi;

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream SwapIrqVector involution)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
