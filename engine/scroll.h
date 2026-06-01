#ifndef ENGINE_SCROLL_H
#define ENGINE_SCROLL_H

// scroll.h — the portable scroll subsystem.
//
// `ScrollManager<Platform>` tracks a viewport position into a map and splits it
// into the two parts ANTIC-class hardware scrolls in (docs/API_DESIGN.md
// "Scroll", docs/ARCHITECTURE.md "Scroll"):
//
//   * fine scroll — the sub-cell remainder, written straight to the fine-scroll
//     registers through Platform::hal::set_fine_scroll_x / set_fine_scroll_y.
//   * coarse scroll — whole-cell movement, exposed as coarse_col()/coarse_row()
//     for the backend to turn into display-program load addresses.
//
// IMPORTANT (Dependency Rule 2): this is the generic layer. It knows NOTHING of
// the backend display-list byte encoding — no LMS layout, no opcode bits, no
// pointer into the list. It owns only the position, the fine/coarse split, and
// the fine-register writes (via the HAL). The coordinator (engine/screen.h) reads
// coarse_col()/coarse_row() and hands them to the backend's patch routine, which
// is the sole owner of the LMS bytes.
//
// Mode-dependent geometry is supplied by the screen manager through activate()
// when a scroll-active screen is bound; deactivate() clears it.
//
// Depends on types.h only.

#include "types.h"

namespace engine {

template <typename Platform>
class ScrollManager {
public:
    // ── Position ──────────────────────────────────────────────────────
    // Absolute viewport position in the map (horizontal in fine units, vertical
    // in scanlines).
    void set(u16 x, u16 y) { scroll_x_ = x; scroll_y_ = y; }

    // Relative scroll. Signed deltas are applied into the unsigned position
    // and clamped at 0 (the map origin) rather than wrapping.
    void move(i16 dx, i16 dy) {
        scroll_x_ = add_clamped(scroll_x_, dx);
        scroll_y_ = add_clamped(scroll_y_, dy);
    }

    u16 x() const { return scroll_x_; }
    u16 y() const { return scroll_y_; }

    // ── Suspension ────────────────────────────────────────────────────
    // While suspended the engine stops writing the scroll hardware; the
    // tracked position keeps updating so resume() picks up where it left off.
    void suspend() { suspended_ = true;  }
    void resume()  { suspended_ = false; }

    bool active()    const { return active_; }
    bool suspended() const { return suspended_; }

    // ── Activation (driven by the screen manager, not the user) ────────
    // Called when a scroll-active screen is bound. Geometry comes from the active
    // mode (display traits) and the scroll region's map:
    //   map_width / map_height — full map size in the region's native units
    //                            (text: columns/rows). Bounds coarse scrolling.
    //   visible_lines          — on-screen mode lines of the scroll region.
    //   fetch_width            — bytes ANTIC fetches per scrolled line, in the
    //                            region's native units. With horizontal scrolling
    //                            ANTIC reads WIDER than the display (Mode 2: 48 vs
    //                            40), so the right-edge clamp uses this, not the
    //                            display width, to stay inside the map.
    //   scanlines_per_line     — mode-line height; the vertical fine modulus.
    //   fine_scroll_range      — fine units per cell; the horizontal fine modulus.
    void activate(u16 map_width, u16 map_height, u8 visible_lines,
                  u8 fetch_width, u8 scanlines_per_line, u8 fine_scroll_range) {
        map_width_     = map_width;
        map_height_    = map_height;
        visible_lines_ = visible_lines;
        fetch_         = fetch_width;
        splpl_         = scanlines_per_line;
        fsr_           = fine_scroll_range;
        active_        = true;
    }

    void deactivate() {
        active_ = false;
        map_width_ = map_height_ = 0;
        visible_lines_ = fetch_ = splpl_ = fsr_ = 0;
    }

    // ── Fine scroll → hardware ────────────────────────────────────────
    // Write the sub-cell remainder to the fine-scroll registers. The caller gates
    // this on active()/suspended(); kept register-only so this header stays off
    // the display list entirely.
    //
    // Hardware direction (ANTIC): the two fine registers are NOT symmetric.
    // Raising HSCROL pushes the picture RIGHT — opposite to the way the LMS (coarse)
    // pointer moves it horizontally — so the raw remainder would make horizontal
    // fine creep against coarse and snap at each cell boundary; we invert it (and
    // coarse_col absorbs it with a one-cell step) so fine and coarse advance
    // together. Raising VSCROL pushes the picture UP, which is the SAME direction
    // the LMS pointer moves it vertically, so vertical needs no inversion.
    void write_fine() const {
        Platform::hal::set_fine_scroll_x(invert(static_cast<u8>(scroll_x_ % fsr_), fsr_));
        Platform::hal::set_fine_scroll_y(static_cast<u8>(scroll_y_ % splpl_));
    }

    // ── Coarse scroll (whole cells), clamped to the map edges ─────────
    // The horizontal fine remainder is inverted (write_fine), so its cell is
    // advanced by one to keep the total displacement monotonic; the vertical
    // remainder is not, so coarse_row is the plain quotient. Horizontal stops so the
    // visible window's right edge never passes the map's; vertical likewise at the
    // bottom.
    u16 coarse_col() const {
        u16 c = static_cast<u16>(scroll_x_ / fsr_);
        if (scroll_x_ % fsr_) ++c;
        // Clamp to the FETCH width: ANTIC reads `fetch_` bytes per scrolled line,
        // so the last in-map coarse column is map_width - fetch_, not - display.
        const u16 max_c = map_width_ > fetch_ ? static_cast<u16>(map_width_ - fetch_) : 0;
        return c > max_c ? max_c : c;
    }
    u16 coarse_row() const {
        const u16 r = static_cast<u16>(scroll_y_ / splpl_);
        const u16 max_r =
            map_height_ > visible_lines_ ? static_cast<u16>(map_height_ - visible_lines_) : 0;
        return r > max_r ? max_r : r;
    }

private:
    // Invert a sub-cell remainder for a fine register that scrolls opposite to the
    // coarse pointer: 0 stays 0, otherwise `cell - r` (so the register counts down
    // as coarse, bumped by one cell, counts up). See write_fine().
    static u8 invert(u8 r, u8 cell) { return r ? static_cast<u8>(cell - r) : 0; }

    static u16 add_clamped(u16 base, i16 delta) {
        if (delta < 0) {
            const u16 mag = static_cast<u16>(-delta);
            return base > mag ? static_cast<u16>(base - mag) : 0;
        }
        return static_cast<u16>(base + static_cast<u16>(delta));
    }

    u16  scroll_x_      = 0;
    u16  scroll_y_      = 0;
    bool suspended_     = false;
    bool active_        = false;
    u16  map_width_     = 0;   // full map row width  (native units) — coarse-col bound
    u16  map_height_    = 0;   // full map height     (native units) — coarse-row bound
    u8   visible_lines_ = 0;   // on-screen scroll mode lines
    u8   fetch_         = 0;   // fetch_width        — bytes ANTIC reads per scrolled line
    u8   splpl_         = 0;   // scanlines_per_line — vertical fine modulus / row divisor
    u8   fsr_           = 0;   // fine_scroll_range  — horizontal fine modulus / col divisor
};

} // namespace engine

#endif // ENGINE_SCROLL_H
