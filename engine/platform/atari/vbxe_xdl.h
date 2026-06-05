#ifndef ENGINE_PLATFORM_ATARI_VBXE_XDL_H
#define ENGINE_PLATFORM_ATARI_VBXE_XDL_H

// platform/atari/vbxe_xdl.h — the XDL (eXtended Display List) builder.
//
// The XDL is VBXE's equivalent of the ANTIC display list, but it lives in VRAM
// and controls the overlay plane (FX Core v1.26 manual, "THE XDL" pp. 8-14).
// build_fullscreen_xdl() emits the common single-record case into a small CPU
// scratch buffer (pure byte writes, constexpr so the layout is unit-testable);
// the hardware helpers upload it via MEMAC and point the VBXE at it.
//
// Byte-emit style mirrors the ANTIC builder in display_list.h. Each XDLC is a
// 2-byte little-endian control word followed by additional data in a FIXED order
// (manual p14): RPTL, OVADR, OVSCRL, CHBASE, MAPADR, MAPPAR, ATT — present only
// for the bits set in the control word. Max 20 data bytes per record.

#include "../../types.h"
#include "vbxe_config.h"
#include "vbxe_layout.h"
#include "vbxe_memac.h"
#include "vbxe_registers.h"

namespace atari::vbxe {

using engine::u8;
using engine::u16;
using engine::u32;

// XDLC control-word bits (manual pp. 9-13).
namespace xdlc {
    inline constexpr u16 TMON   = 0x0001;  // enable overlay text mode
    inline constexpr u16 GMON   = 0x0002;  // enable overlay graphic mode
    inline constexpr u16 OVOFF  = 0x0004;  // disable overlay
    inline constexpr u16 MAPON  = 0x0008;  // enable colour attributes
    inline constexpr u16 MAPOFF = 0x0010;  // disable colour attributes
    inline constexpr u16 RPTL   = 0x0020;  // repeat line N times (1 data byte)
    inline constexpr u16 OVADR  = 0x0040;  // set overlay address + step (5 bytes)
    inline constexpr u16 OVSCRL = 0x0080;  // set text scroll values (2 bytes)
    inline constexpr u16 CHBASE = 0x0100;  // set character base (1 byte)
    inline constexpr u16 MAPADR = 0x0200;  // set attribute map addr + step (5 bytes)
    inline constexpr u16 MAPPAR = 0x0400;  // set map scroll/width/height (4 bytes)
    inline constexpr u16 ATT    = 0x0800;  // set display size + priority (2 bytes)
    inline constexpr u16 HR     = 0x1000;  // enable hi-res pixel mode (640px)
    inline constexpr u16 LR     = 0x2000;  // enable low-res pixel mode (160px)
    // bit 2.6 reserved
    inline constexpr u16 END    = 0x8000;  // last XDL record, wait for VSYNC
} // namespace xdlc

// ATT byte 1 fields (manual p12): b0-b1 OV_WIDTH, b4-b5 XDL OV PALETTE,
// b6-b7 XDL PF PALETTE.
namespace ov_width {
    inline constexpr u8 NARROW = 0;   // 256px
    inline constexpr u8 NORMAL = 1;   // 320px
    inline constexpr u8 WIDE   = 2;   // 336px
} // namespace ov_width

// The overlay reads its colours from this hardware palette. Palette 1 is the
// manual's default for the overlay (palette 0 is the GTIA/playfield palette),
// and is where the engine/game uploads the overlay colours. The field occupies
// b4-b5 of ATT byte 1.
inline constexpr u8 ATT_OV_PALETTE = 1;

// Build a single-record full-screen overlay XDL into `buf`. Returns bytes used
// (<= 22). `fb_addr` is the 19-bit VRAM framebuffer address; `fb_stride` the
// per-line byte step (added after each scanline in pixel modes); `height` the
// number of scanlines. Graphic modes use GMON (+ HR/LR per Config::overlay_mode);
// VBXE_T80 uses TMON + CHBASE (font page from the VRAM layout).
template <typename Config>
constexpr u8 build_fullscreen_xdl(u8* buf, u32 fb_addr, u16 fb_stride, u8 height) {
    constexpr atari::Mode mode = Config::overlay_mode;
    constexpr bool is_text = (mode == atari::Mode::VBXE_T80);

    u16 ctrl = xdlc::RPTL | xdlc::OVADR | xdlc::ATT | xdlc::END;
    if constexpr (is_text) {
        ctrl |= xdlc::TMON | xdlc::CHBASE;
    } else {
        ctrl |= xdlc::GMON;
        if constexpr (mode == atari::Mode::VBXE_HR) ctrl |= xdlc::HR;
        else if constexpr (mode == atari::Mode::VBXE_LR) ctrl |= xdlc::LR;
        // VBXE_SR: neither HR nor LR.
    }

    u8 p = 0;
    buf[p++] = static_cast<u8>(ctrl & 0xFF);          // XDLC low
    buf[p++] = static_cast<u8>(ctrl >> 8);            // XDLC high

    // Data, in the manual's fixed order for the bits set above.
    buf[p++] = static_cast<u8>(height - 1);           // RPTL: repeat count
    buf[p++] = static_cast<u8>(fb_addr & 0xFF);       // OVADR[7:0]
    buf[p++] = static_cast<u8>((fb_addr >> 8) & 0xFF);// OVADR[15:8]
    buf[p++] = static_cast<u8>((fb_addr >> 16) & 0x07);// OVADR[18:16]
    buf[p++] = static_cast<u8>(fb_stride & 0xFF);     // OVSTEP[7:0]
    buf[p++] = static_cast<u8>((fb_stride >> 8) & 0x0F);// OVSTEP[11:8]
    if constexpr (is_text) {
        // CHBASE: font base in 2K pages within VBXE VRAM.
        buf[p++] = static_cast<u8>(VRAMLayout<Config>::fonts >> 11);
    }
    // ATT byte1: NORMAL width + select OV palette 1 (b4-b5). Selecting the
    // overlay's palette here is required — leaving the field 0 forces palette 0
    // (the playfield palette), so overlay colours would come from the wrong set.
    buf[p++] = static_cast<u8>(ov_width::NORMAL | (ATT_OV_PALETTE << 4));
    buf[p++] = 0xFF;                                  // ATT byte2: priority (overlay on top)
    return p;
}

// ── Hardware helpers ─────────────────────────────────────────────────

// Upload a built XDL to VRAM at `vram_dest` (e.g. VRAMLayout<Config>::xdl).
template <typename Config>
void upload_xdl(const u8* buf, u8 len, u32 vram_dest) {
    MemacWindow<Config>::write(vram_dest, buf, len);
}

// Point the VBXE at the XDL at `vram_addr` (19-bit).
template <typename Config>
void set_xdl_addr(u32 vram_addr) {
    Regs<Config>::XDL_ADR0 = static_cast<u8>(vram_addr & 0xFF);
    Regs<Config>::XDL_ADR1 = static_cast<u8>((vram_addr >> 8) & 0xFF);
    Regs<Config>::XDL_ADR2 = static_cast<u8>((vram_addr >> 16) & 0x07);
}

// Start / stop XDL processing. NOTE: VIDEO_CONTROL is write-only (its read side
// is CORE_VERSION), so it cannot be read-modified-written — the full control
// byte must be composed on each write. `extra_flags` lets the caller fold in the
// other VIDEO_CONTROL bits (XCOLOR, NO_TRANS, TRANS15) in the same store; Phase 4
// owns the canonical control value.
template <typename Config>
void xdl_enable(u8 extra_flags = 0) {
    Regs<Config>::VIDEO_CONTROL = static_cast<u8>(video_control::XDL_ENABLED | extra_flags);
}
template <typename Config>
void xdl_disable(u8 extra_flags = 0) {
    Regs<Config>::VIDEO_CONTROL = extra_flags;   // XDL_ENABLED cleared
}

} // namespace atari::vbxe

#endif // ENGINE_PLATFORM_ATARI_VBXE_XDL_H
