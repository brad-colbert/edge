// demo/arena/arena_vbxe.cpp — Edge engine "Arena" demo (Berzerk-style), VBXE Tier-2
// full-overlay build (arena_vbxe.xex; mos-atari8-dos target — see CMakeLists.txt).
//
// The SAME game as the native build (arena_native.cpp), rendered ENTIRELY in the VBXE
// overlay: the room, HUD, and all text are drawn into the overlay bitmap with the ANTIC
// playfield OFF, and multi-colour pixel sprites composite over it. Game logic, room
// layout, sound, collision, difficulty, and screen flow are shared verbatim via
// arena_shared.h; only the rendering (and the screen/coordinate plumbing it needs)
// differs from the native build.
//
// Why full-overlay (Tier 2) rather than a transparent overlay over ANTIC text
// (Tier 1): a transparent overlay forces ANTIC playfield DMA to stay ON, and ANTIC
// has VRAM-bus priority over the blitter — running both every frame is the documented
// bus-contention wall (the blitter starves, the VBI overruns, the loop freezes). A
// pure-overlay screen keeps ANTIC DMA off (set_screen does this automatically), so
// the blitter owns the bus. This is the same proven path as the sprites-over-bitmap
// demo. The room/HUD/text are drawn into a VRAM "master" canvas (Background::Bitmap)
// and the compositor restores sprite footprints from it each frame; double-buffered
// for flicker-free motion. Run with BASIC disabled and a VBXE FX core.

#include <engine/platform/atari/platform.h>
#include <engine/platform/atari/vbxe.h>   // power-user: VBXE Config + palette set_color

// The VBXE blitter has no 4-sprite hardware limit (unlike the P/M native build), and
// the skip-unchanged compositor makes stationary enemies free, so this build carries
// more enemies. Must be set before arena_shared.h. Slot budget: player(0) +
// kEnemyCount enemies + 4 bullets must fit OverlayHal::kMaxSlots (12).
#define ARENA_ENEMY_COUNT 6
#include "arena_shared.h"

namespace M = atari;
namespace V = atari::vbxe;

// ── Platform + game configuration ────────────────────────────────────────
//
// DOUBLE-buffered Bitmap — the same proven configuration as the working sprites-
// over-bitmap demo. The room/HUD/text are drawn once into the VRAM master and
// published to both pages; each frame the compositor restores sprite footprints from
// the master and blits the sprites, and overlay_present WAITS for that blit to finish
// before flipping (proper completion sync). Single-buffer's async submit without a
// per-frame completion wait let a long blit be overwritten in flight → cascading
// corruption; double-buffer avoids it. play_enter draws the room with ~15 blitter
// rects (not ~900 per-cell ops), so the old "flip shows a half-drawn page" problem is
// gone too.
// MEMAC window at $A000 (NOT the engine default $B000). CRITICAL: the llvm-mos soft
// (call) stack lives at the top of RAM — ~$BC01 here — which is INSIDE the default
// $B000-$BFFF MEMAC-A window. Enabling that window for VRAM access aliases the call
// stack onto VRAM: every overlay/VRAM operation then corrupts both the call stack
// (→ crash) and the displayed overlay (→ progressive noise). Hardware-diagnosed via
// the soft-stack pointer ($80/$81 = $BC01) sitting in the window. $A000-$AFFF is free
// RAM (BASIC off) between BSS/heap (ends well below $A000) and the stack (above
// $AFFF), so the window no longer overlaps the stack. (The sprites-over-bitmap demo
// happens to survive $B000, but this is a real trap for any overlay program whose
// stack reaches into $B000-$BFFF.)
using VBXECfg = V::Config<M::Mode::VBXE_SR, V::Buffers::Double, V::RegBase::D640,
                          V::MEMAC_A_Cfg<0xA0>, 0x00000, V::Background::Bitmap>;
using Platform = M::Platform<M::Machine::XL, M::RAM::Baseline,
                             M::gfx::VBXE<VBXECfg>, M::Sound::Mono, M::TV::NTSC>;

// Tier 2: every screen is one full-screen VBXE overlay (pure overlay → set_screen
// keeps ANTIC DMA off). All content is drawn into the overlay bitmap; there are no
// ANTIC text regions, no charset base, and no DLI colour split.
struct TitleScreen {
    using display = engine::DisplayLayout<engine::OverlayRegion<M::Mode::VBXE_SR, 240>>;
};
struct PlayScreen {
    using display = engine::DisplayLayout<engine::OverlayRegion<M::Mode::VBXE_SR, 240>>;
};
struct GameOverScreen {
    using display = engine::DisplayLayout<engine::OverlayRegion<M::Mode::VBXE_SR, 240>>;
};

struct GameConfig {
    using screens        = engine::ScreenSet<TitleScreen, PlayScreen, GameOverScreen>;
    using initial_screen = TitleScreen;
    // Tier 2: player (0) + kEnemyCount enemies (1..N) + 4 bullets all composite as
    // blitter overlay sprites (P/M would sit under the opaque overlay). The blitter
    // has no 4-sprite hardware limit, so we carry many more than the native build.
    static constexpr u8 max_sprites      = ARENA_ENEMY_COUNT + 5;   // player + enemies + 4 bullets
    static constexpr u8 sound_channels   = 2;
    static constexpr u8 max_raster_hooks = 1;   // unused on the overlay; keep minimal
    // Bullets are blitter overlay sprites, not hardware missiles, so drop the 2K P/M
    // buffer entirely (frees RAM and keeps .bss below the $A000 MEMAC window).
    static constexpr bool uses_missiles  = false;
};

