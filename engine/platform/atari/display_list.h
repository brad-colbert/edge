#ifndef ENGINE_PLATFORM_ATARI_DISPLAY_LIST_H
#define ENGINE_PLATFORM_ATARI_DISPLAY_LIST_H

// platform/atari/display_list.h — the ANTIC display-list builder.
//
// This is the Atari backend's "display program" builder: it turns a portable
// engine::DisplayLayout (region geometry only) into a fully-addressed ANTIC
// display list — blank lines, one LMS per region plus an extra LMS at every mode
// line that enters a new 4K page, and a terminating JVB. It lives in the backend
// (not engine/screen.h) because the byte encoding, the LMS/JVB opcodes, and the
// 4K-scan-boundary rule are all ANTIC specifics.
//
// engine::ScreenManager reaches this type only as `Platform::display_program<Layout>`
// (platform.h), so the generic screen manager names nothing ANTIC; it owns the
// shared buffer and views and asks the builder to emit `bytes`/`size`.
//
// 4K scan-address boundaries (ADR-026 refinement): ANTIC increments only the low
// 12 bits of its memory-scan counter, so screen data cannot cross a $x000 boundary
// without a fresh LMS reloading the full address. Crossing positions depend on the
// absolute (runtime) buffer address, so the list is built at set_screen time and
// the builder inserts an extra LMS at every mode line that enters a new 4K page.

#include "../../display.h"
#include "../../types.h"
#include "antic.h"

namespace atari {

using engine::u8;
using engine::u16;

namespace detail {

// Base display-list length for `Layout` assuming one LMS per region (no 4K
// crossings): 3 blank-line instructions + per region (1 mode/LMS byte + 2 LMS
// address bytes + height-1 mode bytes) + a 3-byte JVB. A scroll region instead
// emits one LMS (3 bytes) per visible line, so it contributes 3*height.
template <typename Layout>
constexpr u16 display_list_base_size() {
    u16 s = 3 + 3;
    for (u8 i = 0; i < Layout::region_count; ++i) {
        if (Layout::region_is_overlay[i]) {
            // Overlay region: ceil(H/8) blank instructions, no mode lines / LMS.
            s += static_cast<u16>((Layout::region_height[i] + 7) / 8);
        } else if (Layout::region_is_scroll[i]) {
            s += static_cast<u16>(3 * Layout::region_height[i]);
        } else {
            s += static_cast<u16>(Layout::region_height[i]) + 2;
        }
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
    // A pure-overlay layout is just the 3-byte self-looping JVB stub.
    if constexpr (Layout::is_pure_overlay) return 3;
    u16 s = display_list_base_size<Layout>();
    for (u8 i = 0; i < Layout::region_count; ++i) {
        s += static_cast<u16>(2 * lms_crossings_max(Layout::region_ram[i]));
    }
    return s;
}

// Worst-case total LMS count: a scroll region emits one LMS per visible line; a
// normal region emits one plus its potential 4K crossings.
template <typename Layout>
constexpr u16 max_lms_count() {
    u16 n = 0;
    for (u8 i = 0; i < Layout::region_count; ++i) {
        if (Layout::region_is_overlay[i]) continue;   // overlays emit no LMS
        n += Layout::region_is_scroll[i]
                 ? static_cast<u16>(Layout::region_height[i])
                 : static_cast<u16>(1 + lms_crossings_max(Layout::region_ram[i]));
    }
    // Min 1 so a pure-overlay layout never declares a zero-length lms_pos[] array.
    return n < 1 ? 1 : n;
}

// Total scroll LMS (one per visible line of every scroll region). Sized at least
// 1 so DisplayProgram::scroll_lms_pos[] is never a zero-length array.
template <typename Layout>
constexpr u16 max_scroll_lms_count() {
    u16 n = 0;
    for (u8 i = 0; i < Layout::region_count; ++i) {
        if (Layout::region_is_scroll[i]) n += Layout::region_height[i];
    }
    return n < 1 ? 1 : n;
}

} // namespace detail

// ── DisplayProgram ────────────────────────────────────────────────────
//
// The RAM-resident ANTIC display list for one layout (ADR-026, refined for 4K
// boundaries). `build()` emits a fully-addressed program so ANTIC can read
// `bytes` directly. The arrays are sized at compile time for the worst case over
// all base alignments; `build()` records the actual size and the LMS positions
// for inspection.
//
// LMS-insertion rule: an LMS is placed at the first mode line whose START enters
// a new 4K page. This is exact when 4K boundaries fall on mode-line boundaries.
// A single mode line whose own data straddles a boundary (only with a
// mode-line-misaligned base) still corrupts its tail — unavoidable via the
// display list alone; place the shared buffer so 4K boundaries align to
// mode-line edges.
template <typename Layout>
struct DisplayProgram {
    static constexpr u8  region_count = Layout::region_count;
    static constexpr u16 capacity     = detail::display_list_capacity<Layout>();
    static constexpr u16 max_lms      = detail::max_lms_count<Layout>();
    static constexpr u16 max_scroll_lms = detail::max_scroll_lms_count<Layout>();

