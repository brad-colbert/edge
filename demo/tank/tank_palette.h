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
    // Colours chosen in the Mode4 editor, translated to true Atari (GTIA NTSC)
    // register values. NOTE: Mode4's on-screen swatches use a DIFFERENT palette
    // than the hardware, so the raw bytes it saves (02 07 12 5b 3a) render as the
    // wrong hues on a real Atari/Altirra (e.g. its "green" 0x5b is magenta in
    // NTSC). These values reproduce the *appearance* of the Mode4 swatches.
    // Mode4 panel order: Colr4(BG), Colr0..Colr3 -> COLBK, COLPF0..COLPF3.
    static constexpr engine::u8 colbk  = 0x04;  // Colr4(BG) — dark grey
    static constexpr engine::u8 colpf0 = 0x0e;  // Colr0     — light grey / white (walls)
    static constexpr engine::u8 colpf1 = 0x32;  // Colr1     — brick red
    static constexpr engine::u8 colpf2 = 0xc6;  // Colr2     — green
    static constexpr engine::u8 colpf3 = 0x84;  // Colr3     — blue (unused by the maps)
};

}  // namespace tank

#endif  // DEMO_TANK_TANK_PALETTE_H
