#ifndef ENGINE_SCROLL_H
#define ENGINE_SCROLL_H

// scroll.h — the portable scroll subsystem.
//
// `ScrollManager<Platform>` tracks a pixel-space viewport position into a
// tilemap and pushes it to the display in two parts (docs/API_DESIGN.md "Scroll",
// docs/ARCHITECTURE.md "Scroll"):
//
//   * fine scroll — the sub-cell remainder, applied through the HAL's
//     Platform::hal::set_fine_scroll_x / set_fine_scroll_y.
//   * coarse scroll — whole-cell movement, applied by patching the display
//     program's load address so the display fetches from a shifted point in the
//     map (see the backend display-program builder).
//
// Mode-dependent geometry (cell width in fine units, cell height in scanlines,
// line width in bytes) is not known to this layer. The screen manager owns the
// active layout/mode and supplies the geometry through activate() when a
// scroll-active screen is selected; deactivate() clears it. This keeps the
// subsystem off any platform header, reaching hardware only through Platform::hal
// (Dependency Rule 2).
//
// Depends on types.h only.

#include "types.h"

namespace engine {

template <typename Platform>
class ScrollManager {
public:
    // ── Position ──────────────────────────────────────────────────────
    // Absolute viewport position in the tilemap (fine units / scanlines).
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

    // ── Activation (driven by the screen manager, not the user) ────────
    // Called at set_screen time for a scroll-active screen. The geometry comes
    // from the active mode (the display traits the screen manager owns):
    //   bytes_per_line     — display width of one mode line, in bytes.
    //   scanlines_per_line — height of one mode line, in scanlines.
    //   fine_scroll_range  — fine units per cell column (fine-scroll wrap point).
    void activate(u8 bytes_per_line, u8 scanlines_per_line, u8 fine_scroll_range) {
        bpl_    = bytes_per_line;
        splpl_  = scanlines_per_line;
        fsr_    = fine_scroll_range;
        active_ = true;
    }

    void deactivate() {
        active_ = false;
        bpl_ = splpl_ = fsr_ = 0;
    }

    // ── Apply to hardware ─────────────────────────────────────────────
    // Push the current position to the display: write the fine-scroll registers
    // and patch the display program's load address for the coarse offset. Called
    // during the frame service (or at set_screen). `load_pos` is the low-byte index
    // of the load address to patch (the backend builder's region_lms_pos[0]).
    // `screen_base` is the tilemap origin; `map_width_bytes` is its full row stride.
    //
    // No-op while suspended or inactive (geometry is then unset).
    void apply(u8* display_list, u16 load_pos, u8* screen_base, u16 map_width_bytes) {
        if (suspended_ || !active_) return;

        const u8 fine_x = static_cast<u8>(scroll_x_ % fsr_);
        const u8 fine_y = static_cast<u8>(scroll_y_ % splpl_);
        Platform::hal::set_fine_scroll_x(fine_x);
        Platform::hal::set_fine_scroll_y(fine_y);

        u16 coarse_col = static_cast<u16>(scroll_x_ / fsr_);
        const u16 coarse_row = static_cast<u16>(scroll_y_ / splpl_);

        // Keep the visible window inside the map's right edge.
        if (map_width_bytes > bpl_) {
            const u16 max_col = static_cast<u16>(map_width_bytes - bpl_);
            if (coarse_col > max_col) coarse_col = max_col;
        } else {
            coarse_col = 0;
        }

        const u16 load_addr = static_cast<u16>(addr(screen_base)
                        + coarse_row * map_width_bytes + coarse_col);
        display_list[load_pos]     = lo(load_addr);
        display_list[load_pos + 1] = hi(load_addr);
    }

private:
    static u16 add_clamped(u16 base, i16 delta) {
        if (delta < 0) {
            const u16 mag = static_cast<u16>(-delta);
            return base > mag ? static_cast<u16>(base - mag) : 0;
        }
        return static_cast<u16>(base + static_cast<u16>(delta));
    }

    static u16 addr(const void* p) {
        return static_cast<u16>(reinterpret_cast<uintptr_t>(p));
    }
    static u8 lo(u16 a) { return static_cast<u8>(a & 0xFF); }
    static u8 hi(u16 a) { return static_cast<u8>(a >> 8); }

    u16  scroll_x_  = 0;
    u16  scroll_y_  = 0;
    bool suspended_ = false;
    bool active_    = false;
    u8   bpl_       = 0;   // bytes_per_line     — scroll-region line width
    u8   splpl_     = 0;   // scanlines_per_line — vertical fine modulus / row divisor
    u8   fsr_       = 0;   // fine_scroll_range  — horizontal fine modulus / col divisor
};

} // namespace engine

#endif // ENGINE_SCROLL_H