using Game = engine::Core<Platform, GameConfig>;

// ── Sprite <-> play-area coordinate mapping ──
//
// Tier 2 overlay coordinates: the play area lives in the VBXE framebuffer's pixel
// space, mapped 1:1 onto blitter-sprite (x,y). The exact baseline cell math is kept
// (4px cells; the 8px sprite spans 2 columns / 1 row) so game logic and difficulty
// are byte-identical with the native build — only the origin moves. The blitter-sprite
// X is u8, so the 160px-wide room (40*4) is centred at x=80 to stay within 0..255. The
// HUD occupies the HUD row, then the play area. A top margin pushes everything down out
// of the NTSC top overscan (otherwise the HUD row at y=0 is hidden off the top).
static constexpr u8 kPlayLeftX = 80;             // overlay px of play-area column 0 ((320-160)/2)
static constexpr u8 kCellW     = 4;              // play-area cell width in overlay px
static constexpr u8 kCellH     = 8;              // play-area row height in overlay px
static constexpr u8 kTopMargin = 24;             // shift content below the top overscan (also ~centres 192px in 240)
static constexpr u8 kPlayTopY  = kTopMargin + kCellH; // overlay y of play-area row 0 (HUD row sits at kTopMargin)

// Kept for parity with the native build's P/M colour setup (player_init / play_enter
// still call Game::sprite_color); the overlay's pixel sprites take their colours from
// the palette below, so these per-sprite colour writes are harmless no-ops here.
static constexpr u8 kPlayerColor = 0x0F;
static constexpr u8 kEnemyColor  = 0x34;

// Tile code at a sprite coordinate within the play area, or kNoTile if outside.
static u8 tile_at(u8 sprite_x, u8 sprite_y) {
    if (sprite_x < kPlayLeftX || sprite_y < kPlayTopY) return kNoTile;
    const u8 col = static_cast<u8>((sprite_x - kPlayLeftX) / kCellW);
    const u8 row = static_cast<u8>((sprite_y - kPlayTopY) / kCellH);
    if (col >= 40 || row >= 23) return kNoTile;
    return (&kRoom.t[0][0])[kRowBase.o[row] + col];
}

// True if the 8x8 sprite at (new_x, new_y) clears walls at all four corners.
// Four-corner checking stops the sprite from clipping a wall corner-first.
static bool can_move(u8 new_x, u8 new_y) {
    const u8 x0 = new_x, x1 = static_cast<u8>(new_x + kSpriteW - 1);
    const u8 y0 = new_y, y1 = static_cast<u8>(new_y + kSpriteH - 1);
    return !is_blocked(tile_at(x0, y0)) && !is_blocked(tile_at(x1, y0)) &&
           !is_blocked(tile_at(x0, y1)) && !is_blocked(tile_at(x1, y1));
}

// ── Sprite shapes (8bpp pixel art) ────────────────────────────────────────────

// 8x8 humanoid silhouette as 8bpp pixel art — body (palette-1 idx 1), darker outline
// (2), and a highlight visor/belt (3); idx 0 is transparent.
constexpr auto player_shape = engine::make_pixel_sprite<8, 8>({
    0,0,2,1,1,2,0,0,   // head (outline-capped)
    0,0,1,3,3,1,0,0,   // visor highlight
    0,0,0,1,1,0,0,0,   // neck
    0,2,1,1,1,1,2,0,   // shoulders / arms (outline tips)
    0,0,1,3,3,1,0,0,   // torso + belt highlight
    0,0,1,1,1,1,0,0,   // torso
    0,0,1,0,0,1,0,0,   // legs
    0,2,1,0,0,1,2,0,   // feet (outline)
});

// 8x8 blocky-robot silhouette as menacing 8bpp pixel art — red body (palette-1 idx 4),
// orange trim (5), and glowing yellow eyes (6); idx 0 transparent.
constexpr auto enemy_shape = engine::make_pixel_sprite<8, 8>({
    5,0,0,5,5,0,0,5,   // antennae (orange)
    4,4,4,4,4,4,4,4,   // head
    4,6,4,4,4,4,6,4,   // glowing eyes
    4,4,4,4,4,4,4,4,
    4,4,5,4,4,5,4,4,   // body (orange trim)
    4,5,4,4,4,4,5,4,
    5,0,4,0,0,4,0,5,   // legs
    0,5,0,0,0,0,5,0,   // feet
});

// ── VBXE overlay renderer (Tier 2) ──────────────────────────────────────────
//
// All screen content is drawn into the overlay "master" canvas via Game::gfx()
// (Background::Bitmap), then published; the compositor restores sprite footprints
// from the master each frame. Glyphs — room tiles AND text — are rendered straight
// from the shared arena_charset (each Mode-4 cell is 4px wide x 8 tall, 2bpp), so
// no separate overlay font is needed. Cell (col,row) maps to overlay pixel
// (kPlayLeftX + col*kCellW, row*kCellH); the play-area room rows are offset by the
// HUD row via kPlayTopY (= one cell). Walls fill via the blitter (fast); sparse
// text/dots plot per pixel (cheap one-time per screen).

