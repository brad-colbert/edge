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
//   Row-12 DLI         : COLBK $94 -> $C4 colour split (DLI fires mid-frame)
//   Fire press         : a pure POKEY tone           (sound subsystem)
//   Sprite overlap     : a POKEY noise burst         (GTIA P0PL collision)
//
// ── Why this demo touches some hardware directly ─────────────────────────
//
// The engine is mid-build: its portable subsystems (display list, sprites,
// sound, input, collisions, text) are complete and used here through the public
// API, but two live-hardware seams are still stubs that this demo fills in:
//
//   * DLI chain delivery. engine::InterruptManager builds the handler tables,
//     but nothing yet sets the DL DLI bit, points VDSLST, or arms NMIEN bit 7.
//     The single colour-split DLI is therefore installed directly here (a tiny
//     raw handler), rather than via Game::interrupts.add_dli().
//
//   * OS shadow registers + P/M DMA bits. The atari OS deferred VBI copies its
//     shadow registers (SDLSTL, SDMCTL, COLOR*, PCOLR*) to the hardware every
//     frame *before* our handler runs, and the engine does not yet set the
//     DMACTL player/missile-DMA bits. So the demo writes the OS shadows (and,
//     as the task asks, the hardware colour registers too) to keep the engine's
//     display list, P/M DMA, and colours alive frame to frame.
//
// Everything else — frame loop, sprite buffering/commit, sound playback, input
// snapshot, collision latching, text output — goes through engine::Core.

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

// ── Colour-split DLI (raw handler) ───────────────────────────────────────
//
// Fires near row 12; rewrites COLBK to dark green ($C4). The OS colour-shadow
// copy restores COLBK to $94 at the top of every frame, so the change is a
// clean per-frame split. Pure assembly (only A touched), so it neither needs
// the C++ dispatcher nor disturbs the main thread's zero-page registers.
extern "C" void hw_dli();
asm(R"(
    .globl hw_dli
hw_dli:
    pha
    lda #$c4
    sta $d018          ; COLPF2 not $d01a COLBK
    pla
    rti
)");

// ── Small helpers ────────────────────────────────────────────────────────

static inline void poke(u16 a, u8 v) {
    *reinterpret_cast<volatile u8*>(static_cast<uintptr_t>(a)) = v;
}

// ASCII -> ANTIC internal screen code, for single-character HUD writes.
static inline u8 g(char c) { return M::ascii_to_internal(c); }

// Set the DLI bit on the 13th mode line (row 12) of the engine-built display
// list and arm the DLI: point VDSLST at hw_dli and enable NMIEN bit 7. Walking
// the list (rather than assuming a fixed offset) tolerates the 4K-crossing LMS
// the screen builder may insert, since that does not add a mode line.
static void install_color_split_dli() {
    u8* dl = const_cast<u8*>(Game::screen.active_dl());
    u16 i = 0;
    u8  mode_line = 0;
    for (;;) {
        const u8 b  = dl[i];
        const u8 op = b & 0x0F;
        if (op == 0x01) break;                 // JMP / JVB terminator
        if (op == 0x00) { ++i; continue; }     // blank-line instruction
        if (++mode_line == 13) { dl[i] = b | 0x80; break; }  // row 12: set DLI
        i += (b & 0x40) ? 3 : 1;               // skip the 2-byte LMS operand
    }

    const uint16_t h = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(&hw_dli));
    poke(0x0200, static_cast<u8>(h & 0xFF));    // VDSLST low
    poke(0x0201, static_cast<u8>(h >> 8));      // VDSLST high
    // NMIEN is write-only; install_vbi() left it at $40 (VBI). Set VBI + DLI.
    *atari::reg::NMIEN = 0xC0;
}

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

    // Suppress the OS attract-mode colour cycling so the display stays bright.
    poke(0x004D, 0);   // ATRACT
}

// ── Entry point ──────────────────────────────────────────────────────────

int main() {
    // Builds the display list, sets up P/M, and installs the (now hardware-
    // correct) deferred-VBI service. No charset argument -> CHBASE keeps its
    // power-on value pointing at the Atari ROM character set at $E000.
    Game::init();

    // P/M object sizes: single-width players, single-width missiles.
    *atari::reg::SIZEP0 = 0;
    *atari::reg::SIZEP1 = 0;
    *atari::reg::SIZEM  = 0;

    // POKEY base config so AUDC writes are audible (these are the OS defaults;
    // set explicitly to be self-contained). AUDCTL = 0, SKCTL = normal.
    *atari::reg::AUDCTL = 0x00;
    poke(0xD20F, 0x03);                 // SKCTL

    // Colours. Per the task, write the hardware registers directly; also write
    // the matching OS shadow registers, because the OS deferred VBI copies the
    // shadows over the hardware every frame and would otherwise wipe these.
    *atari::reg::COLBK  = 0x94;  poke(0x02C8, 0x94);   // background / border
    *atari::reg::COLPF2 = 0x94;  poke(0x02C6, 0x94);   // text background
    *atari::reg::COLPF1 = 0x0E;  poke(0x02C5, 0x0E);   // text luminance
    *atari::reg::COLPM0 = 0x46;  poke(0x02C0, 0x46);   // player 0 (arrow), red
    *atari::reg::COLPM1 = 0xB6;  poke(0x02C1, 0xB6);   // player 1 (diamond), green

    // Keep the engine's display list and P/M DMA alive through the OS VBI's
    // shadow copy: SDLSTL/H -> our list, SDMCTL -> DL + normal playfield +
    // player DMA + missile DMA + one-line P/M resolution ($3E).
    const u16 dl = static_cast<u16>(reinterpret_cast<uintptr_t>(Game::screen.active_dl()));
    poke(0x0230, static_cast<u8>(dl & 0xFF));   // SDLSTL
    poke(0x0231, static_cast<u8>(dl >> 8));     // SDLSTH
    poke(0x022F, 0x3E);                         // SDMCTL

    // Static HUD labels (written once).
    Game::print(0, 0, "EDGE ENGINE V0.1");
    Game::print(20, 0, "FRAME:");
    Game::print(0, 1, "JOY:");
    Game::print(9, 1, "FIRE:");
    Game::print(16, 1, "COL:");
    Game::print(22, 1, "SND:");

    // Arm the colour-split DLI after the display list exists.
    install_color_split_dli();

    // One callback per frame, forever.
    Game::run(frame_step);
}
