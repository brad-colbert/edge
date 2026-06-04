#ifndef ENGINE_PLATFORM_ATARI_VBXE_CONFIG_H
#define ENGINE_PLATFORM_ATARI_VBXE_CONFIG_H

// platform/atari/vbxe_config.h — compile-time configuration for the VBXE
// (VideoBoard XE) graphics extension.
//
// This is backend (Atari) code: it holds the VBXE-specific knobs that the
// `gfx::VBXE<Config>` graphics axis carries into the Platform. Nothing here
// leaks into the generic engine layer — the engine only ever sees the neutral
// capability fields (config/capabilities.h). The values below describe a VBXE
// installation: overlay display mode, framebuffer policy, the register decode
// base ($D640 vs $D740), and the MEMAC VRAM window used to reach the 512KB of
// on-board VRAM.
//
// Phase 1 is purely structural — these are types and constants only. The HAL,
// registers, XDL, blitter, and MEMAC runtime arrive in later phases and consume
// this Config (see the Phase 2 MEMAC/register work, which can build on the
// banked-window MMIO primitives keyed off reg_base and the MEMAC window).

#include <stdint.h>

namespace atari::vbxe {

// Register decode base. The VBXE answers at $D640 or $D740 depending on how the
// board is jumpered / which slot it occupies (the manual's "Dx40" with x = 6/7).
enum class RegBase : uint16_t { D640 = 0xD640, D740 = 0xD740 };

// Overlay display mode (the VBXE overlay plane drawn over/under ANTIC output):
//   SR_320  — Standard Resolution, 320 px, 256 colours (1 byte/pixel)
//   HR_640  — High Resolution,     640 px, 16 colours  (1 nibble/pixel)
//   LR_160  — Low Resolution,      160 px, 256 colours
//   Text_80 — 80-column text mode  (char + attribute pairs)
enum class Mode : uint8_t { SR_320, HR_640, LR_160, Text_80 };

// Framebuffer double-buffering policy.
enum class Buffers : uint8_t { Single, Double };

// Background composition policy. Flat: sprites are composed over (and erased back
// to) a single solid colour. Bitmap: sprites are composed over a drawn bitmap
// kept in a VRAM "master" canvas, and their footprints are restored from it each
// frame so the background survives under moving sprites (sprites-over-bitmap).
enum class Background : uint8_t { Flat, Bitmap };

// MEMAC window size (these values are the hardware MEMAC_CONTROL SIZE bits, not
// byte counts — 00=4K, 01=8K, 10=16K, 11=32K; the byte size is derived later).
// MEMAC-A and MEMAC-B are mutually exclusive window choices (never both):
//   MEMAC-A: configurable CPU base page and one of the sizes below.
//   MEMAC-B: fixed at $4000, 16KB.
enum class WindowSize : uint8_t { _4K = 0, _8K = 1, _16K = 2, _32K = 3 };

template <uint8_t BasePage = 0xB0, WindowSize Size = WindowSize::_4K>
struct MEMAC_A_Cfg {
    static constexpr uint8_t base_page = BasePage;
    static constexpr WindowSize size = Size;
};
using MEMAC_A = MEMAC_A_Cfg<>;  // default $B000, 4KB

struct MEMAC_B {};

// Full VBXE configuration carried by gfx::VBXE<Config>.
template <
    Mode       OverlayMode = Mode::SR_320,
    Buffers    BufferPolicy = Buffers::Single,
    RegBase    Base = RegBase::D640,
    typename   MemacCfg = MEMAC_A,
    uint32_t   VRAMOffset = 0x00000,   // for SDX VBXE driver coexistence
    Background Bg = Background::Flat
>
struct Config {
    static constexpr Mode    overlay_mode  = OverlayMode;
    static constexpr Buffers buffer_policy = BufferPolicy;
    static constexpr RegBase reg_base      = Base;
    using memac = MemacCfg;
    static constexpr uint32_t vram_offset  = VRAMOffset;
    static constexpr Background background  = Bg;

    // Derived: framebuffer size in bytes
    static constexpr uint32_t fb_width =
        (OverlayMode == Mode::SR_320) ? 320 :
        (OverlayMode == Mode::HR_640) ? 320 :  // 640 pixels but 4bpp = 320 bytes/line
        (OverlayMode == Mode::LR_160) ? 160 :
        160;  // Text_80: 160 bytes/line (80 chars x 2 bytes)
    static constexpr uint32_t fb_height = 240;
    static constexpr uint32_t fb_bytes = fb_width * fb_height;
};

using DefaultConfig = Config<>;

} // namespace atari::vbxe

#endif // ENGINE_PLATFORM_ATARI_VBXE_CONFIG_H
