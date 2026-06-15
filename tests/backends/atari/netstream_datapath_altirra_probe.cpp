// tests/backends/atari/netstream_datapath_altirra_probe.cpp
// Stage 9R.3 Altirra Mode B end-to-end data-path probe -- NOT a CTest.
//
// Drives the REAL realtime adapter against a live FujiNet responder (fujinet-pc firmware via
// the NetSIO hub) and a UDP echo peer: open_udp_seq -> send a 16-byte packet (A0..AF) -> wait
// for the peer's 16-byte echo (50..5F) -> recv -> close. Self-dumps page 6 to H1:NSDUMP.BIN
// (tests/backends/atari/atari_hostdump.h) so scripts/altirra_probe.sh captures it automatically.
//
// Peer host is compiled in (NS_PEER_HOST, default the Docker echo IP 172.30.0.2), port 9000.
// Build: MOS_ATARI8_CXX ... -DNS_PEER_HOST="172.30.0.2"  (see documents/PLATFORM_ATARI.md).
//
// This is emulator / FujiNet-PC validation, NOT physical FujiNet hardware. DSTATS=$01 alone is
// NOT success: success requires the peer's recv.bin == A0..AF AND $0630 == 50..5F.
//
// Page-6 map (dumped $0600..$065F):
//   $0600  pre-open DCB ($0300-$030B)            reference
//   $0610  status row:
//     +0 open status   +1 active-after-open   +2 DSTATS   +3 nsFinalFlags
//     +4 nsFinalAudf3  +5 nsFinalAudf4        +6 tx_space pre-send   +7 send status
//     +8 tx_space post-send (diagnostic)      +9 bytes_avail pre-recv (lo)   +A recv status
//     +B adapter last_error.status            +C adapter last_error.detail (lo)
//     +D active-after-close   +E recv-wait frames   +F reached-end $AA
//   $0620  post-open DCB ($0300-$030B, 12 bytes)
//   $0630  16 RECEIVED bytes (expect 50..5F)
//   $0640  16 SENT bytes (A0..AF, reference)
//   $0650  +0 heartbeat (incremented after the dump)   +1 post-poll raw status (may read 0)

#include <stdint.h>

#include <engine/platform/atari/fujinet_netstream_realtime.h>
#include "atari_hostdump.h"

#ifndef NS_PEER_HOST
#define NS_PEER_HOST "172.30.0.2"
#endif
#ifndef NS_PEER_PORT
#define NS_PEER_PORT 9000
#endif

extern "C" {
    uint8_t  _ns_get_final_flags(void);
    uint8_t  _ns_get_final_audf3(void);
    uint8_t  _ns_get_final_audf4(void);
    uint8_t  _edge_ns_tx_space(void);     // diagnostic only
    uint16_t _edge_ns_bytes_avail(void);  // diagnostic only
    uint8_t  _edge_ns_get_status(void);   // clears on read; post-poll may be 0
}

namespace nsr = atari::fujinet_netstream;
namespace n = engine::net;

static inline uint8_t peek(uint16_t a) { return *(volatile uint8_t*)a; }
static inline uint8_t u8st(n::NetStatus s) { return static_cast<uint8_t>(s); }
static void snap_dcb(volatile uint8_t* d) {
    for (uint8_t i = 0; i < 12; ++i) d[i] = peek((uint16_t)(0x0300 + i));
}

