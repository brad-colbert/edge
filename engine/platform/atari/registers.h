#ifndef ENGINE_PLATFORM_ATARI_REGISTERS_H
#define ENGINE_PLATFORM_ATARI_REGISTERS_H

// platform/atari/registers.h — hardware register pointers for the Atari 8-bit
// family, organised by chip in the atari::reg namespace.
//
// Each name is a `volatile uint8_t* const` pointing at a fixed hardware
// address, used as e.g. `*atari::reg::COLPF0 = 0x2A;` (API_DESIGN.md "Register
// Access"). These are NOT marked `constexpr`: an integer-to-pointer
// reinterpret_cast is not a constant expression in C++, so the keyword is
// rejected. They are still statically initialised to absolute addresses (no
// runtime init / no .init_array), and the matching `*_ADDR` constants below
// ARE constexpr for compile-time address math. This `inline ... * const` +
// `constexpr NAME_ADDR` pattern is the ratified convention for all platform
// register definitions (DECISIONS.md ADR-017).
//
// Memory map: ANTIC $D400, GTIA $D000, POKEY $D200, PIA $D300. GTIA and POKEY
// have distinct read vs. write functions at the same address — both are given
// here under their conventional names.
//
// This header depends only on hardware documentation (Dependency Rule 7).

#include <stdint.h>

