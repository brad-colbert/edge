#ifndef ENGINE_PLATFORM_ATARI_VBXE_OVERLAY_H
#define ENGINE_PLATFORM_ATARI_VBXE_OVERLAY_H

// platform/atari/vbxe_overlay.h — the VBXE overlay HAL seam.
//
// This is the backend half of the portable engine's graphics seam. The generic
// engine (engine/core.h) reaches VBXE only through neutral, capability-gated
// `overlay_*` static methods — it never names a VBXE type. All VBXE state (the
// blitter queue, the active-framebuffer index) and all VBXE types (Config, the
// MEMAC window, the XDL/blitter/layout helpers) live here, behind the seam.
//
// The Platform bundles OverlayHal<Config> into Platform::hal (see platform.h) so
// `Platform::hal::overlay_init()` etc. resolve alongside the baseline Hal
// methods. Non-VBXE platforms get NullOverlay (no-op seams). Phase 4a wires the
// bring-up spine (init / submit / collision / flip); the sprite-commit and
// bitmap paths that populate the queue land in 4b.

#include "../../types.h"
#include "vbxe_blitter.h"
#include "vbxe_config.h"
#include "vbxe_layout.h"
#include "vbxe_memac.h"
#include "vbxe_registers.h"
#include "vbxe_xdl.h"

namespace atari {

using engine::u8;
using engine::u16;
using engine::u32;

// Overlay seam for a VBXE Config. All members static (the HAL is stateless from
// the engine's view; the VBXE state below is per-Config static storage).
template <typename Config>
struct OverlayHal {
    using Layout = vbxe::VRAMLayout<Config>;
    using Memac  = vbxe::MemacWindow<Config>;
    using R      = vbxe::Regs<Config>;

    static constexpr u16 fb_stride = static_cast<u16>(Config::fb_width);
    static constexpr u8  fb_height = 240;                       // overlay scanlines
    static constexpr bool double_buffered =
        (Config::buffer_policy == vbxe::Buffers::Double);

    // Two XDL copies in the reserved XDL region (single-buffer uses only the first).
    static constexpr u32 xdl_a = Layout::xdl;
    static constexpr u32 xdl_b = Layout::xdl + 32;

    // ── Backend VBXE state (lives here, never in the generic engine) ──
    static inline vbxe::BlitterQueue<16> queue_{};
    static inline u8 active_fb_ = 0;       // 0 = framebuffer A shown, 1 = B

    // Build a full-screen XDL for framebuffer at `fb_addr` and upload it to `dest`.
    static void build_and_upload_xdl(u32 fb_addr, u32 dest) {
        u8 buf[24];
        const u8 len = vbxe::build_fullscreen_xdl<Config>(buf, fb_addr, fb_stride, fb_height);
        vbxe::upload_xdl<Config>(buf, len, dest);
    }

    // ── Neutral seams the engine calls (gated by caps in core.h) ──

    // One-time bring-up: open the MEMAC window, clear the framebuffer(s) to black,
    // build + point at the XDL, and enable XDL processing.
    static void overlay_init() {
        Memac::init();
        Memac::fill(Layout::fb_a, Config::fb_bytes, 0);
        build_and_upload_xdl(Layout::fb_a, xdl_a);
        if constexpr (double_buffered) {
            Memac::fill(Layout::fb_b, Config::fb_bytes, 0);
            build_and_upload_xdl(Layout::fb_b, xdl_b);
        }
        active_fb_ = 0;
        vbxe::set_xdl_addr<Config>(xdl_a);
        vbxe::xdl_enable<Config>();
    }

    // These VBI-time seams touch VBXE core registers. They do NOT need to guard
    // the MEMAC bank: main-thread VBXE operations run inside an NmiGuard
    // (vbxe_memac.h / vbxe_palette.h), so the VBI can never preempt one — the two
    // contexts never interleave their VBXE access.

    // Submit the frame's queued blitter commands (no-op while the queue is empty,
    // i.e. until the 4b sprite/bitmap paths push BCBs).
    static void overlay_submit() {
        queue_.template submit<Config>();
        queue_.reset();
    }

    // Latch the overlay-vs-playfield/PMG raster collision and clear it for next frame.
    static u8 overlay_collision() {
        const u8 c = static_cast<u8>(R::COLDETECT);
        R::COLCLR = 0;                        // any write clears COLDETECT
        return c;
    }

    // Latch the blitter (overlay-vs-overlay) collision code.
    static u8 overlay_blit_collision() {
        return static_cast<u8>(R::BLT_COLLISION);
    }

    // Double-buffer page flip: show the other framebuffer by repointing the XDL.
    // Single-buffer is a compile-time no-op (the engine never sees the policy).
    static void overlay_flip() {
        if constexpr (double_buffered) {
            active_fb_ = static_cast<u8>(active_fb_ ^ 1);
            vbxe::set_xdl_addr<Config>(active_fb_ ? xdl_b : xdl_a);
        }
    }
};

// Overlay seam for non-VBXE platforms: every operation is a no-op. The generic
// engine only calls these inside `if constexpr (caps::has_blitter/...)`, but the
// names exist so a HalBundle that inherits this always compiles.
struct NullOverlay {
    static void overlay_init() {}
    static void overlay_submit() {}
    static u8   overlay_collision() { return 0; }
    static u8   overlay_blit_collision() { return 0; }
    static void overlay_flip() {}
};

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_VBXE_OVERLAY_H