int main() {
    static const char host[] = NS_PEER_HOST;
    uint8_t out[16], in[16];
    for (uint8_t i = 0; i < 16; ++i) { out[i] = (uint8_t)(0xA0 + i); in[i] = 0; }

    snap_dcb((volatile uint8_t*)0x0600);

    const n::NetStatus open_st =
        nsr::NetstreamRealtimeAdapter::realtime_open_udp_seq(host, NS_PEER_PORT, 0);
    const uint8_t dstats = peek(0x0303);                 // SIO completion (no later SIOV)
    const bool active1 = nsr::NetstreamRealtimeAdapter::realtime_active();

    uint8_t tx_pre = 0, tx_post = 0, tx_postwait = 0, send_st = 0xFF, recv_st = 0xFF, frames = 0;
    uint16_t avail_pre = 0;
    n::NetError le{n::NetStatus::Ok, 0};

    if (active1) {
#if defined(NS_PROBE_OPEN_DELAY)
        // 9R.3: settle the external TX clock after begin before transmitting (probe-side test
        // of the startup-corruption fix; the production settle belongs in the adapter open).
        { uint8_t l=peek(0x14),f=0; while(f<(NS_PROBE_OPEN_DELAY)){uint8_t v=peek(0x14);uint8_t d=(uint8_t)(v-l);if(d){f=(uint8_t)(f+d);l=v;}} }
#endif
        tx_pre = _edge_ns_tx_space();
        send_st = u8st(nsr::NetstreamRealtimeAdapter::realtime_send_nb(out, 16));
        tx_post = _edge_ns_tx_space();

        // Wait for the peer echo, bounded by ~120 OS frames (RTCLOK low byte $14, ~2 s NTSC).
        // The RX input IRQ fills the ring in the background; recv_nb returns Ok at >=16 bytes.
        uint8_t last14 = peek(0x14);
        n::NetStatus r = n::NetStatus::WouldBlock;
        for (;;) {
            avail_pre = _edge_ns_bytes_avail();
            r = nsr::NetstreamRealtimeAdapter::realtime_recv_nb(in, 16);
            if (r == n::NetStatus::Ok) break;
            const uint8_t v = peek(0x14);
            const uint8_t d = (uint8_t)(v - last14);
            if (d) { frames = (uint8_t)(frames + d); last14 = v; }
            if (frames >= 120) break;
        }
        recv_st = u8st(r);

        // One status read through the adapter so last_error reflects any accumulated overflow
        // (serial status is not cleared by recv_nb/bytes_avail; only get_status clears it).
        nsr::NetstreamRealtimeAdapter::realtime_poll();
        le = nsr::NetstreamRealtimeAdapter::realtime_last_error();
        tx_postwait = _edge_ns_tx_space();   // BEFORE close (close resets the ring to 128)
    }

    snap_dcb((volatile uint8_t*)0x0620);
    nsr::NetstreamRealtimeAdapter::realtime_close();
    const bool active2 = nsr::NetstreamRealtimeAdapter::realtime_active();

    volatile uint8_t* s = (volatile uint8_t*)0x0610;
    s[0]  = u8st(open_st);
    s[1]  = active1 ? 1 : 0;
    s[2]  = dstats;
    s[3]  = _ns_get_final_flags();
    s[4]  = _ns_get_final_audf3();
    s[5]  = _ns_get_final_audf4();
    s[6]  = tx_pre;
    s[7]  = send_st;
    s[8]  = tx_post;
    s[9]  = (uint8_t)(avail_pre & 0xff);
    s[10] = recv_st;
    s[11] = u8st(le.status);
    s[12] = (uint8_t)(le.detail & 0xff);
    s[13] = active2 ? 1 : 0;
    s[14] = frames;
    s[15] = 0xAA;

    volatile uint8_t* rin  = (volatile uint8_t*)0x0630;
    volatile uint8_t* rout = (volatile uint8_t*)0x0640;
    for (uint8_t i = 0; i < 16; ++i) { rin[i] = in[i]; rout[i] = out[i]; }

    *(volatile uint8_t*)0x0650 = 0;                 // heartbeat seed (separate from packet rows)
    *(volatile uint8_t*)0x0651 = _edge_ns_get_status();  // post-poll raw status (may be cleared)
    // Diagnostic: TX drain after the recv wait. If tx_space climbed back toward 128, POKEY
    // clocked the buffered bytes OUT (they left the Atari -> bridge dropped them); if it
    // stayed ~113, the output IRQ never drained (transmitter not clocking in this setup).
    *(volatile uint8_t*)0x0652 = tx_postwait;

    edge_host_dump("H1:NSDUMP.BIN", (const void*)0x0600, 0x60);

    for (;;) { ++*(volatile uint8_t*)0x0650; }       // heartbeat
    return 0;
}
