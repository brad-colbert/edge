#ifndef ENGINE_PLATFORM_ATARI_VBXE_REGISTERS_H
#define ENGINE_PLATFORM_ATARI_VBXE_REGISTERS_H

// platform/atari/vbxe_registers.h — VBXE FX Core register handles + bit
// constants, addressed off Config::reg_base ($D640 or $D740).
//
// The VBXE register block occupies Dx40..Dx5F (x = 6/7). Unlike the fixed-address
// ANTIC/GTIA/POKEY registers in registers.h, the VBXE base is a compile-time
// Config value, so the typed handles are produced by a template (Regs<Config>)
// rather than file-scope `inline ... * const`. Each register still exposes a
// `constexpr u16 NAME_ADDR` for compile-time address math (ADR-017) plus an
// engine::Mmio<addr> handle for access:  Regs<Cfg>::VIDEO_CONTROL = v;
//
// Register addresses and bit layouts come from the FX Core v1.26 Programmer's
// Manual, "CORE REGISTERS" (pp. 38-46). Depends only on the VBXE config, the
// MMIO primitive, and hardware documentation (Dependency Rule 7).

#include "../../mmio.h"
#include "../../types.h"
#include "vbxe_config.h"

namespace atari::vbxe {

using engine::u8;
using engine::u16;

// Register offsets from reg_base (Dx40 = offset 0x00). Read-side names that
// share an address with a write-side register are listed explicitly, mirroring
// the dual-function convention in registers.h.
namespace reg_offset {
    inline constexpr u8 VIDEO_CONTROL   = 0x00;  // (R: CORE_VERSION)
    inline constexpr u8 XDL_ADR0        = 0x01;  // (R: MINOR_REVISION)
    inline constexpr u8 XDL_ADR1        = 0x02;
    inline constexpr u8 XDL_ADR2        = 0x03;
    inline constexpr u8 CSEL            = 0x04;
    inline constexpr u8 PSEL            = 0x05;
    inline constexpr u8 CR              = 0x06;
    inline constexpr u8 CG              = 0x07;
    inline constexpr u8 CB              = 0x08;
    inline constexpr u8 COLMASK         = 0x09;
    inline constexpr u8 COLCLR          = 0x0A;  // (W)
    inline constexpr u8 COLDETECT       = 0x0A;  // (R) same address
    inline constexpr u8 BL_ADR0         = 0x10;  // (W)
    inline constexpr u8 BLT_COLLISION   = 0x10;  // (R) BLT_COLLISION_CODE
    inline constexpr u8 BL_ADR1         = 0x11;
    inline constexpr u8 BL_ADR2         = 0x12;
    inline constexpr u8 BLITTER_START   = 0x13;  // (W)
    inline constexpr u8 BLITTER_BUSY    = 0x13;  // (R) same address
    inline constexpr u8 IRQ_CONTROL     = 0x14;  // (W)
    inline constexpr u8 IRQ_STATUS      = 0x14;  // (R) same address
    inline constexpr u8 P0              = 0x15;
    inline constexpr u8 P1              = 0x16;
    inline constexpr u8 P2              = 0x17;
    inline constexpr u8 P3              = 0x18;
    inline constexpr u8 MEMAC_B_CONTROL = 0x1D;
    inline constexpr u8 MEMAC_CONTROL   = 0x1E;
    inline constexpr u8 MEMAC_BANK_SEL  = 0x1F;
} // namespace reg_offset

// Typed register access for a given VBXE Config. `base` is Config::reg_base.
template <typename Config>
struct Regs {
    static constexpr u16 base = static_cast<u16>(Config::reg_base);

    // One register: its absolute address constant + an Mmio handle at it. The
    // macro keeps the map transcription-safe; it is #undef'd below.
#define EDGE_VBXE_REG(name)                                                    \
    static constexpr u16 name##_ADDR = base + reg_offset::name;                \
    static constexpr engine::Mmio<base + reg_offset::name> name{}

