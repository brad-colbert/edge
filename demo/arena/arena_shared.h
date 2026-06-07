// demo/arena/arena_shared.h — Edge "Arena" demo, backend-agnostic core.
//
// Shared by both arena demo frontends:
//   demo/arena/arena_native.cpp  — native Atari ANTIC/GTIA build  (arena_base.xex)
//   demo/arena/arena_vbxe.cpp    — VBXE Tier-2 full-overlay build (arena_vbxe.xex)
//
// This header holds ONLY the code that is byte-identical between the two backends:
// the Mode-4 charset (the VBXE overlay plots glyphs straight from it too), the baked
// room, the entity pools + game state, the gameplay constants, the difficulty ramp,
// the sound tables, and the pure helpers (collision boxes, fire-edge tracking). It
// deliberately does NOT reference Platform/Game, rendering, or the play-area
// coordinate constants (kPlayLeftX/kPlayTopY/kCellW/kCellH) — those differ between
// backends and live in each frontend .cpp alongside its renderer.
//
// Each arena demo is a single translation unit (one .cpp -> one .xex; the two arena
// TUs are never linked together), so the data and mutable game state below use plain
// internal linkage (`static` / `constexpr`) rather than `inline`. This is deliberate:
// it matches the codegen of the original single-file demo so the BSS layout — and thus
// where the ANTIC screen buffer lands relative to the 4K scan-boundary — is unchanged.
// (External-linkage `inline` shifted .text enough to push the play region across $8000
// mid-row, corrupting the bottom row's right side. See display_list.h's 4K-boundary
// note.)
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
// The room/HUD tiles (0x00-0x05) are written by raw index, so they don't collide
// with the printable glyphs.

#ifndef EDGE_DEMO_ARENA_SHARED_H
#define EDGE_DEMO_ARENA_SHARED_H

#include <stdint.h>

#include <engine/core.h>
#include <engine/math.h>
#include <engine/pool.h>

using engine::u8;
using engine::u16;
using engine::i8;

// Cold path: screen setup + non-gameplay (title/game-over) callbacks. Keep these OUT
// of the hot, -O2-inlined main loop and compile them for minimum size. The per-frame
// path (play_step, engine frame_service) stays untouched at -O2 — only these one-shot /
// non-gameplay functions are size-optimized, so there's no gameplay-framerate impact.
#define EDGE_COLD [[gnu::noinline, clang::minsize]]

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
// 40 columns x 23 rows of tile indices for the play area. Bordered with wall (0x01);
// symmetric interior bars for cover; sparse floor dots for texture.

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

// Precomputed row*40 byte offsets into the flat room array. tile_at (defined per
// backend) is the play_step hot path (player + 3 enemies, four-corner can_move each
// frame), and kRoom.t[row][col] otherwise costs a 16-bit row*40 multiply on the 6502
// per call; this turns it into a table lookup + add.
struct RowBase { u16 o[23]; };
constexpr RowBase make_row_base() {
    RowBase rb{};
    for (u8 r = 0; r < 23; ++r) rb.o[r] = static_cast<u16>(r * 40);
    return rb;
}
constexpr RowBase kRowBase = make_row_base();

// ── Shared sprite / collision constants ───────────────────────────────────

static constexpr u8 kSpriteW   = 8;              // player sprite footprint (color clocks / scanlines)
static constexpr u8 kSpriteH   = 8;
static constexpr u8 kPlayerSpeed = 2;            // pixels moved per frame

// Wall tiles (from make_room): solid border/outer bars and the inner-bar variant.
static constexpr u8 kWallA  = 0x01;
static constexpr u8 kWallB  = 0x02;
static constexpr u8 kNoTile = 0xFF;              // sentinel: outside the play area

// A tile that blocks movement: either wall variant, or off the play area.
static bool is_blocked(u8 tile) {
    return tile == kWallA || tile == kWallB || tile == kNoTile;
}

// ── Player entity ──────────────────────────────────────────────────────────

struct Player {
    u8 x;       // sprite position, same coordinate system as Game::sprite() (raw HPOSP)
    u8 y;       // P/M strip offset (≈ display scanline from top)
    u8 dir;     // last movement direction: 0=up, 1=right, 2=down, 3=left
    u8 lives;
    u8 iframe;  // invincibility frames remaining
};
static Player player;

// ── Enemy entities ───────────────────────────────────────────────────────────

struct Enemy {
    u8 x;       // sprite coords, same system as the player
    u8 y;
    u8 period;  // frames between moves: LOWER = faster. Set per spawn (difficulty ramp).
    u8 timer;   // movement tick counter
};
static engine::SlotPool<Enemy, 3> enemies;

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

