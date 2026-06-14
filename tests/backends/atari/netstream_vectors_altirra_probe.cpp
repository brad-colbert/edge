// tests/backends/atari/netstream_vectors_altirra_probe.cpp
// Stage 9O.3a Altirra/hardware validation harness -- NOT a CTest.
//
// Real _ns_begin_stream / _ns_end_stream write the OS-vector page ($020A-$0216) and
// PACTL, so they cannot run under mos-sim (which loads the program at $0200). This
// .xex exercises the REAL begin/end on Altirra (or hardware) and snapshots the
// relevant state into page 6 ($0600/$0610/$0620) so you can read three rows in the
// debugger -- before begin, after begin, after end -- with no breakpoint hunting.
//
// Build (Atari .xex, atari8-dos toolchain):
//   cmake -B <dir> -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON ...
//   cmake --build <dir> --target netstream_vectors_altirra_probe
//
// Run in Altirra; the program loops forever after taking the 3 snapshots. Open the
// debugger and dump memory at $0600. Layout per 16-byte row (offsets):
//   +0  PACTL        ($D302)        begin: 0x34 (motor on)  end: 0x3c (motor off)
//   +1  VSERIN  lo   ($020A)        after begin -> &SerialInputIrqHandler
//   +2  VSERIN  hi   ($020B)
//   +3  VSEROR  lo   ($020C)        after begin -> &SerialOutputIrqHandler
//   +4  VSEROR  hi   ($020D)
//   +5  VSEROC  lo   ($020E)        after begin -> &SerialCompleteIrqHandler
//   +6  VSEROC  hi   ($020F)
//   +7  VIMIRQ  lo   ($0216)        after begin -> &IrqHandler
//   +8  VIMIRQ  hi   ($0217)
//   +9  POKMSK       ($0010)        9O.4: serial bits 0x30 SET after begin, CLEAR after
//                                   end (these bits are EDGE-owned; the OS never touches
//                                   them, so they read reliably even though bits 6-7 are
//                                   noisy OS keyboard/break housekeeping)
//   +A  nsVideoStd   (_ns_get_video_std)   0=NTSC / 1=PAL after begin's DetectPAL
//
// Rows:  $0600 = before begin   $0610 = after begin   $0620 = after end
//
// PASS criteria:
//   - $0610: PACTL=0x34; the four vectors point at the EDGE handlers (cross-ref the
//     .map for SerialInputIrqHandler / SerialOutputIrqHandler /
//     SerialCompleteIrqHandler / IrqHandler); (POKMSK & 0x30) == 0x30 (9O.4 armed);
//     videoStd in {0,1}.
//   - $0620: PACTL=0x3c; the four vectors == $0600's vectors (restored);
//     (POKMSK & 0x30) == 0x00 (9O.4 disarmed); other POKMSK bits preserved.
//   - $0630 keeps incrementing (heartbeat) -- proves the system stayed alive across
//     begin->end with serial IRQs LIVE (any spurious POKEY output-ready IRQ is absorbed
//     by SerialOutputIrqHandler draining the empty TX ring) and IrqHandler chains the
//     old handler correctly.
//
// IRQEN itself is write-only (reads return IRQST), so it is NOT in the page-6 snapshot;
// confirm it via the Altirra `.pokey` dump -- expect IRQEN=0xF0 (0xC0|0x30) after begin
// and 0xC0 after end.
//
// 9O.4 re-entry note: to validate the early hardwareActivated guard, drive begin/end
// twice in the debugger (or extend main below) -- a 2nd begin while armed must return
// without re-installing/re-arming (vectors + POKMSK 0x30 unchanged, rings intact); a
// 2nd end must be a safe no-op (system stable, heartbeat alive).

#include <stdint.h>

extern "C" {
    void    _edge_ns_begin_stream(void);
    void    _edge_ns_end_stream(void);
    uint8_t _ns_get_video_std(void);
}

static inline uint8_t peek(uint16_t a) { return *(volatile uint8_t*)a; }

// Snapshot the watched state into an 11-byte row at `d`.
static void snap(volatile uint8_t* d) {
    d[0]  = peek(0xD302);   // PACTL
    d[1]  = peek(0x020A);   // VSERIN lo
    d[2]  = peek(0x020B);   // VSERIN hi
    d[3]  = peek(0x020C);   // VSEROR lo
    d[4]  = peek(0x020D);   // VSEROR hi
    d[5]  = peek(0x020E);   // VSEROC lo
    d[6]  = peek(0x020F);   // VSEROC hi
    d[7]  = peek(0x0216);   // VIMIRQ lo
    d[8]  = peek(0x0217);   // VIMIRQ hi
    d[9]  = peek(0x0010);   // POKMSK shadow
    d[10] = _ns_get_video_std();
}

int main() {
    snap((volatile uint8_t*)0x0600);   // before begin
    _edge_ns_begin_stream();
    snap((volatile uint8_t*)0x0610);   // after begin
    _edge_ns_end_stream();
    snap((volatile uint8_t*)0x0620);   // after end

    // Heartbeat at $0630: a volatile loop (not optimized away) that also proves the
    // system survived begin->end (if $0630 keeps incrementing in Altirra, IrqHandler
    // chained correctly; if it never ticks, begin's IRQ install hung the machine).
    for (;;) {
        ++*(volatile uint8_t*)0x0630;
    }
    return 0;
}
