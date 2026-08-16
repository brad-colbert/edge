// demo/atari_vscroll_split_test.cpp — vertical fine-scroll SPLIT-layout
// validation demo: a scrolled viewport with fixed text rows BELOW it.
//
// This is the layout shape that exposed the scroll-region exit-line defect: the
// region after a vertically fine-scrolled zone is ANTIC's variable-height exit
// line, so without the region's own terminating buffer line the first text row
// under the viewport shrinks and grows with the VSCROL phase. The in-tree
// scroll demo (atari_scroll_test.cpp) has its HUD ABOVE the region, so it never
// showed the defect; this demo is the regression guard for regions below.
//
// The demo runs an unattended sweep holding each vertical fine-scroll phase
// (VSCROL 0..7) for kHold frames, cycling forever, and renders everything a
// headless screenshot needs to *measure* the behavior:
//
//   map row 1  (top visible row)  : full-width inverse band — its visible height
//                                   is the zone's top truncation (8 - VSCROL).
//   map row 12 (mid window)       : full-width inverse band — a full-height
//                                   8-scanline reference for pixel scaling.
//   map row 23 (row scrolling in) : inverse at map cols 8..17 ("left window") —
//                                   visible only on the region's buffer line,
//                                   growing with VSCROL (the bottom-edge mirror
//                                   of the top truncation).
//   HUD row 0                     : inverse at cols 24..39 ("right window") —
//                                   the first fixed row below the region. Must
//                                   be a full 8 scanlines at every phase.
//   HUD row 1                     : the current phase, drawn as (VSCROL+1)
//                                   inverse blocks at cols 0,2,..,14 so a
//                                   screenshot self-labels which phase it caught.
//
// Build with -DEDGE_VSWEEP_BOTTOM to hold the camera at the bottom stop instead
// of sweeping: the last map row (31) carries an inverse band at map cols 20..29
// ("centre window") to show the buffer line's clamped repeat of the last row
// (centre band reads 8+1 scanlines; without the clamp the buffer line would
// fetch out-of-map RAM).

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

using engine::u8;
using engine::u16;
namespace M = atari;

using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::gfx::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC>;

static constexpr u16 kMapW = 64;
static constexpr u16 kMapH = 32;
static constexpr u8  kVis  = 22;

struct SplitScreen {
    // Region 0: the 22-row scrolled viewport. Region 1: a fixed 2-row Mode-2 HUD
    // BELOW it — the region the zone's exit line would truncate.
    using display = engine::DisplayLayout<
        engine::ScrollRegion<engine::TextRegion<M::Mode::MODE_2, kVis>, kMapW, kMapH>,
        engine::TextRegion<M::Mode::MODE_2, 2>>;
};

struct GameConfig {
    using screens = engine::ScreenSet<SplitScreen>;
    static constexpr u8 max_sprites    = 1;
    static constexpr u8 sound_channels = 1;
};

using Game = engine::Core<Platform, GameConfig>;

static engine::TileMap<kMapW, kMapH> g_map;

static constexpr u8 kInv = 0x80;   // inverse space: a solid full-height block

static void fill_map() {
    for (u16 r = 0; r < kMapH; ++r) {
        for (u16 c = 0; c < kMapW; ++c) {
            u8 t = M::ascii_to_internal('.');
            if (r == 1 || r == 12)                   t = kInv;   // full-width bands
            if (r == 23 && c >= 8  && c <= 17)       t = kInv;   // buffer-line band (left)
            if (r == kMapH - 1 && c >= 20 && c <= 29) t = kInv;  // bottom-stop band (centre)
            g_map.set_tile(c, r, t);
        }
    }
}

static void frame_step(const engine::Input&) {
#ifdef EDGE_VSWEEP_BOTTOM
    // Hold the camera at the bottom stop: coarse row = kMapH - kVis, phase 0.
    Game::scroll.set(0, (kMapH - kVis) * 8);
    const u8 phase = 0;
#else
    // Hold each vertical phase for kHold frames, cycling 0..7. The coarse row
    // stays 1 throughout (y = 8..15), so only the fine phase changes.
    static u16 g_frame = 0;
    constexpr u16 kHold = 120;   // frames per phase (~2 s NTSC)
    const u8 phase = static_cast<u8>((g_frame / kHold) % 8);
    Game::scroll.set(0, static_cast<u16>(8 + phase));
    ++g_frame;
#endif

    // HUD row 0: the truncation-measurement band (right half). Static content,
    // rewritten each frame for robustness.
    auto& hud = Game::region<1>();
    for (u8 c = 24; c < 40; ++c) hud.put_char(c, 0, kInv);
    // HUD row 1: phase label — (phase+1) blocks at even columns.
    for (u8 i = 0; i < 8; ++i)
        hud.put_char(static_cast<u8>(2 * i), 1, i <= phase ? kInv : 0);
}

int main() {
    Game::init();

    Platform::hal::set_color_pf(4, 0x90);   // COLBK  : dark blue background
    Platform::hal::set_color_pf(2, 0x90);   // COLPF2 : text background
    Platform::hal::set_color_pf(1, 0x0E);   // COLPF1 : text luminance (white)

    fill_map();
    Game::scroll_map(g_map);

    Game::run(frame_step);
}
