#ifndef DEMO_TANK_TANK_PALETTE_H
#define DEMO_TANK_TANK_PALETTE_H

// tank_palette.h — the tank demo's Mode 4 colour-register values, in pixel-value
// order (from the ATank .m4c designer palette): 00->COLBK, 01->COLPF0,
// 10->COLPF1, 11->COLPF2 (codes <128; COLPF3 unused by the maps).
//
// Kept separate from the embedded asset arrays (playfield_assets.h) so the
// LiveSession build, which links no embedded tileset/chunks, can still set the
// palette.

#include <engine/types.h>

namespace tank {

struct Palette {
    static constexpr engine::u8 colbk  = 4;    // background / arena floor
    static constexpr engine::u8 colpf0 = 0;    // walls (black)
    static constexpr engine::u8 colpf1 = 7;    // light detail
    static constexpr engine::u8 colpf2 = 27;   // accent (greenish)
    static constexpr engine::u8 colpf3 = 54;   // unused (inverse) accent
};

}  // namespace tank

#endif  // DEMO_TANK_TANK_PALETTE_H