// Palette-1 indices. 0 = transparent (sprite/text skip). 1..6 are the sprite colours
// (must match the pixel-shape data above); 7..13 are room/HUD/text colours.
namespace pal {
    constexpr u8 kPlayerBody = 1, kPlayerOutline = 2, kPlayerHi = 3;
    constexpr u8 kEnemyBody = 4, kEnemyTrim = 5, kEnemyEye = 6;
    constexpr u8 kText     = 7;    // white text / HUD digits & letters
    constexpr u8 kFloorBg  = 8;    // play-area background fill
    constexpr u8 kFloorDot = 9;    // sparse floor texture
    constexpr u8 kWallLite = 10;   // brick face
    constexpr u8 kWallDark = 11;   // mortar course / shadow
    constexpr u8 kHeart    = 12;   // life heart
    constexpr u8 kHudBg    = 13;   // HUD background bar
    constexpr u8 kExploRing = kEnemyTrim;  // reuse orange
}

EDGE_COLD static void load_overlay_palette() {
    V::set_color<VBXECfg>(1, pal::kPlayerBody,    0x20, 0xA0, 0xFE); // sky blue
    V::set_color<VBXECfg>(1, pal::kPlayerOutline, 0x10, 0x18, 0x40); // navy
    V::set_color<VBXECfg>(1, pal::kPlayerHi,      0xFE, 0xFE, 0xFE); // white visor
    V::set_color<VBXECfg>(1, pal::kEnemyBody,     0xE0, 0x10, 0x10); // red
    V::set_color<VBXECfg>(1, pal::kEnemyTrim,     0xFE, 0x80, 0x00); // orange
    V::set_color<VBXECfg>(1, pal::kEnemyEye,      0xFE, 0xF0, 0x00); // yellow
    V::set_color<VBXECfg>(1, pal::kText,          0xFE, 0xFE, 0xFE); // white
    V::set_color<VBXECfg>(1, pal::kFloorBg,       0x00, 0x00, 0x18); // near-black blue
    V::set_color<VBXECfg>(1, pal::kFloorDot,      0x28, 0x34, 0x60); // dim blue
    V::set_color<VBXECfg>(1, pal::kWallLite,      0xC0, 0x60, 0x20); // brick
    V::set_color<VBXECfg>(1, pal::kWallDark,      0x50, 0x28, 0x10); // mortar
    V::set_color<VBXECfg>(1, pal::kHeart,         0xE0, 0x20, 0x20); // red heart
    V::set_color<VBXECfg>(1, pal::kHudBg,         0x18, 0x18, 0x40); // HUD bar
}

// Overlay pixel of a play-area / screen cell.
static constexpr u16 cell_px(u8 col) { return static_cast<u16>(kPlayLeftX + col * kCellW); }
static constexpr u16 cell_py(u8 row) { return static_cast<u16>(kTopMargin + row * kCellH); }

// Plot a charset glyph at overlay pixel (px,py): 4px wide x 8 tall, each 2-bit pixel
// value -> color (value 0 skipped — leaves the background). Used for text glyphs.
static void plot_glyph(u8 code, u16 px, u16 py, u8 color) {
    auto& g = Game::gfx();
    const u16 base = static_cast<u16>(code) * 8;
    for (u8 r = 0; r < 8; ++r) {
        const u8 byte = arena_charset.data[base + r];
        for (u8 c = 0; c < 4; ++c)
            if ((byte >> (6 - 2 * c)) & 3) g.plot(static_cast<u16>(px + c),
                                                  static_cast<u16>(py + r), color);
    }
}

// Draw a string of arena glyphs at (col,row) in `color` (skips spaces).
static void draw_text(u8 col, u8 row, const char* s, u8 color) {
    u16 px = cell_px(col);
    const u16 py = cell_py(row);
    for (; *s; ++s, px += kCellW) {
        const u8 code = M::ascii_to_internal(*s);
        if (code) plot_glyph(code, px, py, color);     // 0 = space -> nothing
    }
}

// Draw a zero-padded number of `digits` at (col,row): right-aligned like print_num.
static void draw_num(u8 col, u8 row, u16 value, u8 digits, u8 color) {
    for (u8 i = 0; i < digits; ++i) {
        const u8 d = static_cast<u8>(value % 10);
        value = static_cast<u16>(value / 10);
        // Digit glyphs sit at internal codes 0x10..0x19.
        plot_glyph(static_cast<u8>(0x10 + d),
                   cell_px(static_cast<u8>(col + digits - 1 - i)), cell_py(row), color);
    }
}

// ── Wide (2x-horizontal) text, pixel-positioned ──
//
// The 4px charset glyphs look cramped at 1:1 (square pixels). The title and game-
// over screens are pure full-screen text (no room to align to), so they use 2x-wide
// glyphs (8px) for readability, laid out directly in overlay pixels across the full
// 320px width. The in-game HUD/GET READY stay on the 4px room grid (they must align
// with the room, and read fine there). kCharW2x is the per-character pitch (8px).
static constexpr u8  kCharW2x = 8;
static constexpr u16 row_py(u8 row) { return static_cast<u16>(kTopMargin + row * kCellH); }
// Left pixel that centres `n` wide chars in the 320px overlay.
static constexpr u16 centre_px(u8 n) { return static_cast<u16>((320 - n * kCharW2x) / 2); }

