#ifndef ENGINE_PLATFORM_ATARI_VBXE_MEMAC_H
#define ENGINE_PLATFORM_ATARI_VBXE_MEMAC_H

// platform/atari/vbxe_memac.h — MEMAC window manager.
//
// The CPU reaches the VBXE's 512KB VRAM through a small banked aperture in 6502
// address space (MEMAC). This is a low-level utility the engine uses internally
// to read/write VRAM; it is parameterised by the VBXE Config and supports both
// window flavours (FX Core v1.26 manual, "MEMAC" pp. 27-29):
//   MEMAC-A: CPU base $Y000 (Config::memac::base_page), size 4K/8K/16K/32K.
//   MEMAC-B: fixed $4000, 16KB.
// The bank-switch boundary math lives entirely in engine::MmioBankedWindow, so
// it exists in exactly one (tested) place; MemacWindow only derives the four
// window parameters from the Config and drives the init/enable registers.

#include "../../mmio.h"
#include "../../types.h"
#include "nmi.h"             // atari::NmiGuard — VBXE access must not race the VBI
#include "registers.h"       // atari::reg::PORTB (MEMAC-B EXTSEL warning)
#include "vbxe_config.h"
#include "vbxe_registers.h"

namespace atari::vbxe {

using engine::u8;
using engine::u16;
using engine::u32;

// is_memac_b<M>::value — true iff the MEMAC config is the fixed $4000/16K window.
// Uses the engine `static constexpr bool value` trait idiom (cf. detail::same
// in screen.h).
template <typename M> struct is_memac_b              { static constexpr bool value = false; };
template <>           struct is_memac_b<MEMAC_B>     { static constexpr bool value = true; };

// Window geometry per MEMAC flavour. The primary template covers any MEMAC_A_Cfg
// instantiation (it reads base_page/size); the MEMAC_B specialisation touches
// neither member, so referencing the geometry never instantiates a missing field.
template <typename M> struct memac_geom {            // MEMAC-A
    static constexpr u16 cpu_base     = static_cast<u16>(M::base_page) << 8;
    static constexpr u32 window_bytes = 4096u << static_cast<u8>(M::size);
};
template <> struct memac_geom<MEMAC_B> {             // MEMAC-B: fixed $4000/16K
    static constexpr u16 cpu_base     = 0x4000;
    static constexpr u32 window_bytes = 16384u;
};

template <typename Config>
struct MemacWindow {
    using R     = Regs<Config>;
    using Memac = typename Config::memac;

    static constexpr bool is_b          = is_memac_b<Memac>::value;
    static constexpr u16  cpu_base       = memac_geom<Memac>::cpu_base;
    static constexpr u32  window_bytes   = memac_geom<Memac>::window_bytes;
    static constexpr u16  bank_sel_addr  = is_b ? R::MEMAC_B_CONTROL_ADDR
                                                : R::MEMAC_BANK_SEL_ADDR;
    static constexpr u8   bank_or_mask   = is_b ? memac_b_control::MBCE
                                                : memac_bank_sel::MGE;

    // The banked window owns the bank/offset split and boundary crossing. The
    // OR-mask keeps the global-enable bit (MGE for A, MBCE for B) set on every
    // bank write, so a bank select is a single store.
    using Window = engine::MmioBankedWindow<cpu_base, window_bytes,
                                            bank_sel_addr, bank_or_mask>;

    // Configure the window once at init. ANTIC access (MAE/MBAE) is left off:
    // the engine reads/writes VRAM from the CPU side only.
    static void init() {
        if constexpr (is_b) {
            // NOTE: if a PORTB-based RAM expansion is present, the caller must
            // write $FF to PORTB ($D301) before MEMAC-B access (EXTSEL takes
            // priority over MEMAC-B otherwise — manual MEMAC warning). Whether
            // that is needed depends on the RAM axis, so it is the caller's
            // responsibility, not an unconditional clobber here.
            R::MEMAC_B_CONTROL = memac_b_control::MBCE;   // bank 0, CPU enable
        } else {
            R::MEMAC_CONTROL = static_cast<u8>(
                memac_control::base_field(static_cast<u8>(cpu_base >> 8))
                | static_cast<u8>(Memac::size)            // size bits b0-b1
                | memac_control::MCE);                    // CPU enable (MAE clear)
            R::MEMAC_BANK_SEL = memac_bank_sel::MGE;       // bank 0 + global enable
        }
    }

    // Make `bank` the VRAM bank visible through the window (keeps the enable bit).
    static void set_bank(u8 bank) { Window{}.bank(bank); }

    // Toggle the window's global-enable bit (re-selecting bank 0).
    static void enable()  {
        if constexpr (is_b) R::MEMAC_B_CONTROL = memac_b_control::MBCE;
        else                R::MEMAC_BANK_SEL  = memac_bank_sel::MGE;
    }
    static void disable() {
        if constexpr (is_b) R::MEMAC_B_CONTROL = 0;
        else                R::MEMAC_BANK_SEL  = 0;
    }

    // Bulk transfers between CPU RAM and VRAM. vram_addr is a linear VRAM offset
    // (0 .. 512K); the window handles any bank boundaries the run crosses.
    //
    // Two-part hazard fix (both required), proven on hardware via the bring-up
    // probe: a VBI that touches a VBXE $D6xx register while the MEMAC window is
    // ENABLED (MGE set) corrupts VRAM at the window's current address — whether
    // or not the CPU is mid-burst. So each transfer (1) runs inside an NmiGuard
    // (no VBI while MGE is up for the burst), and (2) closes the window (clears
    // MGE) before re-enabling NMI, so VBIs between transfers see a disabled
    // window and can't corrupt VRAM. MGE only gates the CPU aperture; the overlay
    // video fetch reads VRAM independently, so the display is unaffected.
    static void write(u32 vram_addr, const u8* src, u16 len) {
        NmiGuard cs;
        Window{}.copy(vram_addr, src, len);
        disable();
    }
    static void read(u32 vram_addr, u8* dst, u16 len) {
        NmiGuard cs;
        Window{}.read(vram_addr, dst, len);
        disable();
    }
    static void fill(u32 vram_addr, u32 len, u8 value) {
        NmiGuard cs;
        Window{}.fill(vram_addr, len, value);
        disable();
    }
};

} // namespace atari::vbxe

#endif // ENGINE_PLATFORM_ATARI_VBXE_MEMAC_H
