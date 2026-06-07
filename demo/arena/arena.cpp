// demo/arena/arena.cpp — Edge engine "Arena" demo scaffold (Berzerk-style).
//
// First deliverable: SCAFFOLD ONLY. It stands up the three-screen flow
// (title -> play -> game-over), a custom ANTIC Mode 4 (4-colour text) charset, a
// baked room layout, and the HUD/play-area colour split via a raster hook. There
// are no game entities, sprites, or sound yet — those land in later steps. The
// result is a loadable .xex (mos-atari8-dos target; see CMakeLists.txt) meant to
// be run on Altirra / Fujisan to confirm the screen flow and the DLI colour split.
//
//   Title    : "EDGE ARENA" centred + a blinking "PRESS FIRE".          (fire -> play)
//   Play     : a bordered brick room + a HUD row (score + 3 hearts) with a
//              DLI colour split between the HUD palette and the play palette. (fire -> over)
//   GameOver : "GAME OVER" + final score + "PRESS FIRE".                (fire -> title)
//
// ── Mode 4 charset note ───────────────────────────────────────────────────
//
// Mode 4 is 4-colour text: 40x24, 8 bytes/char, 2 bits per pixel, so each glyph
// is 4 px wide x 8 tall. The 2-bit pixel values select colour registers:
//   0 = COLBK, 1 = COLPF0, 2 = COLPF1, 3 = COLPF2   (bit 7 of the screen byte
//   would select COLPF3 via inverse chars — unused here).
// print()/print_num() convert ASCII through atari::ascii_to_internal, so the
// letter/digit glyphs MUST sit at their internal-screen-code offsets (code*8):
//   space=$00, '0'-'9' -> $10-$19, ':' -> $1A, 'A'-'Z' -> $21-$3A.
// The room/HUD tiles (0x00-0x05) are written by raw index via put_char(), so they
// don't collide with the printable glyphs.

#include <stdint.h>

#include <engine/core.h>
#include <engine/math.h>
#include <engine/pool.h>
#include <engine/platform/atari/platform.h>
#ifdef EDGE_VBXE
#include <engine/platform/atari/vbxe.h>   // power-user: VBXE Config + palette set_color
#endif

using engine::u8;
using engine::u16;
using engine::i8;
namespace M = atari;

// ── Platform + game configuration ────────────────────────────────────────
//
// VBXE Tier 2 variant (-DEDGE_VBXE): the SAME game, rendered ENTIRELY in the VBXE
// overlay (room, HUD, and all text drawn into the overlay bitmap) with the ANTIC
// playfield OFF, multi-colour pixel sprites composited over it. Game logic, room
// layout, sound, collision, and difficulty are shared verbatim with the baseline;
// only the rendering (and the screen/coordinate plumbing it needs) differs, inside
// #ifdef EDGE_VBXE blocks.
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
#ifdef EDGE_VBXE
namespace V = atari::vbxe;
// DOUBLE-buffered Bitmap — the same proven configuration as the working sprites-
// over-bitmap demo. The room/HUD/text are drawn once into the VRAM master and
// published to both pages; each frame the compositor restores sprite footprints from
// the master and blits the sprites, and overlay_present WAITS for that blit to finish
// before flipping (proper completion sync). Single-buffer's async submit without a
// per-frame completion wait let a long blit be overwritten in flight → cascading
// corruption; double-buffer avoids it. play_enter now draws the room with ~15 blitter
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
#else
using Platform = atari::StockXL_NTSC;
#endif

#ifdef EDGE_VBXE
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
#else
struct TitleScreen {
    using display = engine::DisplayLayout<engine::TextRegion<M::Mode::MODE_4, 24>>;
};

struct PlayScreen {
    // Region 0: 1-row HUD.  Region 1: 23-row play area.
    using display = engine::DisplayLayout<
        engine::TextRegion<M::Mode::MODE_4, 1>,
        engine::TextRegion<M::Mode::MODE_4, 23>>;
};

struct GameOverScreen {
    using display = engine::DisplayLayout<engine::TextRegion<M::Mode::MODE_4, 24>>;
};
#endif

struct GameConfig {
    using screens        = engine::ScreenSet<TitleScreen, PlayScreen, GameOverScreen>;
    using initial_screen = TitleScreen;
#ifdef EDGE_VBXE
    // Tier 2: player (0) + 3 enemies (1..3) + 4 bullets (4..7) all composite as
    // blitter overlay sprites (P/M would sit under the opaque overlay). The blitter
    // has no 4-sprite hardware limit, so 8 logical sprites is fine.
    static constexpr u8 max_sprites      = 8;
    static constexpr u8 sound_channels   = 2;
    static constexpr u8 max_raster_hooks = 1;   // unused on the overlay; keep minimal
#else
    // Player (slot 0) + up to 3 enemies (slots 1..3) = 4 sprites, the number of P/M
    // hardware players. At <=4 the multiplexer forms a single zone and maps every
    // sprite 1:1 onto a hardware player for the whole frame — no Y-zone splitting,
    // no boundary DLIs, so the multiplex commit path (which still cross-contaminates
    // shape/colour between players when a chase crowds >4 sprites into one Y band)
    // is bypassed entirely. Held here until the multiplexer is hardened; raise once
    // combat thins enemies and the per-zone commit is trustworthy under load.
    static constexpr u8 max_sprites      = 4;
    static constexpr u8 sound_channels   = 2;
    // Static + dynamic raster hooks share this one budget (interrupt.h). At 4 sprites
    // the multiplexer is a single zone -> zero boundary DLIs, so the play screen needs
    // only its 1 static colour-split hook; 2 leaves a slot of headroom. (The title
    // animates via the COLPF0 shadow, not a DLI, so it needs no hooks.)
    static constexpr u8 max_raster_hooks = 2;
#endif
};

using Game = engine::Core<Platform, GameConfig>;

// ── Mode 4 pixel encoding helpers (constexpr) ─────────────────────────────

// Pack four 2-bit pixel values (leftmost first) into one Mode-4 byte.
constexpr u8 px(u8 a, u8 b, u8 c, u8 d) {
    return static_cast<u8>((a << 6) | (b << 4) | (c << 2) | (d << 0));
}
// Expand a 4-bit monochrome row (bit3 = leftmost) into a Mode-4 byte where each
// set bit becomes a COLPF0 pixel (value 1).
constexpr u8 mono(u8 bits) {
    return px((bits >> 3) & 1, (bits >> 2) & 1, (bits >> 1) & 1, bits & 1);
}

