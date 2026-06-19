#ifndef DEMO_TANK_PLAYFIELD_GEOMETRY_H
#define DEMO_TANK_PLAYFIELD_GEOMETRY_H

// playfield_geometry.h — demo-local geometry for the four-chunk ANTIC Mode 4
// playfield (Stage 2). Shared by atari_tank_demo.cpp and the host geometry test.
//
// All values are the Altirra-MEASURED Stage 1.1 invariants (ADR-034 terminology):
// an 80x48 logical tile map built from a 2x2 grid of 40x24 map chunks, scrolled
// inside a 40x24 viewport, stored in an 88x48 PHYSICAL tile map whose row carries
// 4 padding cells on each side ([4 pad][80 logical][4 pad]). No vertical padding.
//
// This header holds ONLY demo-local geometry, the direct chunk-placement routine,
// and the camera<->scroll conversions. It introduces no generic engine types and
// no chunk-management abstraction (no MapChunk/ChunkGrid/ChunkLoader/...). Depends
// only on engine/tiles.h (TileMap) + types.h.

#include <engine/tiles.h>
#include <engine/types.h>

namespace tank {

using engine::u8;
using engine::u16;
using engine::i16;

// ── Authoritative compile-time geometry ───────────────────────────────────
struct PlayfieldGeometry {
    static constexpr u8 chunk_columns = 2;
    static constexpr u8 chunk_rows    = 2;

    static constexpr u8 chunk_width   = 40;   // map cells
    static constexpr u8 chunk_height  = 24;   // map cells

    static constexpr u8 logical_width  = 80;  // map cells
    static constexpr u8 logical_height = 48;  // map cells

    static constexpr u8 physical_left_padding  = 4;   // cells (measured Stage 1.1)
    static constexpr u8 physical_right_padding = 4;    // cells

    static constexpr u8 physical_width  = 88;  // row stride (cells/bytes)
    static constexpr u8 physical_height = 48;

    // Camera in nominal square pixels (8 px/cell both axes).
    static constexpr u16 max_camera_nominal_x = 320;  // (logical_width  - 40) * 8
    static constexpr u16 max_camera_nominal_y = 192;  // (logical_height - 24) * 8

    // EDGE scroll units (measured): horizontal color clocks, vertical scanlines.
    static constexpr u16 max_scroll_x = 160;  // (80-40 cells) * 4 cc
    static constexpr u16 max_scroll_y = 192;  // (48-24 cells) * 8 sl

    // Byte footprints.
    static constexpr u16 logical_payload_bytes  = u16(logical_width)  * logical_height;   // 3840
    static constexpr u16 physical_alloc_bytes   = u16(physical_width) * physical_height;   // 4224
    static constexpr u16 chunk_payload_bytes    = u16(chunk_width)    * chunk_height;      // 960
};

// Geometry proofs (the demo must not drift from the measured invariants).
static_assert(PlayfieldGeometry::logical_width  ==
                  PlayfieldGeometry::chunk_columns * PlayfieldGeometry::chunk_width,
              "logical_width must equal chunk_columns * chunk_width");
static_assert(PlayfieldGeometry::logical_height ==
                  PlayfieldGeometry::chunk_rows * PlayfieldGeometry::chunk_height,
              "logical_height must equal chunk_rows * chunk_height");
static_assert(PlayfieldGeometry::physical_width ==
                  PlayfieldGeometry::physical_left_padding +
                  PlayfieldGeometry::logical_width +
                  PlayfieldGeometry::physical_right_padding,
              "physical_width must equal left pad + logical + right pad");
static_assert(PlayfieldGeometry::logical_payload_bytes == 3840, "logical payload is 3840 bytes");
static_assert(PlayfieldGeometry::physical_alloc_bytes  == 4224, "physical allocation is 4224 bytes");
static_assert(PlayfieldGeometry::chunk_payload_bytes   == 960,  "chunk payload is 960 bytes");

// The final writable physical tile map type (bound via Game::scroll_map). Exactly
// one of these exists in the demo; ANTIC DMA-reads it directly.
using PhysicalMap = engine::TileMap<PlayfieldGeometry::physical_width,
                                    PlayfieldGeometry::physical_height>;
static_assert(sizeof(PhysicalMap::cells) == 4224, "physical map is 4224 bytes");

// Neutral fill for padding + pre-load clear. Tile code 0 is the ATank arena
// floor/space glyph, which renders as COLBK — a safe boundary/background.
inline constexpr u8 kNeutralTileCode = 0;

// ── Direct chunk placement (no staging, no per-frame work, no allocation) ──
//
// Copy a 40x24 chunk payload straight into its final position in the physical
// map. logical = chunk*size + local ; physical_x = left_pad + logical_x.
inline void copy_chunk_to_map(PhysicalMap& map, const u8* chunk,
                              u8 chunk_x, u8 chunk_y) {
    using G = PlayfieldGeometry;
    if (chunk_x >= G::chunk_columns || chunk_y >= G::chunk_rows) return;  // grid-bounded
    for (u8 ly = 0; ly < G::chunk_height; ++ly) {
        const u16 physical_y = static_cast<u16>(chunk_y * G::chunk_height + ly);
        const u16 physical_x = static_cast<u16>(G::physical_left_padding +
                                                chunk_x * G::chunk_width);
        const u16 dst = static_cast<u16>(physical_y * G::physical_width + physical_x);
        const u16 src = static_cast<u16>(ly * G::chunk_width);
        for (u8 lx = 0; lx < G::chunk_width; ++lx) map.cells[dst + lx] = chunk[src + lx];
    }
}

// Byte offset of a chunk's first cell (for tests / documentation).
inline constexpr u16 chunk_first_offset(u8 chunk_x, u8 chunk_y) {
    using G = PlayfieldGeometry;
    return static_cast<u16>((chunk_y * G::chunk_height) * G::physical_width +
                            G::physical_left_padding + chunk_x * G::chunk_width);
}

// Initialize every physical cell (padding + logical) to the neutral tile code.
inline void clear_physical_map(PhysicalMap& map) {
    for (u16 i = 0; i < PlayfieldGeometry::physical_alloc_bytes; ++i)
        map.cells[i] = kNeutralTileCode;
}

// ── Camera <-> scroll conversions (pure; explicit unit names) ──────────────
inline constexpr i16 clamp_i16(i16 v, i16 lo, i16 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Clamp a (possibly negative) camera position to the legal nominal-pixel range.
inline constexpr u16 clamp_camera_nominal_x(i16 v) {
    return static_cast<u16>(clamp_i16(v, 0,
        static_cast<i16>(PlayfieldGeometry::max_camera_nominal_x)));
}
inline constexpr u16 clamp_camera_nominal_y(i16 v) {
    return static_cast<u16>(clamp_i16(v, 0,
        static_cast<i16>(PlayfieldGeometry::max_camera_nominal_y)));
}

// nominal px -> EDGE scroll units: X is color clocks (2 nominal px each), Y is
// scanlines (1 nominal px each). See Stage 1.1 measurements.
inline constexpr u16 scroll_color_clocks_x(u16 camera_nominal_x) {
    return static_cast<u16>(camera_nominal_x >> 1);
}
inline constexpr u16 scroll_scanlines_y(u16 camera_nominal_y) {
    return camera_nominal_y;
}

}  // namespace tank

#endif  // DEMO_TANK_PLAYFIELD_GEOMETRY_H