namespace atari {
namespace reg {

// Define a register pointer + its constexpr address in one line, then expose
// both. (A macro keeps the ~90-entry map free of transcription errors; it is
// #undef'd at the end of the header so nothing leaks.)
#define EDGE_ATARI_REG(name, addr)                                            \
    inline constexpr uint16_t name##_ADDR = (addr);                           \
    inline volatile uint8_t* const name =                                     \
        reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(addr))

// ── ANTIC ($D400) ────────────────────────────────────────────────────
EDGE_ATARI_REG(DMACTL, 0xD400);   // (W) DMA control
EDGE_ATARI_REG(CHACTL, 0xD401);   // (W) character control
EDGE_ATARI_REG(DLISTL, 0xD402);   // (W) display list address low
EDGE_ATARI_REG(DLISTH, 0xD403);   // (W) display list address high
EDGE_ATARI_REG(HSCROL, 0xD404);   // (W) horizontal fine scroll
EDGE_ATARI_REG(VSCROL, 0xD405);   // (W) vertical fine scroll
EDGE_ATARI_REG(PMBASE, 0xD407);   // (W) player/missile base address
EDGE_ATARI_REG(CHBASE, 0xD409);   // (W) character set base address
EDGE_ATARI_REG(WSYNC,  0xD40A);   // (W) wait for horizontal sync
EDGE_ATARI_REG(VCOUNT, 0xD40B);   // (R) vertical line counter
EDGE_ATARI_REG(NMIEN,  0xD40E);   // (W) NMI enable
EDGE_ATARI_REG(NMIRES, 0xD40F);   // (W) NMI status reset
EDGE_ATARI_REG(NMIST,  0xD40F);   // (R) NMI status

// ── GTIA ($D000) — write registers ───────────────────────────────────
EDGE_ATARI_REG(HPOSP0, 0xD000);   // player 0 horizontal position
EDGE_ATARI_REG(HPOSP1, 0xD001);
EDGE_ATARI_REG(HPOSP2, 0xD002);
EDGE_ATARI_REG(HPOSP3, 0xD003);
EDGE_ATARI_REG(HPOSM0, 0xD004);   // missile 0 horizontal position
EDGE_ATARI_REG(HPOSM1, 0xD005);
EDGE_ATARI_REG(HPOSM2, 0xD006);
EDGE_ATARI_REG(HPOSM3, 0xD007);
EDGE_ATARI_REG(SIZEP0, 0xD008);   // player 0 size
EDGE_ATARI_REG(SIZEP1, 0xD009);
EDGE_ATARI_REG(SIZEP2, 0xD00A);
EDGE_ATARI_REG(SIZEP3, 0xD00B);
EDGE_ATARI_REG(SIZEM,  0xD00C);   // missile sizes (all four)
EDGE_ATARI_REG(GRAFP0, 0xD00D);   // player 0 graphics
EDGE_ATARI_REG(GRAFP1, 0xD00E);
EDGE_ATARI_REG(GRAFP2, 0xD00F);
EDGE_ATARI_REG(GRAFP3, 0xD010);
EDGE_ATARI_REG(GRAFM,  0xD011);   // missile graphics
EDGE_ATARI_REG(COLPM0, 0xD012);   // player/missile 0 colour
EDGE_ATARI_REG(COLPM1, 0xD013);
EDGE_ATARI_REG(COLPM2, 0xD014);
EDGE_ATARI_REG(COLPM3, 0xD015);
EDGE_ATARI_REG(COLPF0, 0xD016);   // playfield 0 colour
EDGE_ATARI_REG(COLPF1, 0xD017);
EDGE_ATARI_REG(COLPF2, 0xD018);
EDGE_ATARI_REG(COLPF3, 0xD019);
EDGE_ATARI_REG(COLBK,  0xD01A);   // background colour
EDGE_ATARI_REG(PRIOR,  0xD01B);   // priority / GTIA mode control
EDGE_ATARI_REG(GRACTL, 0xD01D);   // graphics control (P/M DMA enable)
EDGE_ATARI_REG(HITCLR, 0xD01E);   // (W) clear collision registers

// ── GTIA ($D000) — read registers (collision + PAL flag) ─────────────
// Same addresses as the write registers above, different function on read.
EDGE_ATARI_REG(M0PF, 0xD000);     // missile 0 -> playfield collisions
EDGE_ATARI_REG(M1PF, 0xD001);
EDGE_ATARI_REG(M2PF, 0xD002);
EDGE_ATARI_REG(M3PF, 0xD003);
EDGE_ATARI_REG(P0PF, 0xD004);     // player 0 -> playfield collisions
EDGE_ATARI_REG(P1PF, 0xD005);
EDGE_ATARI_REG(P2PF, 0xD006);
EDGE_ATARI_REG(P3PF, 0xD007);
EDGE_ATARI_REG(M0PL, 0xD008);     // missile 0 -> player collisions
EDGE_ATARI_REG(M1PL, 0xD009);
EDGE_ATARI_REG(M2PL, 0xD00A);
EDGE_ATARI_REG(M3PL, 0xD00B);
EDGE_ATARI_REG(P0PL, 0xD00C);     // player 0 -> player collisions
EDGE_ATARI_REG(P1PL, 0xD00D);
EDGE_ATARI_REG(P2PL, 0xD00E);
EDGE_ATARI_REG(P3PL, 0xD00F);
EDGE_ATARI_REG(PAL,  0xD014);     // (R) PAL/NTSC flag (low bits set => PAL)

// ── POKEY ($D200) — write registers ──────────────────────────────────
EDGE_ATARI_REG(AUDF1,  0xD200);   // voice 1 frequency
EDGE_ATARI_REG(AUDC1,  0xD201);   // voice 1 control (distortion/volume)
EDGE_ATARI_REG(AUDF2,  0xD202);
EDGE_ATARI_REG(AUDC2,  0xD203);
EDGE_ATARI_REG(AUDF3,  0xD204);
EDGE_ATARI_REG(AUDC3,  0xD205);
EDGE_ATARI_REG(AUDF4,  0xD206);
EDGE_ATARI_REG(AUDC4,  0xD207);
EDGE_ATARI_REG(AUDCTL, 0xD208);   // audio control (clock/filter config)
EDGE_ATARI_REG(STIMER, 0xD209);   // (W) reset audio timers
EDGE_ATARI_REG(SKRES,  0xD20A);   // (W) reset serial/keyboard status
EDGE_ATARI_REG(IRQEN,  0xD20E);   // (W) IRQ enable

// ── POKEY ($D200) — read registers ───────────────────────────────────
EDGE_ATARI_REG(KBCODE, 0xD209);   // (R) keyboard scan code
EDGE_ATARI_REG(RANDOM, 0xD20A);   // (R) hardware random number
EDGE_ATARI_REG(IRQST,  0xD20E);   // (R) IRQ status

// ── PIA ($D300) ──────────────────────────────────────────────────────
EDGE_ATARI_REG(PORTA, 0xD300);    // port A (joysticks 0/1 directions)
EDGE_ATARI_REG(PORTB, 0xD301);    // port B (joysticks 2/3 / XL bank control)
EDGE_ATARI_REG(PACTL, 0xD302);    // port A control
EDGE_ATARI_REG(PBCTL, 0xD303);    // port B control

#undef EDGE_ATARI_REG

} // namespace reg
} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_REGISTERS_H