// ── Custom Mode 4 charset (constexpr, ROM-resident) ───────────────────────

constexpr void set8(engine::Charset1K& cs, u8 code,
                    u8 a, u8 b, u8 c, u8 d, u8 e, u8 f, u8 g, u8 h) {
    const u16 o = static_cast<u16>(code) * 8;
    cs.data[o + 0] = a; cs.data[o + 1] = b; cs.data[o + 2] = c; cs.data[o + 3] = d;
    cs.data[o + 4] = e; cs.data[o + 5] = f; cs.data[o + 6] = g; cs.data[o + 7] = h;
}
// Set a monochrome (COLPF0) glyph from eight 4-bit rows.
constexpr void setm(engine::Charset1K& cs, u8 code,
                    u8 a, u8 b, u8 c, u8 d, u8 e, u8 f, u8 g, u8 h) {
    set8(cs, code, mono(a), mono(b), mono(c), mono(d),
                   mono(e), mono(f), mono(g), mono(h));
}

constexpr engine::Charset1K make_arena_charset() {
    engine::Charset1K cs{};   // zero-initialised: 0x00 is the empty/space tile.

    // ── Room / HUD tiles (raw 2-bit values; written by index) ──
    // 0x01 solid wall: brick courses, COLPF1 (2) over COLPF2 (3), offset seams.
    set8(cs, 0x01, px(2,2,2,2), px(2,0,2,2), px(2,2,2,2), px(3,3,3,3),
                   px(3,3,0,3), px(3,3,3,3), px(2,2,2,2), px(2,0,2,2));
    // 0x02 wall variant: courses swapped + different seam offsets.
    set8(cs, 0x02, px(3,3,3,3), px(3,3,0,3), px(3,3,3,3), px(2,2,2,2),
                   px(2,0,2,2), px(2,2,2,2), px(3,3,3,3), px(3,0,3,3));
    // 0x03 floor dot: a single COLPF0 (1) pixel for a subtle grid.
    set8(cs, 0x03, px(0,0,0,0), px(0,0,0,0), px(0,0,0,0), px(0,0,1,0),
                   px(0,0,0,0), px(0,0,0,0), px(0,0,0,0), px(0,0,0,0));
    // 0x04 explosion: a bright burst, COLPF1 (2) core inside a COLPF0 (1) ring.
    set8(cs, 0x04, px(0,1,1,0), px(1,2,2,1), px(1,2,2,1), px(1,1,1,1),
                   px(1,1,1,1), px(1,2,2,1), px(1,2,2,1), px(0,1,1,0));
    // 0x05 heart: small heart in COLPF0 (1).
    set8(cs, 0x05, px(1,0,0,1), px(1,1,1,1), px(1,1,1,1), px(1,1,1,1),
                   px(0,1,1,0), px(0,1,1,0), px(0,0,0,0), px(0,0,0,0));

    // ── Digits 0-9 at internal codes $10-$19 (COLPF0) ──
    setm(cs, 0x10, 0b0110,0b1001,0b1001,0b1001,0b1001,0b1001,0b0110,0b0000); // 0
    setm(cs, 0x11, 0b0100,0b1100,0b0100,0b0100,0b0100,0b0100,0b1110,0b0000); // 1
    setm(cs, 0x12, 0b0110,0b1001,0b0001,0b0010,0b0100,0b1000,0b1111,0b0000); // 2
    setm(cs, 0x13, 0b1110,0b0001,0b0001,0b0110,0b0001,0b0001,0b1110,0b0000); // 3
    setm(cs, 0x14, 0b0010,0b0110,0b1010,0b1111,0b0010,0b0010,0b0010,0b0000); // 4
    setm(cs, 0x15, 0b1111,0b1000,0b1110,0b0001,0b0001,0b1001,0b0110,0b0000); // 5
    setm(cs, 0x16, 0b0110,0b1000,0b1000,0b1110,0b1001,0b1001,0b0110,0b0000); // 6
    setm(cs, 0x17, 0b1111,0b0001,0b0010,0b0010,0b0100,0b0100,0b0100,0b0000); // 7
    setm(cs, 0x18, 0b0110,0b1001,0b1001,0b0110,0b1001,0b1001,0b0110,0b0000); // 8
    setm(cs, 0x19, 0b0110,0b1001,0b1001,0b0111,0b0001,0b0001,0b0110,0b0000); // 9
    // Colon ':' at internal code $1A.
    setm(cs, 0x1A, 0b0000,0b0100,0b0100,0b0000,0b0100,0b0100,0b0000,0b0000); // :

    // ── Letters A-Z at internal codes $21-$3A (COLPF0) ──
    setm(cs, 0x21, 0b0110,0b1001,0b1001,0b1111,0b1001,0b1001,0b1001,0b0000); // A
    setm(cs, 0x22, 0b1110,0b1001,0b1001,0b1110,0b1001,0b1001,0b1110,0b0000); // B
    setm(cs, 0x23, 0b0111,0b1000,0b1000,0b1000,0b1000,0b1000,0b0111,0b0000); // C
    setm(cs, 0x24, 0b1110,0b1001,0b1001,0b1001,0b1001,0b1001,0b1110,0b0000); // D
    setm(cs, 0x25, 0b1111,0b1000,0b1000,0b1110,0b1000,0b1000,0b1111,0b0000); // E
    setm(cs, 0x26, 0b1111,0b1000,0b1000,0b1110,0b1000,0b1000,0b1000,0b0000); // F
    setm(cs, 0x27, 0b0111,0b1000,0b1000,0b1011,0b1001,0b1001,0b0111,0b0000); // G
    setm(cs, 0x28, 0b1001,0b1001,0b1001,0b1111,0b1001,0b1001,0b1001,0b0000); // H
    setm(cs, 0x29, 0b1110,0b0100,0b0100,0b0100,0b0100,0b0100,0b1110,0b0000); // I
    setm(cs, 0x2A, 0b0011,0b0001,0b0001,0b0001,0b1001,0b1001,0b0110,0b0000); // J
    setm(cs, 0x2B, 0b1001,0b1010,0b1100,0b1100,0b1010,0b1010,0b1001,0b0000); // K
    setm(cs, 0x2C, 0b1000,0b1000,0b1000,0b1000,0b1000,0b1000,0b1111,0b0000); // L
    setm(cs, 0x2D, 0b1001,0b1111,0b1111,0b1001,0b1001,0b1001,0b1001,0b0000); // M
    setm(cs, 0x2E, 0b1001,0b1101,0b1101,0b1011,0b1011,0b1001,0b1001,0b0000); // N
    setm(cs, 0x2F, 0b0110,0b1001,0b1001,0b1001,0b1001,0b1001,0b0110,0b0000); // O
    setm(cs, 0x30, 0b1110,0b1001,0b1001,0b1110,0b1000,0b1000,0b1000,0b0000); // P
    setm(cs, 0x31, 0b0110,0b1001,0b1001,0b1001,0b1010,0b1010,0b0111,0b0000); // Q
    setm(cs, 0x32, 0b1110,0b1001,0b1001,0b1110,0b1010,0b1010,0b1001,0b0000); // R
    setm(cs, 0x33, 0b0111,0b1000,0b1000,0b0110,0b0001,0b0001,0b1110,0b0000); // S
    setm(cs, 0x34, 0b1111,0b0100,0b0100,0b0100,0b0100,0b0100,0b0100,0b0000); // T
    setm(cs, 0x35, 0b1001,0b1001,0b1001,0b1001,0b1001,0b1001,0b0110,0b0000); // U
    setm(cs, 0x36, 0b1001,0b1001,0b1001,0b1001,0b1001,0b0110,0b0110,0b0000); // V
    setm(cs, 0x37, 0b1001,0b1001,0b1001,0b1001,0b1111,0b1111,0b1001,0b0000); // W
    setm(cs, 0x38, 0b1001,0b1001,0b0110,0b0110,0b0110,0b1001,0b1001,0b0000); // X
    setm(cs, 0x39, 0b1001,0b1001,0b0110,0b0100,0b0100,0b0100,0b0100,0b0000); // Y
    setm(cs, 0x3A, 0b1111,0b0001,0b0010,0b0100,0b0100,0b1000,0b1111,0b0000); // Z

    return cs;
}

