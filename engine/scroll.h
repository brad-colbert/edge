#ifndef ENGINE_SCROLL_H
#define ENGINE_SCROLL_H

// scroll.h — the portable scroll subsystem.
//
// `ScrollManager<Platform>` tracks a viewport position into a map and splits it
// into the two parts hardware scrolling uses (docs/API_DESIGN.md "Scroll",
// docs/ARCHITECTURE.md "Scroll"):
//
//   * fine scroll — the sub-cell remainder, written straight to the fine-scroll
//     registers through Platform::hal::set_fine_scroll_x / set_fine_scroll_y.
//   * coarse scroll — whole-cell movement, exposed as coarse_col()/coarse_row()
//     for the backend to turn into display-program load addresses.
//
// IMPORTANT (Dependency Rule 2): this is the generic layer. It knows NOTHING of
// the backend display-program byte encoding — no load-address layout, no opcode
// bits, no pointer into the program. It owns only the position, the fine/coarse
// split, and the fine-register writes (via the HAL). The coordinator
// (engine/screen.h) reads coarse_col()/coarse_row() and hands them to the
// backend's patch routine, which is the sole owner of the display-program bytes.
//
// Geometry and hardware conventions are supplied by the screen manager through
// activate() when a scroll-active screen is bound; deactivate() clears them. In
// particular, whether a fine-scroll register scrolls the picture *opposite* to
// the coarse pointer is a backend property passed in (invert_x/invert_y) rather
// than assumed here.
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
    // Called when a scroll-active screen is bound. Geometry and conventions come
    // from the active mode (display traits) and the scroll region's map:
    //   map_width / map_height — full map size in the region's native units
    //                            (text: columns/rows). Bounds coarse scrolling.
    //   visible_lines          — on-screen mode lines of the scroll region.
    //   fetch_width            — cells the hardware fetches per scrolled line, in
    //                            the region's native units. Scroll hardware can
    //                            fetch wider than it displays, so the right-edge
    //                            clamp uses this, not the display width, to stay
    //                            inside the map.
    //   scanlines_per_line     — mode-line height; the vertical fine modulus.
    //   fine_scroll_range      — fine units per cell; the horizontal fine modulus.
    //   invert_x / invert_y    — true when that axis's fine-scroll register moves
    //                            the picture OPPOSITE to the coarse pointer, so the
    //                            remainder must be inverted (and the cell carried)
    //                            to keep fine and coarse advancing together.
    void activate(u16 map_width, u16 map_height, u8 visible_lines,
                  u8 fetch_width, u8 scanlines_per_line, u8 fine_scroll_range,
                  bool invert_x, bool invert_y) {
        map_width_     = map_width;
        map_height_    = map_height;
        visible_lines_ = visible_lines;
        fetch_         = fetch_width;
        splpl_         = scanlines_per_line;
        fsr_           = fine_scroll_range;
        invert_x_      = invert_x;
        invert_y_      = invert_y;
        active_        = true;
    }

    void deactivate() {
        active_ = false;
        map_width_ = map_height_ = 0;
        visible_lines_ = fetch_ = splpl_ = fsr_ = 0;
        invert_x_ = invert_y_ = false;
    }

    // ── Fine scroll → hardware ────────────────────────────────────────
    // Write the sub-cell remainder to the fine-scroll registers. The caller gates
    // this on active()/suspended(); kept register-only so this header stays off
    // the display program entirely.
    //
    // On an axis whose register scrolls opposite to the coarse pointer (invert_*),
    // the raw remainder would make fine creep against coarse and snap at each cell
    // boundary, so it is inverted (and coarse_col/coarse_row carry one cell) to
    // keep the two advancing together. Whether each axis inverts is a backend fact
    // passed to activate(), not assumed here.
    void write_fine() const {
        const u8 rx = static_cast<u8>(scroll_x_ % fsr_);
        const u8 ry = static_cast<u8>(scroll_y_ % splpl_);
        Platform::hal::set_fine_scroll_x(invert_x_ ? invert(rx, fsr_)   : rx);
        Platform::hal::set_fine_scroll_y(invert_y_ ? invert(ry, splpl_) : ry);
    }

    // ── Coarse scroll (whole cells), clamped to the map edges ─────────
    // When an axis's fine remainder is inverted (write_fine), its cell is advanced
    // by one to keep the total displacement monotonic; otherwise the coarse value
    // is the plain quotient. Horizontal stops so the visible window's right edge
    // never passes the map's; vertical likewise at the bottom.
    u16 coarse_col() const {
        u16 c = static_cast<u16>(scroll_x_ / fsr_);
        if (invert_x_ && (scroll_x_ % fsr_)) ++c;
        // Clamp to the FETCH width: the hardware reads `fetch_` cells per scrolled
        // line, so the last in-map coarse column is map_width - fetch_, not - display.
        const u16 max_c = map_width_ > fetch_ ? static_cast<u16>(map_width_ - fetch_) : 0;
        return c > max_c ? max_c : c;
    }
    u16 coarse_row() const {
        u16 r = static_cast<u16>(scroll_y_ / splpl_);
        if (invert_y_ && (scroll_y_ % splpl_)) ++r;
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
    u8   fetch_         = 0;   // fetch_width        — cells fetched per scrolled line
    u8   splpl_         = 0;   // scanlines_per_line — vertical fine modulus / row divisor
    u8   fsr_           = 0;   // fine_scroll_range  — horizontal fine modulus / col divisor
    bool invert_x_      = false; // fine X register opposes the coarse pointer
    bool invert_y_      = false; // fine Y register opposes the coarse pointer
};

} // namespace engine

#endif // ENGINE_SCROLL_H
