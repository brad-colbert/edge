// tests/backends/atari/raster_vector_tear_altirra_probe.cpp
// Altirra/hardware probe: does a DLI ever split Hal::set_raster_vector's two stores?
//
// The defect this pins (ATank, 2026-08-30) cannot be reproduced under mos-sim, which
// has neither ANTIC nor NMI, and it cannot be observed from inside a DLI either: by
// the time a handler runs, ANTIC has already fetched VDSLST, so a torn vector has
// already been jumped to. The trick here is to make every possible tear LAND ON A
// HANDLER OF OUR OWN instead of on whatever code the hybrid address happens to hit.
//
// Four stubs are placed 128 bytes apart inside one 256-byte-aligned block, so that
// the two legal vectors and both of their hybrids are all addresses we own:
//
//     base+$000  A   lo=$00 hi=P        <- legal vector A
//     base+$080  H1  lo=$80 hi=P        <- hybrid lo(B):hi(A)   TEAR
//     base+$100  H2  lo=$00 hi=P+1      <- hybrid lo(A):hi(B)   TEAR
//     base+$180  B   lo=$80 hi=P+1      <- legal vector B
//
// A and B just bump a 16-bit counter and RTI. H1 and H2 bump a TEAR counter and
// repair VDSLST so the run continues instead of wedging. The main loop then hammers
// set_raster_vector(A)/set_raster_vector(B) while ~24 DLIs a frame fire against it,
// which puts the interrupt at a random phase relative to the two stores.
//
// Read the result as an A/B: built against the guarded set_raster_vector the tear
// counters must be exactly zero, and built against an unguarded one they must NOT be
// -- that second half is what proves the probe is actually exercising the window and
// not just failing to look. Altirra-only; intentionally NOT a CTest.
//
// Dump layout at $0600 (little-endian 16-bit counters):
//   $0600 dispatches through A      $0608 address of A
//   $0602 dispatches through B      $060A address of B
//   $0604 TEARS caught at H1        $060C loop iterations
//   $0606 TEARS caught at H2        $060E frames run / $060F NMIEN shadow

#include <stdint.h>

#include <engine/platform/atari/platform.h>

#include "atari_hostdump.h"

namespace A = atari;

// ── Result block (page 6, free RAM on every Atari) ────────────────────
static volatile uint8_t* const kResult = reinterpret_cast<volatile uint8_t*>(0x0600);

static constexpr uint16_t kCountA = 0x0600;
static constexpr uint16_t kCountB = 0x0602;
static constexpr uint16_t kTearH1 = 0x0604;
static constexpr uint16_t kTearH2 = 0x0606;

// 256-alignment is achieved by over-allocating a page and rounding up, rather than
// by trusting an alignas() the linker may or may not honour on this target.
static uint8_t g_stub_pool[512 + 256];

// A stub that bumps a 16-bit counter and returns. `inc abs` touches no register, so
// nothing needs saving around it -- an NMI handler that clobbered A/X/Y would corrupt
// the interrupted main loop.
//
//   EE lo hi   inc counter+0
//   D0 03      bne +3
//   EE lo hi   inc counter+1
//   40         rti
static uint8_t* emit_counter(uint8_t* p, uint16_t counter) {
    *p++ = 0xEE; *p++ = uint8_t(counter);       *p++ = uint8_t(counter >> 8);
    *p++ = 0xD0; *p++ = 0x03;
    *p++ = 0xEE; *p++ = uint8_t(counter + 1);   *p++ = uint8_t((counter + 1) >> 8);
    return p;
}