static void plot_glyph2x(u8 code, u16 px, u16 py, u8 color) {
    auto& g = Game::gfx();
    const u16 base = static_cast<u16>(code) * 8;
    for (u8 r = 0; r < 8; ++r) {
        const u8 byte = arena_charset.data[base + r];
        for (u8 c = 0; c < 4; ++c)
            if ((byte >> (6 - 2 * c)) & 3) {
                const u16 x = static_cast<u16>(px + c * 2);
                g.plot(x, static_cast<u16>(py + r), color);
                g.plot(static_cast<u16>(x + 1), static_cast<u16>(py + r), color);
            }
    }
}
static void draw_text2x(u16 px, u16 py, const char* s, u8 color) {
    for (; *s; ++s, px += kCharW2x) {
        const u8 code = M::ascii_to_internal(*s);
        if (code) plot_glyph2x(code, px, py, color);
    }
}
static void draw_num2x(u16 px, u16 py, u16 value, u8 digits, u8 color) {
    for (u8 i = 0; i < digits; ++i) {
        const u8 d = static_cast<u8>(value % 10);
        value = static_cast<u16>(value / 10);
        plot_glyph2x(static_cast<u8>(0x10 + d),
                     static_cast<u16>(px + (digits - 1 - i) * kCharW2x), py, color);
    }
}

// Draw one room tile (index) as an 8-tall, 4-wide overlay cell at (col,row-in-screen).
static void draw_tile(u8 code, u8 col, u8 screen_row) {
    auto& g = Game::gfx();
    const u16 px = cell_px(col), py = cell_py(screen_row);
    switch (code) {
        case kWallA:
        case kWallB:
            g.fill_rect(px, py, kCellW, kCellH, pal::kWallLite);     // brick face
            g.fill_rect(px, static_cast<u16>(py + 3), kCellW, 1, pal::kWallDark); // mortar
            g.fill_rect(px, static_cast<u16>(py + 7), kCellW, 1, pal::kWallDark);
            break;
        case 0x03:  // floor dot
            g.fill_rect(px, py, kCellW, kCellH, pal::kFloorBg);
            g.plot(static_cast<u16>(px + 1), static_cast<u16>(py + 4), pal::kFloorDot);
            break;
        default:    // blank floor
            g.fill_rect(px, py, kCellW, kCellH, pal::kFloorBg);
            break;
    }
}

// Redraw a single play-area cell from the baked room (used to erase explosions /
// GET READY back to the room). screen_row = room row + 1 (HUD occupies row 0).
static void restore_room_cell(u8 col, u8 room_row) {
    draw_tile(kRoom.t[room_row][col], col, static_cast<u8>(room_row + 1));
}

// Draw the whole room with a handful of blitter rectangles (mirrors make_room's
// structure) instead of ~920 per-cell ops + ~200 dot plots — far fewer blitter
// round-trips, which the overlay compositor shares. Play rows map to screen rows
// (room row r -> screen row r+1, below the HUD). Floor dots are omitted (texture only).
static void draw_room_fast() {
    auto& g = Game::gfx();
    g.fill_rect(cell_px(0), cell_py(1), 40 * kCellW, 23 * kCellH, pal::kFloorBg); // floor
    g.fill_rect(cell_px(0),  cell_py(1),  40 * kCellW, kCellH, pal::kWallLite);   // top wall
    g.fill_rect(cell_px(0),  cell_py(23), 40 * kCellW, kCellH, pal::kWallLite);   // bottom
    g.fill_rect(cell_px(0),  cell_py(1),  kCellW, 23 * kCellH, pal::kWallLite);   // left
    g.fill_rect(cell_px(39), cell_py(1),  kCellW, 23 * kCellH, pal::kWallLite);   // right
    // Interior bars (same coords as make_room): outer in wall-lite, inner in wall-dark.
    const u8 ys1[2] = {5, 17}, xs1[2] = {8, 28};
    for (u8 a = 0; a < 2; ++a)
        for (u8 b = 0; b < 2; ++b)
            g.fill_rect(cell_px(xs1[b]), cell_py(static_cast<u8>(ys1[a] + 1)),
                        4 * kCellW, kCellH, pal::kWallLite);
    const u8 ys2[2] = {9, 13}, xs2[2] = {14, 22};
    for (u8 a = 0; a < 2; ++a)
        for (u8 b = 0; b < 2; ++b)
            g.fill_rect(cell_px(xs2[b]), cell_py(static_cast<u8>(ys2[a] + 1)),
                        4 * kCellW, kCellH, pal::kWallDark);
}

// Publish the freshly-drawn master canvas to the live display page(s).
static void overlay_present_master() { Game::overlay_publish_background(); }

// ── Player ───────────────────────────────────────────────────────────────────

