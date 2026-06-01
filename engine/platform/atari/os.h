#ifndef ENGINE_PLATFORM_ATARI_OS_H
#define ENGINE_PLATFORM_ATARI_OS_H

// platform/atari/os.h — Atari OS shadow RAM and ROM entry points, in the
// atari::os namespace.
//
// The Atari OS keeps a set of RAM "shadow" registers that its deferred
// vertical-blank routine copies into the hardware chip registers every frame.
// That copy happens BEFORE the engine's own deferred-VBI service runs (the
// engine installs through VVBLKD — see hal.h install_vbi), so anything the
// engine writes directly to those chip registers is silently overwritten on the
// next OS VBI. The HAL therefore writes the shadow, not the register, for every
// value the OS manages (display list, DMA control, colours). See DECISIONS.md
// ADR-017 for the pointer convention and ARCHITECTURE.md "Data Flow Per Frame".
//
// Same `inline ... * const` + constexpr `*_ADDR` pattern as registers.h: the
// shadows are RAM bytes (so, unlike write-only chip registers, they can be
// read-modified-written), and the ROM entry points are plain address constants.
//
// Depends only on hardware documentation (Dependency Rule 7).

#include <stdint.h>

namespace atari {
namespace os {

// Define a shadow pointer + its constexpr address in one line (mirrors the
// EDGE_ATARI_REG macro in registers.h); #undef'd at the end so nothing leaks.
#define EDGE_ATARI_OS(name, addr)                                             \
    inline constexpr uint16_t name##_ADDR = (addr);                           \
    inline volatile uint8_t* const name =                                     \
        reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(addr))

// ── Display shadows ──────────────────────────────────────────────────
EDGE_ATARI_OS(SDLSTL, 0x0230);   // shadow of DLISTL (display list address low)
EDGE_ATARI_OS(SDLSTH, 0x0231);   // shadow of DLISTH (display list address high)
EDGE_ATARI_OS(SDMCTL, 0x022F);   // shadow of DMACTL (DMA control)

// ── Colour shadows ───────────────────────────────────────────────────
// PCOLR0[n] shadows COLPM0-3 (player/missile colours); COLOR0[n] shadows
// COLPF0-3 then COLBK at index 4 (the five playfield colours).
EDGE_ATARI_OS(PCOLR0, 0x02C0);   // base of 4: PCOLR0..PCOLR3
EDGE_ATARI_OS(COLOR0, 0x02C4);   // base of 5: COLOR0..COLOR3, COLOR4 (COLBK)

// ── Interrupt vector shadows (2-byte, [0]=low [1]=high) ──────────────
EDGE_ATARI_OS(VDSLST, 0x0200);   // display-list (DLI) NMI vector
EDGE_ATARI_OS(VVBLKI, 0x0222);   // immediate vertical-blank vector
EDGE_ATARI_OS(VVBLKD, 0x0224);   // deferred vertical-blank vector

// ── System ───────────────────────────────────────────────────────────
EDGE_ATARI_OS(ATRACT, 0x004D);   // attract-mode timer (write 0/frame to suppress)
EDGE_ATARI_OS(CHBAS,  0x02F4);   // shadow of CHBASE (character set base page)

#undef EDGE_ATARI_OS

// ── ROM entry points ─────────────────────────────────────────────────
// XITVBV exits a deferred VBI handler (restores the OS-saved A/X/Y and RTIs);
// SETVBV installs a VBI vector through the OS. Plain addresses (called, not
// dereferenced as data).
inline constexpr uint16_t XITVBV = 0xE462;   // deferred-VBI exit
inline constexpr uint16_t SETVBV = 0xE45C;   // set VBI vector

} // namespace os
} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_OS_H
