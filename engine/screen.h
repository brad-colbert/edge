#ifndef ENGINE_SCREEN_H
#define ENGINE_SCREEN_H

// screen.h — the screen manager: shared-buffer screens and two-phase display
// list construction.
//
// A `ScreenSet<Screens...>` collects every screen a game can show. All screens
// share ONE screen-memory buffer sized to the largest (DECISIONS.md ADR-014).
// Each screen owns a `DisplayListTemplate` (a small RAM-resident builder);
// `set_screen<S>()` builds that screen's fully-addressed display list using the
// real screen-buffer base, points each region view at its slice of the buffer,
// and programs ANTIC through the platform HAL.
//
// 4K scan-address boundaries (ADR-026 refinement): ANTIC increments only the
// low 12 bits of its memory-scan counter, so screen data cannot cross a $x000
// boundary without a fresh LMS reloading the full address. Crossing positions
// depend on the absolute (runtime) buffer address, so the display list is built
// at set_screen time rather than patched from a fixed template; the builder
// inserts an extra LMS at every mode line that enters a new 4K page.
//
// Hardware is reached only through `Platform::hal` (Dependency Rule 2). The
// HAL-driven steps (display list + DMA) and the remaining set_screen steps that
// depend on the interrupt / sprite / scroll subsystems are noted inline; those
// subsystems land later.
//
// Depends on display.h and types.h.

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

// Base display-list length for `Layout` assuming one LMS per region (no 4K
// crossings): 3 blank-line instructions + per region (1 mode/LMS byte + 2 LMS
// address bytes + height-1 mode bytes) + a 3-byte JVB.
template <typename Layout>
constexpr u16 display_list_base_size() {
    u16 s = 3 + 3;
    for (u8 i = 0; i < Layout::region_count; ++i) {
        s += static_cast<u16>(Layout::region_height[i]) + 2;
    }
    return s;
}

// The most 4K boundaries a B-byte region can straddle, over all base addresses.
// A region's interior crossing points span its B-1 trailing bytes (the first
// byte is covered by the region's own LMS), so the max is floor((B-2)/4096)+1.
constexpr u16 lms_crossings_max(u16 b) {
    return b < 2 ? 0 : static_cast<u16>((b - 2) / 4096 + 1);
}

// Worst-case display-list capacity: base size plus 2 bytes for every potential
// crossing LMS (a 1-byte mode line becomes a 3-byte LMS).
template <typename Layout>
constexpr u16 display_list_capacity() {
    u16 s = display_list_base_size<Layout>();
    for (u8 i = 0; i < Layout::region_count; ++i) {
        s += static_cast<u16>(2 * lms_crossings_max(Layout::region_ram[i]));
    }
    return s;
}

// Worst-case total LMS count: one per region plus its potential crossings.
template <typename Layout>
constexpr u16 max_lms_count() {
    u16 n = 0;
    for (u8 i = 0; i < Layout::region_count; ++i) {
        n += static_cast<u16>(1 + lms_crossings_max(Layout::region_ram[i]));
    }
    return n;
}

// Minimal is-same trait (avoids <type_traits>).
template <typename A, typename B> struct same { static constexpr bool value = false; };
template <typename A>             struct same<A, A> { static constexpr bool value = true; };

} // namespace detail

// ── DisplayListTemplate ───────────────────────────────────────────────
//
// The RAM-resident display list for one layout (ADR-026, refined for 4K
// boundaries). `build()` emits a fully-addressed program — blank lines, one LMS
// per region plus an extra LMS at every mode line that enters a new 4K page, and
// a terminating JVB — so ANTIC can read `bytes` directly. The arrays are sized
// at compile time for the worst case over all base alignments; `build()` records
// the actual size and the LMS positions for inspection.
//
// LMS-insertion rule: an LMS is placed at the first mode line whose START enters
// a new 4K page. This is exact when 4K boundaries fall on mode-line boundaries.
// A single mode line whose own data straddles a boundary (only with a
// mode-line-misaligned base) still corrupts its tail — unavoidable via the
// display list alone; place the shared buffer so 4K boundaries align to
// mode-line edges.
template <typename Layout>
struct DisplayListTemplate {
    static constexpr u8  region_count = Layout::region_count;
    static constexpr u16 capacity     = detail::display_list_capacity<Layout>();
    static constexpr u16 max_lms      = detail::max_lms_count<Layout>();