constexpr auto arena_charset = make_arena_charset();

// ── Room layout (constexpr, baked) ────────────────────────────────────────
//
// 40 columns x 23 rows of tile indices for the play area (PlayScreen region 1).
// Bordered with wall (0x01); symmetric interior bars for cover; sparse floor dots
// for texture.

struct Room { u8 t[23][40]; };

constexpr Room make_room() {
    Room r{};
    for (u8 y = 0; y < 23; ++y) {
        for (u8 x = 0; x < 40; ++x) {
            if (y == 0 || y == 22 || x == 0 || x == 39)
                r.t[y][x] = 0x01;                              // border wall
            else
                r.t[y][x] = (((x + y) & 3) == 0) ? 0x03 : 0x00; // sparse floor dot
        }
    }
    // Interior wall bars, 4 tiles each, mirrored left/right (about col 20) and
    // top/bottom (about row 11). Outer bars in 0x01, inner bars in 0x02.
    const u8 ys1[2] = {5, 17};
    const u8 xs1[2] = {8, 28};
    for (u8 a = 0; a < 2; ++a)
        for (u8 b = 0; b < 2; ++b)
            for (u8 i = 0; i < 4; ++i) r.t[ys1[a]][xs1[b] + i] = 0x01;
    const u8 ys2[2] = {9, 13};
    const u8 xs2[2] = {14, 22};
    for (u8 a = 0; a < 2; ++a)
        for (u8 b = 0; b < 2; ++b)
            for (u8 i = 0; i < 4; ++i) r.t[ys2[a]][xs2[b] + i] = 0x02;
    return r;
}

constexpr Room kRoom = make_room();

// ── Colours ───────────────────────────────────────────────────────────────

#ifndef EDGE_VBXE
// Field map for set_color_pf: 0-3 = COLPF0-3, 4 = COLBK.
static void set_hud_palette() {
    Platform::hal::set_color_pf(4, 0x92);   // COLBK  : dark blue
    Platform::hal::set_color_pf(0, 0x0E);   // COLPF0 : white
    Platform::hal::set_color_pf(1, 0x1A);   // COLPF1 : yellow
    Platform::hal::set_color_pf(2, 0xC4);   // COLPF2 : bright green
    Platform::hal::set_color_pf(3, 0x34);   // COLPF3 : red
}

// Play-area background colour, driven into COLBK by the play-area DLI every frame.
// Normally black; the death-flash (play_step) raises it to kFlashColor for a few frames.
// A one-shot register write wouldn't survive — this DLI overwrites COLBK each frame — so
// the flash flows through the same rendering path as the rest of the play palette.
static u8 g_play_bg = 0x00;

// Play-area palette, applied mid-frame by the raster hook below.
static void play_palette_split() {
    engine::RasterContext<Platform> ctx{};
    ctx.set_background_color(g_play_bg); // COLBK  : black (flashes white on a hit)
    ctx.set_playfield_color<0>(0x96);   // COLPF0 : medium blue
    ctx.set_playfield_color<1>(0x2A);   // COLPF1 : orange
    ctx.set_playfield_color<2>(0x24);   // COLPF2 : brown
    ctx.set_playfield_color<3>(0xC6);   // COLPF3 : green
}

// Boundary between region 0 (HUD) and region 1 (play): the display list opens
// with 3 dl_blank(8) (24 scanlines), then the 1-row HUD (8 scanlines), so the
// play area begins at scanline 24 + 1*8 = 32. (Same derivation as the row-12
// split in atari_hw_test.cpp; tune on hardware if the seam is off.)
static constexpr u8 kSplitScanline = 32;
#endif // !EDGE_VBXE

// ── Small helpers ──────────────────────────────────────────────────────────

#ifndef EDGE_VBXE
// Fill every cell of a text-region view with one tile (the view has no clear()).
template <typename View>
static void fill_region(View& v, u8 tile) {
    for (u16 i = 0; i < View::length; ++i) v.ptr[i] = tile;
}
#endif

