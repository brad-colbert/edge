#ifndef ENGINE_SCREEN_H
#define ENGINE_SCREEN_H

// screen.h — the screen manager: shared-buffer screens and two-phase display
// list construction.
//
// A `ScreenSet<Screens...>` collects every screen a game can show. All screens
// share ONE screen-memory buffer sized to the largest (DECISIONS.md ADR-014).
// Each screen owns a display-program builder (a small RAM-resident value supplied
// by the backend as `Platform::display_program<Layout>`); `set_screen<S>()` builds
// that screen's fully-addressed display program using the real screen-buffer base,
// points each region view at its slice of the buffer, and programs the display
// hardware through the platform HAL.
//
// The display-program byte encoding (and any backend-specific rule such as a
// scan-boundary address-reload insertion) lives in the backend builder, not here:
// this generic header owns only the shared buffer, the region views, and the
// transition sequence. Hardware is reached only through `Platform::hal`
// (Dependency Rule 2). The set_screen steps that depend on the interrupt / sprite
// / scroll subsystems are noted inline; those subsystems land later.
//
// Depends on display.h and types.h.

#include "config/capabilities.h"
#include "display.h"
#include "types.h"

namespace engine {

namespace detail {

// Variadic compile-time max over u16 values (avoids <algorithm>/<initializer_list>).
constexpr u16 vmax() { return 0; }
template <typename... Rest>
constexpr u16 vmax(u16 a, Rest... rest) {
    u16 m = vmax(rest...);
    return a > m ? a : m;
}

// Minimal is-same trait (avoids <type_traits>).
template <typename A, typename B> struct same { static constexpr bool value = false; };
template <typename A>             struct same<A, A> { static constexpr bool value = true; };

} // namespace detail

// ── ScreenSet ─────────────────────────────────────────────────────────
//
// The set of screens a game declares. Computes the shared-buffer and active
// display-list sizes as the max across all screens.
template <typename... Screens>
struct ScreenSet {
    static constexpr u8 screen_count = sizeof...(Screens);
    static_assert(screen_count >= 1, "a ScreenSet needs at least one screen");

    static constexpr u16 max_screen_ram =
        detail::vmax(Screens::display::total_ram...);

    template <u8 I>
    using screen_at = pack_element_t<I, Screens...>;
};

// ── View storage ──────────────────────────────────────────────────────
//
// A chain of the typed view instances for one layout, addressable by region
// index. Inheritance gives each region its own typed member with a compile-time
// path to it (a tuple without <tuple>).
namespace detail {

template <typename Layout, u8 N, u8 Count>
struct ViewChain : ViewChain<Layout, N + 1, Count> {
    typename Layout::template view_at<N> view_;

    void set_base(u8* buf) {
        view_.ptr = buf + Layout::offset(N);
        ViewChain<Layout, N + 1, Count>::set_base(buf);
    }
};

template <typename Layout, u8 Count>
struct ViewChain<Layout, Count, Count> {
    void set_base(u8*) {}
};

template <typename Layout>
struct LayoutViews : ViewChain<Layout, 0, Layout::region_count> {
    template <u8 N>
    auto& get() {
        return static_cast<ViewChain<Layout, N, Layout::region_count>&>(*this).view_;
    }
};

// One LayoutViews per screen, addressable by screen type.
template <typename... Screens>
struct ScreenViews;

template <>
struct ScreenViews<> {};

template <typename S0, typename... Rest>
struct ScreenViews<S0, Rest...> : ScreenViews<Rest...> {
    LayoutViews<typename S0::display> views_;

    template <typename Q>
    auto& for_screen() {
        if constexpr (same<S0, Q>::value) {
            return views_;
        } else {
            return ScreenViews<Rest...>::template for_screen<Q>();
        }
    }
};

// Map a ScreenSet<...> to the matching ScreenViews<...>.
template <typename ScreenSetT>
struct screen_views_of;
template <typename... Screens>
struct screen_views_of<ScreenSet<Screens...>> {
    using type = ScreenViews<Screens...>;
};

} // namespace detail

// ── ScreenManager ─────────────────────────────────────────────────────
//
// Owns the shared screen buffer and the per-screen region views. Each screen's
// display list lives in its own RAM-resident DisplayListTemplate (a function-
// local static, built on demand). `GameConfig::screens` must be a ScreenSet.
template <typename Platform, typename GameConfig>
class ScreenManager {
public:
    using screens = typename GameConfig::screens;
    using caps    = engine::caps_of_t<Platform>;

    // Clamp to at least 1 byte: a pure-overlay-only ScreenSet contributes no
    // screen RAM (overlay pixels live in VRAM, ram_bytes == 0), which would
    // otherwise make screen_buffer_ a zero-length array (ill-formed). The 1-byte
    // buffer is never read — set_base only computes buf + offset(0).
    static constexpr u16 buffer_size =
        screens::max_screen_ram > 0 ? screens::max_screen_ram : 1;

