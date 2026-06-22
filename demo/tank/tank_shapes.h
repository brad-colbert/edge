#ifndef DEMO_TANK_TANK_SHAPES_H
#define DEMO_TANK_TANK_SHAPES_H

// tank_shapes.h — the eight displayed tank silhouettes (Stage 3), ROM-resident.
//
// Derived from ATank's eight directional 8x8 silhouettes
// (/home/brad/Projects/Atari/atank/src/atari/standard_tank.inc, the "Base images"
// block: N, NE, E, SE, S, SW, W, NW — clockwise from North; the trailing idle /
// other frames are not used). Each 8x8 source row is doubled vertically into an
// 8x16 sprite: a normal-width player displays 8 source columns as 16 nominal
// pixels, so an 8x16 source renders as a square 16x16 nominal tank with a clearer
// hull + barrel. The hull centre sits at ~(col 4, row 8) = the anchor used by the
// world->PMG conversion (see tank_motion.h).
//
// Eight silhouettes only: the movement model has sixteen headings (tank_motion.h)
// but display maps them to the nearest of these eight. This is NOT true 22.5-deg
// artwork.
//
// ROM-resident packed-1bpp shapes via the public engine::make_sprite builder.

#include <engine/sprites.h>

namespace tank {

inline constexpr auto tank_n = engine::make_sprite<8, 16>({
    0b00011000,
    0b00011000,
    0b00011000,
    0b00011000,
    0b00100100,
    0b00100100,
    0b01011010,
    0b01011010,
    0b01011010,
    0b01011010,
    0b01011010,
    0b01011010,
    0b01011010,
    0b01011010,
    0b00111100,
    0b00111100,
});

inline constexpr auto tank_ne = engine::make_sprite<8, 16>({
    0b00000000,
    0b00000000,
    0b00000110,
    0b00000110,
    0b00111110,
    0b00111110,
    0b01000100,
    0b01000100,
    0b10010100,
    0b10010100,
    0b10100100,
    0b10100100,
    0b01001000,
    0b01001000,
    0b00110000,
    0b00110000,
});

inline constexpr auto tank_e = engine::make_sprite<8, 16>({
    0b00000000,
    0b00000000,
    0b01111000,
    0b01111000,
    0b10000100,
    0b10000100,
    0b10111011,
    0b10111011,
    0b10111011,
    0b10111011,
    0b10000100,
    0b10000100,
    0b01111000,
    0b01111000,
    0b00000000,
    0b00000000,
});

inline constexpr auto tank_se = engine::make_sprite<8, 16>({
    0b00110000,
    0b00110000,
    0b01001000,
    0b01001000,
    0b10100100,
    0b10100100,
    0b10010100,
    0b10010100,
    0b01000100,
    0b01000100,
    0b00111110,
    0b00111110,
    0b00000110,
    0b00000110,
    0b00000000,
    0b00000000,
});

inline constexpr auto tank_s = engine::make_sprite<8, 16>({
    0b00111100,
    0b00111100,
    0b01011010,
    0b01011010,
    0b01011010,
    0b01011010,
    0b01011010,
    0b01011010,
    0b01011010,
    0b01011010,
    0b00100100,
    0b00100100,
    0b00011000,
    0b00011000,
    0b00011000,
    0b00011000,
});

inline constexpr auto tank_sw = engine::make_sprite<8, 16>({
    0b00001100,
    0b00001100,
    0b00010010,
    0b00010010,
    0b00100101,
    0b00100101,
    0b00101001,
    0b00101001,
    0b00100010,
    0b00100010,
    0b01111100,
    0b01111100,
    0b01100000,
    0b01100000,
    0b00000000,
    0b00000000,
});

inline constexpr auto tank_w = engine::make_sprite<8, 16>({
    0b00000000,
    0b00000000,
    0b00011110,
    0b00011110,
    0b00100001,
    0b00100001,
    0b11011101,
    0b11011101,
    0b11011101,
    0b11011101,
    0b00100001,
    0b00100001,
    0b00011110,
    0b00011110,
    0b00000000,
    0b00000000,
});

inline constexpr auto tank_nw = engine::make_sprite<8, 16>({
    0b00000000,
    0b00000000,
    0b01100000,
    0b01100000,
    0b01111100,
    0b01111100,
    0b00100010,
    0b00100010,
    0b00101001,
    0b00101001,
    0b00100101,
    0b00100101,
    0b00010010,
    0b00010010,
    0b00001100,
    0b00001100,
});

// Eight displayed silhouettes indexed 0..7 (N,NE,E,SE,S,SW,W,NW). All are the
// same SpriteShape<8,16> type, so a runtime-selected const reference is uniform
// and Game::sprite() deduces one Shape type.
inline const engine::SpriteShape<8, 16>& shape_for(engine::u8 silhouette) {
    switch (silhouette) {
        case 0: return tank_n;
        case 1: return tank_ne;
        case 2: return tank_e;
        case 3: return tank_se;
        case 4: return tank_s;
        case 5: return tank_sw;
        case 6: return tank_w;
        default: return tank_nw;
    }
}

}  // namespace tank

#endif  // DEMO_TANK_TANK_SHAPES_H
