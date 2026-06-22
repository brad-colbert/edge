// demo/atari_mode4_geometry_probe.cpp — ANTIC Mode 4 scroll + PMG geometry probe.
//
// Stage 1.1 hardware-validation target. NOT the tank demo. Its sole job is to
// MEASURE, on real silicon (Altirra), the geometry the planned tile-scrolling
// tank demo depends on, replacing the Stage 1 report's hypotheses with measured
// invariants:
//
//   * which physical map column is the first VISIBLE one at each HSCROL value
//     (i.e. the true horizontal fetch/display offset, hypothesised 4 left),
//   * the left/right physical padding actually required around an 80-cell world,
//   * correct behaviour for every horizontal fine-scroll value (0..3),
//   * correct behaviour for every vertical fine-scroll value (0..7) and the
//     coarse-row handoff, including the bottom mode line,
//   * the PMG X/Y origin + units for the FULL-SCREEN 40x24 Mode 4 layout.
//
// Display: ONE full 40x24 ANTIC Mode 4 scroll region over an 88x48 map. No HUD —
// a HUD would change the viewport height and the PMG vertical origin we measure.
// Diagnostics use a synthetic high-contrast map/tileset, a calibration player,
// and a documented RAM state block (g_probe_state) for Altirra memory probes.
//
// Everything goes through the public EDGE API (engine::Core / Platform::hal) —
// no pokes, no inline asm, no magic addresses. NO ATank assets are used here.

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

// Physical map geometry (HYPOTHESIS under test — see report).
//   logical world : 80 x 48 cells
//   left/right pad : 4 + 4   (provisional; 8 = scroll_margin(MODE_4))
//   physical map   : 88 x 48 cells, row stride 88
static constexpr u16 kMapW    = 88;
static constexpr u16 kMapH    = 48;
static constexpr u16 kLeftPad = 4;    // provisional; the probe MEASURES the real value
static constexpr u16 kLogW    = 80;
static constexpr u16 kLogH    = 48;

// One full-screen Mode 4 scroll region (40x24 visible) over the 88x48 map. No HUD.
struct ProbeScreen {
    using display = engine::DisplayLayout<
        engine::ScrollRegion<engine::TextRegion<M::Mode::MODE_4, 24>, kMapW, kMapH>>;
};

struct GameConfig {
    using screens = engine::ScreenSet<ProbeScreen>;
    static constexpr u8 max_sprites    = 1;
    static constexpr u8 sound_channels = 1;
};

using Game = engine::Core<Platform, GameConfig>;

// ── Synthetic Mode 4 tileset ──────────────────────────────────────────────
//
// Mode 4 glyph row byte = 4 pixels x 2 bits: 00=COLBK, 01=COLPF0, 10=COLPF1,
// 11=COLPF2 (code<0x80). High-contrast, deliberately un-terrain-like glyphs so a
// single-cell off-by-one is obvious in a screenshot.
enum : u8 {
    G_BLANK  = 0,   // background (stripe "off")
    G_STRIPE = 1,   // solid COLPF0 (stripe "on") — alternates every logical column
    G_VBAR   = 2,   // COLPF2 bar at the cell's LEFT edge — every 8th logical column
    G_HBAR   = 3,   // COLPF2 bar at the cell TOP — first/last visible rows
    G_CORNER = 4,   // COLPF2 box outline — the four logical-world corners
    G_CROSS  = 5,   // plus sign — the central four-chunk intersection
    G_PAD    = 6,   // busy checker — physical padding columns (must stay off-screen)
    G_SENT   = 7,   // COLPF2 double vertical bar — logical boundary columns 0/39/40/79
    G_COUNT  = 8,
};

static void put_glyph(engine::Charset1K& cs, u8 code, const u8 (&rows)[8]) {
    for (u8 i = 0; i < 8; ++i) cs.data[code * 8 + i] = rows[i];
}

