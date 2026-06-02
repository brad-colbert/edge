// demo/atari_scroll_test.cpp — Edge engine ANTIC hardware-scroll validation demo.
//
// A minimal Atari program that exercises the engine's hardware scroll path and
// the Tile infrastructure, producing a loadable .xex (build with the
// mos-atari8-dos target; see CMakeLists.txt). It is the scroll counterpart to
// atari_hw_test.cpp and is meant to be run on Altirra / Fujisan as a visual
// confirmation that per-line-LMS scrolling works on real silicon:
//
//   Top 2 rows : fixed HUD — live scroll X, Y and a frame counter (does NOT
//                scroll, so the coordinates stay readable).
//   Below      : a 22-row Mode-2 window onto a 64x32 tilemap. Joystick scrolls
//                the window through the map; the engine splits the position into
//                fine (HSCROL/VSCROL) and coarse (per-line LMS) automatically.
//
// The map is an engine::TileMap<64,32> in RAM (a tile index is the ANTIC screen
// code in Mode 2, so the map serves directly as scroll screen memory). It is
// filled with a coordinate grid so any scroll glitch is obvious: the left two
// columns are the row number, every 4th column carries a column-number marker,
// and the rest is a checkerboard texture.
//
// ── Pure engine API ──────────────────────────────────────────────────────
//
// Every hardware interaction goes through engine::Core / Platform::hal — no
// poke()s, no inline assembly, no magic addresses. The game loop only calls
// Game::scroll.move(); the engine's frame service writes the fine-scroll
// registers and repoints the display-list LMS each frame. This file is meant to
// read as example game code.

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

using engine::u8;
using engine::u16;
namespace M = atari;

// ── Platform + game configuration ────────────────────────────────────────

using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::gfx::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC>;

// Map geometry (cells). The scroll region's MapW/MapH must match g_map.
static constexpr u16 kMapW = 64;
static constexpr u16 kMapH = 32;

// Horizontal scroll margin: enabling HSCROLL makes ANTIC fetch a wider line
// (Mode 2: 48 bytes vs 40), so the displayed window sits this many columns into
// the fetched data and the leftmost columns fall in the left border. Reserving
// these columns as off-screen margin puts logical column 0 at the visible edge.
// (= scroll_margin(MODE_2) / 2.)
static constexpr u16 kMargin = 4;

struct ScrollScreen {
    // Region 0: a fixed 2-row Mode-2 HUD. Region 1: a 22-row Mode-2 window that
    // scrolls over the 64x32 map. ANTIC renders the regions top-to-bottom, so the
    // HUD simply omits the scroll bits and stays put while the field below moves.
    using display = engine::DisplayLayout<
        engine::TextRegion<M::Mode::MODE_2, 2>,
        engine::ScrollRegion<engine::TextRegion<M::Mode::MODE_2, 22>, kMapW, kMapH>>;
};

struct GameConfig {
    using screens = engine::ScreenSet<ScrollScreen>;
    static constexpr u8 max_sprites    = 1;
    static constexpr u8 sound_channels = 1;
};

using Game = engine::Core<Platform, GameConfig>;

// ── The scroll map (RAM-resident; ANTIC reads it via DMA) ─────────────────

static engine::TileMap<kMapW, kMapH> g_map;

// ASCII -> ANTIC internal screen code, for direct tile writes.
static inline u8 g(char c) { return M::ascii_to_internal(c); }

// Fill the map with a coordinate grid, offset right by kMargin so logical column
// 0 sits at the visible left edge once the HSCROLL margin is accounted for:
//   physical cols 0..kMargin-1 : '#' left margin (lives in the border)
//   logical (c-kMargin), rows 0-1 : 2-digit COLUMN ruler (tens / ones)
//   logical col 0-1 (cols kMargin, kMargin+1) : 2-digit ROW ruler
//   logical (0,0)              : a '*' origin marker
//   elsewhere                 : '.' texture
// At scroll (0,0) the field's top-left cell should now be '*', the top two rows
// read 00 01 02 ... across, and the left two columns 00 01 02 ... down.
static void fill_map() {
    for (u16 r = 0; r < kMapH; ++r) {
        for (u16 c = 0; c < kMapW; ++c) {
            u8 t;
            if (c < kMargin) {
                t = g('#');                                     // left margin / border
            } else {
                const u16 lc = static_cast<u16>(c - kMargin);   // logical column
                if (r == 0)       t = g(static_cast<char>('0' + (lc / 10) % 10));  // col tens
                else if (r == 1)  t = g(static_cast<char>('0' + (lc % 10)));       // col ones
                else if (lc == 0) t = g(static_cast<char>('0' + (r / 10) % 10));   // row tens
                else if (lc == 1) t = g(static_cast<char>('0' + (r % 10)));        // row ones
                else              t = g('.');                                      // texture
            }
            g_map.set_tile(c, r, t);
        }
    }
    g_map.set_tile(kMargin, 0, g('*'));   // logical origin (0,0)
}

// ── Game state ────────────────────────────────────────────────────────────

static u16 g_frame = 0;

// ── Per-frame logic (runs once per VBI via Game::run) ────────────────────

static void frame_step(const engine::Input& in) {
    // Joystick scrolls the window. X is in fine (color-clock) units, Y in
    // scanlines; the engine clamps at the map edges and handles fine<->coarse.
    if (in.left())  Game::scroll.move(-1, 0);
    if (in.right()) Game::scroll.move(1, 0);
    if (in.up())    Game::scroll.move(0, -1);
    if (in.down())  Game::scroll.move(0, 1);

    ++g_frame;

    // HUD: live scroll position + frame counter (region 0, the fixed rows).
    Game::print_num(2,  1, Game::scroll.x(), 3);
    Game::print_num(9,  1, Game::scroll.y(), 3);
    Game::print_num(15, 1, g_frame, 5);
}

// ── Entry point ──────────────────────────────────────────────────────────

int main() {
    // Builds the display list (the scroll region gets one LMS per visible line),
    // programs the OS shadows, arms the dispatcher, and installs the VBI service.
    // No charset is loaded, so the default OS font renders the grid + HUD.
    Game::init();

    // Playfield colours via the OS shadows (field map: 0-3 = COLPF0-3, 4 = COLBK).
    Platform::hal::set_color_pf(4, 0x90);   // COLBK  : dark blue background
    Platform::hal::set_color_pf(2, 0x90);   // COLPF2 : text background
    Platform::hal::set_color_pf(1, 0x0E);   // COLPF1 : text luminance (white)

    // Fill the map, then bind it as the scroll source and start scrolling.
    fill_map();
    Game::scroll_map(g_map);

    // Static HUD labels (written once into the fixed top region).
    Game::print(0, 0, "EDGE SCROLL TEST");
    Game::print(0, 1, "X:");
    Game::print(7, 1, "Y:");
    Game::print(13, 1, "F:");

    // One callback per frame, forever.
    Game::run(frame_step);
}