    EDGE_VBXE_REG(VIDEO_CONTROL);
    EDGE_VBXE_REG(XDL_ADR0);
    EDGE_VBXE_REG(XDL_ADR1);
    EDGE_VBXE_REG(XDL_ADR2);
    EDGE_VBXE_REG(CSEL);
    EDGE_VBXE_REG(PSEL);
    EDGE_VBXE_REG(CR);
    EDGE_VBXE_REG(CG);
    EDGE_VBXE_REG(CB);
    EDGE_VBXE_REG(COLMASK);
    EDGE_VBXE_REG(COLCLR);
    EDGE_VBXE_REG(COLDETECT);
    EDGE_VBXE_REG(BL_ADR0);
    EDGE_VBXE_REG(BLT_COLLISION);
    EDGE_VBXE_REG(BL_ADR1);
    EDGE_VBXE_REG(BL_ADR2);
    EDGE_VBXE_REG(BLITTER_START);
    EDGE_VBXE_REG(BLITTER_BUSY);
    EDGE_VBXE_REG(IRQ_CONTROL);
    EDGE_VBXE_REG(IRQ_STATUS);
    EDGE_VBXE_REG(P0);
    EDGE_VBXE_REG(P1);
    EDGE_VBXE_REG(P2);
    EDGE_VBXE_REG(P3);
    EDGE_VBXE_REG(MEMAC_B_CONTROL);
    EDGE_VBXE_REG(MEMAC_CONTROL);
    EDGE_VBXE_REG(MEMAC_BANK_SEL);

#undef EDGE_VBXE_REG
};

// ── Register bit constants (FX Core v1.26 manual, pp. 40-44) ──────────

namespace video_control {   // VIDEO_CONTROL
inline constexpr u8 XDL_ENABLED = 0x01;
inline constexpr u8 XCOLOR      = 0x02;
inline constexpr u8 NO_TRANS    = 0x04;
inline constexpr u8 TRANS15     = 0x08;
} // namespace video_control

namespace blitter_start {   // BLITTER_START (W)
inline constexpr u8 START = 0x01;   // b0 1=start, 0=stop
} // namespace blitter_start

namespace blitter_busy {    // BLITTER_BUSY (R)
inline constexpr u8 BCB_LOAD = 0x01;
inline constexpr u8 BUSY     = 0x02;
} // namespace blitter_busy

namespace irq_control {     // IRQ_CONTROL (W)
inline constexpr u8 IRQE = 0x01;
} // namespace irq_control

namespace irq_status {      // IRQ_STATUS (R)
inline constexpr u8 IRQF = 0x01;
} // namespace irq_status

namespace memac_control {   // MEMAC_CONTROL (MEMAC-A)
// Window size in b0-b1 (matches vbxe::WindowSize enum values).
inline constexpr u8 SIZE_4K  = 0x00;
inline constexpr u8 SIZE_8K  = 0x01;
inline constexpr u8 SIZE_16K = 0x02;
inline constexpr u8 SIZE_32K = 0x03;
inline constexpr u8 MAE      = 0x04;   // b2 MEMAC ANTIC ENABLE
inline constexpr u8 MCE      = 0x08;   // b3 MEMAC CPU ENABLE
// Base address nibble lives in b4-b7. page_hi is the CPU base high byte (e.g.
// 0xB0 for a $B000 window); its high nibble is the address field already in
// position, so masking to b4-b7 is all that's needed.
inline constexpr u8 base_field(u8 page_hi) { return static_cast<u8>(page_hi & 0xF0); }
} // namespace memac_control

namespace memac_bank_sel {  // MEMAC_BANK_SEL (MEMAC-A)
inline constexpr u8 MGE = 0x80;        // b7 MEMAC GLOBAL ENABLE; bank in b0-b6
} // namespace memac_bank_sel

namespace memac_b_control { // MEMAC_B_CONTROL (MEMAC-B)
inline constexpr u8 MBCE = 0x80;       // b7 MEMAC-B CPU ENABLE
inline constexpr u8 MBAE = 0x40;       // b6 MEMAC-B ANTIC ENABLE; bank 0-31 in b0-b4
} // namespace memac_b_control

} // namespace atari::vbxe

#endif // ENGINE_PLATFORM_ATARI_VBXE_REGISTERS_H
