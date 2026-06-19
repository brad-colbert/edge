// demo/tank/atari_tank_demo.cpp — EDGE tank demo, Stage 2: static four-chunk
// ANTIC Mode 4 playfield with a free validation camera.
//
// This stage builds ONLY the playfield: a full-screen 40x24 Mode 4 viewport onto
// an 80x48 logical tile map assembled from a 2x2 grid of 40x24 ATank-derived map
// chunks, stored in one 88x48 physical tile map (4 padding cells each side). A
// temporary joystick free-camera scrolls it for validation.
//
// NOT YET IMPLEMENTED (later stages): tank sprite, tank steering, fixed-point
// movement, camera following, PMG rendering, networking. There is intentionally
// no tank here.
//
// All hardware access is through the public EDGE API (engine::Core / Platform::hal).
// Geometry/placement/camera math live in playfield_geometry.h (shared with the
// host test); assets in playfield_assets.h (embedded, ATank-derived).

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

#include "playfield_assets.h"
#include "playfield_geometry.h"

using engine::u8;
using engine::u16;
using engine::i16;
namespace M = atari;
using G = tank::PlayfieldGeometry;

// ── Platform + game configuration ────────────────────────────────────────

using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::gfx::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC>;

// One full-screen Mode 4 scroll region over the 88x48 physical map. No HUD (a HUD
// would change the 24-row viewport height and invalidate the measured geometry).
struct PlayScreen {
    using display = engine::DisplayLayout<
        engine::ScrollRegion<engine::TextRegion<M::Mode::MODE_4, 24>,
                             G::physical_width, G::physical_height>>;
};

struct GameConfig {
    using screens = engine::ScreenSet<PlayScreen>;
    static constexpr u8 max_sprites    = 1;   // none drawn this stage; Core needs >= 1
    static constexpr u8 sound_channels = 1;
};

using Game = engine::Core<Platform, GameConfig>;

// ── The single writable physical tile map (RAM; ANTIC DMA-reads it) ───────
static tank::PhysicalMap g_map;

// ── Documented RAM state block (Altirra memory inspector: see the .map) ───
struct TankState {
    u16 camera_nominal_x;
    u16 camera_nominal_y;
    u16 scroll_color_clocks_x;
    u16 scroll_scanlines_y;
};
static volatile TankState g_tank_state = {};

// ── Free-camera state (nominal square pixels) ─────────────────────────────
//
// Optional deterministic start for headless screenshots (Altirra captures only
// the boot state). Define TANK_FORCE_CAMERA=<index> to pin the initial camera to
// a named validation position; default (interactive) opens on the centre.
struct CamPos { i16 nx, ny; };
[[maybe_unused]] static constexpr CamPos kValidationCameras[] = {
    {  0,   0},   // 0 top-left
    {320,   0},   // 1 top-right          (max X, top)
    {  0, 192},   // 2 bottom-left        (max Y, left)
    {320, 192},   // 3 bottom-right       (max X, max Y)
    {160,  96},   // 4 four-chunk centre / both seams crossed
    {160,   0},   // 5 horizontal chunk seam (vertical seam x=40 cells), at top
    {  0,  96},   // 6 vertical chunk seam   (horizontal seam y=24 cells), at left
    {  3,   0},   // 7 horizontal fine-scroll transition (scroll_x = 1)
    {  0,   3},   // 8 vertical fine-scroll transition   (scroll_y = 3)
    {320,  96},   // 9 maximum legal camera X
    {160, 192},   // 10 maximum legal camera Y
};

#ifdef TANK_FORCE_CAMERA
static i16 g_cam_nx = kValidationCameras[TANK_FORCE_CAMERA].nx;
static i16 g_cam_ny = kValidationCameras[TANK_FORCE_CAMERA].ny;
#else
static i16 g_cam_nx = 160;   // interactive default: centre
static i16 g_cam_ny = 96;
#endif

// Camera speed in nominal px/frame. 1 px/frame steps through fine scroll cleanly
// (scroll_x = nx>>1 advances every other frame; scroll_y = ny advances each frame).
static constexpr i16 kCamSpeed = 1;

// ── Per-frame logic ────────────────────────────────────────────────────────

static void frame_step(const engine::Input& in) {
    // Free camera (held directions; level, not edges).
    if (in.left())  g_cam_nx = static_cast<i16>(g_cam_nx - kCamSpeed);
    if (in.right()) g_cam_nx = static_cast<i16>(g_cam_nx + kCamSpeed);
    if (in.up())    g_cam_ny = static_cast<i16>(g_cam_ny - kCamSpeed);
    if (in.down())  g_cam_ny = static_cast<i16>(g_cam_ny + kCamSpeed);

    // Explicit clamp to the legal logical camera range (NOT ScrollManager's coarse
    // clamp), then convert nominal px -> EDGE scroll units.
    const u16 cam_nx = tank::clamp_camera_nominal_x(g_cam_nx);
    const u16 cam_ny = tank::clamp_camera_nominal_y(g_cam_ny);
    g_cam_nx = static_cast<i16>(cam_nx);   // write clamped value back
    g_cam_ny = static_cast<i16>(cam_ny);

    const u16 scroll_x = tank::scroll_color_clocks_x(cam_nx);
    const u16 scroll_y = tank::scroll_scanlines_y(cam_ny);
    Game::scroll.set(scroll_x, scroll_y);   // frame service applies it via apply_scroll

    g_tank_state.camera_nominal_x      = cam_nx;
    g_tank_state.camera_nominal_y      = cam_ny;
    g_tank_state.scroll_color_clocks_x = scroll_x;
    g_tank_state.scroll_scanlines_y    = scroll_y;
}

// ── Entry point ────────────────────────────────────────────────────────────

int main() {
    // Build the display list (one LMS per scroll line), load the tileset into
    // charset RAM + bind CHBASE, install the VBI service.
    Game::init(tank::tileset);

    // Mode 4 colour registers for the ATank palette (field map: 0-3 = COLPF0-3,
    // 4 = COLBK). Public HAL colour seam — no raw register writes.
    using P = tank::Palette;
    Platform::hal::set_color_pf(0, P::colpf0);
    Platform::hal::set_color_pf(1, P::colpf1);
    Platform::hal::set_color_pf(2, P::colpf2);
    Platform::hal::set_color_pf(3, P::colpf3);
    Platform::hal::set_color_pf(4, P::colbk);

    // 1) Initialize every physical cell (padding + logical) to the neutral code.
    tank::clear_physical_map(g_map);
    // 2) Copy each ROM-resident chunk directly into its final physical position.
    for (u8 cy = 0; cy < G::chunk_rows; ++cy)
        for (u8 cx = 0; cx < G::chunk_columns; ++cx)
            tank::copy_chunk_to_map(g_map, tank::chunk_payload(cx, cy), cx, cy);

    // 3) Bind the physical map as the scroll source (compile-time size check vs
    //    the ScrollRegion's 88x48).
    Game::scroll_map(g_map);

    Game::run(frame_step);
}
