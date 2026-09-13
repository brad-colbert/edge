#ifndef ENGINE_PLATFORM_ATARI_NMI_H
#define ENGINE_PLATFORM_ATARI_NMI_H

// platform/atari/nmi.h — NMIEN management + a VBXE critical section.
//
// NMIEN ($D40E) is write-only (reads return NMIST), so the engine can't read the
// current enable mask back — it must track an intended value in a shadow. All
// intended NMIEN changes funnel through nmien_set() so the shadow stays accurate.
//
// NmiGuard is a RAII critical section that masks ALL NMI for its scope and
// restores the intended mask on exit. It exists because the VBXE shares internal
// decode state between its core registers ($D6xx) and the MEMAC CPU window
// ($B000): if the VBI (an NMI) touches a VBXE register in the middle of a
// main-thread MEMAC window burst (or a multi-register palette upload), the
// transfer is corrupted. Bracketing a main-thread VBXE operation in an NmiGuard
// makes it atomic with respect to the VBI — the VBI simply cannot fire during it.
// (Confirmed on hardware via the bring-up A/B probe: with the VBI unmasked a
// burst drops ~one byte per VBI; masked, it is clean.)
//
// Depends only on registers.h (hardware documentation).

#include "registers.h"

namespace atari {

// The engine's intended NMIEN. The OS leaves NMIEN = VBI at startup, so default
// to that; install_frame_isr and the DLI enable/disable paths keep it current.
inline engine::u8 g_nmien_shadow = nmien::VBI;

// The single funnel for intended NMIEN changes: update the shadow, then the chip.
inline void nmien_set(engine::u8 v) {
    g_nmien_shadow = v;
    *reg::NMIEN = v;
}

// Nesting depth for NmiGuard so a guarded VBXE op that calls another guarded op
// doesn't unmask early. Safe without atomics: while masked (depth > 0) the VBI
// can't fire, so the counter is never modified concurrently.
inline engine::u8 g_nmi_guard_depth = 0;

// RAII VBXE critical section: mask all NMI on entry (outermost only), restore the
// intended mask on exit (outermost only). (-fno-exceptions safe: the destructor
// just stores the shadow back.)
struct NmiGuard {
    NmiGuard() noexcept {
        if (g_nmi_guard_depth++ == 0) *reg::NMIEN = 0x00;
    }
    ~NmiGuard() noexcept {
        if (--g_nmi_guard_depth == 0) *reg::NMIEN = g_nmien_shadow;
    }
    NmiGuard(const NmiGuard&) = delete;
    NmiGuard& operator=(const NmiGuard&) = delete;
};

// The same critical section for a LEAF body -- a few stores that open no guard of
// their own and call nothing that does. Masking is unconditional (a store of zero
// under an enclosing guard changes nothing) and only the restore is gated, on the
// enclosing depth rather than on a count of its own: inside an NmiGuard it leaves
// NMI masked for the outer scope to restore, outside one it restores the shadow.
// No counter is kept because nothing can observe one -- no guard nests inside a
// leaf body, and no NMI can run inside it to open one.
//
// It exists for size, not speed: it drops NmiGuard's counter bookkeeping, which
// matters where a single guarded write is the only guarded code a program links
// (Hal::set_raster_vector). Anything larger than a leaf takes NmiGuard.
//
// The mask is stored TWICE, and the second store is not redundant. ANTIC asserts a
// DLI's NMI on cycle 8 of the line, a disabling NMIEN write must land by cycle 8 to
// withdraw it, and the CPU enters the handler at the first instruction boundary from
// cycle 10 on (Altirra Hardware Reference Manual, 4.8). A mask store that lands on
// cycle 9 is therefore too late for an NMI that is already on its way, and that NMI
// is taken one instruction AFTER the store -- which, with no settle, is the first
// store of the body. Measured with the raster-vector tear probe: a leaf guard whose
// mask store was followed directly by the first VDSLST store tore 258 times in 180
// frames on Altirra. The second store is that one instruction; the NMI lands on
// the boundary after it, with the body not yet begun. Four cycles, where one
// instruction of any length is the requirement, for margin. It is volatile, so the
// optimiser can neither drop it nor move it past the body.
struct NmiLeafGuard {
    NmiLeafGuard() noexcept {
        *reg::NMIEN = 0x00;
        *reg::NMIEN = 0x00;   // settle: a pending NMI is taken here, not in the body
    }
    ~NmiLeafGuard() noexcept {
        if (g_nmi_guard_depth == 0) *reg::NMIEN = g_nmien_shadow;
    }
    NmiLeafGuard(const NmiLeafGuard&) = delete;
    NmiLeafGuard& operator=(const NmiLeafGuard&) = delete;
};

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_NMI_H