static void player_init() {
    // Centre of the 40x23 play area (col 20, row 11 — open floor in the room).
    player.x      = static_cast<u8>(kPlayLeftX + 20 * kCellW);
    player.y      = static_cast<u8>(kPlayTopY  + 11 * kCellH);
    player.dir    = 0;
    player.lives  = 3;
    player.iframe = 0;
    Game::sprite_color(0, kPlayerColor);
}

static void player_update(const engine::Input& in) {
    // Read the joystick into a per-axis delta; the last direction held wins `dir`.
    i8 dx = 0, dy = 0;
    if (in.up())    { dy = -static_cast<i8>(kPlayerSpeed); player.dir = 0; }
    if (in.down())  { dy =  static_cast<i8>(kPlayerSpeed); player.dir = 2; }
    if (in.left())  { dx = -static_cast<i8>(kPlayerSpeed); player.dir = 3; }
    if (in.right()) { dx =  static_cast<i8>(kPlayerSpeed); player.dir = 1; }

    if (dx != 0 || dy != 0) {
        const u8 cand_x = static_cast<u8>(player.x + dx);
        const u8 cand_y = static_cast<u8>(player.y + dy);
        if (can_move(cand_x, cand_y)) {
            player.x = cand_x;
            player.y = cand_y;
        } else {
            // Blocked: try each axis on its own so movement slides along walls
            // instead of stopping dead at a diagonal.
            if (dx != 0 && can_move(cand_x, player.y)) player.x = cand_x;
            if (dy != 0 && can_move(player.x, cand_y)) player.y = cand_y;
        }
    }

    // Invincibility blink: toggle visibility every 4 frames while iframe runs.
    bool visible = true;
    if (player.iframe > 0) {
        --player.iframe;
        visible = ((player.iframe >> 2) & 1) == 0;
    }

    if (visible) Game::sprite(0, player_shape, player.x, player.y);
    else         Game::sprite_hide(0);
}

// ── Enemies ──────────────────────────────────────────────────────────────────

// Spawn one enemy at a random room edge, biased toward open floor. Picks an edge, a
// random position along it, and verifies the whole 8x8 footprint clears walls via
// can_move(); if blocked it nudges up to three cells along the same edge before giving up.
static void spawn_enemy() {
    const u8 edge = static_cast<u8>(engine::random() & 3);  // 0=top 1=bottom 2=left 3=right
    const bool vertical = (edge >= 2);                      // left/right vary the row
    const u8 base = vertical ? static_cast<u8>(1 + engine::random() % kInteriorMaxRow)
                             : static_cast<u8>(1 + engine::random() % kMaxSpawnCol);

    for (u8 attempt = 0; attempt < 3; ++attempt) {
        u8 col, row;
        switch (edge) {
            case 0: row = 1;               col = static_cast<u8>(base + attempt); break; // top
            case 1: row = kInteriorMaxRow; col = static_cast<u8>(base + attempt); break; // bottom
            case 2: col = 1;               row = static_cast<u8>(base + attempt); break; // left
            default: col = kMaxSpawnCol;   row = static_cast<u8>(base + attempt); break; // right
        }
        if (col > kMaxSpawnCol || row > kInteriorMaxRow) break;  // footprint would clip a wall

        const u8 sx = static_cast<u8>(kPlayLeftX + col * kCellW);
        const u8 sy = static_cast<u8>(kPlayTopY + row * kCellH);
        if (can_move(sx, sy)) {
            if (Enemy* e = enemies.acquire()) {
                e->x = sx; e->y = sy; e->timer = 0;
                e->period = enemy_period_for_level(g_spawn_level);
                ++g_spawn_level;          // next spawn is a touch faster
            }
            return;
        }
    }
    // Edge fully blocked this attempt — skip the spawn.
}

// ── Bullets (overlay blitter sprites) ──────────────────────────────────────────
//
// The native build fires the four hardware P/M missiles, but on the opaque VBXE
// overlay those projectiles sit UNDER the overlay and are invisible — so bullets are
// drawn as small blitter sprites in the slots after the enemies (player is 0, enemies
// 1..kEnemyCount, bullets kBulletSlotBase + pool index).
static constexpr u8 kBulletSlotBase = kEnemyCount + 1;
constexpr auto kBulletShape = engine::make_pixel_sprite<4, 4>({
    0, 3, 3, 0,
    3, 3, 3, 3,
    3, 3, 3, 3,
    0, 3, 3, 0,
});
static void bullet_show(u8 idx, u8 x, u8 y) {
    Game::sprite(static_cast<u8>(kBulletSlotBase + idx), kBulletShape, x, y);
}
static void bullet_hide(u8 idx) {
    Game::sprite_hide(static_cast<u8>(kBulletSlotBase + idx));
}

