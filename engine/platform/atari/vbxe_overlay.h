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

#include "../../sprites.h"   // engine::SpriteFormat (neutral shape pixel format)
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

    // Shape registry: maps a ROM shape pointer to its uploaded VRAM copy. Shapes
    // are bump-allocated in the layout's shapes region and never freed (they live
    // for the run). Keyed by the ROM pointer the generic SpriteManager already
    // holds, so no per-sprite width/format needs to live in LogicalSprite.
    static constexpr u8 kMaxShapes = 32;
    struct ShapeEntry {
        const u8*           rom;
        u32                 vram;
        u8                  w, h;
        engine::SpriteFormat fmt;
    };
    static inline ShapeEntry registry_[kMaxShapes] = {};
    static inline u8  reg_count_  = 0;
    static inline u32 next_shape_ = Layout::shapes;

    // Dirty-rect bookkeeping: each frame erases the small rects the previous frame
    // drew (cheap, vs a full-screen clear) and records this frame's rects for the
    // next erase. kMaxRects*2 BCBs (erase + draw) must fit the queue.
    static constexpr u8 kMaxRects = 8;
    struct Rect { u32 dest; u8 w, h; };
    static inline Rect prev_[kMaxRects] = {};
    static inline Rect cur_[kMaxRects]  = {};
    static inline u8   prev_count_ = 0;
    static inline u8   cur_count_  = 0;

    // Overlay clear/erase colour. 0 = transparent (ANTIC shows through); a non-zero
    // opaque index makes the overlay a solid field. The per-frame clear and the
    // single-buffer dirty-rect erase both use it, so sprites erase back to it.
    static inline u8 bg_color_ = 0;

    static const ShapeEntry* find_shape(const u8* rom) {
        for (u8 i = 0; i < reg_count_; ++i)
            if (registry_[i].rom == rom) return &registry_[i];
        return nullptr;
    }

    // The framebuffer the current frame composes into: the inactive page when
    // double-buffered (flip shows it afterwards), else the single page (tears).
    static u32 back_fb() {
        if constexpr (double_buffered) return active_fb_ ? Layout::fb_a : Layout::fb_b;
        else                           return Layout::fb_a;
    }

    // Build a full-screen XDL for framebuffer at `fb_addr` and upload it to `dest`.
    static void build_and_upload_xdl(u32 fb_addr, u32 dest) {
        u8 buf[24];
        const u8 len = vbxe::build_fullscreen_xdl<Config>(buf, fb_addr, fb_stride, fb_height);
        vbxe::upload_xdl<Config>(buf, len, dest);
    }

    // Fill one whole framebuffer page with a solid colour using the blitter (far
    // faster than a MEMAC memset of 76800 bytes). Synchronous: waits for the fill
    // to finish before returning. The NmiGuard makes it safe to call even after the
    // VBI is armed (init / set_background), since the VBI shares queue_/the blitter.
    static void blit_fill(u32 dest, u8 color) {
        NmiGuard cs;
        queue_.reset();
        queue_.push(vbxe::bcb_fill_rect(dest, fb_stride, fb_height, fb_stride, color));
        queue_.template submit<Config>();
        for (u16 spin = 0; spin < 50000; ++spin)
            if (!queue_.template busy<Config>()) break;
        queue_.reset();
    }

    // ── Neutral seams the engine calls (gated by caps in core.h) ──

    // One-time bring-up: open the MEMAC window, clear the framebuffer(s) to black,
    // build + point at the XDL, and enable XDL processing.
    static void overlay_init() {
        Memac::init();
        blit_fill(Layout::fb_a, 0);
        build_and_upload_xdl(Layout::fb_a, xdl_a);
        if constexpr (double_buffered) {
            blit_fill(Layout::fb_b, 0);
            build_and_upload_xdl(Layout::fb_b, xdl_b);
        }
        active_fb_ = 0;
        vbxe::set_xdl_addr<Config>(xdl_a);
        vbxe::xdl_enable<Config>();
    }

    // Set the overlay background colour and paint the framebuffer(s) with it now,
    // so the field is opaque immediately (and stays so under dirty-rect erasing).
    static void overlay_set_background(u8 color) {
        bg_color_ = color;
        blit_fill(Layout::fb_a, color);
        if constexpr (double_buffered) blit_fill(Layout::fb_b, color);
    }

    // ── Sprite seams (called from the generic SpriteManager) ──

    // Register a shape by its ROM pointer, lazily uploading it to VRAM. Called
    // from sprite() on the main thread (which knows width/format). Packed1bpp is
    // expanded row-by-row to 8bpp (set pixel -> 0xFF, clear -> 0x00); Pixel8bpp
    // is uploaded as-is. No-op if already registered (or registry/shape too big).
    static void overlay_register_shape(const u8* rom, u8 w, u8 h, engine::SpriteFormat fmt) {
        if (find_shape(rom) || reg_count_ >= kMaxShapes) return;
        const u16 bytes = static_cast<u16>(w) * h;
        const u32 vram  = next_shape_;

        if (fmt == engine::SpriteFormat::Pixel8bpp) {
            Memac::write(vram, rom, bytes);
        } else {
            if (w > 64) return;                 // wider than the row scratch
            const u8 ppb = static_cast<u8>(w / 8);   // pixels per source bit (1 or 2)
            u8 row[64];
            for (u8 r = 0; r < h; ++r) {
                u8 bits = rom[r];
                u8 c = 0;
                for (u8 b = 0; b < 8; ++b) {
                    const u8 v = (bits & 0x80) ? 0xFF : 0x00;
                    bits = static_cast<u8>(bits << 1);
                    for (u8 k = 0; k < ppb; ++k) row[c++] = v;
                }
                Memac::write(vram + static_cast<u32>(r) * w, row, w);
            }
        }
        registry_[reg_count_++] = ShapeEntry{ rom, vram, w, h, fmt };
        next_shape_ += bytes;
    }

    // Start a frame's overlay composition. Double-buffered: clear the whole hidden
    // back page to the background colour (cheap to do, invisible until the flip).
    // Single-buffered: erase only the rects the previous frame drew (a full clear
    // would be visible and would race the async blitter), then begin recording
    // this frame's rects.
    static void overlay_frame_begin() {
        queue_.reset();
        if constexpr (double_buffered) {
            queue_.push(vbxe::bcb_clear(back_fb(), fb_stride, fb_height,
                                        fb_stride, bg_color_));
        } else {
            for (u8 i = 0; i < prev_count_; ++i)
                queue_.push(vbxe::bcb_clear(prev_[i].dest, prev_[i].w, prev_[i].h,
                                            fb_stride, bg_color_));
            cur_count_ = 0;
        }
    }

    // Queue one sprite blit into the back buffer at (x, y). Packed1bpp uses the
    // AND-mask to colour the instance; Pixel8bpp carries its own colours. Single-
    // buffered also records the rect so the next frame erases it.
    static void overlay_blit_sprite(const u8* rom, u8 x, u8 y, u8 color) {
        const ShapeEntry* e = find_shape(rom);
        if (!e) return;
        const u32 dest = back_fb() + static_cast<u32>(y) * fb_stride + x;
        if (e->fmt == engine::SpriteFormat::Pixel8bpp)
            queue_.push(vbxe::bcb_sprite(e->vram, dest, e->w, e->h, e->w, fb_stride));
        else
            queue_.push(vbxe::bcb_sprite_colored(e->vram, dest, e->w, e->h,
                                                 e->w, fb_stride, color));
        if constexpr (!double_buffered)
            if (cur_count_ < kMaxRects) cur_[cur_count_++] = Rect{ dest, e->w, e->h };
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
        // Single-buffered: this frame's drawn rects become next frame's erase list.
        if constexpr (!double_buffered) {
            for (u8 i = 0; i < cur_count_; ++i) prev_[i] = cur_[i];
            prev_count_ = cur_count_;
        }
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

    // Present the page composed during the PREVIOUS frame, then flip so this frame
    // composes the other one. Called at the START of frame composition (before the
    // sprite commit), so the page being shown has had a full frame's display time
    // to finish rendering — the wait below returns immediately. This is what keeps
    // the VBI short: we never busy-wait for the CURRENT frame's blit (that would
    // overrun the frame and let the next VBI NMI re-enter frame_service → stack
    // overflow). Single-buffer is a compile-time no-op (composes in place, no flip).
    static void overlay_present() {
        if constexpr (double_buffered) {
            for (u16 spin = 0; spin < 50000; ++spin)
                if (!queue_.template busy<Config>()) break;
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
    static void overlay_present() {}
    static void overlay_register_shape(const u8*, u8, u8, engine::SpriteFormat) {}
    static void overlay_frame_begin() {}
    static void overlay_blit_sprite(const u8*, u8, u8, u8) {}
    static void overlay_set_background(u8) {}
};

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_VBXE_OVERLAY_H
