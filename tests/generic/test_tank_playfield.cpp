// test_tank_playfield.cpp — pure geometry + chunk-placement + camera invariants
// for the Stage 2 tank demo (demo/tank/playfield_geometry.h). Built for mos-sim,
// run under CTest (exit 0 = pass). No Atari hardware, no assets — synthetic chunks
// verify placement; the camera math is pure.

#include <stdint.h>
#include <stdio.h>

#include "demo/tank/playfield_geometry.h"

using engine::u8;
using engine::u16;

static unsigned g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

using GG = tank::PlayfieldGeometry;

// One real physical map + four synthetic chunk payloads (each a distinct marker).
EDGE_SCROLL_TILE_MAP(tank::PhysicalMap, g_map);
static u8 g_chunk[2][2][960];   // [chunk_x][chunk_y]
static constexpr u8 marker(u8 cx, u8 cy) { return static_cast<u8>(1 + cy * 2 + cx); } // 1..4

static void fill_synthetic_chunks() {
    for (u8 cy = 0; cy < 2; ++cy)
        for (u8 cx = 0; cx < 2; ++cx)
            for (u16 i = 0; i < 960; ++i) g_chunk[cx][cy][i] = marker(cx, cy);
}

// ── 1-7: geometry constants ────────────────────────────────────────────────
static void test_geometry() {
    CHECK(GG::chunk_columns == 2 && GG::chunk_rows == 2);
    CHECK(GG::chunk_width == 40 && GG::chunk_height == 24);
    CHECK(GG::logical_width == 80 && GG::logical_height == 48);
    CHECK(GG::logical_payload_bytes == 3840);
    CHECK(GG::physical_width == 88 && GG::physical_height == 48);
    CHECK(GG::physical_alloc_bytes == 4224);
    CHECK(GG::physical_left_padding == 4 && GG::physical_right_padding == 4);
    // First-cell offsets (derive + verify, not copy from the prompt).
    CHECK(tank::chunk_first_offset(0, 0) == 4);
    CHECK(tank::chunk_first_offset(1, 0) == 44);
    CHECK(tank::chunk_first_offset(0, 1) == 2116);
    CHECK(tank::chunk_first_offset(1, 1) == 2156);
}

// ── 8-15: chunk placement, coverage, padding, bounds ───────────────────────
static void test_placement() {
    fill_synthetic_chunks();
    tank::clear_physical_map(g_map);
    for (u8 cy = 0; cy < 2; ++cy)
        for (u8 cx = 0; cx < 2; ++cx)
            tank::copy_chunk_to_map(g_map, &g_chunk[cx][cy][0], cx, cy);

    // 12 + 8-11: every logical cell holds exactly its owning chunk's marker, at
    // physical column (logical_x + 4).
    bool coverage_ok = true;
    for (u16 ly = 0; ly < GG::logical_height; ++ly) {
        for (u16 lx = 0; lx < GG::logical_width; ++lx) {
            const u8 cx = static_cast<u8>(lx / GG::chunk_width);
            const u8 cy = static_cast<u8>(ly / GG::chunk_height);
            const u16 phys = static_cast<u16>(ly * GG::physical_width +
                                              GG::physical_left_padding + lx);
            if (g_map.cells[phys] != marker(cx, cy)) coverage_ok = false;
        }
    }
    CHECK(coverage_ok);  // covers items 8,9,10,11,12

    // 13 + 15: padding columns (0..3 and 84..87) keep the neutral code (no chunk
    // write reached them, and the pre-clear value survives).
    bool padding_clean = true;
    for (u16 r = 0; r < GG::physical_height; ++r) {
        for (u8 p = 0; p < GG::physical_left_padding; ++p)
            if (g_map.cells[r * GG::physical_width + p] != tank::kNeutralTileCode)
                padding_clean = false;
        for (u8 p = 0; p < GG::physical_right_padding; ++p) {
            const u16 col = static_cast<u16>(GG::physical_left_padding +
                                             GG::logical_width + p);
            if (g_map.cells[r * GG::physical_width + col] != tank::kNeutralTileCode)
                padding_clean = false;
        }
    }
    CHECK(padding_clean);

    // 14: no chunk write exceeds the physical map. The last cell written is chunk
    // (1,1)'s bottom-right: offset = chunk_first_offset(1,1) + 23*88 + 39.
    const u16 last = static_cast<u16>(tank::chunk_first_offset(1, 1) + 23 * 88 + 39);
    CHECK(last < GG::physical_alloc_bytes);   // 4219 < 4224
}

// ── 16-20: camera clamp + scroll conversion + coarse legality ──────────────
static void test_camera() {
    // 16: camera X clamps to 0..320.
    CHECK(tank::clamp_camera_nominal_x(-50) == 0);
    CHECK(tank::clamp_camera_nominal_x(500) == 320);
    CHECK(tank::clamp_camera_nominal_x(160) == 160);
    // 17: camera Y clamps to 0..192.
    CHECK(tank::clamp_camera_nominal_y(-1)  == 0);
    CHECK(tank::clamp_camera_nominal_y(999) == 192);
    CHECK(tank::clamp_camera_nominal_y(96)  == 96);
    // 18/19: conversions produce 0..160 / 0..192.
    CHECK(tank::scroll_color_clocks_x(0)   == 0);
    CHECK(tank::scroll_color_clocks_x(320) == 160);
    CHECK(tank::scroll_scanlines_y(0)   == 0);
    CHECK(tank::scroll_scanlines_y(192) == 192);
    // 20: maximum legal camera -> legal coarse map coordinates.
    const u16 sx = tank::scroll_color_clocks_x(GG::max_camera_nominal_x);  // 160
    const u16 sy = tank::scroll_scanlines_y(GG::max_camera_nominal_y);     // 192
    const u16 coarse_col = static_cast<u16>(sx / 4 + (sx % 4 ? 1 : 0));    // invert_x
    const u16 coarse_row = static_cast<u16>(sy / 8);
    const u16 max_coarse_col = GG::physical_width  - 48;   // 40
    const u16 max_coarse_row = GG::physical_height - 24;   // 24
    CHECK(coarse_col <= max_coarse_col);   // 40 <= 40
    CHECK(coarse_row <= max_coarse_row);   // 24 <= 24
    // and the last visible line's fetch stays inside the physical map.
    const uint32_t last_fetch =
        static_cast<uint32_t>(coarse_row + 24 - 1) * GG::physical_width + coarse_col + 47;
    CHECK(last_fetch < GG::physical_alloc_bytes);
}

int main() {
    test_geometry();
    test_placement();
    test_camera();
    if (g_failures == 0) printf("ALL TESTS PASSED\n");
    else                 printf("%u FAILURES\n", g_failures);
    return g_failures != 0;
}
