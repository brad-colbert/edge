// tests/backends/atari/test_netstream_init_prepare.cpp
// Stage 9Q.1: pure / mos-sim-testable Netstream init-PREPARE behavioral self-test.
//
// Exercises the sim-safe half of init (baud selection, PAL/TCP flag derivation, host
// validation, SIO-payload build) via the deterministic, PARAMETERLESS test hooks:
//   - ns_test_select_baud_staged()            -> ns_select_baud (no detect)
//   - ns_test_init_prepare_no_detect_staged() -> ns_prepare_core (no detect)
// Both convert the internal carry to a normal uint8_t status (0 = success, 1 = fail);
// the C++ side stages inputs by writing the exported .bss bytes / nsVideoStd directly
// and reads results from nsFinal* / nsPayloadBuf / nsPayloadLen. NO SIOV, no $D200, no
// POKMSK, no OS-vector writes, and no reliance on VCOUNT -- so it runs under mos-sim.
// DetectPALViaVCOUNT is skipped (the no-detect hooks), so PAL vs NTSC is deterministic.
//
// Gated by EDGE_ATARI_FUJINET_REALTIME_NETSTREAM + EDGE_NETSTREAM_TEST_HOOKS; built and
// run with the mos toolchain (mos-sim), same pattern as the other netstream sim tests.

#include <stdint.h>
#include <stdio.h>

extern "C" {
    // Staged init inputs / final outputs (handler .bss; .global under the test hooks).
    extern uint8_t nsNominalBaudLo, nsNominalBaudHi;
    extern uint8_t nsPortLo, nsPortHi;
    extern uint8_t nsHostPtrLo, nsHostPtrHi;
    extern uint8_t nsInitFlags;
    extern uint8_t nsPayloadLen;
    extern uint8_t nsPayloadBuf[64];
    extern uint8_t nsVideoStd;
    extern uint8_t nsFinalFlags, nsFinalAudf3, nsFinalAudf4;

    // Deterministic, parameterless hooks (carry -> uint8_t: 0 = success, 1 = failure).
    uint8_t ns_test_select_baud_staged(void);
    uint8_t ns_test_init_prepare_no_detect_staged(void);
}

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

enum { VIDEO_NTSC = 0, VIDEO_PAL = 1 };

static void stage_host(const char* s) {
    uintptr_t p = (uintptr_t)s;
    nsHostPtrLo = (uint8_t)(p & 0xff);
    nsHostPtrHi = (uint8_t)((p >> 8) & 0xff);
}

// Stage a 16-bit nominal baud into the select_baud key bytes.
static void stage_nominal(uint16_t baud) {
    nsNominalBaudLo = (uint8_t)(baud & 0xff);
    nsNominalBaudHi = (uint8_t)((baud >> 8) & 0xff);
}

