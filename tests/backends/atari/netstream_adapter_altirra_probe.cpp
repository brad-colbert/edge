// tests/backends/atari/netstream_adapter_altirra_probe.cpp
// Stage 9R.1 Altirra/hardware validation harness for the REAL realtime adapter -- NOT a CTest.
//
// Drives the production adapter entrypoints
//   atari::fujinet_netstream::NetstreamRealtimeAdapter::realtime_open_udp_seq / _active /
//   _poll / _close  (RealNetstreamOps -> edge_ns_init_netstream + _edge_ns_begin_stream +
//   _edge_ns_end_stream + _edge_ns_get_status)
// which fill the SIO DCB, call SIOV, and write the OS vector page / POKEY / IRQEN -- none of
// which run under mos-sim. Snapshots the result into page 6 for the Altirra debugger.
//
// Build (Atari .xex, atari8-dos toolchain):
//   cmake -B <dir> -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON ...
//   cmake --build <dir> --target netstream_adapter_altirra_probe
//
// NetStatus enum (u8): Ok=0, WouldBlock=1, Closed=2, Overflow=3, InvalidArgument=4,
//                      BadConfig=5, Unsupported=6, TransportError=7.
//
// Page-6 layout:
//   $0600  pre-open DCB snapshot ($0300-$030B, reference)
//   $0610  adapter result row (below)
//   $0620  post-open DCB snapshot ($0300-$030B): stable fields inspectable; $0303 = DSTATS
//   $0630  heartbeat counter (must keep incrementing -> no crash)
//
// $0610 adapter result row (offsets):
//   +0  open() return status (NetStatus u8)   Mode A: 7 (TransportError) / Mode B: 0 (Ok)
//   +1  realtime_active() after open (0/1)     Mode A: 0 / Mode B: 1
//   +2  DSTATS ($0303)                          Mode A: ~0x8A timeout / Mode B: 0x01
//   +3  nsFinalFlags        (_ns_get_final_flags)
//   +4  nsFinalAudf3        (_ns_get_final_audf3)   19200 NTSC -> 39 (0x27): prepare ran
//   +5  nsFinalAudf4        (_ns_get_final_audf4)
//   +6  nsVideoStd          (_ns_get_video_std)     0=NTSC / 1=PAL
//   +7  poll() status after open (NetStatus u8)     active->Ok(0); inactive->Closed(2)
//   +8  realtime_active() after close (0/1)          -> 0
//   +9  last_error().status after close (NetStatus u8) -> 2 (Closed)
//
// Validation modes:
//   Mode A (no FujiNet, /cleardevices): open -> 7 (TransportError); active 0; DSTATS ~0x8A;
//     nsFinal* populated (AUDF3=39 -> prepare ran before SIOV); poll after a failed open is
//     not driven (open failed, so the probe goes straight to close); close is a no-op (end
//     not entered) -> active 0, last_error 2; heartbeat alive; no crash.
//   Mode B (FujiNet present): open -> 0 (Ok); active 1; DSTATS 0x01; poll -> 0; close -> end,
//     active 0, last_error 2; lifecycle stable. NOT claimed without real FujiNet.
//
// Altirra-only; intentionally NOT a CTest.

#include <stdint.h>

#include <engine/platform/atari/fujinet_netstream_realtime.h>
#include "atari_hostdump.h"   // edge_host_dump() -> H: host file (Altirra auto-capture)

extern "C" {
    uint8_t _ns_get_final_flags(void);
    uint8_t _ns_get_final_audf3(void);
    uint8_t _ns_get_final_audf4(void);
    uint8_t _ns_get_video_std(void);
}

namespace nsr = atari::fujinet_netstream;
namespace n = engine::net;

static inline uint8_t peek(uint16_t a) { return *(volatile uint8_t*)a; }
static inline uint8_t u8st(n::NetStatus s) { return static_cast<uint8_t>(s); }

static void snap_dcb(volatile uint8_t* d) {
    for (uint8_t i = 0; i < 12; ++i) d[i] = peek((uint16_t)(0x0300 + i));
}

int main() {
    static const char host[] = "edge";

    snap_dcb((volatile uint8_t*)0x0600);   // pre-open DCB (reference)

    const n::NetStatus open_st =
        nsr::NetstreamRealtimeAdapter::realtime_open_udp_seq(host, 9000, 0);
    const bool active_after_open = nsr::NetstreamRealtimeAdapter::realtime_active();

    // Only poll when the open actually activated the lane (Mode B). On a failed open
    // (Mode A) we do NOT drive poll/begin -- straight to close.
    n::NetStatus poll_st = n::NetStatus::Closed;
    if (active_after_open) {
        poll_st = nsr::NetstreamRealtimeAdapter::realtime_poll();
    }

    volatile uint8_t* r = (volatile uint8_t*)0x0610;
    r[0] = u8st(open_st);
    r[1] = active_after_open ? 1 : 0;
    r[2] = peek(0x0303);                 // DSTATS
    r[3] = _ns_get_final_flags();
    r[4] = _ns_get_final_audf3();
    r[5] = _ns_get_final_audf4();
    r[6] = _ns_get_video_std();
    r[7] = u8st(poll_st);

    snap_dcb((volatile uint8_t*)0x0620);   // post-open DCB

    nsr::NetstreamRealtimeAdapter::realtime_close();
    r[8] = nsr::NetstreamRealtimeAdapter::realtime_active() ? 1 : 0;
    r[9] = u8st(nsr::NetstreamRealtimeAdapter::realtime_last_error().status);

    // Self-dump the whole page-6 snapshot ($0600..$064F, 0x50 bytes) to the host via H:,
    // so scripts/altirra_probe.sh can read it back automatically (== what `db $0600 $50`
    // shows in the debugger). Harmless if H: is not mounted (CIO just errors). The page-6
    // rows are fully populated by this point (open/poll/DCB/close above).
    edge_host_dump("H1:NSDUMP.BIN", (const void*)0x0600, 0x50);

    // Heartbeat: proves the machine survived open(+poll)+close without hanging.
    for (;;) {
        ++*(volatile uint8_t*)0x0630;
    }
    return 0;
}
