// demo/multiplex_demo.cpp — Edge engine P/M multiplexer demo.
//
// Nine logical sprites (8x8 diamonds) bounce over a full-screen ANTIC mode-2
// text field. With only four hardware players, the engine's sprite multiplexer
// (engine/sprites.h) Y-sorts the active sprites every frame and splits the
// screen into vertical zones of up to four players each, reprogramming the
// player horizontal positions + colours at each zone boundary via a raster
// hook. Watch the ZONES count in the status line change as the diamonds cross
// each other in Y.
//
// ── CODEBASE FACTS (gathered before writing; see citations) ──────────────
//
// A) .xex reference program: demo/atari_hw_test.cpp
//    - includes:  <stdint.h>, <engine/platform/atari/platform.h>, <engine/core.h>
//    - Platform alias:  using Platform = atari::Platform<Machine, RAM, gfx,
//                       Sound, TV>;                       (hw_test.cpp:43-48)
//    - GameConfig shape: `using screens = engine::ScreenSet<...>;` plus the
//                       scalar fields max_sprites / sound_channels.
//                                                          (hw_test.cpp:55-59)
//    - main pattern:    Game::init(); ... setup ... Game::run(frame_step);
//                                                          (hw_test.cpp:169-208)
//      (atari_scroll_test.cpp:139 confirms the no-charset Game::init() form.)
//
// B) engine/core.h
//    - Game::print(col,row,str) / Game::put_char / Game::print_num ARE provided
//      as single-screen shorthands for region 0.            (core.h:271-280)
//    - Multiplex accessor: Game::multiplex is an alias for the sprite manager;
//      Game::multiplex.zone_count() returns the live zone count.
//                                          (core.h:150; sprites.h:417)
//    - max_raster_hooks IS a GameConfig field (defaulted to 12 otherwise).
//                                                            (core.h:65-70)
//    - initial_screen typedef is OPTIONAL (defaults to screen_at<0>); supplying
//      it is fine.                                           (core.h:47-53)
//
// C) engine/sprites.h
//    - sprite() signature:  void sprite(u8 slot, const Shape&, u8 x, u8 y);
//      Game::sprite forwards to it.            (sprites.h:151; core.h:198-201)
//    - sprite_color(slot,color) takes a LOGICAL SPRITE SLOT, not a hardware
//      player. It stores the colour on the logical sprite; the multiplexer
//      then drives that colour onto whichever hardware player the sprite lands
//      on per zone (zone.color[p] = sprites_[..].color), so colour FOLLOWS the
//      sprite across player reassignments.   (sprites.h:175, 243, 437)
//      >> Consequence: all 9 sprites must be coloured (not just 4). The task
//         brief's "per hardware player 0-3" note is inverted from the actual
//         engine model; this demo colours every logical sprite instead.
//    - X coordinate maps to RAW HPOSP: HAL set_sprite_x writes HPOSP directly
//      (hal.h:214), so the visible window is ~48..208. => X_MIN=48, X_MAX=208.
//    - make_sprite is engine::make_sprite<W,H>(...).         (sprites.h:62)
//
// D) engine/display.h / engine/screen.h
//    - engine::TextRegion<Mode, Height> is the text region template.
//                                                            (display.h:159)
//    - ANTIC mode-2 token: atari::Mode::MODE_2.   (platform/atari/modes.h:45)
//    - ScreenSet does NOT require an initial_screen typedef. (screen.h:50-58)
//    - The DemoScreen flags sprites_active / pm_resolution / scroll_active /
//      use_row_table are forward-looking: the current engine does not read them
//      (sprite resolution defaults to SingleLine in SpriteManager, which is what
//      this demo wants). They are declared as documentation, per the brief.
//
// E) engine/types.h
//    - u8 / u16 / i8(int8_t) live in namespace engine.       (types.h:23-27)
//
// F) CMakeLists.txt — existing .xex targets use, per demo .cpp:
//      add_executable(<name> demo/<file>.cpp)
//      target_compile_options(<name> PRIVATE <atari8-dos opts>)
//      target_include_directories(<name> PRIVATE ${CMAKE_SOURCE_DIR})
//      set_target_properties(<name> PROPERTIES OUTPUT_NAME "<name>.xex")
//      target_link_options(<name> PRIVATE "LINKER:--Map=.../<name>.map")
//    (CMakeLists.txt:63-72) — replicated for multiplex_demo in the next step.
//
// ── Pure engine API ──────────────────────────────────────────────────────
// Every interaction is through engine::Core (Game::*) or Platform::hal (the
// same surface the hw_test demo treats as engine API for one-time GTIA setup).
// No poke()s, no magic addresses, no inline assembly.

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

using engine::u8;
using engine::u16;
using engine::i8;
namespace M = atari;

// ── Platform + game configuration ────────────────────────────────────────

using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::gfx::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC>;

struct DemoScreen {
    using display = engine::DisplayLayout<
        engine::TextRegion<M::Mode::MODE_2, 24>
    >;
    static constexpr bool sprites_active = true;
    static constexpr auto pm_resolution =
        engine::SpriteVerticalResolution::SingleLine;
    static constexpr bool scroll_active  = false;
    static constexpr bool use_row_table  = false;
};

