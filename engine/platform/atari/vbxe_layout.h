#ifndef ENGINE_PLATFORM_ATARI_VBXE_LAYOUT_H
#define ENGINE_PLATFORM_ATARI_VBXE_LAYOUT_H

// platform/atari/vbxe_layout.h — VRAM layout constants for a VBXE Config.
//
// Carves the 512KB VBXE VRAM into fixed regions: the framebuffer(s) at the
// configured vram_offset, a shapes/sprite area, then a fixed tail (XDL, the
// blitter BCB queue, fonts) placed at the top, with the remainder left as a
// user area. All addresses are linear VRAM offsets (0 .. 0x80000) suitable for
// MemacWindow read/write. Compile-time only — no storage, no runtime code.
//
// The Attribute Map is deferred (0 bytes) until hybrid ANTIC+VBXE rendering
// lands; it will later be carved from the user_area or shapes region.

#include "../../types.h"
#include "vbxe_config.h"

namespace atari::vbxe {

using engine::u32;

template <typename Config>
struct VRAMLayout {
    static constexpr u32 vram_size = 0x80000;   // 512KB
    static constexpr u32 base = Config::vram_offset;

    // Framebuffer A
    static constexpr u32 fb_a = base;
    static constexpr u32 fb_a_size = Config::fb_bytes;

    // Framebuffer B (only if double-buffered)
    static constexpr u32 fb_b = fb_a + fb_a_size;
    static constexpr u32 fb_b_size =
        (Config::buffer_policy == Buffers::Double) ? Config::fb_bytes : 0;

    // Shape/sprite data area: everything between the framebuffers and the fixed
    // tail regions below.
    static constexpr u32 shapes = fb_b + fb_b_size;

    // Fixed tail region sizes.
    static constexpr u32 xdl_size       = 2048;    // XDL (small)
    static constexpr u32 bcb_queue_size = 4096;    // blitter command staging
    static constexpr u32 fonts_size     = 61440;   // 60KB font/tileset area
    static constexpr u32 fixed_tail     = xdl_size + bcb_queue_size + fonts_size;

    // Tail placed at the top of VRAM; shapes get the gap before it.
    static constexpr u32 shapes_end  = vram_size - fixed_tail;
    static constexpr u32 shapes_size = shapes_end - shapes;

    static constexpr u32 xdl       = shapes_end;
    static constexpr u32 bcb_queue = xdl + xdl_size;
    static constexpr u32 fonts     = bcb_queue + bcb_queue_size;

    // Free/user VRAM (everything remaining up to 512KB).
    static constexpr u32 user_area      = fonts + fonts_size;
    static constexpr u32 user_area_size = vram_size - user_area;

    // Attribute Map: reserved for future use (deferred -- 0 bytes now).
    static constexpr u32 attr_map      = 0;
    static constexpr u32 attr_map_size = 0;

    static_assert(fonts + fonts_size <= vram_size,
        "VRAM layout exceeds 512KB with current offset and buffer policy");
    static_assert(shapes <= shapes_end,
        "framebuffers + offset leave no room for shapes before the fixed tail");
    // Single-buffered: framebuffer B is zero-sized and coincides with shapes.
    static_assert(fb_b_size != 0 || fb_b == shapes,
        "single-buffer layout: empty fb_b must coincide with shapes start");
};

} // namespace atari::vbxe

#endif // ENGINE_PLATFORM_ATARI_VBXE_LAYOUT_H
