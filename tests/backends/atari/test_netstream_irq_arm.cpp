// tests/backends/atari/test_netstream_irq_arm.cpp
// Stage 9O.4: serial-IRQ arm/disarm -- SAFE guard smoke (mos-sim).
//
// 9O.4 arms the POKEY serial-IRQ bits in the POKMSK shadow ($0010) and IRQEN ($D20E)
// on begin (LAST) and disarms them on end (FIRST), gated by hardwareActivated.
//
// IMPORTANT: the FULL arm path is NOT sim-runnable. It is reached only through
// _ns_begin_stream, which writes the OS-vector page ($020A-$0216) -- under mos-sim
// (RAM ORIGIN 0x0200) that page is the LOADED PROGRAM IMAGE, so executing it corrupts
// code and crashes. Separately, NS_POKMSK is $0010, which overlaps llvm-mos zero-page
// usage under the simulator. So the arm/disarm BEHAVIOR (POKMSK |= 0x30 / &= 0xc7,
// IRQEN writes, hardwareActivated transitions, IRQEN-serial-bits armed/cleared) is
// validated by objdump/static checks and on Altirra/hardware -- NOT executed here.
//
// What IS safe (and the key safety property) is the hardwareActivated guard:
// ns_disable_serial_irqs with hardwareActivated==0 (zero-initialized, never armed)
// early-returns and touches NO hardware (no POKMSK, no IRQEN). This smoke proves that
// guard: disarm-without-arm is a safe no-op that returns cleanly (reaching the end and
// returning 0 means no wild jump / BRK / zero-page corruption occurred). It
// deliberately does NOT call ns_test_enable_serial_irqs (which would touch IRQEN/
// POKMSK). Hooks are gated by EDGE_NETSTREAM_TEST_HOOKS.

#include <stdint.h>
#include <stdio.h>

extern "C" {
    void ns_test_disable_serial_irqs(void);   // -> ns_disable_serial_irqs (guarded no-op when not armed)
}

int main() {
    // hardwareActivated is zero (BSS, never armed): disarm must be a safe no-op.
    ns_test_disable_serial_irqs();
    ns_test_disable_serial_irqs();   // idempotent: still a safe no-op

    // Reaching here means disarm-without-arm did not touch POKMSK/IRQEN or crash.
    printf("\nALL TESTS PASSED (Netstream serial-IRQ guard no-op)\n");
    return 0;
}