// ── HUD + explosions (overlay master) ───────────────────────────────────────────
//
// Redraw the changed field into the master canvas (clearing it to the HUD background
// first) and republish. HUD fields change only on a kill / life / level event, so the
// whole-master republish cost lands rarely, not per frame.
static void hud_draw_score() {
    Game::gfx().fill_rect(cell_px(7), cell_py(0), 5 * kCellW, kCellH, pal::kHudBg);
    draw_num(7, 0, g_score, 5, pal::kText);
    overlay_present_master();
}
static void hud_draw_lives() {
    Game::gfx().fill_rect(cell_px(36), cell_py(0), 3 * kCellW, kCellH, pal::kHudBg);
    for (u8 i = 0; i < player.lives; ++i)
        plot_glyph(0x05, cell_px(static_cast<u8>(36 + i)), cell_py(0), pal::kHeart);
    overlay_present_master();
}
static void hud_draw_level() {
    Game::gfx().fill_rect(cell_px(kHudLevelCol + 3), cell_py(0), 2 * kCellW, kCellH, pal::kHudBg);
    draw_num(static_cast<u8>(kHudLevelCol + 3), 0, g_level, 2, pal::kText);
    overlay_present_master();
}

// Overlay explosion: stamp the explosion glyph into the master cell and republish.
static void spawn_explosion(u8 sprite_x, u8 sprite_y) {
    if (sprite_x < kPlayLeftX || sprite_y < kPlayTopY) return;
    const u8 col = static_cast<u8>((sprite_x - kPlayLeftX) / kCellW);
    const u8 row = static_cast<u8>((sprite_y - kPlayTopY) / kCellH);
    if (col >= 40 || row >= 23) return;
    if (Explosion* e = explosions.acquire()) {
        e->col = col; e->row = row; e->timer = kExplosionFrames;
        Game::gfx().fill_rect(cell_px(col), cell_py(static_cast<u8>(row + 1)),
                              kCellW, kCellH, pal::kFloorBg);
        plot_glyph(kExplosionTile, cell_px(col), cell_py(static_cast<u8>(row + 1)),
                   pal::kExploRing);
        overlay_present_master();
    }
}

// ── Screen entry + per-frame callbacks ──────────────────────────────────────

EDGE_COLD static void title_enter() {
    // Draw the whole title into the overlay master once (PRESS FIRE static — no
    // per-frame republish; the blink/colour-cycle are cosmetic and skipped here).
    auto& g = Game::gfx();
    g.clear(pal::kFloorBg);
    draw_text2x(centre_px(10), row_py(kTitleRowEdge),  "EDGE ARENA", pal::kPlayerHi);
    draw_text2x(centre_px(10), row_py(kTitleRowPress), "PRESS FIRE", pal::kEnemyTrim);
    draw_text2x(centre_px(31), row_py(kTitleRowInstr), "JOYSTICK TO MOVE  FIRE TO SHOOT", pal::kText);
    if (g_high_score > 0) {
        draw_text2x(centre_px(11), row_py(kTitleRowBest), "BEST: 00000", pal::kText);
        draw_num2x(static_cast<u16>(centre_px(11) + 6 * kCharW2x), row_py(kTitleRowBest),
                   g_high_score, 5, pal::kText);
    }
    overlay_present_master();
    g_title_frames = 0;
    arm_fire();
}

EDGE_COLD static bool title_step(const engine::Input& in) {
    // Blink "PRESS FIRE" every 30 frames; republish only on the toggle (2x/sec).
    const bool show = ((g_title_frames / 30) & 1) == 0;
    static bool last_show = false;
    if (g_title_frames == 0) last_show = !show;     // force a redraw on (re)enter
    if (show != last_show) {
        last_show = show;
        if (show) draw_text2x(centre_px(10), row_py(kTitleRowPress), "PRESS FIRE", pal::kEnemyTrim);
        else      Game::gfx().fill_rect(centre_px(10), row_py(kTitleRowPress),
                                        10 * kCharW2x, kCellH, pal::kFloorBg);
        overlay_present_master();
    }
    ++g_title_frames;
    return fire_edge(in);
}

EDGE_COLD static void play_enter() {
    // Draw the whole play screen into the overlay master: HUD bar (row 0), the baked
    // room (rows 1..23), HUD labels/values, then publish once. Sprites composite over
    // this each frame; dynamic HUD/explosions redraw + republish on change.
    auto& g = Game::gfx();
    g.clear(pal::kFloorBg);                                                  // whole canvas (covers margins)
    g.fill_rect(cell_px(0), cell_py(0), 40 * kCellW, kCellH, pal::kHudBg);   // HUD bar
    draw_room_fast();
    draw_text(0, 0, "SCORE: 00000", pal::kText);
    draw_text(kHudLevelCol, 0, "LV:01", pal::kText);
    for (u8 c = 36; c <= 38; ++c) plot_glyph(0x05, cell_px(c), cell_py(0), pal::kHeart);
    // GET READY (play row 9 -> screen row 10).
    draw_text(kGetReadyCol, static_cast<u8>(kGetReadyRow + 1), "GET READY", pal::kText);
    overlay_present_master();

    player_init();
    // Draw the player up front so it's visible during the GET READY pause (player_update
    // is what normally renders it, and that's blocked while the countdown runs).
    Game::sprite(0, player_shape, player.x, player.y);

    // Enemies: empty the pool, colour slots 1..3, arm the spawn clock, and reset the
    // difficulty ramp so each round starts slow again.
    enemies.clear();
    for (u8 s = 1; s <= kEnemyCount; ++s) Game::sprite_color(s, kEnemyColor);
    spawn_timer    = kSpawnInterval;
    g_spawn_level  = 0;

    // Combat: fresh bullets/explosions, zeroed score, cleared timers.
    bullets.clear();
    explosions.clear();
    g_score      = 0;
    g_level      = 1;
    g_kills      = 0;
    g_death_timer= 0;
    g_new_high   = false;

    // GET READY orientation pause: hold the room/player a beat before action starts.
    // (Already drawn into the master above.)
    g_get_ready = kGetReadyFrames;

    arm_fire();
}

