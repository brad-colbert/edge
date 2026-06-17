// tests/backends/atari/netstream_init_altirra_probe.cpp
// Stage 9Q.2 Altirra/hardware validation harness for the REAL init path -- NOT a CTest.
//
// The raw _ns_init_netstream path fills the Atari SIO DCB ($0300-$030B), calls SIOV
// ($E459), and checks DSTATS ($0303) -- it cannot run under mos-sim. This .xex drives
// the safe 4-arg wrapper edge_ns_init_netstream() on Altirra (or hardware) and snapshots
// the result into page 6 so the outcome can be read in the debugger.
//
// Build (Atari .xex, atari8-dos toolchain):
//   cmake -B <dir> -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON ...
//   cmake --build <dir> --target netstream_init_altirra_probe
//
// IMPORTANT validation split (the pre-SIOV DCB *fill* is NOT runtime-observable):
//   * The DCB FILL (DDEVIC=$70, DUNIT=1, DCOMND=$F0, DSTATS=$80 pre-call,
//     DBUFLO/HI=&nsPayloadBuf, DTIMLO=$0F, $0307=0, DBYTLO=nsPayloadLen, DBYTHI=0,
//     DAUX1/2=nsPortLo/Hi) is validated STATICALLY via objdump/disassembly of
//     _ns_init_netstream. After SIOV returns, $0303 holds the COMPLETION status, not
//     the pre-call $80 -- so do NOT expect a $80 readback at $0303 here.
//   * This probe validates POST-SIOV behavior at runtime.
//
// Page-6 layout:
//   $0600  pre-init DCB snapshot ($0300-$030B, 12 bytes) -- mostly stale/uninit before
//          the first init; useful only as a "before" reference.
//   $0610  post-init result row (see snap_init below).
//   $0620  post-init DCB snapshot ($0300-$030B): the STABLE fields (device, command,
//          buffer ptr, byte count, aux) are still inspectable; DSTATS ($0303 == +3) now
//          holds completion status, NOT $80.
//   $0630  heartbeat counter (must keep incrementing -> no crash).
//
// $0610 init result row (offsets):
//   +0  init return status      -> 0 success / 1 failure  (must match DSTATS==0x01)
//   +1  DSTATS  ($0303)         -> 0x01 on success; e.g. 0x8A timeout with no FujiNet
//   +2  nsFinalFlags            (_ns_get_final_flags)
//   +3  nsFinalAudf3            (_ns_get_final_audf3)
//   +4  nsFinalAudf4            (_ns_get_final_audf4)
//   +5  nsVideoStd              (_ns_get_video_std)   0=NTSC / 1=PAL
//   +6  nsPayloadBuf[0]         payload prefix sanity (first host char)
//   +7  nsPayloadBuf[1]
//   +8  nsPayloadBuf[2]
//   +9  nsPayloadBuf[3]
//
// Validation modes:
//   Mode A (no FujiNet): init MAY fail. Failure must be CLEAN -- SIOV returns without
//     crashing, $0610+0 == 1, $0610+1 is a non-$01 completion code (typically 0x8A),
//     heartbeat ($0630) keeps ticking. Do NOT call begin after a failed init (this probe
//     skips begin unless init succeeded).
//   Mode B (FujiNet present): init returns success -- $0610+0 == 0, $0610+1 == 0x01,
//     nsFinal* populated, payload prefix == host. init+begin then yields the expected
//     9O.3b/9P POKEY/lifecycle values (validate with the vectors probe expectations).
//
// Built WITH EDGE_NETSTREAM_TEST_HOOKS so nsPayloadBuf is readable. Altirra-only; NOT a
// CTest. Do NOT claim Mode-B success without real FujiNet (or equivalent) support.

#include <stdint.h>

#include <engine/platform/atari/fujinet_netstream_realtime_abi.h>

extern "C" {
    uint8_t _ns_get_final_flags(void);
    uint8_t _ns_get_final_audf3(void);
    uint8_t _ns_get_final_audf4(void);
    uint8_t _ns_get_video_std(void);

    // Payload buffer (EDGE_NETSTREAM_TEST_HOOKS-global) for prefix sanity.
    extern volatile uint8_t nsPayloadBuf[64];
}

static inline uint8_t peek(uint16_t a) { return *(volatile uint8_t*)a; }

// Snapshot the 12-byte DCB ($0300-$030B) into `d`.
static void snap_dcb(volatile uint8_t* d) {
    for (uint8_t i = 0; i < 12; ++i) d[i] = peek((uint16_t)(0x0300 + i));
}

int main() {
    static const char host[] = "edge";

    snap_dcb((volatile uint8_t*)0x0600);   // pre-init DCB (reference only)

    // Run the real init: marshal args + DCB fill + SIOV + DSTATS check.
    uint8_t init_status = edge_ns_init_netstream(host, /*flags=*/0x00,
                                                 /*nominal_baud=*/9600,
                                                 /*port_swapped=*/0x0000);

    // $0610 result row.
    volatile uint8_t* r = (volatile uint8_t*)0x0610;
    r[0] = init_status;
    r[1] = peek(0x0303);            // DSTATS completion status (NOT pre-call $80)
    r[2] = _ns_get_final_flags();
    r[3] = _ns_get_final_audf3();
    r[4] = _ns_get_final_audf4();
    r[5] = _ns_get_video_std();
    r[6] = nsPayloadBuf[0];
    r[7] = nsPayloadBuf[1];
    r[8] = nsPayloadBuf[2];
    r[9] = nsPayloadBuf[3];

    snap_dcb((volatile uint8_t*)0x0620);   // post-init DCB (stable fields inspectable)

    // Mode B only: bring the stream up after a SUCCESSFUL init. On failure (Mode A) we
    // deliberately do NOT call begin -- a failed init must not be followed by begin.
    if (init_status == 0) {
        _edge_ns_begin_stream();
    }

    // Heartbeat: proves the machine survived init (+ optional begin) without hanging.
    for (;;) {
        ++*(volatile uint8_t*)0x0630;
    }
    return 0;
}