// Centre column for an n-character string on a 40-column line. (Baseline text +
// in-game overlay HUD layout; the overlay title/game-over use centre_px instead.)
[[maybe_unused]] static constexpr u8 centre(u8 n) { return static_cast<u8>((40 - n) / 2); }


// ── Player entity ────────────────────────────────────────────────────────────

struct Player {
    u8 x;       // sprite position, same coordinate system as Game::sprite() (raw HPOSP)
    u8 y;       // P/M strip offset (≈ display scanline from top)
    u8 dir;     // last movement direction: 0=up, 1=right, 2=down, 3=left
    u8 lives;
    u8 iframe;  // invincibility frames remaining
};
static Player player;

// 8x8 humanoid silhouette. Baseline: 1bpp P/M (each set bit is a player pixel).
// VBXE: the same silhouette as 8bpp pixel art — body (palette-1 idx 1), darker
// outline (2), and a highlight visor/belt (3); idx 0 is transparent.
#ifndef EDGE_VBXE
constexpr auto player_shape = engine::make_sprite<8, 8>({
    0b00111100,   // ..XXXX..  head
    0b00111100,   // ..XXXX..
    0b00011000,   // ...XX...  neck
    0b01111110,   // .XXXXXX.  shoulders / arms
    0b00111100,   // ..XXXX..  torso
    0b00111100,   // ..XXXX..
    0b00100100,   // ..X..X..  legs
    0b01100110,   // .XX..XX.  feet
});
#else
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
#endif

// ── Sprite <-> play-area coordinate mapping ──
//
// X is written straight to HPOSP, so the normal playfield's left edge (column 0)
// sits at HPOSP 48 and each Mode 4 cell is 4 color clocks wide (modes.h
// fine_scroll_range == 4). Y is the P/M strip offset; in practice a player
// drawn at strip offset Y renders a few scanlines ABOVE the playfield row at the
// same display-list scanline (the P/M DMA and ANTIC don't start at the same
// line), so the collision grid's Y origin is biased down by kPmYBias to line the
// tile rows up with what's actually drawn. Region 1 (play area) begins at
// kSplitScanline; each Mode 4 row is 8 scanlines tall. Hardware-tuned: nudge
// kPmYBias if the sprite stops a little early/late against horizontal walls.
#ifdef EDGE_VBXE
// Tier 2 overlay coordinates: the play area lives in the VBXE framebuffer's pixel
// space, mapped 1:1 onto blitter-sprite (x,y). The exact baseline cell math is kept
// (4px cells; the 8px sprite spans 2 columns / 1 row) so game logic and difficulty
// are byte-identical — only the origin moves. The blitter-sprite X is u8, so the
// 160px-wide room (40*4) is centred at x=80 to stay within 0..255. The HUD occupies
// the HUD row, then the play area. A top margin pushes everything down out of the
// NTSC top overscan (otherwise the HUD row at y=0 is hidden off the top of screen).
static constexpr u8 kPlayLeftX = 80;             // overlay px of play-area column 0 ((320-160)/2)
static constexpr u8 kCellW     = 4;              // play-area cell width in overlay px
static constexpr u8 kCellH     = 8;              // play-area row height in overlay px
static constexpr u8 kTopMargin = 24;             // shift content below the top overscan (also ~centres 192px in 240)
static constexpr u8 kPlayTopY  = kTopMargin + kCellH; // overlay y of play-area row 0 (HUD row sits at kTopMargin)
#else
static constexpr u8 kPlayLeftX = 48;             // HPOSP of play-area column 0
static constexpr u8 kCellW     = 4;              // Mode 4 cell width in color clocks
static constexpr u8 kPmYBias   = 8;              // P/M-vs-playfield vertical offset (scanlines, hardware-measured: one full Mode 4 row)
static constexpr u8 kPlayTopY  = kSplitScanline + kPmYBias; // collision row-0 origin (36)
static constexpr u8 kCellH     = 8;              // Mode 4 row height in scanlines
#endif

static constexpr u8 kSpriteW   = 8;              // player sprite footprint (color clocks / scanlines)
static constexpr u8 kSpriteH   = 8;
static constexpr u8 kPlayerSpeed = 2;            // pixels moved per frame
static constexpr u8 kPlayerColor = 0x0F;         // white — stands out over the play palette

// Wall tiles (from make_room): solid border/outer bars and the inner-bar variant.
static constexpr u8 kWallA  = 0x01;
static constexpr u8 kWallB  = 0x02;
static constexpr u8 kNoTile = 0xFF;              // sentinel: outside the play area

// Precomputed row*40 byte offsets into the flat room array. tile_at is the
// play_step hot path (player + 3 enemies, four-corner can_move each frame), and
// kRoom.t[row][col] otherwise costs a 16-bit row*40 multiply on the 6502 per call;
// this turns it into a table lookup + add.
struct RowBase { u16 o[23]; };
constexpr RowBase make_row_base() {
    RowBase rb{};
    for (u8 r = 0; r < 23; ++r) rb.o[r] = static_cast<u16>(r * 40);
    return rb;
}
constexpr RowBase kRowBase = make_row_base();

// Tile index at a sprite coordinate within the play area, or kNoTile if outside.
static u8 tile_at(u8 sprite_x, u8 sprite_y) {
    if (sprite_x < kPlayLeftX || sprite_y < kPlayTopY) return kNoTile;
    const u8 col = static_cast<u8>((sprite_x - kPlayLeftX) / kCellW);
    const u8 row = static_cast<u8>((sprite_y - kPlayTopY) / kCellH);
    if (col >= 40 || row >= 23) return kNoTile;
    return (&kRoom.t[0][0])[kRowBase.o[row] + col];
}

// A tile that blocks movement: either wall variant, or off the play area.
static bool is_blocked(u8 tile) {
    return tile == kWallA || tile == kWallB || tile == kNoTile;
}

// True if the 8x8 sprite at (new_x, new_y) clears walls at all four corners.
// Four-corner checking stops the sprite from clipping a wall corner-first.
static bool can_move(u8 new_x, u8 new_y) {
    const u8 x0 = new_x, x1 = static_cast<u8>(new_x + kSpriteW - 1);
    const u8 y0 = new_y, y1 = static_cast<u8>(new_y + kSpriteH - 1);
    return !is_blocked(tile_at(x0, y0)) && !is_blocked(tile_at(x1, y0)) &&
           !is_blocked(tile_at(x0, y1)) && !is_blocked(tile_at(x1, y1));
}