int main() {
    // ----- baud lookup hits (select_baud hook; deterministic video std) -----
    // 9600 NTSC -> AUDF3/AUDF4 = 86,0
    nsVideoStd = VIDEO_NTSC;
    stage_nominal(9600);
    nsFinalAudf3 = 0xAA; nsFinalAudf4 = 0xAA;
    CHECK(ns_test_select_baud_staged() == 0);
    CHECK(nsFinalAudf3 == 86);
    CHECK(nsFinalAudf4 == 0);

    // 9600 PAL -> 85,0
    nsVideoStd = VIDEO_PAL;
    stage_nominal(9600);
    CHECK(ns_test_select_baud_staged() == 0);
    CHECK(nsFinalAudf3 == 85);
    CHECK(nsFinalAudf4 == 0);

    // Second row (1200) proves the NTSC/PAL pair selection for a multi-byte divisor.
    nsVideoStd = VIDEO_NTSC;
    stage_nominal(1200);
    CHECK(ns_test_select_baud_staged() == 0);
    CHECK(nsFinalAudf3 == 226);
    CHECK(nsFinalAudf4 == 2);

    nsVideoStd = VIDEO_PAL;
    stage_nominal(1200);
    CHECK(ns_test_select_baud_staged() == 0);
    CHECK(nsFinalAudf3 == 219);
    CHECK(nsFinalAudf4 == 2);

    // 31250 is the rate the REALTIME lane actually requests
    // (kNetstreamNominalBaud, fujinet_netstream_realtime.h): verify it is a real
    // BaudTable row and resolves to AUDF3=21,AUDF4=0 (single-byte divisor, same for
    // NTSC/PAL) -> effective ~31960 bps via baud = 1789790/(2*(AUDF3+7)). A retune of
    // the adapter constant to a value NOT in the table would MISS here (init fails),
    // so this case guards the lane's configured datarate.
    nsVideoStd = VIDEO_NTSC;
    stage_nominal(31250);
    nsFinalAudf3 = 0xAA; nsFinalAudf4 = 0xAA;
    CHECK(ns_test_select_baud_staged() == 0);
    CHECK(nsFinalAudf3 == 21);
    CHECK(nsFinalAudf4 == 0);

    nsVideoStd = VIDEO_PAL;
    stage_nominal(31250);
    CHECK(ns_test_select_baud_staged() == 0);
    CHECK(nsFinalAudf3 == 21);
    CHECK(nsFinalAudf4 == 0);

    // 49700 is the closest BaudTable row to 50k (AUDF3=11 -> ~49716 bps), the
    // EDGE_NETSTREAM_NOMINAL_BAUD=49700 build override. Guards that overriding the lane
    // baud to this row still hits select_baud (not a miss -> init fail).
    nsVideoStd = VIDEO_NTSC;
    stage_nominal(49700);
    CHECK(ns_test_select_baud_staged() == 0);
    CHECK(nsFinalAudf3 == 11);
    CHECK(nsFinalAudf4 == 0);

    // ----- baud lookup miss: failure + nsFinal* untouched -----
    nsVideoStd = VIDEO_NTSC;
    stage_nominal(0x1234);           // not present in BaudTable
    nsFinalAudf3 = 0x5A; nsFinalAudf4 = 0xA5;   // sentinels
    CHECK(ns_test_select_baud_staged() == 1);
    CHECK(nsFinalAudf3 == 0x5A);     // untouched on miss
    CHECK(nsFinalAudf4 == 0xA5);

    // ----- PAL flag set / NTSC flag clear (prepare, no detect) -----
    static const char host_h[] = "h";
    stage_host(host_h);
    stage_nominal(9600);

    nsVideoStd = VIDEO_PAL;
    nsInitFlags = 0x00;
    CHECK(ns_test_init_prepare_no_detect_staged() == 0);
    CHECK((nsFinalFlags & 0x10) != 0);          // PAL bit set

    nsVideoStd = VIDEO_NTSC;
    nsInitFlags = 0x00;
    CHECK(ns_test_init_prepare_no_detect_staged() == 0);
    CHECK((nsFinalFlags & 0x10) == 0);          // PAL bit clear

    // ----- TCP clears UDP-seq (0x20); UDP preserves it -----
    nsVideoStd = VIDEO_NTSC;
    nsInitFlags = 0x01 | 0x20;       // TCP + (would-be) UDP-seq
    CHECK(ns_test_init_prepare_no_detect_staged() == 0);
    CHECK((nsFinalFlags & 0x01) != 0);          // TCP retained
    CHECK((nsFinalFlags & 0x20) == 0);          // UDP-seq cleared for TCP

    nsInitFlags = 0x20;              // UDP (bit0 clear) + UDP-seq
    CHECK(ns_test_init_prepare_no_detect_staged() == 0);
    CHECK((nsFinalFlags & 0x20) != 0);          // UDP-seq preserved for UDP

    // ----- divisor/flag consistency through the no-detect path -----
    // The EDGE reorder keeps the selected divisor row and the 0x10 flag agreeing for the
    // same staged std (the upstream quirk could let them disagree).
    nsVideoStd = VIDEO_PAL;
    nsInitFlags = 0x00;
    stage_nominal(9600);
    CHECK(ns_test_init_prepare_no_detect_staged() == 0);
    CHECK(nsFinalAudf3 == 85);                   // PAL divisor row
    CHECK((nsFinalFlags & 0x10) != 0);          // and PAL flag -- coherent

    nsVideoStd = VIDEO_NTSC;
    CHECK(ns_test_init_prepare_no_detect_staged() == 0);
    CHECK(nsFinalAudf3 == 86);                   // NTSC divisor row
    CHECK((nsFinalFlags & 0x10) == 0);          // and NTSC flag -- coherent

    // ----- normal host payload build -----
    static const char host_edge[] = "edge";
    stage_host(host_edge);
    stage_nominal(9600);
    nsVideoStd = VIDEO_NTSC;
    nsInitFlags = 0x00;
    CHECK(ns_test_init_prepare_no_detect_staged() == 0);
    CHECK(nsPayloadLen == 64);
    CHECK(nsPayloadBuf[0] == 'e');
    CHECK(nsPayloadBuf[1] == 'd');
    CHECK(nsPayloadBuf[2] == 'g');
    CHECK(nsPayloadBuf[3] == 'e');
    CHECK(nsPayloadBuf[4] == 0);                 // host NUL terminator
    CHECK(nsPayloadBuf[5] == nsFinalFlags);      // flags follow host\0
    CHECK(nsPayloadBuf[6] == nsFinalAudf3);      // then AUDF3 (== 86)
    CHECK(nsPayloadBuf[6] == 86);
    // zero padding after host/flags/audf3
    {
        int padded = 1;
        for (int i = 7; i < 64; ++i) if (nsPayloadBuf[i] != 0) padded = 0;
        CHECK(padded);
    }

    // ----- long host cap at 61 with forced NUL at index 61 -----
    static char host_long[80];
    for (int i = 0; i < 70; ++i) host_long[i] = 'A';
    host_long[70] = 0;
    stage_host(host_long);
    stage_nominal(9600);
    nsVideoStd = VIDEO_NTSC;
    nsInitFlags = 0x00;
    CHECK(ns_test_init_prepare_no_detect_staged() == 0);
    CHECK(nsPayloadLen == 64);
    {
        int all_a = 1;
        for (int i = 0; i < 61; ++i) if (nsPayloadBuf[i] != 'A') all_a = 0;
        CHECK(all_a);                            // 61 host chars (indices 0..60)
    }
    CHECK(nsPayloadBuf[61] == 0);                // forced NUL at the cap

    // ----- null host fails closed (nsFinal* untouched) -----
    nsHostPtrLo = 0; nsHostPtrHi = 0;
    stage_nominal(9600);
    nsVideoStd = VIDEO_NTSC;
    nsFinalAudf3 = 0x33; nsFinalAudf4 = 0x44;    // sentinels
    CHECK(ns_test_init_prepare_no_detect_staged() == 1);
    CHECK(nsFinalAudf3 == 0x33);                 // validate_host runs first -> untouched
    CHECK(nsFinalAudf4 == 0x44);

    // ----- empty host fails closed -----
    static const char host_empty[] = "";
    stage_host(host_empty);
    stage_nominal(9600);
    nsVideoStd = VIDEO_NTSC;
    CHECK(ns_test_init_prepare_no_detect_staged() == 1);

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream init-prepare 9Q.1)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
