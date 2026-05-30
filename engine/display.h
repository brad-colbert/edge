#ifndef ENGINE_DISPLAY_H
#define ENGINE_DISPLAY_H

// display.h — compile-time display layout and typed region views.
//
// A game describes a screen as a `DisplayLayout<Regions...>` where each region
// is a `TextRegion<Mode, Height>` or `BitmapRegion<Mode, Height>` (API_DESIGN.md
// "Mixed Display Layout"). From that the layout computes, at compile time, the
// total screen RAM and each region's byte offset into the shared screen buffer
// (DECISIONS.md ADR-014). Each region also names a typed *view* — a thin handle
// over a screen pointer that exposes only the operations valid for its mode, so
// calling `plot()` on a text region (or `print()` on a bitmap region) is a
// compile error (ARCHITECTURE.md "Screen Manager").
//
// The mode vocabulary and per-mode geometry come from platform/atari/antic.h;
// see that header for the note on this coupling. Views and layouts are otherwise
// platform-free — they only touch screen memory, never hardware registers.

#include "platform/atari/antic.h"
#include "types.h"

namespace engine {

// ── TextRegionView ────────────────────────────────────────────────────
//
// Handle over a text region's screen memory. Templated on the ANTIC mode and
// the row count so the column stride and total length are compile-time
// constants (zero runtime geometry cost). `ptr` is set at set_screen time.
template <atari::Mode Mode, u8 Rows>
struct TextRegionView {
    static_assert(atari::is_text(Mode), "TextRegionView requires a text mode");

    static constexpr u8  columns = atari::bytes_per_line(Mode);
    static constexpr u8  rows    = Rows;
    static constexpr u16 length  = static_cast<u16>(columns) * Rows;

    u8* ptr = nullptr;

    // Write one screen-code tile at (col, row). No bounds check (6502 ethos).
    void put_char(u8 col, u8 row, u8 tile) const {
        ptr[static_cast<u16>(row) * columns + col] = tile;
    }

    // Read the tile at (col, row).
    u8 get_char(u8 col, u8 row) const {
        return ptr[static_cast<u16>(row) * columns + col];
    }

    // Print an ASCII string starting at (col, row), converting to ANTIC internal
    // screen codes. Stops at the NUL terminator; does not wrap rows.
    void print(u8 col, u8 row, const char* s) const {
        u16 base = static_cast<u16>(row) * columns + col;
        for (u8 i = 0; s[i] != '\0'; ++i) {
            ptr[base + i] = atari::ascii_to_internal(s[i]);
        }
    }

    // Print `value` right-aligned in `digits` columns ending at column
    // `col + digits - 1` of `row`. Leading positions are filled with '0'.
    void print_num(u8 col, u8 row, u16 value, u8 digits) const {
        u16 base = static_cast<u16>(row) * columns + col;
        for (u8 i = 0; i < digits; ++i) {
            u8 d = static_cast<u8>(value % 10);
            value /= 10;
            ptr[base + (digits - 1 - i)] = atari::ascii_to_internal('0' + d);
        }
    }
};

// ── BitmapRegionView ──────────────────────────────────────────────────
//
// Handle over a bitmap region's screen memory. Templated on the ANTIC mode and
// the scanline count. Pixel packing is mode-dependent (1bpp / 2bpp). Row
// addressing uses a precomputed row table when `row_table` is set (the
// use_row_table screen flag), otherwise shift-and-add (`y * bytes_per_line`).
template <atari::Mode Mode, u8 Lines>
struct BitmapRegionView {
    static_assert(!atari::is_text(Mode), "BitmapRegionView requires a bitmap mode");

    static constexpr u8  bytes_per_line  = atari::bytes_per_line(Mode);
    static constexpr u8  bpp             = atari::bits_per_pixel(Mode);
    static constexpr u8  pixels_per_byte = static_cast<u8>(8 / bpp);
    static constexpr u8  pixel_mask      = static_cast<u8>((1u << bpp) - 1);
    static constexpr u8  width           = bytes_per_line * pixels_per_byte;
    static constexpr u8  height          = Lines;
    static constexpr u16 length          = static_cast<u16>(bytes_per_line) * Lines;

    u8*        ptr       = nullptr;
    const u16* row_table = nullptr;   // null => shift-and-add row addressing

    // Byte offset of the start of scanline `y`.
    u16 row_base(u8 y) const {
        return row_table ? row_table[y]
                         : static_cast<u16>(y) * bytes_per_line;
    }

    // Set one pixel to `color` (0..pixel_mask). Read-modify-write of the packed
    // byte; the leftmost pixel of a byte occupies the high bits.
    void plot(u8 x, u8 y, u8 color) const {
        u16 idx   = row_base(y) + (x / pixels_per_byte);
        u8  shift = static_cast<u8>((pixels_per_byte - 1 - (x % pixels_per_byte)) * bpp);
        u8  keep  = static_cast<u8>(~(pixel_mask << shift));
        ptr[idx]  = static_cast<u8>((ptr[idx] & keep) |
                                    ((color & pixel_mask) << shift));
    }

    // Read one pixel's colour value.
    u8 point(u8 x, u8 y) const {
        u16 idx   = row_base(y) + (x / pixels_per_byte);
        u8  shift = static_cast<u8>((pixels_per_byte - 1 - (x % pixels_per_byte)) * bpp);
        return static_cast<u8>((ptr[idx] >> shift) & pixel_mask);
    }