// An enemy sprite is kSpriteW px wide, and a play-area cell is only 4 px wide, so the
// sprite spans kSpriteCols = 2 columns (col..col+1). Its left column must stay one cell
// in from the right wall or the right half lands in the col-39 border — which is exactly
// what got enemies stuck on the right. (The sprite is 8 px tall = one cell, so rows need
// no such margin.) Spawn validity uses can_move(), the same four-corner footprint test
// movement uses, so a spawn can never start clipping a wall.
static constexpr u8 kSpriteCols  = kSpriteW / 4;                        // 2
static constexpr u8 kMaxSpawnCol = kInteriorMaxCol - (kSpriteCols - 1); // 37

// ── Combat: bullets, explosions ──────────────────────────────────────────────
//
// The bullet pool index maps to the hardware projectile / overlay sprite slot per
// backend. Enemies are pool indices 0..2 (logical sprite slots 1..3).

struct Bullet {
    u8 x, y;     // sprite coords, same system as the player
    i8 dx, dy;   // per-frame velocity
};
static engine::SlotPool<Bullet, 4> bullets;   // pool index == hardware missile / overlay slot

static constexpr u8  kBulletSpeed  = 4;       // pixels per frame
static constexpr u8  kBulletHeight = 4;       // missile-strip rows drawn
static constexpr u8  kBulletW      = 4;       // bullet collision box width
static constexpr u8  kIFrames      = 120;     // ~2 s invincibility after a hit
static constexpr u16 kScorePerKill = 10;

// Axis-aligned box overlap. Promotes to int for the +w/+h so u8 edges don't wrap.
static bool boxes_overlap(u8 ax, u8 ay, u8 aw, u8 ah,
                          u8 bx, u8 by, u8 bw, u8 bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

// Explosion: tile 0x04 stamped at a play-area cell, restored to the baked room tile
// when its timer runs out.
struct Explosion {
    u8 col, row;   // play-area character cell
    u8 timer;      // frames until the room tile is restored
};
static engine::PackedPool<Explosion, 8> explosions;
static constexpr u8 kExplosionFrames = 12;
static constexpr u8 kExplosionTile   = 0x04;

// ── Round / scoring / pacing state ──────────────────────────────────────────

static u16  g_score      = 0;     // current round score
static u16  g_high_score = 0;     // best score so far; persists across rounds
static bool g_new_high   = false; // this round beat the high score (game-over banner)
static u8   g_level      = 1;     // wave / difficulty indicator shown in the HUD
static u8   g_kills      = 0;     // kills this round; drives g_level
static u8   g_get_ready  = 0;     // "GET READY" countdown at play start (0 = playing)
static u8   g_death_timer= 0;     // freeze frames before the game-over screen (0 = alive)

static constexpr u8  kGetReadyFrames  = 90;    // ~1.5 s orientation pause at play start
static constexpr u8  kDeathPause      = 60;    // ~1 s frozen-room beat before game-over
static constexpr u8  kKillsPerLevel   = 8;     // kills between level (difficulty) ticks
// "GET READY" is 9 chars (the charset has no '!' glyph — '!' maps to the wall tile),
// centred at col 15 (cols 15..23), on row 9 so it clears the player at centre row 11.
static constexpr u8  kGetReadyRow     = 9;
static constexpr u8  kGetReadyCol     = 15;    // centre(9)
static constexpr u16 kGetReadyClearEnd= kGetReadyCol + 9; // 24 (one past the last glyph)

// Level/wave readout in the HUD gap between SCORE (cols 0..11) and the hearts
// (cols 36..38). Event-driven like the score — redrawn only when the level ticks.
static constexpr u8 kHudLevelCol = 18;   // "LV:" label; digits start at +3

// Title-screen text rows (shared layout; per-backend renderers position from these).
static constexpr u8 kTitleRowEdge  = 10;   // EDGE ARENA
static constexpr u8 kTitleRowPress = 14;   // PRESS FIRE (blinks)
static constexpr u8 kTitleRowInstr = 16;   // instruction line
static constexpr u8 kTitleRowBest  = 18;   // BEST: nnnnn

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

// ── Screen-flow helpers (fire-edge tracking) ────────────────────────────────

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

// Centre column for an n-character string on a 40-column line.
[[maybe_unused]] static constexpr u8 centre(u8 n) { return static_cast<u8>((40 - n) / 2); }

#endif // EDGE_DEMO_ARENA_SHARED_H
