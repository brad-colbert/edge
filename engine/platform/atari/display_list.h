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
        bytes[p++] = dl_blank(8);
        bytes[p++] = dl_blank(8);
        bytes[p++] = dl_blank(8);

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
        bytes[p++] = DL_JVB;
        jvb_pos = p;
        bytes[p++] = lo(dl_base);
        bytes[p++] = hi(dl_base);
        size = p;
    }

private:
    // Emit `mode | LMS` + the 2-byte address `a`; record and return the low-byte
    // position. `p` is advanced past the 3 emitted bytes.
    constexpr u16 emit_lms(u16& p, u8 mode, u16 a) {
        bytes[p++] = static_cast<u8>(mode | DL_LMS);
        const u16 pos = p;
        bytes[p++] = lo(a);
        bytes[p++] = hi(a);
        lms_pos[lms_count++] = pos;
        return pos;
    }
    static constexpr u8 lo(u16 a) { return static_cast<u8>(a & 0xFF); }
    static constexpr u8 hi(u16 a) { return static_cast<u8>(a >> 8); }
};

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_DISPLAY_LIST_H
