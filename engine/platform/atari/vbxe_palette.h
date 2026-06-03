#ifndef ENGINE_PLATFORM_ATARI_VBXE_PALETTE_H
#define ENGINE_PLATFORM_ATARI_VBXE_PALETTE_H

// platform/atari/vbxe_palette.h — VBXE RGB palette management.
//
// The VBXE has four 256-colour palettes in dedicated WRITE-ONLY registers (not
// VRAM). A colour is set by writing PSEL (palette 0-3), CSEL (index 0-255), then
// CR/CG/CB (7-bit components in bits 7..1). Writing CB auto-increments CSEL, so a
// whole palette streams as PSEL once, CSEL=0 once, then 256× (CR,CG,CB)
// (FX Core v1.26 manual, "RGB PALETTE MODIFICATION" p26).
//
// Because the registers cannot be read back, the engine ships a ROM copy of the
// power-on palette 0 (laoo_palette.h) and uploads it back to restore system
// state on exit.

#include "../../types.h"
#include "laoo_palette.h"
#include "nmi.h"             // atari::NmiGuard — palette writes must not race the VBI
#include "vbxe_registers.h"

namespace atari::vbxe {

using engine::u8;
using engine::u16;

// One palette entry. Components are 7-bit values pre-shifted into bits 7..1
// (the form CR/CG/CB expect; hardware ignores bit 0).
struct PaletteEntry {
    u8 r, g, b;
};

// A compile-time palette (default 256 entries).
template <u16 N = 256>
struct Palette {
    PaletteEntry entries[N];
};

namespace detail {
constexpr Palette<256> make_laoo_palette() {
    Palette<256> p{};
    for (u16 i = 0; i < 256; ++i) {
        p.entries[i] = PaletteEntry{laoo_pal_r[i], laoo_pal_g[i], laoo_pal_b[i]};
    }
    return p;
}
} // namespace detail

// The default palette 0 (laoo.act GTIA-compatible colours), ROM-resident. The
// engine uploads this back to palette 0 on exit to restore the system state.
inline constexpr Palette<256> default_palette_0 = detail::make_laoo_palette();

// Upload a palette to a VBXE hardware palette (palette_index 0-3). Streams via
// CSEL auto-increment: set PSEL + CSEL once, then write CR/CG/CB per entry.
// CAUTION: takes effect immediately (unbuffered); uploading mid-frame causes
// visible artifacts unless timed in VBLANK.
template <typename Config, u16 N>
void upload_palette(u8 palette_index, const Palette<N>& pal) {
    using R = Regs<Config>;
    NmiGuard cs;   // the PSEL/CSEL/CR/CG/CB stream must not be interleaved by the VBI
    R::PSEL = palette_index;
    R::CSEL = 0;
    for (u16 i = 0; i < N; ++i) {
        R::CR = pal.entries[i].r;
        R::CG = pal.entries[i].g;
        R::CB = pal.entries[i].b;   // auto-increments CSEL
    }
}

// Set a single colour (instant effect on screen if that palette/index shows).
template <typename Config>
void set_color(u8 palette, u8 index, u8 r, u8 g, u8 b) {
    using R = Regs<Config>;
    NmiGuard cs;   // PSEL/CSEL/CR/CG/CB is a multi-write sequence; keep it atomic vs the VBI
    R::PSEL = palette;
    R::CSEL = index;
    R::CR = r;
    R::CG = g;
    R::CB = b;
}

// Restore palette 0 to system defaults — call on program exit (the manual
// requires it, since a program may have altered the shared GTIA-compatible
// palette 0 that the OS/other software rely on).
template <typename Config>
void restore_default_palette() {
    upload_palette<Config, 256>(0, default_palette_0);
}

} // namespace atari::vbxe

#endif // ENGINE_PLATFORM_ATARI_VBXE_PALETTE_H
