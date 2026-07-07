// demo/arena/arena_native.cpp — Edge engine "Arena" demo (Berzerk-style), native
// Atari ANTIC/GTIA build (arena_base.xex; mos-atari8-dos target — see CMakeLists.txt).
//
// Renders with a custom ANTIC Mode 4 (4-colour text) charset, a baked room layout,
// hardware P/M players for the player + enemies, hardware missiles for bullets, and a
// HUD/play-area colour split driven by a DLI raster hook. The backend-agnostic charset,
// room, entity pools, gameplay constants, difficulty ramp, sound tables, and screen-flow
// helpers live in arena_shared.h, shared verbatim with the VBXE build (arena_vbxe.cpp).
//
//   Title    : "EDGE ARENA" centred + a blinking "PRESS FIRE".          (fire -> play)
//   Play     : a bordered brick room + a HUD row (score + 3 hearts) with a
//              DLI colour split between the HUD palette and the play palette. (fire -> over)
//   GameOver : "GAME OVER" + final score + "PRESS FIRE".                (fire -> title)

#include <engine/platform/atari/platform.h>

#include "arena_shared.h"

namespace M = atari;

// ── Platform + game configuration ────────────────────────────────────────

using Platform = atari::StockXL_NTSC;

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

struct GameConfig {
    using screens        = engine::ScreenSet<TitleScreen, PlayScreen, GameOverScreen>;
    using initial_screen = TitleScreen;
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
};

using Game = engine::Core<Platform, GameConfig>;

// ── Colours ───────────────────────────────────────────────────────────────