    u8  bytes[capacity]              = {};
    u16 size                         = 0;   // bytes actually used by build()
    u16 jvb_pos                      = 0;   // index of the JVB operand low byte
    u16 lms_count                    = 0;
    u16 lms_pos[max_lms]             = {};   // low-byte index of every LMS
    u16 region_lms_pos[region_count] = {};   // low-byte index of each region's first LMS

    // Build the display list for screen memory based at `screen_base`, with the
    // list itself residing at `dl_base` (used by the JVB to loop the list).
    constexpr void build(u16 screen_base, u16 dl_base) {
        u16 p   = 0;
        lms_count = 0;

        // Three 8-blank-line instructions (24 scanlines) to centre vertically.
        bytes[p++] = atari::dl_blank(8);
        bytes[p++] = atari::dl_blank(8);
        bytes[p++] = atari::dl_blank(8);

        for (u8 r = 0; r < region_count; ++r) {
            const u8  mode = Layout::region_mode_byte[r];
            const u8  bpl  = Layout::region_bytes_per_line[r];
            u16 line_start = static_cast<u16>(screen_base + Layout::offset(r));

            // First mode line of the region always loads the address.
            region_lms_pos[r] = emit_lms(p, mode, line_start);

            for (u8 i = 1; i < Layout::region_height[r]; ++i) {
                const u16 next = static_cast<u16>(line_start + bpl);
                if ((next & 0xF000) != (line_start & 0xF000)) {
                    // This line enters a new 4K page: reload the full address so
                    // ANTIC's 12-bit scan counter doesn't wrap within the page.
                    emit_lms(p, mode, next);
                } else {
                    bytes[p++] = mode;
                }
                line_start = next;
            }
        }

        // JVB loops the display list back to its own start each frame.
        bytes[p++] = atari::DL_JVB;
        jvb_pos = p;
        bytes[p++] = lo(dl_base);
        bytes[p++] = hi(dl_base);
        size = p;
    }

private:
    // Emit `mode | LMS` + the 2-byte address `a`; record and return the low-byte
    // position. `p` is advanced past the 3 emitted bytes.
    constexpr u16 emit_lms(u16& p, u8 mode, u16 a) {
        bytes[p++] = static_cast<u8>(mode | atari::DL_LMS);
        const u16 pos = p;
        bytes[p++] = lo(a);
        bytes[p++] = hi(a);
        lms_pos[lms_count++] = pos;
        return pos;
    }
    static constexpr u8 lo(u16 a) { return static_cast<u8>(a & 0xFF); }
    static constexpr u8 hi(u16 a) { return static_cast<u8>(a >> 8); }
};

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
    // Largest display list across screens (informational; each screen's
    // DisplayListTemplate sizes its own byte buffer to its worst case).
    static constexpr u16 max_dl_size =
        detail::vmax(detail::display_list_capacity<typename Screens::display>()...);

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

    static constexpr u16 buffer_size = screens::max_screen_ram;

    // Switch to screen S: build its display list against the real buffer base
    // (inserting 4K-crossing LMS as needed), rebind views, program ANTIC, then
    // run the user transition callback.
    template <typename S, typename Cb>
    void set_screen(Cb cb) {
        using Layout = typename S::display;

        // One display list per screen, in BSS (not on the 256-byte 6502 stack).
        // ANTIC reads it directly, so it is built at its own resident address.
        static DisplayListTemplate<Layout> dl;
        dl.build(addr(screen_buffer_), addr(&dl.bytes[0]));

        // Rebind this screen's region views to their buffer slices.
        views_.template for_screen<S>().set_base(screen_buffer_);

        // Program ANTIC: blank DMA, install the display list, re-enable DMA.
        Platform::hal::antic_dma_disable();
        Platform::hal::set_display_list(dl.bytes);
        Platform::hal::antic_dma_enable();

        active_dl_      = dl.bytes;
        active_dl_size_ = dl.size;

        // TODO (later subsystems): clear screen buffer, rebuild the DLI chain,
        // enable/disable P/M + scroll per screen flags, generate the row-address
        // table when use_row_table. See ARCHITECTURE.md set_screen steps 3-11.

        cb();
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
    const u8* active_dl() const      { return active_dl_; }
    u16       active_dl_size() const { return active_dl_size_; }
    u8*       buffer()               { return screen_buffer_; }

private:
    static u16 addr(const void* p) {
        return static_cast<u16>(reinterpret_cast<uintptr_t>(p));
    }

    u8 screen_buffer_[buffer_size] = {};
    typename detail::screen_views_of<screens>::type views_;

    const u8* active_dl_      = nullptr;   // last built list (points into a static)
    u16       active_dl_size_ = 0;
};

} // namespace engine

#endif // ENGINE_SCREEN_H