// A tear stub: bump the tear counter, then put VDSLST back to a whole address so the
// probe keeps running and can report a count instead of wedging. This one DOES load
// the accumulator, so it brackets itself with PHA/PLA.
static void emit_tear(uint8_t* p, uint16_t counter, uint16_t repair) {
    *p++ = 0x48;                                            // pha
    p = emit_counter(p, counter);
    *p++ = 0xA9; *p++ = uint8_t(repair);                    // lda #lo(repair)
    *p++ = 0x8D; *p++ = 0x00; *p++ = 0x02;                  // sta VDSLST+0
    *p++ = 0xA9; *p++ = uint8_t(repair >> 8);               // lda #hi(repair)
    *p++ = 0x8D; *p++ = 0x01; *p++ = 0x02;                  // sta VDSLST+1
    *p++ = 0x68;                                            // pla
    *p   = 0x40;                                            // rti
}

static void emit_legal(uint8_t* p, uint16_t counter) {
    p = emit_counter(p, counter);
    *p = 0x40;                                              // rti
}

static void store16(uint16_t at, uint16_t v) {
    volatile uint8_t* q = reinterpret_cast<volatile uint8_t*>(at);
    q[0] = uint8_t(v); q[1] = uint8_t(v >> 8);
}

int main() {
    for (uint16_t i = 0; i < 16; ++i) kResult[i] = 0;

    // Lay the four stubs out on the 128-byte grid described above.
    const uint16_t pool = uint16_t(reinterpret_cast<uintptr_t>(g_stub_pool));
    const uint16_t base = uint16_t((pool + 0xFF) & 0xFF00);
    const uint16_t addrA  = uint16_t(base + 0x000);
    const uint16_t addrH1 = uint16_t(base + 0x080);
    const uint16_t addrH2 = uint16_t(base + 0x100);
    const uint16_t addrB  = uint16_t(base + 0x180);

    emit_legal(reinterpret_cast<uint8_t*>(addrA),  kCountA);
    emit_legal(reinterpret_cast<uint8_t*>(addrB),  kCountB);
    emit_tear (reinterpret_cast<uint8_t*>(addrH1), kTearH1, addrA);
    emit_tear (reinterpret_cast<uint8_t*>(addrH2), kTearH2, addrA);

    // Light a DLI on every mode line of the OS's own GR.0 display list: ~24 raster
    // interrupts a frame, at fixed scanlines, asynchronous to the main loop's phase.
    uint8_t* dl = reinterpret_cast<uint8_t*>(
        uint16_t(*A::os::SDLSTL) | (uint16_t(*A::os::SDLSTH) << 8));
    for (uint16_t p = 0; p < 64; ++p) {
        const uint8_t b = dl[p];
        if ((b & 0x0F) == 0x01) break;                   // JMP/JVB terminates the list
        if ((b & 0x0F) != 0x00) dl[p] |= 0x80;           // a mode line: set its DLI bit
        if (b & 0x40) p += 2;                            // LMS carries a 2-byte address
    }

    const uint8_t saved_lo = A::os::VDSLST[0], saved_hi = A::os::VDSLST[1];

    A::Hal::set_raster_vector(addrA);
    A::nmien_set(A::nmien::VBI | A::nmien::DLI);

    // Hammer the two-store window. RTCLOK+2 ($14) is the OS jiffy counter.
    volatile uint8_t* const jiffy = reinterpret_cast<volatile uint8_t*>(0x0014);
    const uint8_t deadline = uint8_t(*jiffy + 180);       // ~3 s at 60 Hz
    uint16_t iterations = 0;
    uint8_t frames = 0, last = *jiffy;
    while (*jiffy != deadline) {
        A::Hal::set_raster_vector(addrA);
        A::Hal::set_raster_vector(addrB);
        ++iterations;
        if (*jiffy != last) { last = *jiffy; ++frames; }
    }

    A::nmien_set(A::nmien::VBI);                          // DLI off before the dump
    A::os::VDSLST[0] = saved_lo;
    A::os::VDSLST[1] = saved_hi;

    store16(0x0608, addrA);
    store16(0x060A, addrB);
    store16(0x060C, iterations);
    kResult[0x0E] = frames;
    kResult[0x0F] = A::g_nmien_shadow;

    edge_host_dump("H1:NSDUMP.BIN", reinterpret_cast<const void*>(0x0600), 16);
    for (;;) {}
}