// Field map for set_color_pf: 0-3 = COLPF0-3, 4 = COLBK.
EDGE_COLD static void set_hud_palette() {
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
// `extern "C"` (C linkage) so the raw DLI below can read it by name; `volatile` because
// it is written by the game loop and read by the DLI (an NMI).
extern "C" volatile uint8_t g_play_bg = 0x00;

// Play-area palette split — a RAW display-list interrupt (no C++ dispatcher). The shared
// C++ raster dispatcher saves the llvm-mos $80-$9F imaginary registers (~448 cycles)
// before it calls a C++ hook; that delays the colour latch by ~1 row, tinting the play
// area's top-wall row with the HUD palette (a visible "bleed"). This hand-written handler
// writes the five colour registers directly — it saves only A/X/Y, NOT $80-$9F — so the
// swap latches inside the boundary's horizontal blank, with no bleed. It chains through
// the engine's raster-hook tail (edge_dli_op_curx2) exactly like the sprite multiplexer's
// raw zone DLIs: that tail re-points VDSLST from the chain, advances the walk index,
// restores A/X/Y, and RTIs. (COLBK $D01A, COLPF0-3 $D016-$D019 — the play palette that
// play_step / play_enter set up; g_play_bg carries the death flash.)
extern "C" {
[[gnu::naked]] void play_palette_split();
extern uint8_t edge_dli_op_curx2;   // engine raster-chain tail (platform/atari/dli_dispatch.h)
}
asm(R"(
    .globl play_palette_split
play_palette_split:
    pha
    txa
    pha
    tya
    pha
    lda g_play_bg
    sta $d01a              ; COLBK  : play background (death-flash aware)
    lda #$96
    sta $d016              ; COLPF0 : medium blue
    lda #$2a
    sta $d017              ; COLPF1 : orange
    lda #$24
    sta $d018              ; COLPF2 : brown
    lda #$c6
    sta $d019              ; COLPF3 : green
    jmp edge_dli_op_curx2
)");

// Boundary between region 0 (HUD) and region 1 (play): the display list opens
// with 3 dl_blank(8) (24 scanlines), then the 1-row HUD (8 scanlines), so the
// play area begins at scanline 24 + 1*8 = 32.
static constexpr u8 kSplitScanline = 32;

// Scanline that carries the play-area DLI. ANTIC raises the DLI on the LAST scanline
// of the mode line holding the DLI bit, and the new colours latch on the NEXT line.
// So to recolour starting at the play area's first row (kSplitScanline), the DLI must
// sit one Mode-4 row (kCellH=8 scanlines) earlier — on the HUD row. With the DLI on
// the play area's own first row instead, the swap latched one row late and the play
// area's top wall + first row rendered in the HUD palette (the colour "bleed").
// Kept separate from kSplitScanline so tuning the seam never moves the sprite/
// collision origin (kPlayTopY), which derives from kSplitScanline below. Tune on
// hardware if the seam is off.
static constexpr u8 kSplitDli = kSplitScanline - 8;

static constexpr u8 kFlashColor = 0x0E;  // white play-area flash on player damage (ANTIC COLBK)

// ── Small helpers ──────────────────────────────────────────────────────────

// Fill every cell of a text-region view with one tile (the view has no clear()).
template <typename View>
static void fill_region(View& v, u8 tile) {
    for (u16 i = 0; i < View::length; ++i) v.ptr[i] = tile;
}

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
static constexpr u8 kPlayLeftX = 48;             // HPOSP of play-area column 0
static constexpr u8 kCellW     = 4;              // Mode 4 cell width in color clocks
static constexpr u8 kPmYBias   = 8;              // P/M-vs-playfield vertical offset (scanlines, hardware-measured: one full Mode 4 row)
static constexpr u8 kPlayTopY  = kSplitScanline + kPmYBias; // collision row-0 origin (36)
static constexpr u8 kCellH     = 8;              // Mode 4 row height in scanlines

static constexpr u8 kPlayerColor = 0x0F;         // white — stands out over the play palette

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

// ── Player ───────────────────────────────────────────────────────────────────

// 8x8 humanoid silhouette: 1bpp P/M (each set bit is a player pixel).
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

// ── Enemies ──────────────────────────────────────────────────────────────────

// 8x8 blocky-robot silhouette — deliberately unlike the humanoid player. 1bpp P/M.
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

static constexpr u8 kEnemyColor = 0x34;   // red — contrasts the white player + play palette

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

// ── Bullets (hardware missiles, ADR-025) ──────────────────────────────────────

static void bullet_show(u8 idx, u8 x, u8 y) { Game::missile(idx, x, y, kBulletHeight); }
static void bullet_hide(u8 idx)             { Game::missile_hide(idx); }

// ── HUD + explosions (ANTIC text region) ───────────────────────────────────────

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
static constexpr u8 kTitleTextColor = 0x0E; // white — title default and HUD/game-over text

// Bright hues against the dark-blue (0x92) title background: white, cyan, green,
// yellow, orange, red. Advanced every kTitleColorPeriod frames by title_step.
static constexpr u8 kTitleHues[]      = {0x0E, 0x9A, 0xCA, 0x1A, 0x2A, 0x4A};
static constexpr u8 kTitleHueCount    = sizeof(kTitleHues);
static constexpr u8 kTitleColorPeriod = 8;   // frames between hue steps (slow cycle)

// ── Screen entry + per-frame callbacks ──────────────────────────────────────

EDGE_COLD static void title_enter() {
    auto& v = Game::region<TitleScreen, 0>();
    fill_region(v, 0x00);
    v.print(centre(10), kTitleRowEdge, "EDGE ARENA");
    v.print(centre(31), kTitleRowInstr, "JOYSTICK TO MOVE  FIRE TO SHOOT");
    // Show the best score below it once one exists (skip on first boot).
    if (g_high_score > 0) {
        v.print(centre(11), kTitleRowBest, "BEST: 00000");
        v.print_num(static_cast<u8>(centre(11) + 6), kTitleRowBest, g_high_score, 5);
    }
    g_title_frames = 0;
    arm_fire();
}

EDGE_COLD static bool title_step(const engine::Input& in) {
    auto& v = Game::region<TitleScreen, 0>();
    // Blink "PRESS FIRE" on row 14 every 30 frames.
    const bool show = ((g_title_frames / 30) & 1) == 0;
    if (show) v.print(centre(10), kTitleRowPress, "PRESS FIRE");
    else      v.print(centre(10), kTitleRowPress, "          ");
    // Animate the whole title: rotate COLPF0 through the hue table every period. The VBI
    // copies this shadow to the hardware register, so all title text changes colour.
    Platform::hal::set_color_pf(0, kTitleHues[(g_title_frames / kTitleColorPeriod) % kTitleHueCount]);
    ++g_title_frames;
#ifdef EDGE_ARENA_AUTOSTART
    // Headless perf harness (no keyboard/joystick injection available): auto-enter
    // PlayScreen after the title has shown briefly, so the heavy gameplay path runs
    // unattended and the HUD DROP counter can be read from a screenshot. Inert in
    // normal builds (define only for measurement).
    if (g_title_frames > 30) return true;
#endif
    return fire_edge(in);
}

EDGE_COLD static void play_enter() {
    auto& hud  = Game::region<PlayScreen, 0>();
    auto& play = Game::region<PlayScreen, 1>();
    fill_region(hud, 0x00);
    fill_region(play, 0x00);

    // Copy the baked room into the play area.
    for (u8 y = 0; y < 23; ++y)
        for (u8 x = 0; x < 40; ++x)
            play.put_char(x, y, kRoom.t[y][x]);

    // HUD: score label, level readout, and three life hearts (raw tile code 0x05).
    hud.print(0, 0, "SCORE: 00000");
    hud.print(kHudLevelCol, 0, "LV:01");
    // Frame-overrun diagnostic: dropped frames this round (engine counter). A
    // nonzero DROP means the per-frame gameplay path overran 60Hz — the signal
    // that told us -Os throttled this demo. play_step refreshes the digits live.
    hud.print(24, 0, "DROP");
    hud.put_char(36, 0, 0x05);
    hud.put_char(37, 0, 0x05);
    hud.put_char(38, 0, 0x05);

    // Colour split for the play area (removed again when we leave the screen). g_play_bg
    // is black now; the death-flash raises it briefly mid-round.
    g_play_bg = 0x00;
    Game::interrupts.add_raw_raster_hook(kSplitDli, &play_palette_split);

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
    play.print(kGetReadyCol, kGetReadyRow, "GET READY");
    g_get_ready = kGetReadyFrames;

    arm_fire();

#ifndef EDGE_ARENA_AUTOSTART
    // Zero the frame-overrun counter so the HUD DROP readout measures this round's
    // gameplay, not the one-shot screen bring-up above.
    Game::reset_frame_stats();
#endif
    // (Under the autostart perf harness the reset is skipped so DROP accumulates
    //  across every round, capturing the worst-case overrun of a whole run.)
}

static bool play_step(const engine::Input& in) {
    auto& play = Game::region<PlayScreen, 1>();

    // Live frame-overrun readout (engine counter): 4-digit dropped-frame count in
    // the HUD, refreshed every frame. Stays 0 while -Os keeps up; climbs if not.
    Game::region<PlayScreen, 0>().print_num(29, 0, Game::frames_dropped(), 4);

    // Death pause: lives just hit 0. Freeze every entity (skip all updates so sprites
    // hold their last-committed positions) while the room stays on screen, then signal
    // game-over when the beat expires. Checked first so we can't re-enter GET READY.
    if (g_death_timer > 0) {
        g_play_bg = 0x00;                  // clear any lingering damage flash
        return (--g_death_timer == 0);     // transition only when the freeze ends
    }

    // GET READY countdown: hold before action starts. Blocks spawns/movement/fire; on
    // the final frame, erase "GET READY" back to the baked room tiles (cols 15..23).
    if (g_get_ready > 0) {
        if (--g_get_ready == 0) {
            for (u16 c = kGetReadyCol; c < kGetReadyClearEnd; ++c)
                play.put_char(static_cast<u8>(c), kGetReadyRow, kRoom.t[kGetReadyRow][c]);
        }
        return false;
    }

    player_update(in);

    // Damage flash: white play-area background for the first 4 frames of invincibility.
    // The play DLI reads g_play_bg every frame, so the flash needs no extra timer.
    g_play_bg = (player.iframe >= kIFrames - 4) ? kFlashColor : 0x00;

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
            play.put_char(ex.col, ex.row, kRoom.t[ex.row][ex.col]);
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
            player.iframe = kIFrames;
#ifndef EDGE_ARENA_AUTOSTART
            player.lives  = static_cast<u8>(player.lives - 1);
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
#else
            // Autostart perf harness: the player is immortal, so a round runs forever
            // with the full enemy complement chasing — a sustained worst-case 4-sprite
            // same-band load for reading the frame-overrun (DROP) counter unattended.
            (void)0;
#endif
        }
    }

    return false;
}