static bool play_step(const engine::Input& in) {
    // Death pause: lives just hit 0. Freeze every entity (skip all updates so sprites
    // hold their last-committed positions) while the room stays on screen, then signal
    // game-over when the beat expires. Checked first so we can't re-enter GET READY.
    if (g_death_timer > 0) {
        return (--g_death_timer == 0);     // transition only when the freeze ends
    }

    // GET READY countdown: hold before action starts. Blocks spawns/movement/fire; on
    // the final frame, erase "GET READY" back to the baked room tiles (cols 15..23).
    if (g_get_ready > 0) {
        if (--g_get_ready == 0) {
            for (u16 c = kGetReadyCol; c < kGetReadyClearEnd; ++c)
                restore_room_cell(static_cast<u8>(c), kGetReadyRow);
            overlay_present_master();
        }
        return false;
    }

    player_update(in);

    // Fire: launch a bullet in the last-moved direction (overlay slot == 4 + pool slot).
    if (fire_edge(in) && !bullets.full()) {
        u8 slot;
        if (Bullet* b = bullets.acquire(slot)) {
            b->x = static_cast<u8>(player.x + kSpriteW / 2);
            b->y = player.y;
            switch (player.dir) {
                case 1:  b->dx =  static_cast<i8>(kBulletSpeed); b->dy = 0; break;          // right
                case 2:  b->dx = 0; b->dy =  static_cast<i8>(kBulletSpeed); break;          // down
                case 3:  b->dx = -static_cast<i8>(kBulletSpeed); b->dy = 0; break;          // left
                default: b->dx = 0; b->dy = -static_cast<i8>(kBulletSpeed); break;          // up
            }
            Game::sound.play(kShootSfx, 0);
        }
    }

    // Move + render bullets; retire any that hit a wall or leave the play area.
    bullets.for_each_indexed([&](u8 idx, Bullet& b) {
        b.x = static_cast<u8>(b.x + b.dx);
        b.y = static_cast<u8>(b.y + b.dy);
        if (is_blocked(tile_at(b.x, b.y))) {
            bullet_hide(idx);
            bullets.release(idx);
        } else {
            bullet_show(idx, b.x, b.y);
        }
    });

    // Spawn: roughly one enemy per second, until the pool is full.
    if (--spawn_timer == 0) {
        spawn_timer = kSpawnInterval;
        if (!enemies.full()) spawn_enemy();
    }

    // Move + render each enemy. for_each_indexed hands us the pool index, which
    // maps to the logical sprite slot (slot = index + 1).
    enemies.for_each_indexed([&](u8 idx, Enemy& e) {
        // Move only every `period` frames (set per spawn; lower = faster).
        if (++e.timer >= e.period) {
            e.timer = 0;
            const i8 dx = (player.x > e.x) ?  static_cast<i8>(kEnemyStep)
                        : (player.x < e.x) ? static_cast<i8>(-kEnemyStep) : 0;
            const i8 dy = (player.y > e.y) ?  static_cast<i8>(kEnemyStep)
                        : (player.y < e.y) ? static_cast<i8>(-kEnemyStep) : 0;
            const u8 cx = static_cast<u8>(e.x + dx);
            const u8 cy = static_cast<u8>(e.y + dy);
            if (can_move(cx, cy)) {
                e.x = cx; e.y = cy;
            } else {
                // Blocked diagonally: slide along the wall, one axis at a time
                // (same behaviour as the player).
                if (dx != 0 && can_move(cx, e.y)) e.x = cx;
                if (dy != 0 && can_move(e.x, cy)) e.y = cy;
            }
        }
        Game::sprite(static_cast<u8>(idx + 1), enemy_shape, e.x, e.y);
    });

    // Explosions: tick timers, restore the baked room tile when each expires.
    // release() swaps the last element into i, so don't advance on a release.
    for (u8 i = 0; i < explosions.count();) {
        Explosion& ex = explosions[i];
        if (--ex.timer == 0) {
            restore_room_cell(ex.col, ex.row);
            overlay_present_master();
            explosions.release(i);
        } else {
            ++i;
        }
    }

    // Collisions in software (AABB) off the positions we already track. The GTIA
    // collision registers can't be used at face value here: the multiplexer Y-sorts
    // the active sprites and reassigns them to hardware players every frame — even
    // in this single-zone tier — so a register bit names a HARDWARE player, not the
    // logical slot it happens to show this frame. Reading them as logical slots
    // misattributes hits (false kills/damage → cascading explosions + sound + score
    // redraws every frame, which is the "slowdown"). Position AABB sidesteps the
    // whole logical<->hardware remap and the registers' one-frame latch lag.

    // Bullet -> enemy kills: an overlapping bullet destroys the enemy and is spent.
    bullets.for_each_indexed([&](u8 idx, Bullet& b) {
        for (u8 s = 1; s <= kEnemyCount; ++s) {
            const u8 ei = static_cast<u8>(s - 1);
            if (!enemies.active(ei)) continue;
            const Enemy& e = enemies[ei];
            if (boxes_overlap(b.x, b.y, kBulletW, kBulletHeight,
                              e.x, e.y, kSpriteW, kSpriteH)) {
                spawn_explosion(e.x, e.y);
                Game::sprite_hide(s);
                enemies.release(ei);
                bullet_hide(idx);
                bullets.release(idx);
                g_score = static_cast<u16>(g_score + kScorePerKill);
                hud_draw_score();
                // Level ticks every kKillsPerLevel kills — a visible difficulty cue.
                ++g_kills;
                const u8 lv = static_cast<u8>(1 + g_kills / kKillsPerLevel);
                if (lv != g_level) { g_level = lv; hud_draw_level(); }
                Game::sound.play(kExplosionSfx, 1);
                break;   // bullet consumed
            }
        }
    });

    // Enemy -> player damage: contact costs a life unless invincibility is running.
    if (player.iframe == 0) {
        bool enemy_touch = false;
        enemies.for_each([&](Enemy& e) {
            if (boxes_overlap(player.x, player.y, kSpriteW, kSpriteH,
                              e.x, e.y, kSpriteW, kSpriteH))
                enemy_touch = true;
        });
        if (enemy_touch) {
            player.lives  = static_cast<u8>(player.lives - 1);
            player.iframe = kIFrames;
            hud_draw_lives();
            Game::sound.play(kDamageSfx, 0);
            if (player.lives == 0) {
                // Record the high score now, then start the frozen-room death beat.
                // The next play_step takes the death-pause branch and signals game-over
                // only when the beat expires.
                g_new_high = (g_score > g_high_score);
                if (g_new_high) g_high_score = g_score;
                g_death_timer = kDeathPause;
            }
        }
    }

    return false;
}