    // Alignment that keeps a single-page screen buffer off the platform's
    // screen-buffer alignment boundary (caps::screen_buffer_alignment — the display
    // hardware's scan-boundary granularity, or 1 for no constraint). Where the
    // display hardware's scan-line address counter wraps within that boundary, a mode
    // line whose own bytes straddle a boundary fetches its tail from the page start
    // (corruption the display program's per-line address reload can't fix — it only
    // reloads at line starts; see the backend display-program builder). A buffer that
    // fits in one boundary-sized page and is aligned to the smallest power of two >=
    // its size can never cross a boundary, so every line stays within one page.
    // Buffers LARGER than a page must span boundaries regardless; those keep
    // alignment 1 (page-aligning them would force mid-line crossings, since the
    // boundary is not a multiple of bytes_per_line for e.g. 40-col modes) and rely on
    // the builder's per-line address reload landing on row-aligned crossings.
    static constexpr u16 pow2_ceil(u16 n) {
        u16 p = 1;
        while (p < n && p < caps::screen_buffer_alignment) p <<= 1;
        return p;
    }
    static constexpr u16 buffer_align =
        buffer_size <= caps::screen_buffer_alignment ? pow2_ceil(buffer_size) : u16{1};
    // A single-page buffer aligned to >= its own size cannot straddle a boundary.
    static_assert(buffer_size > caps::screen_buffer_alignment || buffer_align >= buffer_size,
                  "single-page screen buffer must be aligned to >= its size to keep "
                  "every mode line within one screen-buffer alignment boundary");

    // Switch to screen S: build its display program against the real buffer base
    // (the backend builder inserts any backend-specific reload instructions),
    // rebind views, program the display hardware, then run the user transition
    // callback.
    template <typename S, typename Cb>
    void set_screen(Cb cb) {
        // One display program per screen, in BSS (not on the 256-byte 6502
        // stack). The hardware reads it directly, so it is built at its own
        // resident address. program_for<S>() returns that one persistent instance
        // so bind_scroll_map()/apply_scroll() patch the same program the display
        // hardware executes.
        auto& dl = program_for<S>();
        dl.build(addr(screen_buffer_), addr(&dl.bytes[0]));

        // Rebind this screen's region views to their buffer slices.
        views_.template for_screen<S>().set_base(screen_buffer_);

        // Program the display. Blank DMA and install the program in all cases.
        Platform::hal::display_dma_disable();
        Platform::hal::set_display_program(dl.bytes);
        if constexpr (!S::display::is_pure_overlay) {
            // Playfield content present (playfield-only or mixed overlay+playfield):
            // re-enable playfield DMA. Mixed layouts cost only the display-program
            // DMA that reads blank instructions (~1 cycle/line), not screen fetches.
            Platform::hal::display_dma_enable();
        }
        // Pure overlay: playfield DMA stays off. The overlay (brought up by
        // Core::init through the graphics axis) drives the display via its own
        // display list; the backend's 3-byte stub is only a safety pointer for the
        // display-list register. Leaving playfield DMA off frees the VRAM bus for the
        // blitter — the bus-contention fix (formerly a manual playfield-DMA disable),
        // now automatic.

        active_dl_      = dl.bytes;
        active_dl_size_ = dl.size;

        // A fresh screen is not scroll-bound until bind_scroll_map(); apply_scroll
        // is a no-op until then (and the caller deactivates the ScrollManager).
        scroll_bound_ = false;

        // TODO (later subsystems): clear screen buffer, rebuild the raster-hook
        // chain, enable/disable sprites per screen flags, generate the
        // row-address table when use_row_table. See ARCHITECTURE.md set_screen
        // steps 3-11.

        cb();
    }

    // ── Scroll binding ────────────────────────────────────────────────
    //
    // Bind a game-held map buffer to screen S's scroll region: point every
    // scroll-region load address at the map (initial coarse 0,0), rebind the region
    // view to the map, and activate the ScrollManager with the geometry from the
    // layout + the mode's display traits. Call after set_screen<S>() (init() builds the
    // initial screen). `map_width` is the map's row stride in the region's native
    // units; it must match the layout's scroll-region map width.
    template <typename S, typename ScrollT>
    void bind_scroll_map(ScrollT& scroll, u8* map_base, u16 map_width) {
        using Layout = typename S::display;
        static_assert(Layout::has_scroll, "screen S has no scroll region");
        constexpr u8 idx = Layout::scroll_region_index();

        auto& dl = program_for<S>();
        scroll_prog_      = &dl;
        scroll_patch_     = &patch_thunk<S>;
        scroll_map_base_  = addr(map_base);
        scroll_map_width_ = map_width;
        scroll_bound_     = true;

        // Point the scroll region's view at the map so region-view writes land in
        // the map buffer, and set the initial (unscrolled) load addresses.
        views_.template for_screen<S>().template get<idx>().ptr = map_base;
        dl.patch_scroll(scroll_map_base_, scroll_map_width_, 0, 0);
        // Invalidate the apply_scroll coarse cache so the next frame re-patches
        // regardless of the prior (possibly different-program) coarse offset.
        last_coarse_col_ = 0xFFFF;
        last_coarse_row_ = 0xFFFF;

        using Region = typename Layout::template region_at<idx>;
        using ModeT  = typename Region::mode_type;
        constexpr ModeT mode = Region::mode;
        using tr = engine::display::traits<ModeT>;
        scroll.activate(Layout::region_map_width[idx], Layout::region_map_height[idx],
                        Layout::region_height[idx], tr::scroll_fetch_width(mode),
                        tr::scanlines_per_line(mode), tr::fine_scroll_range(mode),
                        tr::fine_scroll_inverts_x(mode), tr::fine_scroll_inverts_y(mode));
    }

