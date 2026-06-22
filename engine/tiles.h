#ifndef ENGINE_TILES_H
#define ENGINE_TILES_H

// tiles.h — portable tileset / tile-map subsystem.
//
// This is the Tiles subsystem (ARCHITECTURE.md "Tiles", API_DESIGN.md "Tiles
// and Screen"). It defines three independent pieces:
//
//   * TilesetData — the tileset asset (a collection of character-sized tile
//     definitions). Pure compile-time data, ROM-resident.
//   * TileMap     — a row-major grid of map cells; each cell holds one tile
//     code. It owns its own fixed-size cell array and may live in ROM
//     (constexpr) or RAM (a plain instance the game mutates) depending on how
//     the game declares it.
//   * TileDisplay — the coordinator. It copies a tileset into character-set
//     RAM, binds the backend's character-set base, and tracks the viewport
//     position. It owns NO map data and performs NO map-cell lookup; the map
//     is held by the game or an asset object.
//
// Terminology (see DECISIONS.md ADR and ARCHITECTURE.md "Tile terminology"):
// a *tile* is one character-sized visual element; a *tile code* is the byte
// stored in a *map cell* that selects a tile from the active tileset; a *map
// chunk* is a rectangular loading/management subdivision of a tile map. This
// subsystem implements none of the chunk-management machinery (loading,
// residency, caching, streaming) — that is out of scope here.
//
// Like every engine header it reaches hardware ONLY through the `Platform`
// template parameter (Dependency Rule 2) — never by including a platform
// header. The single hardware touch, bind_charset_page(), goes through
// Platform::hal::set_charset_base, mirroring how sound.h reaches the audio HAL.
//
// The tileset and tile-map assets are constexpr builders that mirror
// make_sprite (sprites.h) and make_sound (sound.h): the author writes a braced
// array of bytes and gets back a ROM-resident value carrying its own size.

#include "types.h"

namespace engine {

// ── TilesetData ───────────────────────────────────────────────────────
//
// A compile-time tileset: the bitmap definitions of the tiles, ready to be
// copied into character-set RAM. `Size` is the byte count: 1024 for a full
// 128-tile set (128 tiles × 8 bytes) or 512 for a half set (64 tiles × 8
// bytes). The type carries its size as a constexpr member (like SpriteShape)
// so init_charset() knows how many bytes to copy without a separate length
// argument. This is the value `make_tileset` returns; it lives in ROM and is
// referenced by pointer/reference.
template <u16 Size>
struct TilesetData {
    static constexpr u16 size = Size;
    u8 data[Size];
};

// Named aliases for the two documented sizes (API_DESIGN.md "Display RAM
// Costs"), named by the character-set RAM footprint they occupy rather than a
// backend mode number. These describe the character-mode (charset) storage a
// tileset fills, so they keep the "charset" spelling.
using Charset1K  = TilesetData<1024>;   // 128 tiles × 8 bytes
using Charset512 = TilesetData<512>;    // 64 tiles × 8 bytes
static_assert(Charset1K::size  == 1024, "1K charset is 1024 bytes");
static_assert(Charset512::size == 512,  "512-byte charset is 512 bytes");

// Build a TilesetData from a braced array of bytes (the eventual
// Game::make_tileset). `N` is deduced from the array, so the returned size
// matches the data exactly. Mirrors make_sprite / make_sound.
template <u16 N>
constexpr TilesetData<N> make_tileset(const u8 (&in)[N]) {
    TilesetData<N> ts{};
    for (u16 i = 0; i < N; ++i) ts.data[i] = in[i];
    return ts;
}

// ── TileMap ───────────────────────────────────────────────────────────
//
// A `Width` × `Height` grid of map cells, each holding one u8 tile code.
// Storage is flat row-major (`cells[row * Width + col]`), matching screen-
// memory layout and avoiding 2D-array template friction. The map owns this
// cell array: a constexpr instance lives in ROM; a plain instance is a RAM
// buffer that set_tile() can modify at runtime. This is the value
// `Game::make_map<W, H>` returns.
template <u16 Width, u16 Height>
struct TileMap {
    static constexpr u16 width  = Width;
    static constexpr u16 height = Height;
    u8 cells[Height * Width];

    // Tile code stored in the map cell at column `col`, row `row` (no bounds
    // check — caller stays in range, like SlotPool's direct access).
    constexpr u8 tile_at(u16 col, u16 row) const {
        return cells[static_cast<u16>(row * Width + col)];
    }