static engine::Charset1K make_probe_tileset() {
    engine::Charset1K cs{};                       // zero-init => G_BLANK is all-0
    const u8 stripe[8] = {0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55};
    const u8 vbar[8]   = {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0};
    const u8 hbar[8]   = {0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00};
    const u8 corner[8] = {0xFF,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xFF};
    const u8 cross[8]  = {0x18,0x18,0x18,0xFF,0xFF,0x18,0x18,0x18};
    const u8 pad[8]    = {0x66,0x99,0x66,0x99,0x66,0x99,0x66,0x99};
    const u8 sent[8]   = {0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3};
    put_glyph(cs, G_STRIPE, stripe);
    put_glyph(cs, G_VBAR,   vbar);
    put_glyph(cs, G_HBAR,   hbar);
    put_glyph(cs, G_CORNER, corner);
    put_glyph(cs, G_CROSS,  cross);
    put_glyph(cs, G_PAD,    pad);
    put_glyph(cs, G_SENT,   sent);
    return cs;
}

static engine::Charset1K g_tileset = make_probe_tileset();

// ── Synthetic diagnostic map (RAM-resident; ANTIC reads it via DMA) ───────

static engine::TileMap<kMapW, kMapH> g_map;

// Pick the diagnostic tile code for physical cell (p, r). Precedence is chosen so
// every reference feature wins over the background stripe.
static u8 cell_code(u16 p, u16 r) {
    // Physical padding columns: a busy checker that must never appear on-screen.
    if (p < kLeftPad || p >= kLeftPad + kLogW) return G_PAD;

    const u16 lc = static_cast<u16>(p - kLeftPad);   // logical column 0..79
    const u16 lr = r;                                 // logical row    0..47

    const bool corner = (lc == 0 || lc == kLogW - 1) && (lr == 0 || lr == kLogH - 1);
    if (corner) return G_CORNER;

    // Central four-chunk intersection (logical 39/40 x 23/24).
    if ((lc == 39 || lc == 40) && (lr == 23 || lr == 24)) return G_CROSS;

    // Logical boundary columns (world edges + the chunk seam at 39/40).
    if (lc == 0 || lc == 39 || lc == 40 || lc == kLogW - 1) return G_SENT;

    // First / last visible logical rows.
    if (lr == 0 || lr == kLogH - 1) return G_HBAR;

    // Absolute column reference every 8 logical columns.
    if ((lc % 8) == 0) return G_VBAR;

    // Background: 1-cell-wide stripes so fine vs coarse stepping is visible.
    return (lc & 1) ? G_BLANK : G_STRIPE;
}

static void fill_map() {
    for (u16 r = 0; r < kMapH; ++r)
        for (u16 p = 0; p < kMapW; ++p)
            g_map.set_tile(p, r, cell_code(p, r));
}

// ── PMG calibration shape (8x8 box outline + 2x2 centre) ──────────────────
// Anchor = the sprite's top-left pixel (row 0, bit 7). Clear edges + centre let
// us read off exactly where the API (x, y) lands on the visible playfield.
constexpr auto g_calib = engine::make_sprite<8, 8>({
    0xFF, 0x81, 0x81, 0x99, 0x99, 0x81, 0x81, 0xFF,
});

// ── Documented RAM state block (read via Altirra memory inspector) ────────
// Find `g_probe_state` in atari_mode4_geometry_probe.map; the fields below are
// the live measured values for the current test case.
struct ProbeState {
    u8 mode;        // 0 = H scroll, 1 = V scroll, 2 = PMG
    u8 index;       // test-case index within the mode
    u8 scroll_x_lo, scroll_x_hi;
    u8 scroll_y_lo, scroll_y_hi;
    u8 coarse_col;  // Game::scroll.coarse_col()
    u8 coarse_row;  // Game::scroll.coarse_row()
    u8 hscrol;      // fine X actually written (inverted: see scroll.h)
    u8 vscrol;      // fine Y actually written
    u8 pmg_x;       // HPOSP used (PMG mode)
    u8 pmg_y;       // strip Y used (PMG mode)
};
static volatile ProbeState g_probe_state = {};

