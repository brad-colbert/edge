#ifndef DEMO_TANK_PLAYFIELD_ASSETS_H
#define DEMO_TANK_PLAYFIELD_ASSETS_H

// playfield_assets.h — embedded ATank-derived assets for the Stage 2 playfield.
//
// The raw bytes live in build-generated headers (see CMakeLists: each binary in
// demo/tank/assets/ is turned into a ROM-resident `unsigned char[]` by
// cmake/generate_charset_header.cmake (tileset) / generate_bytes_header.cmake
// (chunks)). This header wraps them in the canonical EDGE forms.
//
// Provenance + licensing: see demo/tank/assets/PROVENANCE.md (ATank, GPLv3, same
// author; embedded under EDGE/MIT at the owner's direction).

#include <engine/tiles.h>
#include <engine/types.h>

#include "tank_tileset.gen.h"   // demo::tank::assets::tileset_bytes[1024]
#include "chunk_0_0.gen.h"      // demo::tank::assets::chunk_0_0[960]  (top-left)
#include "chunk_1_0.gen.h"      // demo::tank::assets::chunk_1_0[960]  (top-right)
#include "chunk_0_1.gen.h"      // demo::tank::assets::chunk_0_1[960]  (bottom-left)
#include "chunk_1_1.gen.h"      // demo::tank::assets::chunk_1_1[960]  (bottom-right)

namespace tank {

using engine::u8;

// The 1K Mode 4 tileset (ATank a4color1.fnt), ROM-resident. (Palette lives in
// tank_palette.h so LiveSession can set colours without linking these assets.)
inline constexpr engine::Charset1K tileset =
    engine::make_tileset(demo::tank::assets::tileset_bytes);

// ROM-resident chunk payloads, indexed by chunk grid coordinate (chunk_x, chunk_y).
inline const u8* chunk_payload(u8 chunk_x, u8 chunk_y) {
    if (chunk_y == 0) return chunk_x == 0 ? demo::tank::assets::chunk_0_0
                                          : demo::tank::assets::chunk_1_0;
    return chunk_x == 0 ? demo::tank::assets::chunk_0_1
                        : demo::tank::assets::chunk_1_1;
}

}  // namespace tank

#endif  // DEMO_TANK_PLAYFIELD_ASSETS_H
