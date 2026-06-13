// tests/backends/atari/test_netstream_detect_pal.cpp
// Stage 9M.4: DetectPALViaVCOUNT bounded-return smoke (mos-sim).
//
// DetectPALViaVCOUNT samples ANTIC VCOUNT to classify PAL vs NTSC. Its sampling
// loop is BOUNDED (nested 8-bit counters + early wrap exit), so it terminates even
// under mos-sim where VCOUNT is not modeled (reads a constant). This smoke asserts
// only safe properties: the routine returns, and nsVideoStd ends up 0 or 1. It does
// NOT assert which (that needs real hardware/Altirra). The routine is reached only
// through the PRIVATE test wrapper ns_test_detect_pal (gated by
// EDGE_NETSTREAM_TEST_HOOKS); it is never installed or called by begin/end.

#include <stdint.h>
#include <stdio.h>

extern "C" {
    void    ns_test_detect_pal(void);   // -> internal DetectPALViaVCOUNT
    uint8_t _ns_get_video_std(void);     // raw getter: returns nsVideoStd
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
    // Must return (bounded loop) -- reaching here proves termination.
    ns_test_detect_pal();
    uint8_t v = _ns_get_video_std();
    CHECK(v == 0 || v == 1);

    // Idempotent: a second call still returns and yields a valid 0/1.
    ns_test_detect_pal();
    v = _ns_get_video_std();
    CHECK(v == 0 || v == 1);

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream DetectPAL bounded smoke)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
