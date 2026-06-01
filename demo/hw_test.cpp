// demo/hw_test.cpp — Edge engine hardware validation demo.
//
// A minimal Atari program that exercises every engine subsystem and produces a
// loadable .xex (build with the mos-atari8-dos target; see CMakeLists.txt). It
// is meant to be run on Altirra / Fujisan as a visual + audible confirmation
// that the engine's live ANTIC path works on real silicon:
//
//   Row 0 : "EDGE ENGINE V0.1" + a frame counter   (display list + screen
//           memory + per-frame VBI all running)
//   Row 1 : joystick / fire / collision / sound status (input capture)
//   Sprite 0 (arrow)   : moves with the joystick     (P/M graphics + input)
//   Sprite 1 (diamond) : stationary at screen centre (P/M shape at a Y offset)
//   Row-12 DLI         : COLPF2 $94 -> $C4 colour split (DLI fires mid-frame)
//   Fire press         : a pure POKEY tone           (sound subsystem)
//   Sprite overlap     : a POKEY noise burst         (GTIA P0PL collision)
//
// ── Pure engine API ──────────────────────────────────────────────────────
//
// Every hardware interaction goes through engine::Core / Platform::hal — there
// are no poke()s, no inline assembly, and no magic addresses. The engine now
// owns the display list and P/M DMA (init programs the OS shadows), playfield
// colours (Platform::hal::set_color_pf), per-sprite colours (Game::sprite_color,
// driven onto COLPM by the multiplexer), DLI delivery (the split is a C++
// Game::interrupts.add_dli handler dispatched through the engine), and attract-
// mode suppression (vbi_service). POKEY base config (SKCTL=3, AUDCTL=0)
// is left at the values the Atari OS installs at boot. This file is meant to read
// as example game code.

#include <stdint.h>

#include "user_charset.h"

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

using engine::u8;
using engine::u16;
namespace M = atari;

// ── Platform + game configuration ────────────────────────────────────────

using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::Graphics::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC>;

struct HwScreen {
    // Single text screen: ANTIC mode 2, 24 rows (40 columns).
    using display = engine::DisplayLayout<engine::TextRegion<M::Mode::MODE_2, 24>>;
};

struct GameConfig {
    using screens = engine::ScreenSet<HwScreen>;
    static constexpr u8 max_sprites    = 2;
    static constexpr u8 sound_channels = 2;
};

using Game = engine::Core<Platform, GameConfig>;

// ── Assets (constexpr, ROM-resident) ─────────────────────────────────────

constexpr auto user_charset = engine::make_charset(demo::assets::kUserCharsetBytes);

constexpr auto arrow = engine::make_sprite<8, 8>({
    0b00011000,
    0b00111100,
    0b01111110,
    0b11111111,
    0b00011000,
    0b00011000,
    0b00011000,
    0b00000000,
});

constexpr auto diamond = engine::make_sprite<8, 8>({
    0b00011000,
    0b00111100,
    0b01111110,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000,
});

// {waveform, frequency, volume, duration_frames} — make_sound appends SILENT.
constexpr auto sfx_tone  = engine::make_sound({
    {engine::pokey::PURE, 80, 10, 12},
});
constexpr auto sfx_noise = engine::make_sound({
    {engine::pokey::NOISE, 40, 12, 10},
});

// ── Colour-split DLI (C++ handler) ───────────────────────────────────────
//
// A non-capturing C++ DLI handler, registered with Game::interrupts.add_dli and
// entered through the engine's DLI dispatcher. It rewrites COLPF2 (the Mode-2
// text background) to dark green ($C4) via the DLIContext facade — no register
// addresses. The OS colour-shadow copy restores COLPF2 to $94 at the top of every
// frame, so the change is a clean per-frame split.
static void color_split() {
    engine::DLIContext<Platform>{}.write_colpf2(0xC4);
}

// Scanline of the colour split, in the engine's display-list-relative space that
// InterruptManager::prepare_chain expects: the list opens with 3 dl_blank(8)
// instructions (24 scanlines), then Mode-2 rows of 8 scanlines each, so the 13th
// mode line (row 12) begins at 24 + 12*8 = 120. The engine maps this to that
// mode line and sets its DLI bit; a 4K-crossing LMS adds no mode line, so the
// mapping is unaffected.
static constexpr u8 kSplitScanline = 120;

// ── Small helper ─────────────────────────────────────────────────────────

