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
    // sprites-over-bitmap: compose over (and restore from) a VRAM master canvas
    // instead of a flat colour. Drawing targets the master; the compositor copies
    // master->framebuffer under sprite footprints rather than clearing to bg_color_.
    static constexpr bool bitmap_bg =
        (Config::background == vbxe::Background::Bitmap);

    // Two XDL copies in the reserved XDL region (single-buffer uses only the first).
    static constexpr u32 xdl_a = Layout::xdl;
    static constexpr u32 xdl_b = Layout::xdl + 32;

    // ── Backend VBXE state (lives here, never in the generic engine) ──
    // Queue depth covers the worst-case per-frame chain: 2 BCBs (restore + draw) per
    // logical sprite when every slot changes at once (e.g. just after a background
    // republish). Sized to 2*kMaxSlots; the VRAM bcb_queue region (4K) holds far more.
    static constexpr u8 kQueueDepth = 24;
    static inline vbxe::BlitterQueue<kQueueDepth> queue_{};
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

    // Per-slot, per-page sprite cache (skip-unchanged compositing). The generic
    // SpriteManager commits each frame's sprites by logical slot; we remember, for
    // each page, what is currently painted there per slot, and each frame only erase+
    // redraw the slots whose state changed (position/shape/colour/visibility) since
    // this page was last composed. A stationary sprite then costs ZERO blitter work.
    //
    // Per-PAGE because double-buffering composes the alternate page each frame, so a
    // change must be repainted on both pages (it naturally is: the slot reads dirty
    // against each page until both match). Used for single-buffer (any background) AND
    // double-buffer Bitmap (dirty-rect modes). Double-buffer Flat full-clears the page
    // every frame, so it can't skip — it redraws all desired slots (cache unused).
    static constexpr u8 kMaxSlots = 12;   // logical blitter sprites (arena VBXE: player + 7 enemies + 4 bullets)
    static_assert(kMaxSlots * 2 <= kQueueDepth,
        "erase+draw BCBs (one each per dirty slot) must fit the blitter queue");
    // A slot caches the resolved shape entry (e) so the per-frame commit never
    // re-scans the registry; e carries vram/w/h/fmt. e==nullptr ⇒ slot not present.
    struct Slot { u8 x, y, color; const ShapeEntry* e; bool valid; };
    static inline Slot painted_[2][kMaxSlots] = {};   // currently on each page, per slot
    static inline Slot desired_[kMaxSlots]    = {};   // requested this frame (valid = present)
    static inline u8   draw_order_[kMaxSlots] = {};   // slots in back-to-front draw order
    static inline u8   draw_n_                = 0;
    static inline u8   dirty_[kMaxSlots]      = {};   // per-slot dirty flag (commit scratch)

    // Whether the compositor restores per-rect (dirty-rect) rather than full-clearing
    // the back page: always single-buffer; double-buffer only with a bitmap background.
    static constexpr bool dirty_rect = (!double_buffered) || bitmap_bg;

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

    // The canvas the bitmap-drawing seams target: the master in sprites-over-bitmap
    // mode (the game draws the background there), else the live back buffer.
    static u32 canvas_fb() {
        if constexpr (bitmap_bg) return Layout::master;
        else                     return back_fb();
    }

    // Index (0 = fb_a, 1 = fb_b) of the page being composed this frame, for the
    // per-page dirty-rect lists. Single-buffer is always page 0.
    static u8 compose_page_idx() {
        if constexpr (double_buffered) return (back_fb() == Layout::fb_a) ? 0 : 1;
        else                           return 0;
    }

    // Build a full-screen XDL for framebuffer at `fb_addr` and upload it to `dest`.
    static void build_and_upload_xdl(u32 fb_addr, u32 dest) {
        u8 buf[24];
        const u8 len = vbxe::build_fullscreen_xdl<Config>(buf, fb_addr, fb_stride, fb_height);
        vbxe::upload_xdl<Config>(buf, len, dest);
    }

    // Fill a w×h rectangle at `dest` with a solid colour via the blitter (far
    // faster than a MEMAC memset). Synchronous: waits for the fill to finish before
    // returning. The NmiGuard makes it safe to call even after the VBI is armed
    // (init / set_background / bitmap ops), since the VBI shares queue_/the blitter.
    static void blit_rect(u32 dest, u16 w, u8 h, u8 color) {
        NmiGuard cs;
        wait_blitter_idle();   // a compositor blit may still be in flight (async
                               // overlay_submit); never reset/submit over a running op
        queue_.reset();
        queue_.push(vbxe::bcb_fill_rect(dest, w, h, fb_stride, color));
        queue_.template submit<Config>();
        for (u16 spin = 0; spin < 50000; ++spin)
            if (!queue_.template busy<Config>()) break;
        queue_.reset();
    }

    // Fill one whole framebuffer page with a solid colour.
    static void blit_fill(u32 dest, u8 color) { blit_rect(dest, fb_stride, fb_height, color); }

    // Spin until the blitter is idle. The bitmap seams below write VRAM through the
    // MEMAC window (CPU side); in Background::Bitmap mode the compositor blits the
    // whole master to the back page asynchronously every frame, reading the master.
    // A CPU MEMAC write to the master while that copy is in flight is lost (the
    // blitter has the VRAM bus), so each MEMAC draw waits for idle first — under an
    // NmiGuard, so the VBI can't start a new compositor blit between the wait and
    // the write. (When the blitter is already idle this returns at once.)
    static void wait_blitter_idle() {
        for (u16 spin = 0; spin < 50000; ++spin)
            if (!queue_.template busy<Config>()) break;
    }

    // Opaque VRAM->VRAM copy of one whole framebuffer page (src -> dest).
    // Synchronous, like blit_fill: NmiGuard makes it safe after the VBI is armed.
    static void blit_copy(u32 src, u32 dest) {
        NmiGuard cs;
        wait_blitter_idle();   // don't reset/submit over an in-flight compositor blit
        queue_.reset();
        queue_.push(vbxe::bcb_copy(src, dest, fb_stride, fb_height, fb_stride, fb_stride));
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
    // In sprites-over-bitmap mode the colour is the master canvas's base fill: we
    // fill the master and publish it to the display page(s).
    static void overlay_set_background(u8 color) {
        bg_color_ = color;
        overlay_invalidate_cache();   // a full background reset drops any cached sprites
        if constexpr (bitmap_bg) {
            blit_fill(Layout::master, color);
            overlay_publish_background();
        } else {
            blit_fill(Layout::fb_a, color);
            if constexpr (double_buffered) blit_fill(Layout::fb_b, color);
        }
    }

    // Copy the master canvas onto one page AND, in the same blitter chain, redraw the
    // sprites currently cached for that page on top — so a republish never leaves the
    // page sprite-less (the cause of the "sprites blink on a kill" glitch). Synchronous
    // (NmiGuard so the VBI compositor can't race the shared queue). After a reset
    // (clear/set_background) the cache is empty, so this just lays down the clean master.
    static void publish_page(u8 pidx, u32 page) {
        NmiGuard cs;
        wait_blitter_idle();
        queue_.reset();
        queue_.push(vbxe::bcb_copy(Layout::master, page, fb_stride, fb_height,
                                   fb_stride, fb_stride));
        for (u8 s = 0; s < kMaxSlots; ++s)
            if (painted_[pidx][s].valid) push_draw(painted_[pidx][s], page);
        queue_.template submit<Config>();
        wait_blitter_idle();
        queue_.reset();
    }

    // Publish the master canvas to the live display page(s). The game calls this after
    // drawing the background via the bitmap seams (HUD/explosion edits) to refresh what
    // is shown. Sprites cached for each page are re-laid on top so they don't blink.
    // No-op outside sprites-over-bitmap mode.
    static void overlay_publish_background() {
        if constexpr (bitmap_bg) {
            publish_page(0, Layout::fb_a);
            if constexpr (double_buffered) publish_page(1, Layout::fb_b);
        }
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

    // ── Skip-unchanged sprite compositing (begin → set per slot → commit) ──
    //
    // The generic SpriteManager calls overlay_begin_sprites(), then overlay_set_sprite()
    // for each active sprite (by logical slot, in back-to-front order), then
    // overlay_commit_sprites() which builds the frame's blitter chain. Only slots whose
    // state changed since this page was last composed are erased + redrawn, so a
    // stationary sprite costs nothing. overlay_submit() (called by the frame service)
    // uploads the chain afterwards.

    // Begin a frame: mark all slots not-present and start recording draw order.
    static void overlay_begin_sprites() {
        draw_n_ = 0;
        for (u8 s = 0; s < kMaxSlots; ++s) desired_[s].valid = false;
    }

    // Record one logical sprite's requested state for this frame (no BCB yet). The
    // registry is scanned once here; the resolved entry is cached in the slot.
    static void overlay_set_sprite(u8 slot, const u8* rom, u8 x, u8 y, u8 color) {
        if (slot >= kMaxSlots) return;
        const ShapeEntry* e = find_shape(rom);
        if (!e) return;
        desired_[slot] = Slot{ x, y, color, e, true };
        draw_order_[draw_n_++] = slot;
    }

    // AABB overlap of two footprints.
    static bool fp_overlap(u8 ax, u8 ay, u8 aw, u8 ah, u8 bx, u8 by, u8 bw, u8 bh) {
        return ax < static_cast<u8>(bx + bw) && bx < static_cast<u8>(ax + aw) &&
               ay < static_cast<u8>(by + bh) && by < static_cast<u8>(ay + ah);
    }

    // A slot is dirty on `pidx` if its requested state differs from what is painted
    // there (visibility, position, shape, or colour — shape change is a different
    // cached entry, so comparing the entry pointer covers w/h/vram/fmt).
    static bool slot_dirty(u8 s, u8 pidx) {
        const Slot& p = painted_[pidx][s];
        const Slot& d = desired_[s];
        if (d.valid != p.valid) return true;
        if (!d.valid)           return false;
        return d.x != p.x || d.y != p.y || d.e != p.e || d.color != p.color;
    }

    static void push_restore(const Slot& p, u32 page) {
        const u32 dest = page + static_cast<u32>(p.y) * fb_stride + p.x;
        if constexpr (bitmap_bg)
            queue_.push(vbxe::bcb_copy(Layout::master + (dest - page), dest,
                                       p.e->w, p.e->h, fb_stride, fb_stride));
        else
            queue_.push(vbxe::bcb_clear(dest, p.e->w, p.e->h, fb_stride, bg_color_));
    }

    static void push_draw(const Slot& d, u32 page) {
        const ShapeEntry* e = d.e;
        const u32 dest = page + static_cast<u32>(d.y) * fb_stride + d.x;
        if (e->fmt == engine::SpriteFormat::Pixel8bpp)
            queue_.push(vbxe::bcb_sprite(e->vram, dest, e->w, e->h, e->w, fb_stride));
        else
            queue_.push(vbxe::bcb_sprite_colored(e->vram, dest, e->w, e->h,
                                                 e->w, fb_stride, d.color));
    }

    // Build this frame's blitter chain from the recorded sprites.
    static void overlay_commit_sprites() {
        queue_.reset();
        const u32 page = back_fb();

        if constexpr (!dirty_rect) {
            // Double-buffer Flat: the page is wiped every frame, so nothing persists —
            // full-clear then draw every requested sprite (no skip, cache unused).
            queue_.push(vbxe::bcb_clear(page, fb_stride, fb_height, fb_stride, bg_color_));
            for (u8 k = 0; k < draw_n_; ++k) push_draw(desired_[draw_order_[k]], page);
            return;
        }

        const u8 pidx = compose_page_idx();

        // 1. Base dirty set: slots whose state changed vs this page.
        for (u8 s = 0; s < kMaxSlots; ++s) dirty_[s] = slot_dirty(s, pidx) ? 1 : 0;

        // 2. Overlap propagation: erasing a dirty slot's old/new footprint (an opaque
        //    rectangular restore) would punch a hole in any sprite it overlaps, so any
        //    clean+present slot overlapping a dirty slot's old or new footprint must be
        //    redrawn too. Iterate to a fixed point (n is tiny).
        for (bool changed = true; changed;) {
            changed = false;
            for (u8 d = 0; d < kMaxSlots; ++d) {
                if (!dirty_[d]) continue;
                const Slot& po = painted_[pidx][d];   // d's old footprint (this page)
                const Slot& dn = desired_[d];          // d's new footprint
                for (u8 s = 0; s < kMaxSlots; ++s) {
                    if (dirty_[s] || !desired_[s].valid) continue;
                    const Slot& c = desired_[s];        // clean slot's current footprint
                    const bool hit =
                        (po.valid && fp_overlap(c.x, c.y, c.e->w, c.e->h, po.x, po.y, po.e->w, po.e->h)) ||
                        (dn.valid && fp_overlap(c.x, c.y, c.e->w, c.e->h, dn.x, dn.y, dn.e->w, dn.e->h));
                    if (hit) { dirty_[s] = 1; changed = true; }
                }
            }
        }

        // 3. Erase the old footprints of all dirty slots (before any draw, so a draw
        //    can't be clobbered by a later erase).
        for (u8 s = 0; s < kMaxSlots; ++s)
            if (dirty_[s] && painted_[pidx][s].valid) push_restore(painted_[pidx][s], page);

        // 4. Draw the dirty present slots in back-to-front order; update the cache.
        for (u8 k = 0; k < draw_n_; ++k) {
            const u8 s = draw_order_[k];
            if (!dirty_[s]) continue;
            push_draw(desired_[s], page);
            painted_[pidx][s] = desired_[s];   // valid == true
        }

        // 5. Dirty slots no longer present were just erased — mark them gone.
        for (u8 s = 0; s < kMaxSlots; ++s)
            if (dirty_[s] && !desired_[s].valid) painted_[pidx][s].valid = false;
    }

    // Invalidate the per-page sprite cache: forces a full erase+redraw next frames.
    // Call whenever the page contents are overwritten outside the compositor (a
    // background republish blit-copies the master over both pages, wiping sprites).
    static void overlay_invalidate_cache() {
        for (u8 pg = 0; pg < 2; ++pg)
            for (u8 s = 0; s < kMaxSlots; ++s) painted_[pg][s].valid = false;
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

    // ── Bitmap-canvas seams (engine/gfx.h BitmapOps draws into the overlay) ──
    //
    // The canvas is the overlay framebuffer (8bpp; one byte per pixel = a palette
    // index). All ops target back_fb() — single-buffer, so that is the visible
    // page — and are synchronous (blitter rect fills) or direct (MEMAC pixel/row
    // writes), each VBI-atomic via the NmiGuard in blit_rect / Memac.

    // Clearing the whole canvas is a background reset: drop any cached sprites so the
    // next publish lays down a clean field and the compositor repaints sprites fresh
    // (rather than re-laying stale sprites from a previous screen).
    static void overlay_bitmap_clear(u8 color) { overlay_invalidate_cache(); blit_fill(canvas_fb(), color); }

    static void overlay_bitmap_fill_rect(u16 x, u16 y, u16 w, u16 h, u8 color) {
        const u32 dest = canvas_fb() + static_cast<u32>(y) * fb_stride + x;
        blit_rect(dest, w, static_cast<u8>(h), color);
    }

    // Plot via the blitter (a 1x1 fill), NOT a per-pixel MEMAC write. The blitter
    // addresses VRAM linearly (no MEMAC bank switching) and blit_rect is self-
    // serialising (NmiGuard + wait-for-idle), so a per-pixel draw can't land in the
    // wrong bank or race the async compositor copy of the master. (A per-pixel MEMAC
    // write interleaved with the VBI compositor lands in a wrong bank — scattered
    // pixels across VRAM.) 1-wide fills are proven by hline/vline.
    static void overlay_bitmap_plot(u16 x, u16 y, u8 color) {
        const u32 dest = canvas_fb() + static_cast<u32>(y) * fb_stride + x;
        blit_rect(dest, 1, 1, color);
    }

    // Copy a w×h 8bpp source image (row-packed, stride == w) into the canvas. The
    // source is CPU RAM, so this must go through the MEMAC window (the blitter reads
    // VRAM only). One NmiGuard spans all rows (no VBI interleave), and we wait for
    // the blitter to be idle first so we never upload to the master while the
    // compositor copy is reading it.
    static void overlay_bitmap_blit(u16 x, u16 y, const u8* src, u16 w, u16 h) {
        NmiGuard cs;
        wait_blitter_idle();
        const u32 base = canvas_fb() + static_cast<u32>(y) * fb_stride + x;
        for (u16 r = 0; r < h; ++r)
            Memac::write(base + static_cast<u32>(r) * fb_stride,
                         src + static_cast<u32>(r) * w, w);
    }

    // ── Overlay text-mode seams (Mode::VBXE_T80; FX Core manual pp.9,14,16) ──
    //
    // In text mode the "framebuffer" at OVADR is a character map of char+attribute
    // byte pairs (char first), one 8x8 glyph cell per pair; the row stride is
    // 2*cols bytes and the hardware advances OVADR by that step every 8 scanlines.
    // The font (256 glyphs x 8 bytes, 1bpp) lives at the 2K-aligned `fonts` region
    // pointed to by CHBASE. Attribute: b0-b6 = foreground palette index (0..127);
    // b7=1 gives an opaque background of index (attr&127)+128, b7=0 transparent.
    // The map is fb_a (OVADR); all writes are VBI-atomic via Memac/NmiGuard.
    static constexpr u16 text_cols      = static_cast<u16>(Config::fb_width / 2);
    static constexpr u16 text_row_bytes = static_cast<u16>(Config::fb_width);
    static constexpr u16 text_rows      = static_cast<u16>(fb_height / 8);

    // Upload a 1bpp font (256 glyphs x 8 bytes = 2048) to the CHBASE font region.
    static void overlay_text_font(const u8* glyphs, u16 bytes) {
        Memac::write(Layout::fonts, glyphs, bytes);
    }

    // Fill the whole character map with one char+attribute cell.
    static void overlay_text_clear(u8 ch, u8 attr) {
        u8 row[text_row_bytes];
        for (u16 c = 0; c < text_cols; ++c) {
            row[c * 2]     = ch;
            row[c * 2 + 1] = attr;
        }
        NmiGuard cs;
        for (u16 r = 0; r < text_rows; ++r)
            Memac::write(Layout::fb_a + static_cast<u32>(r) * text_row_bytes,
                         row, text_row_bytes);
    }

    // Write one char+attribute cell at (col, row).
    static void overlay_text_put(u16 col, u16 row, u8 ch, u8 attr) {
        const u8 cell[2] = { ch, attr };
        Memac::write(Layout::fb_a + static_cast<u32>(row) * text_row_bytes + col * 2,
                     cell, 2);
    }

    // Write a NUL-terminated string at (col, row) with one attribute, clipped to
    // the row width. The font is indexed by raw byte (PC code page) — no remap.
    static void overlay_text_print(u16 col, u16 row, const char* s, u8 attr) {
        u8 buf[text_row_bytes];
        u16 n = 0;
        for (; s[n] != '\0' && (col + n) < text_cols; ++n) {
            buf[n * 2]     = static_cast<u8>(s[n]);
            buf[n * 2 + 1] = attr;
        }
        if (n)
            Memac::write(Layout::fb_a + static_cast<u32>(row) * text_row_bytes + col * 2,
                         buf, static_cast<u16>(n * 2));
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
    static void overlay_begin_sprites() {}
    static void overlay_set_sprite(u8, const u8*, u8, u8, u8) {}
    static void overlay_commit_sprites() {}
    static void overlay_set_background(u8) {}
    static void overlay_publish_background() {}
    static void overlay_bitmap_clear(u8) {}
    static void overlay_bitmap_fill_rect(u16, u16, u16, u16, u8) {}
    static void overlay_bitmap_plot(u16, u16, u8) {}
    static void overlay_bitmap_blit(u16, u16, const u8*, u16, u16) {}
    static void overlay_text_font(const u8*, u16) {}
    static void overlay_text_clear(u8, u8) {}
    static void overlay_text_put(u16, u16, u8, u8) {}
    static void overlay_text_print(u16, u16, const char*, u8) {}
};

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_VBXE_OVERLAY_H