struct GameConfig {
    using screens         = engine::ScreenSet<DemoScreen>;
    using initial_screen  = DemoScreen;
    static constexpr u8 max_sprites      = 9;
    // The brief asked for 0, but SoundManager static_asserts MaxChannels >= 1
    // (engine/sound.h:81); this demo plays no sound, so the single channel is
    // simply unused.
    static constexpr u8 sound_channels   = 1;
    // 9 sprites => ceil(9/4)=3 zones => 2 zone-boundary DLIs, plus headroom
    // for future user raster hooks.                          (core.h:65-70)
    static constexpr u8 max_raster_hooks = 8;
};

using Game = engine::Core<Platform, GameConfig>;

// ── Sprite shape (constexpr, ROM-resident) ───────────────────────────────

constexpr auto sprite_shape = engine::make_sprite<8, 8>({
    0b00011000,
    0b00111100,
    0b01111110,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000,
});

// ── Game state (static; no heap) ─────────────────────────────────────────

struct Mover {
    u8 x;
    u8 y;
    i8 dx;
    i8 dy;
};

static Mover movers[9];

// Raw-HPOSP visible window (X is written straight to HPOSP; hal.h:214).
static constexpr u8 X_MIN = 48;
static constexpr u8 X_MAX = 208;

// Vertical bounce limits (P/M scanline space).
static constexpr u8 Y_MIN = 16;
static constexpr u8 Y_MAX = 220;

// ── Per-frame logic (runs once per VBI via Game::run) ────────────────────

static void frame_step(const engine::Input& input) {
    (void)input;   // no input in this demo — the sprites self-animate.

    for (u8 i = 0; i < 9; ++i) {
        movers[i].x = static_cast<u8>(movers[i].x + movers[i].dx);
        movers[i].y = static_cast<u8>(movers[i].y + movers[i].dy);

        // Bounce X — clamp THEN reverse (raw-HPOSP window).
        if (movers[i].x <= X_MIN) {
            movers[i].x = X_MIN;
            movers[i].dx = static_cast<i8>(-movers[i].dx);
        }
        if (movers[i].x >= X_MAX) {
            movers[i].x = X_MAX;
            movers[i].dx = static_cast<i8>(-movers[i].dx);
        }

        // Bounce Y.
        if (movers[i].y <= Y_MIN) {
            movers[i].y = Y_MIN;
            movers[i].dy = static_cast<i8>(-movers[i].dy);
        }
        if (movers[i].y >= Y_MAX) {
            movers[i].y = Y_MAX;
            movers[i].dy = static_cast<i8>(-movers[i].dy);
        }

        // Buffer the logical sprite; the VBI Y-sorts, zones, and commits it.
        Game::sprite(i, sprite_shape, movers[i].x, movers[i].y);
    }

    // Status line: live zone count from the multiplexer + the sprite total.
    Game::print(0, 0, "ZONES:");
    Game::print_num(7, 0, Game::multiplex.zone_count(), 1);
    Game::print(10, 0, "SPRITES:9");
}

// ── Entry point ──────────────────────────────────────────────────────────

int main() {
    // Build the display list, set up P/M (single-line DMA), arm the DLI
    // dispatcher, install the deferred-VBI frame service. No charset arg => the
    // OS ROM font is used for the text field.
    Game::init();

    // Single-width player objects, so each diamond is 8 px wide on screen.
    for (u8 p = 0; p < 4; ++p)
        Platform::hal::set_player_size(p, M::sizep::NORMAL);

    // Readable text: white luminance on a black field/border (via the OS colour
    // shadows the deferred VBI copies to hardware each frame).
    Platform::hal::set_color_pf(4, 0x00);   // COLBK  : background / border
    Platform::hal::set_color_pf(2, 0x00);   // COLPF2 : text background
    Platform::hal::set_color_pf(1, 0x0E);   // COLPF1 : text luminance

    // Colour every LOGICAL sprite. sprite_color() indexes the logical slot, not
    // a hardware player (sprites.h:175); the multiplexer drives each sprite's
    // own colour onto whichever player it occupies in its zone, so a diamond
    // keeps its hue no matter which of the 4 players it is multiplexed onto.
    // Four visually distinct Atari hues, cycled across the nine sprites.
    static constexpr u8 kHues[4] = {
        0x28,   // orange
        0x58,   // green
        0x98,   // blue
        0xC8,   // pink
    };
    for (u8 i = 0; i < 9; ++i)
        Game::sprite_color(i, kHues[i & 3]);

    // Spread the diamonds across the vertical field so they start in different
    // zones; no two share a Y, velocities are +/-1 only (no unsigned wrap at the
    // bounce boundaries). X values sit inside the raw-HPOSP window [48,208].
    movers[0] = { 80,  30,  1,  1};
    movers[1] = { 90,  50, -1,  1};
    movers[2] = {100,  70,  1, -1};
    movers[3] = {110,  90, -1, -1};
    movers[4] = {120, 110,  1,  1};
    movers[5] = {130, 130, -1,  1};
    movers[6] = {140, 150,  1, -1};
    movers[7] = {150, 170, -1, -1};
    movers[8] = {160, 190,  1,  1};

    // Static status-line label drawn once; the dynamic fields update per frame.
    // (frame_step rewrites the whole line every frame, so nothing else here.)

    // One callback per frame, forever.
    Game::run(frame_step);
}
