#ifndef ENGINE_GFX_H
#define ENGINE_GFX_H

// gfx.h — the bitmap-drawing subsystem.
//
// BitmapOps is a portable drawing API over a bitmap "canvas". It dispatches at
// compile time on the platform's `has_blitter` capability:
//
//   * Blitter platforms: the canvas is the 256-colour overlay framebuffer
//     (8bpp in VRAM). Operations go through neutral `overlay_bitmap_*` HAL seams,
//     which use the blitter (rectangle fills) and the backend's memory window
//     (single pixels) — the generic engine never names a backend type.
//
//   * Baseline platforms: the canvas is a baseline `BitmapRegion`. Operations
//     delegate to that region's `BitmapRegionView` (engine/display.h), which owns
//     the mode-dependent (1/2/4 bpp) pixel packing — we do not re-implement it.
//
// The `Region` template parameter supplies the baseline view type (its mode/bpp).
// Blitter-only games may leave it `void`; the software path then static-asserts.
//
// This header is platform-agnostic (engine layer): it depends only on types.h and
// the capability contract, and reaches the backend solely via `Platform::hal`.

#include "config/capabilities.h"
#include "types.h"

namespace engine {

namespace gfx_detail {
template <typename...> using void_t = void;

// Placeholder view for Region == void (blitter-only canvases that never touch the
// software path). It has no drawing methods; the software branch is gated so it is
// never instantiated against this type.
struct empty_view {};

template <typename R, typename = void>
struct view_of { using type = empty_view; };
template <typename R>
struct view_of<R, void_t<typename R::view>> { using type = typename R::view; };

template <typename A, typename B> struct is_same      { static constexpr bool value = false; };
template <typename A>             struct is_same<A, A> { static constexpr bool value = true;  };
} // namespace gfx_detail

// Bitmap drawing over a canvas. `Region` is the baseline bitmap region descriptor
// (gives the software view type); leave it `void` on blitter-only platforms.
template <typename Platform, typename Region = void>
class BitmapOps {
    using caps = engine::caps_of_t<Platform>;
    using View = typename gfx_detail::view_of<Region>::type;
    static constexpr bool has_software_view =
        !gfx_detail::is_same<View, gfx_detail::empty_view>::value;

public:
    // Bind the software canvas to its screen-buffer bytes (baseline). No-op on
    // blitter platforms (the canvas lives in VRAM, reached through the seams).
    void attach(u8* buf) {
        if constexpr (has_software_view) view_.ptr = buf;
        else                             (void)buf;
    }

    // Fill the whole canvas with one colour index.
    void clear(u8 color) {
        if constexpr (caps::has_blitter) {
            Platform::hal::overlay_bitmap_clear(color);
        } else {
            static_assert(has_software_view, "baseline BitmapOps needs a bitmap Region");
            view_.clear(color);
        }
    }

    // Set one pixel.
    void plot(u16 x, u16 y, u8 color) {
        if constexpr (caps::has_blitter) {
            Platform::hal::overlay_bitmap_plot(x, y, color);
        } else {
            static_assert(has_software_view, "baseline BitmapOps needs a bitmap Region");
            view_.plot(static_cast<u8>(x), static_cast<u8>(y), color);
        }
    }

    // Horizontal line x1..x2 inclusive on row y.
    void hline(u16 x1, u16 x2, u16 y, u8 color) {
        const u16 lo  = x1 < x2 ? x1 : x2;
        const u16 len = static_cast<u16>((x1 < x2 ? x2 - x1 : x1 - x2) + 1);
        if constexpr (caps::has_blitter) {
            Platform::hal::overlay_bitmap_fill_rect(lo, y, len, 1, color);
        } else {
            static_assert(has_software_view, "baseline BitmapOps needs a bitmap Region");
            view_.hline(static_cast<u8>(x1), static_cast<u8>(x2),
                        static_cast<u8>(y), color);
        }
    }

