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
// Rows:  $0600 = before begin   $0610 = after begin   $0640 = after a post-begin send
//        (SEROUT idle-prime, 9P)   $0620 = after end
//
// Stage 9P adds a $0640 row capturing a single post-begin send -> SEROUT idle-prime.
// Because a CPU read of $D20D returns SERIN (not the SEROUT latch), the primed byte is
// observed through the test-only RAM mirror, NOT a $D20D readback. The send + snapshot
// are fenced by SEI/CLI so the output-ready IRQ cannot mutate the TX state mid-snapshot.
// $0640 layout (offsets):
//   +0  expected test byte (0xA5 constant -- sanity anchor)
//   +1  ns_test_last_prime_byte   -> 0xA5 (the primed byte)
//   +2  ns_test_prime_count       -> 1
//   +3  serialOutIdle             -> 0x00 (transmitter now active)
//   +4  outLevel                  -> 0x00 (primed byte consumed; ring empty)
//   +5  outIndex                  -> 0x01 (consumer advanced one)
//   +6  hardwareActivated         -> 0x01 (still armed)
//   +7  POKMSK shadow ($0010)     -> (& 0x30) == 0x30 (still armed)
//
// PASS criteria:
//   - $0610: PACTL=0x34; the four vectors point at the EDGE handlers (cross-ref the
//     .map for SerialInputIrqHandler / SerialOutputIrqHandler /
//     SerialCompleteIrqHandler / IrqHandler); (POKMSK & 0x30) == 0x30 (9O.4 armed);
//     videoStd in {0,1}.
//   - $0640: ns_test_last_prime_byte==0xA5; ns_test_prime_count==1; serialOutIdle==0;
//     outLevel==0; outIndex==1; hardwareActivated==1; (POKMSK & 0x30)==0x30.
//   - $0620: PACTL=0x3c; the four vectors == $0600's vectors (restored);
//     (POKMSK & 0x30) == 0x00 (9O.4 disarmed); other POKMSK bits preserved.
//   - $0630 keeps incrementing (heartbeat) -- proves the system stayed alive across
//     begin->send->end with serial IRQs LIVE (the primed byte's output-ready IRQ drains
//     the now-empty TX ring without storming) and IrqHandler chains the old handler.
//
// IRQEN itself is write-only (reads return IRQST), so it is NOT in the page-6 snapshot;
// confirm it via the Altirra `.pokey` dump -- expect IRQEN=0xF0 (0xC0|0x30) after begin
// and 0xC0 after end. NOTE (9P): because the prime intentionally writes SEROUT, a
// post-end `SEROUT == FF` is NO LONGER a valid `.pokey` invariant -- drop any such
// check. The post-end invariants that remain: IRQEN==0xC0, (POKMSK & 0x30)==0x00,
// vectors restored, PACTL==0x3c, heartbeat alive.
//
// 9O.4 re-entry note: to validate the early hardwareActivated guard, drive begin/end
// twice in the debugger (or extend main below) -- a 2nd begin while armed must return
// without re-installing/re-arming (vectors + POKMSK 0x30 unchanged, rings intact); a
// 2nd end must be a safe no-op (system stable, heartbeat alive).

#include <stdint.h>

extern "C" {
    void    _edge_ns_begin_stream(void);
    void    _edge_ns_end_stream(void);
    uint8_t _edge_ns_send_byte(uint8_t byte);
    uint8_t _ns_get_video_std(void);

    // 9P test-only RAM mirror + exposed TX/gate state (EDGE_NETSTREAM_TEST_HOOKS).
    extern volatile uint8_t ns_test_last_prime_byte;
    extern volatile uint8_t ns_test_prime_count;
    extern volatile uint8_t serialOutIdle;
    extern volatile uint8_t outLevel;
    extern volatile uint8_t outIndex;
    extern volatile uint8_t hardwareActivated;
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

// 9P: snapshot the post-prime RAM state into an 8-byte row at `d`. Read from the
// test-only mirror + exposed state, never from $D20D (a CPU read there is SERIN).
static void snap_prime(volatile uint8_t* d) {
    d[0] = 0xA5;                      // expected primed byte (sanity anchor)
    d[1] = ns_test_last_prime_byte;   // -> 0xA5
    d[2] = ns_test_prime_count;       // -> 1
    d[3] = serialOutIdle;             // -> 0x00
    d[4] = outLevel;                  // -> 0x00
    d[5] = outIndex;                  // -> 0x01
    d[6] = hardwareActivated;         // -> 0x01
    d[7] = peek(0x0010);              // POKMSK shadow, (& 0x30)==0x30
}

int main() {
    snap((volatile uint8_t*)0x0600);   // before begin
    _edge_ns_begin_stream();
    snap((volatile uint8_t*)0x0610);   // after begin

    // 9P post-begin send -> SEROUT idle-prime. Fence with SEI/CLI so the output-ready
    // IRQ cannot race the snapshot of serialOutIdle/outLevel/outIndex.
    __asm__ volatile("sei");
    _edge_ns_send_byte(0xA5);          // armed + idle -> primes 0xA5 into SEROUT
    snap_prime((volatile uint8_t*)0x0640);
    __asm__ volatile("cli");           // let the output IRQ drain the (now-empty) ring

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