EDGE_COLD static void gameover_enter() {
    auto& g = Game::gfx();
    g.clear(pal::kFloorBg);
    draw_text2x(centre_px(9), row_py(8), "GAME OVER", pal::kEnemyTrim);
    if (g_new_high) draw_text2x(centre_px(14), row_py(10), "NEW HIGH SCORE", pal::kEnemyEye);
    draw_text2x(centre_px(12), row_py(12), "SCORE: 00000", pal::kText);
    draw_num2x(static_cast<u16>(centre_px(12) + 7 * kCharW2x), row_py(12), g_score, 5, pal::kText);
    draw_text2x(centre_px(12), row_py(13), "BEST:  00000", pal::kText);
    draw_num2x(static_cast<u16>(centre_px(12) + 7 * kCharW2x), row_py(13), g_high_score, 5, pal::kText);
    draw_text2x(centre_px(10), row_py(16), "PRESS FIRE", pal::kText);
    overlay_present_master();
    arm_fire();
}

EDGE_COLD static bool gameover_step(const engine::Input& in) {
    return fire_edge(in);
}

// ── Entry point ──────────────────────────────────────────────────────────

// No-VBXE fallback: this build renders entirely through the VBXE overlay, so on a
// machine without VBXE there is nothing to show. Detect the board BEFORE bringing up
// the overlay; if absent, write a message on the OS GR.0 text screen (still up at
// program start, pointed to by SAVMSC $58/$59 — ANTIC internal codes match
// ascii_to_internal) and halt, instead of leaving a black/garbage screen.
EDGE_COLD static void require_vbxe_or_halt() {
    if (V::detect<VBXECfg>()) return;
    u8* scr = *reinterpret_cast<u8* volatile*>(0x58);   // SAVMSC -> GR.0 screen RAM
    const char msg[] = "VBXE REQUIRED";
    for (u8 i = 0; msg[i]; ++i) scr[i] = M::ascii_to_internal(msg[i]);
    for (;;) {}
}

int main() {
    require_vbxe_or_halt();   // must run before Game::init() switches off the OS screen
    Game::init(arena_charset);   // charset unused on the VBXE overlay path, harmless there
    // Tier 2: pure-overlay screens (ANTIC playfield off — no bus contention). Upload
    // the palette; all content (room, HUD, text) is drawn into the overlay master by
    // the per-screen *_enter callbacks. No ANTIC charset bind, no HUD/DLI palette.
    load_overlay_palette();

    for (;;) {
        Game::set_screen<TitleScreen>(&title_enter);
        Game::run_until(title_step);

        Game::set_screen<PlayScreen>(&play_enter);
        Game::run_until(play_step);
        Game::sprite_hide(0);   // don't carry the player onto title / game-over
        // Likewise hide every live enemy and empty the pool for the next round.
        enemies.for_each_indexed(
            [](u8 idx, Enemy&) { Game::sprite_hide(static_cast<u8>(idx + 1)); });
        enemies.clear();
        // Retire bullets (hide their overlay sprites) and empty both pools. The next
        // play_enter redraws the whole room, so explosion cells need no restore here.
        bullets.for_each_indexed([](u8 idx, Bullet&) { bullet_hide(idx); });
        bullets.clear();
        explosions.clear();

        Game::set_screen<GameOverScreen>(&gameover_enter);
        Game::run_until(gameover_step);
    }
}