// ── Test-case tables ──────────────────────────────────────────────────────
//
// Horizontal: scroll_x is in COLOR CLOCKS (fine_scroll_range = 4 per cell). Covers
// each HSCROL phase, a coarse transition, the provisional logical max (160) and
// one unit beyond (161, safe: coarse clamps to 40, fetch stays inside the 88-map).
static constexpr u16 kHTests[] = {
    0, 1, 2, 3, 4, 5,            // HSCROL 0..3 across the first coarse step
    156, 157, 158, 159,          // approaching the right edge
    160,                         // provisional logical max (coarse 40, fine 0)
    161,                         // one beyond — over-scroll (must be safe + visible)
};
static constexpr u8 kHCount = sizeof(kHTests) / sizeof(kHTests[0]);

// Vertical: scroll_y is in SCANLINES (scanlines_per_line = 8 per cell). Covers
// each VSCROL phase, a coarse step, a mid transition, and the bottom edge + beyond.
static constexpr u16 kVTests[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8,   // VSCROL 0..7 then the first coarse step
    100,                         // mid-map fine transition reference
    184, 185, 186, 187, 188, 189, 190, 191,  // approaching the bottom (coarse 23, fine)
    192,                         // logical max (coarse 24, fine 0)
    193,                         // one beyond — over-scroll
};
static constexpr u8 kVCount = sizeof(kVTests) / sizeof(kVTests[0]);

// PMG anchor positions for the 40x24 (320x192 nominal px) viewport, using
// PROVISIONAL origins; the screenshots reveal the true visible corners so the
// real HPOS_LEFT / PM_TOP can be read off.
static constexpr u8 kHposLeft = 48;     // provisional HPOSP at visible X = 0
static constexpr u8 kPmTop    = 32;     // provisional strip Y at visible Y = 0
struct XY { u8 x, y; };
static constexpr XY kPmgTests[] = {
    {kHposLeft,            kPmTop},              // 0 top-left
    {kHposLeft + 80 - 4,   kPmTop},              // 1 top-centre
    {kHposLeft + 160 - 8,  kPmTop},              // 2 top-right
    {kHposLeft + 80 - 4,   kPmTop + 96 - 4},     // 3 centre
    {kHposLeft,            kPmTop + 192 - 8},     // 4 bottom-left
    {kHposLeft + 80 - 4,   kPmTop + 192 - 8},     // 5 bottom-centre
    {kHposLeft + 160 - 8,  kPmTop + 192 - 8},     // 6 bottom-right
};
static constexpr u8 kPmgCount = sizeof(kPmgTests) / sizeof(kPmgTests[0]);

// ── Mode / index state ────────────────────────────────────────────────────

// Optional compile-time initial state, for deterministic headless screenshots
// (Altirra captures only the boot state; there is no input injection). Define
// PROBE_FORCE_MODE / PROBE_FORCE_INDEX to pin the first-shown test case. Defaults
// (0/0 = horizontal scroll_x=0) leave the interactive probe unchanged.
#ifndef PROBE_FORCE_MODE
#define PROBE_FORCE_MODE 0
#endif
#ifndef PROBE_FORCE_INDEX
#define PROBE_FORCE_INDEX 0
#endif

static u8 g_mode  = PROBE_FORCE_MODE;     // 0 H, 1 V, 2 PMG
static u8 g_index = PROBE_FORCE_INDEX;

static u8 count_for_mode(u8 mode) {
    return mode == 0 ? kHCount : mode == 1 ? kVCount : kPmgCount;
}

// Edge helpers: the input snapshot exposes direction/console as LEVELS only, so we
// track the previous frame's levels here to derive presses (one step per press).
static bool g_prev_opt = false, g_prev_sel = false;
static bool g_prev_left = false, g_prev_right = false;