    // Per-frame scroll update (called from the frame service). Writes the fine
    // registers and repoints the scroll-region load addresses for the current coarse
    // offset. No-op unless a map is bound and the ScrollManager is active and not
    // suspended.
    template <typename ScrollT>
    void apply_scroll(ScrollT& scroll) {
        if (!scroll_bound_ || !scroll.active() || scroll.suspended()) return;
        scroll.write_fine();
        // The fine registers move the picture every frame, but the per-line LMS
        // bytes only change when the COARSE offset crosses a cell — which is a small
        // fraction of frames at normal scroll speeds. Rewriting the whole LMS list
        // every frame is the heaviest frame-service step (it tipped scrolling demos
        // to half frame rate), so skip it when the coarse offset is unchanged. The
        // cache is invalidated by bind_scroll_map, so a rebound program re-patches.
        const u16 cc = scroll.coarse_col();
        const u16 cr = scroll.coarse_row();
        if (cc != last_coarse_col_ || cr != last_coarse_row_) {
            scroll_patch_(scroll_prog_, scroll_map_base_, scroll_map_width_, cc, cr);
            last_coarse_col_ = cc;
            last_coarse_row_ = cr;
        }
    }

    // Publish only the fine-scroll position to the backend. Called from the game
    // loop right after the game commits its frame (engine/loop.h), so the value the
    // backend latches at the next frame boundary reflects THIS frame's position.
    // The backend may latch the fine registers at a different point in the frame
    // than the coarse load addresses (apply_scroll, run from the frame service); if
    // the fine value were published only there it could lag the coarse offset by a
    // frame and shear the picture during motion. write_fine() carries no display-
    // program side effects, so it is safe to call outside the frame service.
    template <typename ScrollT>
    void publish_fine_scroll(ScrollT& scroll) {
        if (!scroll_bound_ || !scroll.active() || scroll.suspended()) return;
        scroll.write_fine();
    }

    // Typed region view for region N of screen S (compile-time type safety:
    // text ops on a bitmap region, or vice versa, do not compile).
    template <typename S, u8 N>
    auto& region() {
        return views_.template for_screen<S>().template get<N>();
    }

    // Convenience for single-screen games (API_DESIGN.md `Game::region<0>()`).
    template <u8 N>
    auto& region() {
        static_assert(screens::screen_count == 1,
                      "multi-screen game: use region<Screen, N>()");
        using S = typename screens::template screen_at<0>;
        return views_.template for_screen<S>().template get<N>();
    }

    // ── Test / debug accessors ──
    // active_dl() is mutable: the interrupt manager arms per-line raster delivery
    // on the live display program during prepare_chain (engine/interrupt.h).
    u8*       active_dl()            { return active_dl_; }
    u16       active_dl_size() const { return active_dl_size_; }
    u8*       buffer()               { return screen_buffer_; }

private:
    static u16 addr(const void* p) {
        return static_cast<u16>(reinterpret_cast<uintptr_t>(p));
    }

    // The one persistent display program for screen S (a function-local static so
    // it has a stable resident address the display hardware can read). set_screen, bind_scroll_map,
    // and patch_thunk all reach the same instance through here.
    template <typename S>
    static auto& program_for() {
        static typename Platform::template display_program<typename S::display> dl;
        return dl;
    }

    // Type-erasing trampoline so apply_scroll can call the backend display
    // program's patch_scroll without the ScreenManager naming the per-screen
    // program type at the call site.
    template <typename S>
    static void patch_thunk(void* p, u16 base, u16 width, u16 col, u16 row) {
        using DP = typename Platform::template display_program<typename S::display>;
        static_cast<DP*>(p)->patch_scroll(base, width, col, row);
    }

    alignas(buffer_align) u8 screen_buffer_[buffer_size] = {};
    typename detail::screen_views_of<screens>::type views_;

    u8*       active_dl_      = nullptr;   // last built list (points into a static)
    u16       active_dl_size_ = 0;

    // Scroll binding (type-erased: the active program type is captured in
    // scroll_patch_ at bind time so apply_scroll names no per-screen type).
    void*  scroll_prog_      = nullptr;
    void (*scroll_patch_)(void*, u16, u16, u16, u16) = nullptr;
    u16    scroll_map_base_  = 0;
    u16    scroll_map_width_ = 0;
    bool   scroll_bound_     = false;
    // Last coarse offset written to the LMS list; apply_scroll skips the rewrite
    // while it is unchanged. 0xFFFF = invalid (force a patch next frame).
    u16    last_coarse_col_  = 0xFFFF;
    u16    last_coarse_row_  = 0xFFFF;
};

} // namespace engine

#endif // ENGINE_SCREEN_H