    // Store a tile code in a map cell (for a RAM-resident map). A constexpr/ROM
    // map would fault at runtime, so only call on a mutable instance.
    void set_tile(u16 col, u16 row, u8 tile_code) {
        cells[static_cast<u16>(row * Width + col)] = tile_code;
    }
};

// Build a TileMap from a braced row-major array of tile codes (the eventual
// Game::make_map). `Width`/`Height` are explicit template arguments
// (`make_map<128, 32>({...})`); the array bound `Width * Height` is a dependent
// constant expression, which is valid.
template <u16 Width, u16 Height>
constexpr TileMap<Width, Height> make_map(const u8 (&in)[Width * Height]) {
    TileMap<Width, Height> m{};
    for (u16 i = 0; i < static_cast<u16>(Width * Height); ++i) m.cells[i] = in[i];
    return m;
}

// ── ScrollTileMap (boundary-safe scroll-map storage) ──────────────────────
//
// A RAM TileMap whose cell rows are placed so no row straddles the display's
// scan-address WRAP BOUNDARY. Some display hardware advances only the low bits
// of its memory-scan counter, so a fetched scan line whose bytes span a
// `Boundary`-sized page wraps to the page start and renders garbage. The display
// program reloads the fetch address at every scroll line, which fixes crossings
// *between* lines, but cannot fix a single line whose own fetch straddles a
// boundary — that has to be avoided by where the buffer sits in memory.
//
// This type aligns the buffer to `Boundary` and inserts a head pad of
// `Boundary % Width` cells, so the buffer's single interior boundary falls
// exactly on a row edge (`Boundary - head_pad` is then a whole number of rows).
// Every row therefore lies within one page and no line can straddle. `Boundary`
// is the platform's scan-wrap granularity (Platform caps::screen_buffer_alignment);
// pass 1 for platforms with no such constraint (degenerates to a plain TileMap
// layout). The cell array, width/height, and accessors match TileMap, so a
// ScrollTileMap is a drop-in for any scroll map bound via Core::scroll_map().
//
// Limited to buffers that cross at most one boundary (a single head pad cannot
// row-align two crossings when `Width` does not divide `Boundary`); a static
// assert rejects larger maps, which need a boundary-dividing row stride instead.
namespace detail {
template <u16 N> struct HeadPad { u8 bytes[N]; };
template <>      struct HeadPad<0> {};   // empty base -> zero storage, cells at offset 0
}  // namespace detail

template <u16 Width, u16 Height, u16 Boundary>
struct alignas(Boundary > 1 ? Boundary : 1) ScrollTileMap
    : private detail::HeadPad<(Boundary > 1) ? (Boundary % Width) : 0> {
    static constexpr u16 width    = Width;
    static constexpr u16 height   = Height;
    static constexpr u16 head_pad = (Boundary > 1) ? (Boundary % Width) : 0;

    static_assert(Boundary <= 1 ||
                      static_cast<u32>(head_pad) + static_cast<u32>(Width) * Height <=
                          static_cast<u32>(Boundary) * 2,
                  "ScrollTileMap crosses more than one scan-wrap boundary; a single "
                  "head pad cannot row-align every crossing — use a row stride that "
                  "divides Boundary instead");

    u8 cells[Height * Width];

    constexpr u8 tile_at(u16 col, u16 row) const {
        return cells[static_cast<u16>(row * Width + col)];
    }
    void set_tile(u16 col, u16 row, u8 tile_code) {
        cells[static_cast<u16>(row * Width + col)] = tile_code;
    }
};

// ── TileDisplay ───────────────────────────────────────────────────────
//
// The Tiles coordinator. It loads a tileset into character-set RAM, points the
// backend's character-set base at it, and tracks the viewport position (the
// moving visible window into a tile map). It owns no map data and performs no
// map-cell lookup — the tile map lives in ROM (constexpr) or a RAM buffer the
// game holds, and the game reads it via TileMap::tile_at().
template <typename Platform>
class TileDisplay {
public:
    // Copy a tileset into the destination character-set RAM area (chosen by the
    // screen manager based on display configuration). Templated on the tileset
    // type so any TilesetData<Size> works, mirroring how sprite()/play() accept
    // a template Shape/Effect.
    template <typename Tileset>
    void init_charset(const Tileset& ts, u8* dest) {
        for (u16 i = 0; i < Tileset::size; ++i) dest[i] = ts.data[i];
    }

    // Point the backend's character-set base at the loaded tileset. `page` is
    // the high byte of the char-set address (address / 256).
    void bind_charset_page(u8 page) { Platform::hal::set_charset_base(page); }

    // Set the viewport (top-left) position, in tile-map coordinates. Stored so
    // the game can query it; the Core layer keeps it in sync with the Scroll
    // subsystem each frame. This is plain viewport state — TileDisplay does not
    // use it to index any map.
    void set_viewport(u16 x, u16 y) { viewport_x_ = x; viewport_y_ = y; }

    u16 viewport_x() const { return viewport_x_; }
    u16 viewport_y() const { return viewport_y_; }

private:
    u16 viewport_x_ = 0;
    u16 viewport_y_ = 0;
};

} // namespace engine

#endif // ENGINE_TILES_H
