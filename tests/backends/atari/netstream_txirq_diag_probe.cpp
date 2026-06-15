// tests/backends/atari/netstream_txirq_diag_probe.cpp
// Stage 9R.3 TX serial-output-ready IRQ diagnostic -- Altirra-only, NOT a CTest.
//
// 9R.3 showed the TX ring does not drain past the 9P idle-prime (tx_space stuck at 113,
// peer got 0 bytes). This probe answers WHERE the serial-output path is broken using the
// EDGE_NETSTREAM_TEST_HOOKS counters/mirrors added to the handler (dispatcher / output-IRQ /
// input-IRQ entry counts, IRQST sample, SEROUT-from-IRQ count+byte). No production behavior
// change. Run via scripts/altirra_probe.sh ... B with the FujiNet stack up (init must reach
// begin so POKEY is in serial mode and the serial IRQs are armed).
//
// Page-6 map (dumped $0600..$064F):
//   $0600 lifecycle/TX-state: +0 open  +1 active  +2 DSTATS  +3 send  +4 tx_space@send
//         +5 tx_space@wait  +6 outLevel  +7 outIndex  +8 serialOutIdle  +9 hardwareActivated
//         +A vectorInstalled  +B reached-end $AA
//   $0610 IRQ counters: +0 irq_disp_count  +1 soh_count  +2 sih_count  +3 serout_irq_count
//         +4 last_serout_irq  +5 last_irqst  +6 prime_count  +7 last_prime_byte
//   $0620 registers: +0 POKMSK($0010)  +1 IRQST($D20E rd)  +2 SKSTAT($D20F rd)
//         +3 SSKCTL shadow($0232)  +4 SERIN($D20D rd)
//   $0630 vectors: +0/1 VIMIRQ($0216)  +2/3 VSERIN($020A)  +4/5 VSEROR($020C)  +6/7 VSEROC($020E)
//   $0640 timing: +0 irq_disp_count@send  +1 soh_count@send  +2 outLevel@send

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
    uint8_t _edge_ns_tx_space(void);
    // handler state + 9R.3 diagnostic counters (EDGE_NETSTREAM_TEST_HOOKS .bss).
    extern volatile uint8_t outLevel, outIndex, serialOutIdle, hardwareActivated, vectorInstalled;
    extern volatile uint8_t ns_test_irq_disp_count, ns_test_soh_count, ns_test_sih_count;
    extern volatile uint8_t ns_test_serout_irq_count, ns_test_last_serout_irq, ns_test_last_irqst;
    extern volatile uint8_t ns_test_prime_count, ns_test_last_prime_byte;
}

namespace nsr = atari::fujinet_netstream;
namespace n = engine::net;
static inline uint8_t peek(uint16_t a) { return *(volatile uint8_t*)a; }
static inline uint8_t u8st(n::NetStatus s) { return static_cast<uint8_t>(s); }

int main() {
    static const char host[] = NS_PEER_HOST;
    uint8_t out[16];
    for (uint8_t i = 0; i < 16; ++i) out[i] = (uint8_t)(0xA0 + i);

    const n::NetStatus open_st =
        nsr::NetstreamRealtimeAdapter::realtime_open_udp_seq(host, NS_PEER_PORT, 0);
    const uint8_t dstats = peek(0x0303);
    const uint8_t active = nsr::NetstreamRealtimeAdapter::realtime_active() ? 1 : 0;

#if defined(NS_PROBE_FORCE_CLI)
    // 9R.3 hypothesis test (probe-only, NOT a production change): EDGE begin preserves the
    // caller's I-flag (php/plp) instead of upstream's explicit cli. If the I-flag is set,
    // serial IRQs stay masked (NMIs/VBI still run). Force-clear here; if the ring then drains
    // and disp_count>0, the masked-I-flag is the root cause.
    __asm__ volatile("cli");
#endif
#if defined(NS_PROBE_OPEN_DELAY)
    // 9R.3 startup-corruption test (probe-only): wait NS_PROBE_OPEN_DELAY frames after open
    // (external clock settle) before sending. If the peer then receives a clean A0..AF, the
    // corruption is a transmit-startup/clock-settle timing issue, not data-path logic.
    {
        uint8_t l = peek(0x14), f = 0;
        while (f < (NS_PROBE_OPEN_DELAY)) { uint8_t v=peek(0x14); uint8_t d=(uint8_t)(v-l); if(d){f=(uint8_t)(f+d);l=v;} }
    }
#endif

    const n::NetStatus send_st = nsr::NetstreamRealtimeAdapter::realtime_send_nb(out, 16);
    const uint8_t tx_at_send = _edge_ns_tx_space();

    // immediate (t0) IRQ-count snapshot
    const uint8_t disp0 = ns_test_irq_disp_count;
    const uint8_t soh0  = ns_test_soh_count;
    const uint8_t outl0 = outLevel;

    // wait ~1 s (RTCLOK low byte $14, ~60 frames NTSC); IRQs accumulate in the background.
    uint8_t last14 = peek(0x14), frames = 0;
    while (frames < 60) {
        const uint8_t v = peek(0x14);
        const uint8_t d = (uint8_t)(v - last14);
        if (d) { frames = (uint8_t)(frames + d); last14 = v; }
    }
    const uint8_t tx_at_wait = _edge_ns_tx_space();

    // ---- page 6 ----
    volatile uint8_t* a = (volatile uint8_t*)0x0600;
    a[0]=u8st(open_st); a[1]=active; a[2]=dstats; a[3]=u8st(send_st);
    a[4]=tx_at_send; a[5]=tx_at_wait; a[6]=outLevel; a[7]=outIndex;
    a[8]=serialOutIdle; a[9]=hardwareActivated; a[10]=vectorInstalled; a[11]=0xAA;

    volatile uint8_t* b = (volatile uint8_t*)0x0610;
    b[0]=ns_test_irq_disp_count; b[1]=ns_test_soh_count; b[2]=ns_test_sih_count;
    b[3]=ns_test_serout_irq_count; b[4]=ns_test_last_serout_irq; b[5]=ns_test_last_irqst;
    b[6]=ns_test_prime_count; b[7]=ns_test_last_prime_byte;

    volatile uint8_t* c = (volatile uint8_t*)0x0620;
    c[0]=peek(0x0010); c[1]=peek(0xD20E); c[2]=peek(0xD20F); c[3]=peek(0x0232); c[4]=peek(0xD20D);

    volatile uint8_t* d = (volatile uint8_t*)0x0630;
    d[0]=peek(0x0216); d[1]=peek(0x0217); d[2]=peek(0x020A); d[3]=peek(0x020B);
    d[4]=peek(0x020C); d[5]=peek(0x020D); d[6]=peek(0x020E); d[7]=peek(0x020F);

    volatile uint8_t* e = (volatile uint8_t*)0x0640;
    e[0]=disp0; e[1]=soh0; e[2]=outl0;

    edge_host_dump("H1:NSDUMP.BIN", (const void*)0x0600, 0x50);
    for (;;) { ++*(volatile uint8_t*)0x0650; }
    return 0;
}
