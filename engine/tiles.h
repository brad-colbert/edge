#ifndef ENGINE_TILES_H
#define ENGINE_TILES_H

// tiles.h — portable tileset / tilemap subsystem.
//
// This is the Tiles subsystem (ARCHITECTURE.md "Tiles", API_DESIGN.md "Tiles
// and Screen"). It owns three things: the character set (tileset) data and its
// load into character-set RAM, the tilemap (a 2D grid of tile indices, ROM-
// resident as a constexpr asset or a RAM buffer for runtime edits), and the
// viewport position into that map. Per ARCHITECTURE.md it is "primarily a
// coordinator, not a data owner."
//
// Like every engine header it reaches hardware ONLY through the `Platform`
// template parameter (Dependency Rule 2) — never by including a platform
// header. The single hardware touch, set_chbase(), goes through
// Platform::hal::write_chbase, mirroring how sound.h reaches POKEY.
//
// The character-set and tilemap assets are constexpr builders that mirror
// make_sprite (sprites.h) and make_sound (sound.h): the author writes a braced
// array of bytes and gets back a ROM-resident value carrying its own size.
//
// TileManager is self-contained: set_viewport() stores the viewport position
// internally for tilemap lookups; it does NOT drive the Scroll subsystem. The
// Core integration layer wires Tiles and Scroll together (a later step), so
// this header depends only on types.h — no sibling dependency on scroll.h.

#include "types.h"

namespace engine {

// ── CharsetData ───────────────────────────────────────────────────────
//
// A compile-time character set. `Size` is the byte count: 1024 for modes 2/4
// (128 chars × 8 bytes) or 512 for modes 6/7 (64 chars × 8 bytes). The type
// carries its size as a constexpr member (like SpriteShape) so init_charset()
// knows how many bytes to copy without a separate length argument. This is the
// value `Game::make_charset` returns; it lives in ROM and is referenced by
// pointer/reference.
template <u16 Size>
struct CharsetData {
    static constexpr u16 size = Size;
    u8 data[Size];
};

// Named aliases for the two documented character-set sizes (API_DESIGN.md
// "Display RAM Costs").
using Charset2 = CharsetData<1024>;   // modes 2/4: 128 chars × 8 bytes
using Charset6 = CharsetData<512>;    // modes 6/7: 64 chars × 8 bytes
static_assert(Charset2::size == 1024, "mode 2/4 charset is 1024 bytes");
static_assert(Charset6::size == 512,  "mode 6/7 charset is 512 bytes");

// Build a CharsetData from a braced array of bytes (the eventual
// Game::make_charset). `N` is deduced from the array, so the returned size
// matches the data exactly. Mirrors make_sprite / make_sound.
template <u16 N>
constexpr CharsetData<N> make_charset(const u8 (&in)[N]) {
    CharsetData<N> cs{};
    for (u16 i = 0; i < N; ++i) cs.data[i] = in[i];
    return cs;
}

// ── TileMap ───────────────────────────────────────────────────────────
//
// A `Width` × `Height` grid of u8 tile indices. Storage is flat row-major
// (`tiles[row * Width + col]`), matching screen-memory layout and avoiding 2D-
// array template friction. A constexpr instance lives in ROM; a plain instance
// is a RAM buffer that set_tile() can modify at runtime. This is the value
// `Game::make_map<W, H>` returns.
template <u16 Width, u16 Height>
struct TileMap {
    static constexpr u16 width  = Width;
    static constexpr u16 height = Height;
    u8 tiles[Height * Width];

    // Tile index at column `col`, row `row` (no bounds check — caller stays in
    // range, like SlotPool's direct access).
    constexpr u8 tile_at(u16 col, u16 row) const {
        return tiles[static_cast<u16>(row * Width + col)];
    }

    // Overwrite a tile (for a RAM-resident map). No-op on a constexpr/ROM map at
    // runtime would fault, so only call on a mutable instance.
    void set_tile(u16 col, u16 row, u8 tile) {
        tiles[static_cast<u16>(row * Width + col)] = tile;
    }
};

// Build a TileMap from a braced row-major array of tile indices (the eventual
// Game::make_map). `Width`/`Height` are explicit template arguments
// (`make_map<128, 32>({...})`); the array bound `Width * Height` is a dependent
// constant expression, which is valid.
template <u16 Width, u16 Height>
constexpr TileMap<Width, Height> make_map(const u8 (&in)[Width * Height]) {
    TileMap<Width, Height> m{};
    for (u16 i = 0; i < static_cast<u16>(Width * Height); ++i) m.tiles[i] = in[i];
    return m;
}

// ── TileManager ───────────────────────────────────────────────────────
//
// The Tiles coordinator. It loads a character set into RAM, points ANTIC's
// CHBASE at it, and tracks the viewport position into the active tilemap. It
// owns no map data — the tilemap lives in ROM (constexpr) or a dedicated RAM
// buffer the game holds.
template <typename Platform>
class TileManager {
public:
    // Copy a character set into the destination char-set RAM area (chosen by the
    // screen manager based on display configuration). Templated on the charset
    // type so any CharsetData<Size> works, mirroring how sprite()/play() accept
    // a template Shape/Effect.
    template <typename Charset>
    void init_charset(const Charset& cs, u8* dest) {
        for (u16 i = 0; i < Charset::size; ++i) dest[i] = cs.data[i];
    }

    // Point ANTIC's CHBASE at the character set. `page` is the high byte of the
    // char-set address (address / 256).
    void set_chbase(u8 page) { Platform::hal::write_chbase(page); }

    // Set the viewport (top-left) position into the tilemap, in the map's own
    // coordinates. Stored internally for tile lookups; the Core layer forwards
    // it to the Scroll subsystem when the screen scrolls.
    void set_viewport(u16 x, u16 y) { viewport_x_ = x; viewport_y_ = y; }

    u16 viewport_x() const { return viewport_x_; }
    u16 viewport_y() const { return viewport_y_; }

private:
    u16 viewport_x_ = 0;
    u16 viewport_y_ = 0;
};

} // namespace engine

#endif // ENGINE_TILES_H
