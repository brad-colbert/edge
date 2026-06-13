// tests/backends/atari/test_netstream_vectors.cpp
// Stage 9N: serial vector install/restore -- SAFE guard smoke (mos-sim).
//
// IMPORTANT: the real vector swap writes the OS vector addresses VSERIN/VSEROR/
// VSEROC ($020A-$020F) and VIMIRQ ($0216). Under mos-sim the RAM region starts at
// ORIGIN 0x0200, so those addresses are the LOADED PROGRAM IMAGE (code), not free
// OS RAM. Executing ns_install_vectors therefore corrupts the program and crashes
// the sim. So install/swap BEHAVIOR is validated by objdump/static checks and on
// Altirra/hardware -- NOT executed here.
//
// What IS safe (and the key safety property) is the vectorInstalled guard:
// ns_restore_vectors with vectorInstalled==0 (zero-initialized, never installed)
// early-returns and touches NO memory. This smoke proves that guard: restore-
// without-install is a safe no-op that returns cleanly (reaching the end and
// returning 0 means no wild jump / BRK occurred). It deliberately does NOT call
// ns_test_install_vectors. Hooks are gated by EDGE_NETSTREAM_TEST_HOOKS.

#include <stdint.h>
#include <stdio.h>

extern "C" {
    void ns_test_restore_vectors(void);   // -> ns_restore_vectors (guarded no-op when not installed)
}

int main() {
    // vectorInstalled is zero (BSS, never installed): restore must be a safe no-op.
    ns_test_restore_vectors();
    ns_test_restore_vectors();   // idempotent: still a safe no-op

    // Reaching here means restore-without-install did not swap vectors or crash.
    printf("\nALL TESTS PASSED (Netstream vector guard no-op)\n");
    return 0;
}