    u8  bytes[capacity]              = {};
    u16 size                         = 0;   // bytes actually used by build()
    u16 jvb_pos                      = 0;   // index of the JVB operand low byte
    u16 lms_count                    = 0;
    u16 lms_pos[max_lms]             = {};   // low-byte index of every LMS
    u16 region_lms_pos[region_count] = {};   // low-byte index of each region's first LMS
    // Low-byte index of every scroll-region LMS, in visible-line (top-to-bottom)
    // order, so patch_scroll() can repoint each line independently. The generic
    // ScrollManager never sees these — it only supplies coarse col/row.
    u16 scroll_lms_pos[max_scroll_lms] = {};
    u16 scroll_lms_count               = 0;

    // Build the display list for screen memory based at `screen_base`, with the
    // list itself residing at `dl_base` (used by the JVB to loop the list).
    constexpr void build(u16 screen_base, u16 dl_base) {
        u16 p   = 0;
        lms_count = 0;
        scroll_lms_count = 0;

        // Pure-overlay layout: ANTIC DMA is disabled entirely by the screen
        // manager, so the list need only satisfy the DLISTL pointer requirement.
        // Emit a 3-byte self-looping JVB and nothing else.
        if constexpr (Layout::is_pure_overlay) {
            bytes[0] = DL_JVB;
            jvb_pos  = 1;
            bytes[1] = lo(dl_base);
            bytes[2] = hi(dl_base);
            size     = 3;
            return;
        }

        // Three 8-blank-line instructions (24 scanlines) to centre vertically.
        bytes[p++] = dl_blank(8);
        bytes[p++] = dl_blank(8);
        bytes[p++] = dl_blank(8);

        for (u8 r = 0; r < region_count; ++r) {
            const u8  mode = Layout::region_mode_byte[r];

            if (Layout::region_is_overlay[r]) {
                // Overlay region: its pixels live in VBXE VRAM, not the ANTIC
                // screen buffer, so reserve its scanlines with blank-line
                // instructions (ceil(H/8)) instead of mode lines / LMS. Region
                // order thus controls the overlay's vertical position.
                u16 blanks = Layout::region_height[r];
                while (blanks > 0) {
                    u8 n = blanks > 8 ? static_cast<u8>(8) : static_cast<u8>(blanks);
                    bytes[p++] = dl_blank(n);
                    blanks = static_cast<u16>(blanks - n);
                }
                region_lms_pos[r] = 0;   // sentinel: no LMS for overlay regions
                continue;
            }

            if (Layout::region_is_scroll[r]) {
                // Scroll region: every visible line gets its own LMS (with the
                // HSCROLL/VSCROLL bits) striding by the map width, so a map wider
                // than the fetch width stays row-aligned and 4K crossings are moot
                // (each line reloads the full address anyway). The addresses here
                // are placeholders against `screen_base`; scroll_map() repoints
                // them at the bound map via patch_scroll().
                const u8  op     = static_cast<u8>(mode | DL_LMS | DL_HSCROLL | DL_VSCROLL);
                const u16 stride = Layout::region_map_width[r];
                u16 line_start   = static_cast<u16>(screen_base + Layout::offset(r));
                for (u8 i = 0; i < Layout::region_height[r]; ++i) {
                    const u16 pos = emit_load(p, op, line_start);
                    scroll_lms_pos[scroll_lms_count++] = pos;
                    if (i == 0) region_lms_pos[r] = pos;
                    line_start = static_cast<u16>(line_start + stride);
                }
                continue;
            }

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
        bytes[p++] = DL_JVB;
        jvb_pos = p;
        bytes[p++] = lo(dl_base);
        bytes[p++] = hi(dl_base);
        size = p;
    }

    // Repoint every scroll-region LMS at the bound map for a coarse (col, row)
    // offset: visible line i loads `map_base + (coarse_row + i)*map_width +
    // coarse_col`. This is the ONLY place the ANTIC LMS bytes are rewritten at run
    // time; the generic ScrollManager supplies only the coarse offsets (it never
    // touches the display list). A no-op for layouts with no scroll region.
    void patch_scroll(u16 map_base, u16 map_width, u16 coarse_col, u16 coarse_row) {
        for (u16 i = 0; i < scroll_lms_count; ++i) {
            const u16 a = static_cast<u16>(
                map_base + static_cast<u16>(coarse_row + i) * map_width + coarse_col);
            bytes[scroll_lms_pos[i]]     = lo(a);
            bytes[scroll_lms_pos[i] + 1] = hi(a);
        }
    }

private:
    // Emit display-list opcode `op` + the 2-byte address `a`; record and return
    // the low-byte position. `p` is advanced past the 3 emitted bytes.
    constexpr u16 emit_load(u16& p, u8 op, u16 a) {
        bytes[p++] = op;
        const u16 pos = p;
        bytes[p++] = lo(a);
        bytes[p++] = hi(a);
        lms_pos[lms_count++] = pos;
        return pos;
    }
    constexpr u16 emit_lms(u16& p, u8 mode, u16 a) {
        return emit_load(p, static_cast<u8>(mode | DL_LMS), a);
    }
    static constexpr u8 lo(u16 a) { return static_cast<u8>(a & 0xFF); }
    static constexpr u8 hi(u16 a) { return static_cast<u8>(a >> 8); }
};

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_DISPLAY_LIST_H