#ifdef EDGE_VBXE
// ── VBXE overlay renderer (Tier 2) ──────────────────────────────────────────
//
// All screen content is drawn into the overlay "master" canvas via Game::gfx()
// (Background::Bitmap), then published; the compositor restores sprite footprints
// from the master each frame. Glyphs — room tiles AND text — are rendered straight
// from the existing arena_charset (each Mode-4 cell is 4px wide x 8 tall, 2bpp), so
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

static void load_overlay_palette() {
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
#endif // EDGE_VBXE

static void player_init() {
    // Centre of the 40x23 play area (col 20, row 11 — open floor in the room).
    player.x      = static_cast<u8>(kPlayLeftX + 20 * kCellW);   // 128
    player.y      = static_cast<u8>(kPlayTopY  + 11 * kCellH);   // 120
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

// ── Enemy entities ───────────────────────────────────────────────────────────
//
// Enemies spawn from the room edges and home in on the player. They share the
// player's 8x8 footprint and the same tile_at()/can_move() collision helpers, and
// occupy logical sprite slots 1..3 (player is slot 0): slot = pool_index + 1, which
// is what max_sprites = 4 budgets for. No combat yet — they overlap the player
// harmlessly (collision response lands in a later step).

struct Enemy {
    u8 x;       // sprite coords, same system as the player
    u8 y;
    u8 period;  // frames between moves: LOWER = faster. Set per spawn (difficulty ramp).
    u8 timer;   // movement tick counter
};
static engine::SlotPool<Enemy, 3> enemies;

// 8x8 blocky-robot silhouette — deliberately unlike the humanoid player. Baseline:
// 1bpp P/M. VBXE: the same silhouette as menacing 8bpp pixel art — red body
// (palette-1 idx 4), orange trim (5), and glowing yellow eyes (6); idx 0 transparent.
#ifndef EDGE_VBXE
constexpr auto enemy_shape = engine::make_sprite<8, 8>({
    0b10011001,   // X..XX..X  antennae
    0b11111111,   // XXXXXXXX  head
    0b10100101,   // X.X..X.X  eyes
    0b11111111,   // XXXXXXXX
    0b11111111,   // XXXXXXXX  body
    0b10111101,   // X.XXXX.X
    0b10100101,   // X.X..X.X  legs
    0b01000010,   // .X....X.  feet
});
#else
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
#endif

static constexpr u8 kEnemyColor    = 0x34;   // red — contrasts the white player + play palette
static constexpr u8 kEnemyStep     = 2;      // pixels moved per move tick (matches kPlayerSpeed)
static constexpr u8 kEnemyCount    = 3;      // pool capacity / sprite slots 1..3
static constexpr u8 kSpawnInterval = 60;     // frames between spawn attempts (~1 second)

// Difficulty ramp: each new enemy moves a little more often than the last, gently.
// An enemy steps kEnemyStep px every `period` frames, so period is inverse speed. It
// starts at kEnemyStartPeriod (slow) and shaves one frame every kRampSpawnsPerStep
// spawns, down to a floor of kEnemyMinPeriod — kept above 1 so enemies never reach the
// player's own 2 px/frame. g_spawn_level counts spawns this round; reset in play_enter.
//   period 8 ≈ 0.25 px/f | 4 ≈ 0.5 | 3 ≈ 0.67 (old fixed speed) | 2 = 1.0 (half player)
// Tune feel here: kEnemyStartPeriod (opening pace), kEnemyMinPeriod (top speed),
// kRampSpawnsPerStep (how gradually it builds — higher = slower ramp).
static constexpr u8 kEnemyStartPeriod    = 8;
static constexpr u8 kEnemyMinPeriod      = 3;
static constexpr u8 kRampSpawnsPerStep   = 6;
static u8 g_spawn_level = 0;

// Move period for the next spawn: max(kEnemyMinPeriod, start - level/perStep).
static u8 enemy_period_for_level(u8 level) {
    constexpr u8 span = kEnemyStartPeriod - kEnemyMinPeriod;
    const u8 drop = level / kRampSpawnsPerStep;
    return static_cast<u8>(kEnemyStartPeriod - (drop < span ? drop : span));
}

// Interior bounds (col 0 / 39 and row 0 / 22 are the border wall).
static constexpr u8 kInteriorMaxCol = 38;
static constexpr u8 kInteriorMaxRow = 21;

// An enemy sprite is kSpriteW px wide, and a Mode 4 cell is only kCellW (4) px wide,
// so the sprite spans kSpriteCols = 2 columns (col..col+1). Its left column must stay
// one cell in from the right wall or the right half lands in the col-39 border — which
// is exactly what got enemies stuck on the right. (The sprite is 8 px tall = one cell,
// so rows need no such margin.) Spawn validity uses can_move(), the same four-corner
// footprint test movement uses, so a spawn can never start clipping a wall.
static constexpr u8 kSpriteCols  = kSpriteW / kCellW;                   // 2
static constexpr u8 kMaxSpawnCol = kInteriorMaxCol - (kSpriteCols - 1); // 37

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

// ── Combat: bullets, explosions, sound, scoring ──────────────────────────────
//
// The player fires the four hardware projectiles (ADR-025 — missiles are direct,
// not multiplexed). The bullet pool index IS the hardware missile index (0..3), so
// missile-to-player collision masks map straight onto the bullets. Enemies are
// players 1..3 (single-zone tier), so collision bits 1..3 name enemy pool index
// p-1; bit 0 is the player itself.

struct Bullet {
    u8 x, y;     // sprite coords, same system as the player
    i8 dx, dy;   // per-frame velocity
};
static engine::SlotPool<Bullet, 4> bullets;   // pool index == hardware missile index

static constexpr u8  kBulletSpeed  = 4;       // pixels per frame
static constexpr u8  kBulletHeight = 4;       // missile-strip rows drawn
static constexpr u8  kBulletW      = 4;       // bullet collision box width
static constexpr u8  kIFrames      = 120;     // ~2 s invincibility after a hit
static constexpr u16 kScorePerKill = 10;

// Bullets render differently per backend: baseline uses the P/M hardware missiles
// (ADR-025), but on the VBXE overlay those P/M projectiles sit UNDER the opaque
// overlay and are invisible — so the overlay draws bullets as small blitter sprites
// in slots 4..7 (4 + pool index; the player is 0, enemies 1..3 — max_sprites=8).
#ifdef EDGE_VBXE
static constexpr u8 kBulletSlotBase = 4;
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
#else
static void bullet_show(u8 idx, u8 x, u8 y) { Game::missile(idx, x, y, kBulletHeight); }
static void bullet_hide(u8 idx)             { Game::missile_hide(idx); }
#endif

// Axis-aligned box overlap. Promotes to int for the +w/+h so u8 edges don't wrap.
static bool boxes_overlap(u8 ax, u8 ay, u8 aw, u8 ah,
                          u8 bx, u8 by, u8 bw, u8 bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

// Text-mode explosion: tile 0x04 stamped at a play-area cell, restored to the
// baked room tile when its timer runs out.
struct Explosion {
    u8 col, row;   // play-area character cell
    u8 timer;      // frames until the room tile is restored
};
static engine::PackedPool<Explosion, 8> explosions;
static constexpr u8 kExplosionFrames = 12;
static constexpr u8 kExplosionTile   = 0x04;

// Round state, visible to the game-over screen.
static u16  g_score     = 0;

// ── Polish state (Prompt 5): high score, level, pacing pauses ──
static u16  g_high_score = 0;     // best score so far; persists across rounds
static bool g_new_high   = false; // this round beat the high score (game-over banner)
static u8   g_level      = 1;     // wave / difficulty indicator shown in the HUD
static u8   g_kills      = 0;     // kills this round; drives g_level
static u8   g_get_ready  = 0;     // "GET READY" countdown at play start (0 = playing)
static u8   g_death_timer= 0;     // freeze frames before the game-over screen (0 = alive)

static constexpr u8  kGetReadyFrames  = 90;    // ~1.5 s orientation pause at play start
static constexpr u8  kDeathPause      = 60;    // ~1 s frozen-room beat before game-over
static constexpr u8  kKillsPerLevel   = 8;     // kills between level (difficulty) ticks
#ifndef EDGE_VBXE
static constexpr u8  kFlashColor      = 0x0E;  // white play-area flash on player damage (ANTIC COLBK)
#endif
// "GET READY" is 9 chars (the charset has no '!' glyph — '!' maps to the wall tile),
// centred at col 15 (cols 15..23), on row 9 so it clears the player at centre row 11.
static constexpr u8  kGetReadyRow     = 9;
static constexpr u8  kGetReadyCol     = 15;    // centre(9)
static constexpr u16 kGetReadyClearEnd= kGetReadyCol + 9; // 24 (one past the last glyph)

// ── Sound effects (constexpr ROM tables, engine/sound.h) ──
// AUDF is inverse pitch: a low value is a high note. Shoot is a short bright tone
// on channel 0; explosion is a noisy burst whose pitch descends (rising AUDF) as it
// fades on channel 1; damage is a brief buzzy drop on channel 0.
constexpr auto kShootSfx = engine::make_sound({
    engine::SoundFrame{engine::audio::Waveform::Tone, 0x20, 10, 2},
    engine::SoundFrame{engine::audio::Waveform::Tone, 0x30,  6, 2},
});
constexpr auto kExplosionSfx = engine::make_sound({
    engine::SoundFrame{engine::audio::Waveform::Noise, 0x20, 14, 3},
    engine::SoundFrame{engine::audio::Waveform::Noise, 0x40, 10, 3},
    engine::SoundFrame{engine::audio::Waveform::Noise, 0x70,  6, 4},
});
constexpr auto kDamageSfx = engine::make_sound({
    engine::SoundFrame{engine::audio::Waveform::Buzz, 0x60, 14, 4},
    engine::SoundFrame{engine::audio::Waveform::Buzz, 0x50, 10, 4},
});

// HUD is event-driven, NOT redrawn every frame: print_num costs five u16 divisions
// (~7% of a frame) and the loop runs one game step per VBI, so a constant per-frame
// HUD redraw eats the headroom a live bullet's work needs — push play_step over one
// frame and the loop drops frames, dropping fire-button edges with them. So redraw
// the score only when it changes (a kill) and the hearts only on a life change.
// Level/wave readout in the HUD gap between SCORE (cols 0..11) and the hearts
// (cols 36..38). Event-driven like the score — redrawn only when the level ticks.
static constexpr u8 kHudLevelCol = 18;   // "LV:" label; digits start at +3

#ifdef EDGE_VBXE
// Overlay HUD: redraw the changed field into the master canvas (clearing it to the
// HUD background first) and republish. HUD fields change only on a kill / life /
// level event, so the whole-master republish cost lands rarely, not per frame.
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
#else
static void hud_draw_score() {
    Game::region<PlayScreen, 0>().print_num(7, 0, g_score, 5);
}
static void hud_draw_lives() {
    auto& hud = Game::region<PlayScreen, 0>();
    for (u8 c = 36; c <= 38; ++c) hud.put_char(c, 0, 0x00);
    for (u8 i = 0; i < player.lives; ++i)
        hud.put_char(static_cast<u8>(36 + i), 0, 0x05);
}
static void hud_draw_level() {
    Game::region<PlayScreen, 0>().print_num(kHudLevelCol + 3, 0, g_level, 2);
}

// Stamp a text-mode explosion at the play-area cell under a sprite coordinate.
static void spawn_explosion(u8 sprite_x, u8 sprite_y) {
    if (sprite_x < kPlayLeftX || sprite_y < kPlayTopY) return;
    const u8 col = static_cast<u8>((sprite_x - kPlayLeftX) / kCellW);
    const u8 row = static_cast<u8>((sprite_y - kPlayTopY) / kCellH);
    if (col >= 40 || row >= 23) return;
    if (Explosion* e = explosions.acquire()) {
        e->col = col; e->row = row; e->timer = kExplosionFrames;
        Game::region<PlayScreen, 1>().put_char(col, row, kExplosionTile);
    }
}
#endif

// ── Screen entry + per-frame callbacks ──────────────────────────────────────

static u16  g_title_frames = 0;
static bool g_fire_armed   = false;   // require a fire release before accepting it
static bool g_fire_prev    = false;   // fire level as last seen by a game step
static u8   spawn_timer    = 0;       // counts down to the next enemy spawn attempt

// A transition just happened: ignore fire until it is first seen released, so a
// single press doesn't fall straight through multiple screens.
static void arm_fire() { g_fire_armed = false; g_fire_prev = true; }

// Rising edge of fire, tracked against the level WE last observed in a game step —
// not engine::Input::fire_pressed(), whose prev-state is shifted every VBI. When a
// frame is dropped (the loop misses a frame the VBI serviced) that per-VBI edge is
// lost, so held-fire presses get eaten. Comparing the current level to our own
// per-step history catches the press on the next step regardless of dropped frames.
static bool fire_edge(const engine::Input& in) {
    const bool now = in.fire();
    bool edge = false;
    if (!g_fire_armed) {
        if (!now) g_fire_armed = true;   // wait for release after a screen change
    } else if (now && !g_fire_prev) {
        edge = true;
    }
    g_fire_prev = now;
    return edge;
}

// ── Title screen colour (Prompt 5) ─────────────────────────────────────────
//
// Every printed glyph is a Mode-4 value-1 pixel → COLPF0, so ONE register drives ALL
// title text. Animate it by rotating COLPF0 itself each frame — written to the OS colour
// shadow (set_color_pf), which the VBI copies to the hardware register every frame, the
// same path set_hud_palette() uses. The whole title cycles together (no per-row colours:
// that needs a mid-screen DLI, and a multi-hook C++ DLI chain didn't deliver on the title
// screen — out of scope to fix in engine code here). PRESS FIRE still blinks independently
// (print/erase). COLPF0 is restored to white on exit (main) so the HUD/game-over text,
// which share the register, stay readable.
static constexpr u8 kTitleRowEdge  = 10;   // EDGE ARENA
static constexpr u8 kTitleRowPress = 14;   // PRESS FIRE (blinks)
static constexpr u8 kTitleRowInstr = 16;   // instruction line
static constexpr u8 kTitleRowBest  = 18;   // BEST: nnnnn
#ifndef EDGE_VBXE
static constexpr u8 kTitleTextColor = 0x0E; // white — title default and HUD/game-over text

// Bright hues against the dark-blue (0x92) title background: white, cyan, green,
// yellow, orange, red. Advanced every kTitleColorPeriod frames by title_step.
static constexpr u8 kTitleHues[]      = {0x0E, 0x9A, 0xCA, 0x1A, 0x2A, 0x4A};
static constexpr u8 kTitleHueCount    = sizeof(kTitleHues);
static constexpr u8 kTitleColorPeriod = 8;   // frames between hue steps (slow cycle)
#endif

static void title_enter() {
#ifdef EDGE_VBXE
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
#else
    auto& v = Game::region<TitleScreen, 0>();
    fill_region(v, 0x00);
    v.print(centre(10), kTitleRowEdge, "EDGE ARENA");
    v.print(centre(31), kTitleRowInstr, "JOYSTICK TO MOVE  FIRE TO SHOOT");
    // Show the best score below it once one exists (skip on first boot).
    if (g_high_score > 0) {
        v.print(centre(11), kTitleRowBest, "BEST: 00000");
        v.print_num(static_cast<u8>(centre(11) + 6), kTitleRowBest, g_high_score, 5);
    }
#endif
    g_title_frames = 0;
    arm_fire();
}

static bool title_step(const engine::Input& in) {
#ifdef EDGE_VBXE
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
#else
    auto& v = Game::region<TitleScreen, 0>();
    // Blink "PRESS FIRE" on row 14 every 30 frames.
    const bool show = ((g_title_frames / 30) & 1) == 0;
    if (show) v.print(centre(10), kTitleRowPress, "PRESS FIRE");
    else      v.print(centre(10), kTitleRowPress, "          ");
    // Animate the whole title: rotate COLPF0 through the hue table every period. The VBI
    // copies this shadow to the hardware register, so all title text changes colour.
    Platform::hal::set_color_pf(0, kTitleHues[(g_title_frames / kTitleColorPeriod) % kTitleHueCount]);
#endif
    ++g_title_frames;
    return fire_edge(in);
}

static void play_enter() {
#ifdef EDGE_VBXE
    // Draw the whole play screen into the overlay master: HUD bar (row 0), the baked
    // room (rows 1..23), HUD labels/values, then publish once. Sprites composite over
    // this each frame; dynamic HUD/explosions redraw + republish on change.
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
#else
    auto& hud  = Game::region<PlayScreen, 0>();
    auto& play = Game::region<PlayScreen, 1>();
    fill_region(hud, 0x00);
    fill_region(play, 0x00);

    // Copy the baked room into the play area.
    for (u8 y = 0; y < 23; ++y)
        for (u8 x = 0; x < 40; ++x)
            play.put_char(x, y, kRoom.t[y][x]);

    // HUD: score label, level readout, and three life hearts (raw tile index 0x05).
    hud.print(0, 0, "SCORE: 00000");
    hud.print(kHudLevelCol, 0, "LV:01");
    hud.put_char(36, 0, 0x05);
    hud.put_char(37, 0, 0x05);
    hud.put_char(38, 0, 0x05);

    // Colour split for the play area (removed again when we leave the screen). g_play_bg
    // is black now; the death-flash raises it briefly mid-round.
    g_play_bg = 0x00;
    Game::interrupts.add_raster_hook(kSplitScanline, &play_palette_split);
#endif

    player_init();
    // Draw the player up front so it's visible during the GET READY pause (player_update
    // is what normally renders it, and that's blocked while the countdown runs).
    Game::sprite(0, player_shape, player.x, player.y);

    // Enemies: empty the pool, colour slots 1..8, arm the spawn clock, and reset the
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
#ifndef EDGE_VBXE
    play.print(kGetReadyCol, kGetReadyRow, "GET READY");   // overlay draws it in the branch above
#endif
    g_get_ready = kGetReadyFrames;

    arm_fire();
}

static bool play_step(const engine::Input& in) {
#ifndef EDGE_VBXE
    auto& play = Game::region<PlayScreen, 1>();
#endif

    // Death pause: lives just hit 0. Freeze every entity (skip all updates so sprites
    // hold their last-committed positions) while the room stays on screen, then signal
    // game-over when the beat expires. Checked first so we can't re-enter GET READY.
    if (g_death_timer > 0) {
#ifndef EDGE_VBXE
        g_play_bg = 0x00;                  // clear any lingering damage flash
#endif
        return (--g_death_timer == 0);     // transition only when the freeze ends
    }

    // GET READY countdown: hold before action starts. Blocks spawns/movement/fire; on
    // the final frame, erase "GET READY" back to the baked room tiles (cols 15..23).
    if (g_get_ready > 0) {
        if (--g_get_ready == 0) {
#ifdef EDGE_VBXE
            for (u16 c = kGetReadyCol; c < kGetReadyClearEnd; ++c)
                restore_room_cell(static_cast<u8>(c), kGetReadyRow);
            overlay_present_master();
#else
            for (u16 c = kGetReadyCol; c < kGetReadyClearEnd; ++c)
                play.put_char(static_cast<u8>(c), kGetReadyRow, kRoom.t[kGetReadyRow][c]);
#endif
        }
        return false;
    }

    player_update(in);

#ifndef EDGE_VBXE
    // Damage flash: white play-area background for the first 4 frames of invincibility.
    // The play DLI reads g_play_bg every frame, so the flash needs no extra timer.
    g_play_bg = (player.iframe >= kIFrames - 4) ? kFlashColor : 0x00;
#endif

    // Fire: launch a bullet in the last-moved direction (missile index == pool slot).
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
#ifdef EDGE_VBXE
            restore_room_cell(ex.col, ex.row);
            overlay_present_master();
#else
            play.put_char(ex.col, ex.row, kRoom.t[ex.row][ex.col]);
#endif
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

    // Missile -> enemy kills: an overlapping bullet destroys the enemy and is spent.
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

static void gameover_enter() {
#ifdef EDGE_VBXE
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
#else
    auto& v = Game::region<GameOverScreen, 0>();
    fill_region(v, 0x00);
    v.print(centre(9), 8, "GAME OVER");
    // Prominent banner when this round set a new record (positioned, not recoloured —
    // all text shares COLPF0; no '!' — the charset has no glyph for it).
    if (g_new_high) v.print(centre(14), 10, "NEW HIGH SCORE");
    // Final score and best, on their own rows. Labels "SCORE: " / "BEST:  " are 7 chars,
    // so the 5-digit number starts at +7; pad "BEST:" to 7 so the digits line up.
    v.print(centre(12), 12, "SCORE: 00000");
    v.print_num(static_cast<u8>(centre(12) + 7), 12, g_score, 5);
    v.print(centre(12), 13, "BEST:  00000");
    v.print_num(static_cast<u8>(centre(12) + 7), 13, g_high_score, 5);
    v.print(centre(10), 16, "PRESS FIRE");
#endif
    arm_fire();
}

static bool gameover_step(const engine::Input& in) {
    return fire_edge(in);
}

// ── Entry point ──────────────────────────────────────────────────────────

#ifdef EDGE_VBXE
// No-VBXE fallback: this build renders entirely through the VBXE overlay, so on a
// machine without VBXE there is nothing to show. Detect the board BEFORE bringing up
// the overlay; if absent, write a message on the OS GR.0 text screen (still up at
// program start, pointed to by SAVMSC $58/$59 — ANTIC internal codes match
// ascii_to_internal) and halt, instead of leaving a black/garbage screen.
static void require_vbxe_or_halt() {
    if (V::detect<VBXECfg>()) return;
    u8* scr = *reinterpret_cast<u8* volatile*>(0x58);   // SAVMSC -> GR.0 screen RAM
    const char msg[] = "VBXE REQUIRED";
    for (u8 i = 0; msg[i]; ++i) scr[i] = M::ascii_to_internal(msg[i]);
    for (;;) {}
}
#endif

int main() {
#ifdef EDGE_VBXE
    require_vbxe_or_halt();   // must run before Game::init() switches off the OS screen
#endif
    Game::init(arena_charset);   // charset unused on the VBXE overlay path, harmless there
#ifdef EDGE_VBXE
    // Tier 2: pure-overlay screens (ANTIC playfield off — no bus contention). Upload
    // the palette; all content (room, HUD, text) is drawn into the overlay master by
    // the per-screen *_enter callbacks. No ANTIC charset bind, no HUD/DLI palette.
    load_overlay_palette();
#else
    set_hud_palette();   // OS colour shadows; persists across frames/screens.

    // Normal-width player objects, so the 8px sprite spans 8 color clocks (= 2
    // Mode 4 cells), matching the kSpriteW/kCellW collision mapping.
    for (u8 p = 0; p < 4; ++p)
        Platform::hal::set_player_size(p, M::sizep::NORMAL);
#endif

    for (;;) {
        Game::set_screen<TitleScreen>(&title_enter);
        Game::run_until(title_step);
#ifndef EDGE_VBXE
        // The title animation left COLPF0 on some hue; restore white so the HUD score and
        // the game-over text (all COLPF0, no DLI) render readable.
        Platform::hal::set_color_pf(0, kTitleTextColor);
#endif

        Game::set_screen<PlayScreen>(&play_enter);
        Game::run_until(play_step);
#ifndef EDGE_VBXE
        Game::interrupts.remove_raster_hook(kSplitScanline);
#endif
        Game::sprite_hide(0);   // don't carry the player onto title / game-over
        // Likewise hide every live enemy and empty the pool for the next round.
        enemies.for_each_indexed(
            [](u8 idx, Enemy&) { Game::sprite_hide(static_cast<u8>(idx + 1)); });
        enemies.clear();
        // Retire bullets (hide their projectiles, next commit clears the strip) and
        // empty both pools. On the overlay the next play_enter redraws the whole room,
        // so explosion cells need no restore here.
        bullets.for_each_indexed([](u8 idx, Bullet&) { bullet_hide(idx); });
        bullets.clear();
#ifndef EDGE_VBXE
        explosions.for_each([](Explosion& ex) {
            Game::region<PlayScreen, 1>().put_char(ex.col, ex.row, kRoom.t[ex.row][ex.col]);
        });
#endif
        explosions.clear();

        Game::set_screen<GameOverScreen>(&gameover_enter);
        Game::run_until(gameover_step);
    }
}
