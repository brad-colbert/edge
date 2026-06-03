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

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_NMI_H