static void apply_state() {
    if (g_mode == 2) {
        // PMG calibration: map at origin, place the box at the selected anchor.
        Game::scroll.set(0, 0);
        const XY p = kPmgTests[g_index];
        Game::sprite(0, g_calib, p.x, p.y);
        Game::sprite_color(0, 0x0E);   // white box
        g_probe_state.pmg_x = p.x;
        g_probe_state.pmg_y = p.y;
    } else if (g_mode == 0) {
        Game::sprite_hide(0);
        Game::scroll.set(kHTests[g_index], 0);
        g_probe_state.pmg_x = 0; g_probe_state.pmg_y = 0;
    } else {
        Game::sprite_hide(0);
        Game::scroll.set(0, kVTests[g_index]);
        g_probe_state.pmg_x = 0; g_probe_state.pmg_y = 0;
    }

    const u16 sx = Game::scroll.x();
    const u16 sy = Game::scroll.y();
    g_probe_state.mode        = g_mode;
    g_probe_state.index       = g_index;
    g_probe_state.scroll_x_lo = static_cast<u8>(sx & 0xFF);
    g_probe_state.scroll_x_hi = static_cast<u8>(sx >> 8);
    g_probe_state.scroll_y_lo = static_cast<u8>(sy & 0xFF);
    g_probe_state.scroll_y_hi = static_cast<u8>(sy >> 8);
    g_probe_state.coarse_col  = static_cast<u8>(Game::scroll.coarse_col());
    g_probe_state.coarse_row  = static_cast<u8>(Game::scroll.coarse_row());
    // Fine values as the engine writes them (horizontal inverts; see scroll.h).
    const u8 rx = static_cast<u8>(sx % 4);
    g_probe_state.hscrol = rx ? static_cast<u8>(4 - rx) : 0;
    g_probe_state.vscrol = static_cast<u8>(sy % 8);
}

// ── Per-frame logic ────────────────────────────────────────────────────────

static void frame_step(const engine::Input& in) {
    const bool opt   = in.system_option();      // OPTION: next mode
    const bool sel   = in.system_secondary();   // SELECT: next index
    const bool left  = in.left();               // joystick left:  prev index
    const bool right = in.right();              // joystick right: next index

    if (opt && !g_prev_opt) {
        g_mode  = static_cast<u8>((g_mode + 1) % 3);
        g_index = 0;
    }
    const u8 n = count_for_mode(g_mode);
    if ((sel && !g_prev_sel) || (right && !g_prev_right)) {
        g_index = static_cast<u8>((g_index + 1) % n);
    }
    if (left && !g_prev_left) {
        g_index = static_cast<u8>((g_index + n - 1) % n);
    }

    g_prev_opt = opt; g_prev_sel = sel; g_prev_left = left; g_prev_right = right;

    apply_state();   // applied every frame so the screen always matches the state block
}

// ── Entry point ────────────────────────────────────────────────────────────

int main() {
    // init(tileset) builds the display list (one LMS per scroll line), loads the
    // synthetic tileset into charset RAM + binds CHBASE, and installs the VBI.
    Game::init(g_tileset);

    // Playfield colours (field map: 0-3 = COLPF0-3, 4 = COLBK).
    Platform::hal::set_color_pf(4, 0x90);   // COLBK  : dark blue background
    Platform::hal::set_color_pf(0, 0x0E);   // COLPF0 : white  (stripes)
    Platform::hal::set_color_pf(1, 0x46);   // COLPF1 : red    (checker accent)
    Platform::hal::set_color_pf(2, 0xC8);   // COLPF2 : green  (markers/corners)

    // Single-width calibration player.
    Platform::hal::set_player_size(0, M::sizep::NORMAL);

    // Fill the diagnostic map and bind it as the scroll source.
    fill_map();
    Game::scroll_map(g_map);

    // Start in the configured initial test case (defaults: horizontal, scroll 0).
    apply_state();

    Game::run(frame_step);
}