// ASCII -> ANTIC internal screen code, for single-character HUD writes.
static inline u8 g(char c) { return M::ascii_to_internal(c); }

// ── Game state (static; no heap) ─────────────────────────────────────────

static constexpr u8 kDiamondX = 124;   // P/M horizontal position (screen centre)
static constexpr u8 kDiamondY = 120;   // P/M vertical strip offset (scanline)

static u16 g_frame    = 0;
static u8  g_arrow_x  = 100;
static u8  g_arrow_y  = 100;
static u8  g_prev_col = 0;

// ── Per-frame logic (runs once per VBI via Game::run) ────────────────────

static void frame_step(const engine::Input& in) {
    ++g_frame;

    // Move the arrow with joystick 0, clamped to the visible field.
    if (in.left()  && g_arrow_x > 52)  g_arrow_x -= 2;
    if (in.right() && g_arrow_x < 200) g_arrow_x += 2;
    if (in.up()    && g_arrow_y > 40)  g_arrow_y -= 2;
    if (in.down()  && g_arrow_y < 210) g_arrow_y += 2;

    // Pure tone on the frame fire is pressed (edge).
    if (in.fire_pressed()) Game::sound.play(sfx_tone, 0);

    // Player 0 (arrow) vs player 1 (diamond) collision; noise on the rising edge.
    const u8 col = Game::pm_collisions().player_to_player(0);
    if (col && !g_prev_col) Game::sound.play(sfx_noise, 1);
    g_prev_col = col;

    // Buffer the sprites; the VBI commits them to P/M memory next blank.
    Game::sprite(0, arrow,   g_arrow_x, g_arrow_y);
    Game::sprite(1, diamond, kDiamondX, kDiamondY);

    // HUD: frame counter + live input / collision / sound state.
    Game::print_num(26, 0, g_frame, 5);
    Game::put_char(4, 1,  in.up()    ? g('U') : g('.'));
    Game::put_char(5, 1,  in.down()  ? g('D') : g('.'));
    Game::put_char(6, 1,  in.left()  ? g('L') : g('.'));
    Game::put_char(7, 1,  in.right() ? g('R') : g('.'));
    Game::put_char(14, 1, in.fire()  ? g('Y') : g('N'));
    Game::put_char(20, 1, col        ? g('Y') : g('N'));
    Game::put_char(26, 1, (Game::sound.active(0) || Game::sound.active(1))
                              ? g('Y') : g('N'));
    // (Attract-mode suppression is handled by the engine's vbi_service.)
}

// ── Entry point ──────────────────────────────────────────────────────────

int main() {
    // Builds the display list, programs the OS shadows (display list, SDMCTL with
    // P/M DMA), arms the DLI dispatcher, sets up P/M, loads the demo's custom
    // 1K charset into engine-managed RAM, points CHBASE at it, and installs the
    // deferred-VBI service.
    Game::init(user_charset);

    // Single-width player objects (the arrow and the diamond).
    Platform::hal::set_player_size(0, M::sizep::NORMAL);
    Platform::hal::set_player_size(1, M::sizep::NORMAL);

    // Playfield colours, via the OS colour shadows — the OS deferred VBI copies
    // these to the hardware registers every frame. Field map: 0-3 = COLPF0-3,
    // 4 = COLBK.
    Platform::hal::set_color_pf(4, 0x94);   // COLBK  : background / border
    Platform::hal::set_color_pf(2, 0x94);   // COLPF2 : text background
    Platform::hal::set_color_pf(1, 0x0E);   // COLPF1 : text luminance

    // Sprite colours belong to the sprite, not the player slot: the multiplexer
    // drives COLPM to follow each sprite, so they don't swap when the arrow
    // crosses below the diamond.
    Game::sprite_color(0, 0x46);            // arrow,   red
    Game::sprite_color(1, 0xB6);            // diamond, green

    // Static HUD labels (written once).
    Game::print(0, 0, "EDGE ENGINE V0.1");
    Game::print(20, 0, "FRAME:");
    Game::print(0, 1, "JOY:");
    Game::print(9, 1, "FIRE:");
    Game::print(16, 1, "COL:");
    Game::print(22, 1, "SND:");

    // Register the colour-split DLI. The engine sets the row-12 DLI bit on the
    // display list, points VDSLST at the dispatcher, and arms NMIEN each frame.
    Game::interrupts.add_dli(kSplitScanline, &color_split);

    // One callback per frame, forever.
    Game::run(frame_step);
}