    // Vertical line y1..y2 inclusive on column x.
    void vline(u16 x, u16 y1, u16 y2, u8 color) {
        const u16 lo  = y1 < y2 ? y1 : y2;
        const u16 len = static_cast<u16>((y1 < y2 ? y2 - y1 : y1 - y2) + 1);
        if constexpr (caps::has_blitter) {
            Platform::hal::overlay_bitmap_fill_rect(x, lo, 1, len, color);
        } else {
            static_assert(has_software_view, "baseline BitmapOps needs a bitmap Region");
            for (u16 y = lo; y < static_cast<u16>(lo + len); ++y)
                view_.plot(static_cast<u8>(x), static_cast<u8>(y), color);
        }
    }

    // Filled rectangle (w×h, top-left at x,y).
    void fill_rect(u16 x, u16 y, u16 w, u16 h, u8 color) {
        if constexpr (caps::has_blitter) {
            Platform::hal::overlay_bitmap_fill_rect(x, y, w, h, color);
        } else {
            static_assert(has_software_view, "baseline BitmapOps needs a bitmap Region");
            for (u16 r = 0; r < h; ++r)
                view_.hline(static_cast<u8>(x), static_cast<u8>(x + w - 1),
                            static_cast<u8>(y + r), color);
        }
    }

    // Arbitrary line (integer Bresenham). On blitter platforms the whole line is
    // handed to a single batched seam (one window pass) — drawing it pixel by pixel
    // through plot() would run a full blitter round-trip per pixel. The baseline
    // canvas keeps the software Bresenham below (its plot() is a cheap packed write).
    void line(u16 ax, u16 ay, u16 bx, u16 by, u8 color) {
        if constexpr (caps::has_blitter) {
            Platform::hal::overlay_bitmap_line(ax, ay, bx, by, color);
        } else {
            static_assert(has_software_view, "baseline BitmapOps needs a bitmap Region");
            i16 x0 = static_cast<i16>(ax), y0 = static_cast<i16>(ay);
            const i16 x1 = static_cast<i16>(bx), y1 = static_cast<i16>(by);
            const i16 dx  = x1 >= x0 ? static_cast<i16>(x1 - x0) : static_cast<i16>(x0 - x1);
            const i16 ady = y1 >= y0 ? static_cast<i16>(y1 - y0) : static_cast<i16>(y0 - y1);
            const i16 dy  = static_cast<i16>(-ady);
            const i16 sx  = x0 < x1 ? 1 : -1;
            const i16 sy  = y0 < y1 ? 1 : -1;
            i16 err = static_cast<i16>(dx + dy);
            for (;;) {
                plot(static_cast<u16>(x0), static_cast<u16>(y0), color);
                if (x0 == x1 && y0 == y1) break;
                const i16 e2 = static_cast<i16>(2 * err);
                if (e2 >= dy) { err = static_cast<i16>(err + dy); x0 = static_cast<i16>(x0 + sx); }
                if (e2 <= dx) { err = static_cast<i16>(err + dx); y0 = static_cast<i16>(y0 + sy); }
            }
        }
    }

    // Blit a w×h source image into the canvas at (x, y). On blitter platforms the
    // source is 8bpp (one byte per pixel); on baseline it is packed at the region's
    // bpp (BitmapRegionView::blit handles unpacking).
    void blit(u16 x, u16 y, const u8* src, u16 w, u16 h) {
        if constexpr (caps::has_blitter) {
            Platform::hal::overlay_bitmap_blit(x, y, src, w, h);
        } else {
            static_assert(has_software_view, "baseline BitmapOps needs a bitmap Region");
            view_.blit(static_cast<u8>(x), static_cast<u8>(y), src,
                       static_cast<u8>(w), static_cast<u8>(h));
        }
    }

private:
    [[no_unique_address]] View view_{};
};

} // namespace engine

#endif // ENGINE_GFX_H