    // Fill the whole region with a single colour (replicated across each byte).
    void clear(u8 color) const {
        u8 fill = 0;
        for (u8 k = 0; k < pixels_per_byte; ++k) {
            fill = static_cast<u8>(fill | ((color & pixel_mask) << (k * bpp)));
        }
        for (u16 i = 0; i < length; ++i) ptr[i] = fill;
    }

    // Horizontal line from x1..x2 inclusive on scanline y. Pixel-wise for
    // correctness; a whole-byte run with masked endpoints is a future
    // optimisation (API_DESIGN.md "Bitmap Drawing").
    void hline(u8 x1, u8 x2, u8 y, u8 color) const {
        if (x2 < x1) { u8 t = x1; x1 = x2; x2 = t; }
        for (u8 x = x1; ; ++x) {
            plot(x, y, color);
            if (x == x2) break;
        }
    }

    // Blit a w×h block of packed source pixels (same bpp as this mode) to
    // (x, y). `src` rows are packed ceil(w / pixels_per_byte) bytes each.
    void blit(u8 x, u8 y, const u8* src, u8 w, u8 h) const {
        const u8 src_stride = static_cast<u8>((w + pixels_per_byte - 1) / pixels_per_byte);
        for (u8 r = 0; r < h; ++r) {
            const u8* srow = src + static_cast<u16>(r) * src_stride;
            for (u8 c = 0; c < w; ++c) {
                u8 sh = static_cast<u8>((pixels_per_byte - 1 - (c % pixels_per_byte)) * bpp);
                u8 col = static_cast<u8>((srow[c / pixels_per_byte] >> sh) & pixel_mask);
                plot(static_cast<u8>(x + c), static_cast<u8>(y + r), col);
            }
        }
    }
};

// ── Region descriptors ────────────────────────────────────────────────
//
// Compile-time description of one region: its mode, its height (text rows or
// bitmap mode-lines), its derived byte geometry, and the view type that draws
// into it. `ram_bytes = height * bytes_per_line` (DECISIONS.md ADR-026 worked
// examples).
template <atari::Mode M, u8 Height>
struct TextRegion {
    static constexpr atari::Mode mode           = M;
    static constexpr u8          height         = Height;   // text rows
    static constexpr bool        is_text        = true;
    static constexpr u8          bytes_per_line = atari::bytes_per_line(M);
    static constexpr u16         ram_bytes      = static_cast<u16>(Height) * bytes_per_line;

    using view = TextRegionView<M, Height>;
};

template <atari::Mode M, u8 Height>
struct BitmapRegion {
    static constexpr atari::Mode mode           = M;
    static constexpr u8          height         = Height;   // scanlines / mode-lines
    static constexpr bool        is_text        = false;
    static constexpr u8          bytes_per_line = atari::bytes_per_line(M);
    static constexpr u16         ram_bytes      = static_cast<u16>(Height) * bytes_per_line;

    using view = BitmapRegionView<M, Height>;
};

// ── Pack indexing helper ──────────────────────────────────────────────
//
// pack_element_t<N, Ts...> is the N-th type in the pack (like
// std::tuple_element but without pulling in <tuple>).
namespace detail {
template <u8 N, typename First, typename... Rest>
struct pack_element {
    using type = typename pack_element<N - 1, Rest...>::type;
};
template <typename First, typename... Rest>
struct pack_element<0, First, Rest...> {
    using type = First;
};
} // namespace detail

template <u8 N, typename... Ts>
using pack_element_t = typename detail::pack_element<N, Ts...>::type;

// ── DisplayLayout ─────────────────────────────────────────────────────
//
// Variadic composition of regions. Computes the total screen RAM and each
// region's offset into the shared buffer (the prefix sum of preceding regions'
// ram_bytes). Region types are recoverable by index for the screen manager's
// typed view storage.
template <typename... Regions>
struct DisplayLayout {
    static constexpr u8  region_count = sizeof...(Regions);
    static_assert(region_count >= 1, "a DisplayLayout needs at least one region");

    static constexpr u16 total_ram = (Regions::ram_bytes + ... + 0);

    // Per-region geometry, indexable at compile time and runtime. The height
    // and mode-byte arrays drive the display-list builder (screen.h).
    static constexpr u16 region_ram[region_count]            = { Regions::ram_bytes... };
    static constexpr u8  region_height[region_count]         = { Regions::height... };
    static constexpr u8  region_mode_byte[region_count]      =
        { atari::dl_mode_byte(Regions::mode)... };
    static constexpr u8  region_bytes_per_line[region_count] =
        { Regions::bytes_per_line... };
    static constexpr bool region_is_text[region_count]       = { Regions::is_text... };

    // Byte offset of region n into the shared screen buffer.
    static constexpr u16 offset(u8 n) {
        u16 acc = 0;
        for (u8 i = 0; i < n; ++i) acc += region_ram[i];
        return acc;
    }

    // The N-th region descriptor type, and its view type.
    template <u8 N>
    using region_at = pack_element_t<N, Regions...>;
    template <u8 N>
    using view_at = typename region_at<N>::view;
};

} // namespace engine

#endif // ENGINE_DISPLAY_H