EDGE_COLD static void gameover_enter() {
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
    arm_fire();
}

EDGE_COLD static bool gameover_step(const engine::Input& in) {
    return fire_edge(in);
}

// ── Entry point ──────────────────────────────────────────────────────────

int main() {
    Game::init(arena_charset);
    set_hud_palette();   // OS colour shadows; persists across frames/screens.

    // Normal-width player objects, so the 8px sprite spans 8 color clocks (= 2
    // Mode 4 cells), matching the kSpriteW/kCellW collision mapping.
    for (u8 p = 0; p < 4; ++p)
        Platform::hal::set_player_size(p, M::sizep::NORMAL);

    for (;;) {
        Game::set_screen<TitleScreen>(&title_enter);
        Game::run_until(title_step);
        // The title animation left COLPF0 on some hue; restore white so the HUD score and
        // the game-over text (all COLPF0, no DLI) render readable.
        Platform::hal::set_color_pf(0, kTitleTextColor);

        Game::set_screen<PlayScreen>(&play_enter);
        Game::run_until(play_step);
        Game::interrupts.remove_raster_hook(kSplitDli);
        Game::sprite_hide(0);   // don't carry the player onto title / game-over
        // Likewise hide every live enemy and empty the pool for the next round.
        enemies.for_each_indexed(
            [](u8 idx, Enemy&) { Game::sprite_hide(static_cast<u8>(idx + 1)); });
        enemies.clear();
        // Retire bullets (hide their projectiles, next commit clears the strip) and
        // empty both pools.
        bullets.for_each_indexed([](u8 idx, Bullet&) { bullet_hide(idx); });
        bullets.clear();
        explosions.for_each([](Explosion& ex) {
            Game::region<PlayScreen, 1>().put_char(ex.col, ex.row, kRoom.t[ex.row][ex.col]);
        });
        explosions.clear();

        Game::set_screen<GameOverScreen>(&gameover_enter);
        Game::run_until(gameover_step);
    }
}
