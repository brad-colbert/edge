# API Design

> **Applies to EDGE v0.1.0** — see [CHANGELOG](../CHANGELOG.md) for version history.

The user-facing API for the engine. This document describes what
a game author writes. Internal engine implementation details are
in ARCHITECTURE.md.

All code examples target llvm-mos C++ with the constraints
defined in CONSTRAINTS.md (no heap, no exceptions, no virtual
functions, compile-time configuration).

## North Star: A Complete Game

Before describing subsystems in isolation, here is a minimal
but complete game written against the engine API. This is the
reference for what "feels right" — if any subsystem API makes
this game harder to write, the API is wrong.

The game: "Survivors." Single screen. Player moves in 4
directions. Enemies spawn from edges, move toward player.
Player shoots in the direction they face. Score at top, three
lives. Exercises: P/M graphics, collision, sound, input,
display list, game state, game loop.

This sketch uses the single-screen shorthand for simplicity.
For games with multiple screen states, see "Screen Management."

```cpp
#include <engine/engine.h>

// ── Platform and Engine Configuration ───────────────

using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::Graphics::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC
>;

struct GameConfig {
    static constexpr auto gfx_mode       = atari::Mode::MODE_4;
    static constexpr uint8_t rows        = 24;
    static constexpr uint8_t columns     = 40;
    static constexpr uint8_t max_sprites = 9;
    static constexpr uint8_t max_missiles = 4;
    static constexpr uint8_t sound_channels = 2;
    static constexpr uint8_t user_zp_bytes  = 0;
};

using Game = engine::Core<Platform, GameConfig>;

// ── Assets (constexpr, ROM-resident) ────────────────

constexpr auto tileset = Game::make_charset({
    // tile definitions...
});

constexpr auto player_shape = Game::make_sprite<8, 16>({
    0b00111100,
    0b01111110,
    // ...
});

constexpr auto enemy_shape = Game::make_sprite<8, 8>({
    0b01100110,
    0b11111111,
    // ...
});

constexpr auto sfx_shoot = Game::make_sound({
    {engine::audio::Waveform::Tone, 120, 8, 4},
    {engine::audio::Waveform::Tone, 200, 4, 2},
});

constexpr auto sfx_explode = Game::make_sound({
    {engine::audio::Waveform::Noise, 40, 12, 6},
    {engine::audio::Waveform::Noise, 80, 6,  4},
});

// ── Game State (static, no heap) ────────────────────

struct Player {
    uint8_t x, y;
    uint8_t dir;
    uint8_t lives;
    uint8_t iframe;
};

struct Enemy {
    uint8_t x, y;
    uint8_t speed;
};

struct Bullet {
    uint8_t x, y;
    int8_t dx, dy;
};

static Player player;
static engine::SlotPool<Enemy, 8> enemies;
static engine::SlotPool<Bullet, 4> bullets;
static uint16_t score;
static uint8_t spawn_timer;

// ── Game Logic ──────────────────────────────────────

void init() {
    player = {120, 100, 0, 3, 0};
    enemies.clear();
    bullets.clear();
    score = 0;
    spawn_timer = 60;
}

void update_player(const engine::Input& input) {
    if (input.left())   { player.x -= 2; player.dir = 3; }
    if (input.right())  { player.x += 2; player.dir = 1; }
    if (input.up())     { player.y -= 2; player.dir = 0; }
    if (input.down())   { player.y += 2; player.dir = 2; }

    if (input.fire_pressed()) {
        if (auto* b = bullets.acquire()) {
            b->x = player.x;
            b->y = player.y;
            b->dx = engine::dir_x[player.dir] * 4;
            b->dy = engine::dir_y[player.dir] * 4;
            Game::sound.play(sfx_shoot, 0);
        }
    }

    if (player.iframe > 0) player.iframe--;
}

void update_enemies() {
    if (--spawn_timer == 0) {
        if (auto* e = enemies.acquire()) {
            e->x = /* random edge */;
            e->y = /* random edge */;
            e->speed = 1;
        }
        spawn_timer = 30;
    }

    enemies.for_each([](Enemy& e) {
        if (e.x < player.x) e.x += e.speed;
        if (e.x > player.x) e.x -= e.speed;
        if (e.y < player.y) e.y += e.speed;
        if (e.y > player.y) e.y -= e.speed;
    });
}

void update_bullets() {
    bullets.for_each_indexed([](uint8_t idx, Bullet& b) {
        b.x += b.dx;
        b.y += b.dy;
        if (b.x == 0 || b.x >= 255 || b.y < 16 || b.y >= 220) {
            bullets.release(idx);
        }
    });
}

void check_collisions() {
    auto collisions = Game::sprite_collisions();

    if (player.iframe == 0 && collisions.sprite_to_sprite(0)) {
        player.lives--;
        player.iframe = 120;
        Game::sound.play(sfx_explode, 1);
    }

    for (uint8_t m = 0; m < 4; m++) {
        if (auto hit = collisions.projectile_to_sprite(m)) {
            auto* e = Game::multiplex.resolve_player(hit);
            if (e) {
                enemies.release(e);
                bullets.release(m);
                score += 10;
                Game::sound.play(sfx_explode, 1);
            }
        }
    }
}

void render() {
    Game::sprite(0, player_shape, player.x, player.y);

    enemies.for_each_indexed([](uint8_t idx, Enemy& e) {
        Game::sprite(1 + idx, enemy_shape, e.x, e.y);
    });

    bullets.for_each_indexed([](uint8_t idx, Bullet& b) {
        Game::missile(idx, b.x, b.y, 4);
    });

    Game::print(0, 0, "SCORE:");
    Game::print_num(6, 0, score, 5);
    for (uint8_t i = 0; i < player.lives; i++) {
        Game::put_char(35 + i, 0, HEART_TILE);
    }
}

// ── Entry Point ─────────────────────────────────────

int main() {
    Game::init(tileset);
    init();

    Game::run([](const engine::Input& input) {
        update_player(input);
        update_enemies();
        update_bullets();
        check_collisions();
        render();
    });
}
```

## Configuration Reference

### Platform Type

The platform type selects the hardware target. Each axis is
independent (see CONSTRAINTS.md):

```cpp
using Platform = atari::Platform<
    atari::Machine::XL,       // 400, 800, XL, XE
    atari::RAM::Baseline,     // Baseline, XE128, Rambo256, U1MB
    atari::Graphics::Baseline, // Baseline, VBXE
    atari::Sound::Mono,       // Mono, Stereo, PokeyMax
    atari::TV::NTSC,          // NTSC, PAL
    atari::Network::None      // None, Fujinet (default: None)
>;
```

Common combinations are provided as aliases:

```cpp
using Platform = atari::StockXL_NTSC;   // XL, Baseline everything, NTSC
using Platform = atari::StockXL_PAL;    // XL, Baseline everything, PAL
using Platform = atari::ExpandedXE;     // XE, 128K, Baseline gfx/snd, NTSC
using Platform = atari::FullUpgrade;    // XL, U1MB, VBXE, PokeyMax, NTSC
```

### GameConfig — Canonical Structure

GameConfig declares all compile-time requirements. There are
two display configuration styles: single-screen shorthand and
multi-screen ScreenSet. They are mutually exclusive.

**Single-screen shorthand** (simple games):

```cpp
struct GameConfig {
    // ── Display (single screen) ──
    static constexpr auto gfx_mode = atari::Mode::MODE_4;
    static constexpr uint8_t rows    = 24;
    static constexpr uint8_t columns = 40;

    // ── Sprites ──
    static constexpr uint8_t max_sprites  = 9;
    static constexpr uint8_t max_missiles = 4;

    // ── Sound ──
    static constexpr uint8_t sound_channels = 2;

    // ── User Extension ──
    static constexpr uint8_t user_zp_bytes = 0;
    static constexpr uint16_t user_ram     = 0;
};
```

The single-screen shorthand is syntactic sugar. The engine
internally treats it as:

```cpp
using screens = engine::ScreenSet<
    engine::DefaultScreen<atari::Mode::MODE_4, 24, 40>
>;
```

One code path, not two.

**Multi-screen ScreenSet** (most games):

```cpp
struct TitleScreen {
    using display = engine::DisplayLayout<
        engine::BitmapRegion<atari::Mode::BITMAP_E, 192>
    >;
    static constexpr bool sprites_active = false;
    static constexpr bool scroll_active  = false;
    static constexpr bool use_row_table  = false;
};

struct GameplayScreen {
    using display = engine::DisplayLayout<
        engine::TextRegion<atari::Mode::MODE_2, 1>,
        engine::BitmapRegion<atari::Mode::BITMAP_E, 180>,
        engine::TextRegion<atari::Mode::MODE_2, 1>
    >;
    static constexpr bool sprites_active  = true;
    static constexpr auto pm_resolution   = SpriteVerticalResolution::SingleLine;
    static constexpr bool scroll_active   = true;
    static constexpr bool use_row_table   = true;
};

struct HighScoreScreen {
    using display = engine::DisplayLayout<
        engine::TextRegion<atari::Mode::MODE_2, 24>
    >;
    static constexpr bool sprites_active = false;
    static constexpr bool scroll_active  = false;
    static constexpr bool use_row_table  = false;
};

struct GameConfig {
    // ── Display (multi-screen) ──
    using screens = engine::ScreenSet<
        TitleScreen,
        GameplayScreen,
        HighScoreScreen
    >;
    using initial_screen = TitleScreen;

    // ── Sprites (global capacity) ──
    static constexpr uint8_t max_sprites  = 9;
    static constexpr uint8_t max_missiles = 4;

    // ── Sound ──
    static constexpr uint8_t sound_channels = 2;

    // ── User Extension ──
    static constexpr uint8_t user_zp_bytes = 0;
    static constexpr uint16_t user_ram     = 0;

    // ── Network (only if platform has network) ──
    static constexpr uint8_t net_max_players    = 4;
    static constexpr uint8_t net_send_buffer    = 32;  // bytes
    static constexpr uint8_t net_recv_buffer    = 32;  // bytes
    static constexpr uint8_t net_state_size     = 16;  // bytes per snapshot
    static constexpr uint8_t net_input_size     = 2;   // bytes per input msg
    static constexpr uint8_t net_send_interval  = 2;   // send every N frames
};
```

Note: sprite and sound capacity is global (determines RAM
allocation at compile time). Whether sprites are active on a
given screen is per-screen. The P/M graphics area is allocated
once based on max_sprites; individual screens enable or disable
it.

### Engine Core

```cpp
using Game = engine::Core<Platform, GameConfig>;
```

This single type alias triggers all static allocation. After
this line, the following are determined at compile time:

- Display list layouts for all screens
- Shared screen memory buffer (sized to largest screen)
- P/M graphics area location and size
- Sound channel allocation
- ZP allocation map
- raster-hook chain capacity
- Total RAM usage (queryable via `Game::ram_usage`)

## Display Configuration

### Single Mode (Simple Games)

For games that use one mode for the entire screen:

```cpp
struct GameConfig {
    // Character mode
    static constexpr auto gfx_mode = atari::Mode::MODE_4;
    static constexpr uint8_t rows    = 24;
    static constexpr uint8_t columns = 40;
};
```

Or bitmap:

```cpp
struct GameConfig {
    static constexpr auto gfx_mode    = atari::Mode::BITMAP_E;
    static constexpr uint8_t scanlines = 192;
};
```

When a single mode is specified, the engine provides either
text operations (`Game::print`, `Game::put_char`) or bitmap
operations (`Game::gfx.plot`, `Game::gfx.clear`) depending
on mode type. Using the wrong operation set is a compile error.

### Mixed Display Layout (Common Pattern)

For games with mixed modes (text status bar + bitmap playfield,
split-screen effects, etc.):

```cpp
struct GameplayScreen {
    using display = engine::DisplayLayout<
        engine::TextRegion<atari::Mode::MODE_2, 1>,
        engine::BitmapRegion<atari::Mode::BITMAP_E, 180>,
        engine::TextRegion<atari::Mode::MODE_2, 1>
    >;
};
```

Each region specifies its mode and its height (in text rows for
text regions, in scanlines for bitmap regions). The engine builds
the display program at set_screen time, allocates screen memory
for each region, and inserts the appropriate region boundaries.

Regions are accessed by index:

```cpp
auto& status = Game::region<0>();   // TextRegion
auto& field  = Game::region<1>();   // BitmapRegion
auto& footer = Game::region<2>();   // TextRegion
```

Each region exposes only the operations valid for its type.
TextRegion provides `print`, `put_char`, `print_num`.
BitmapRegion provides `plot`, `point`, `clear`, `hline`, `blit`.
Calling an invalid operation is a compile error.

### Custom Display List (Escape Hatch)

For advanced users who need full control of the ANTIC display
list (per-line mode switching, irregular layouts):

```cpp
struct GameConfig {
    static constexpr auto display = antic::custom_display_list;
};
```

With this option, the engine does not construct a display list.
The user builds it manually via platform HAL calls or inline
data. The engine's text and bitmap subsystems are unavailable
unless the user registers screen memory regions manually.

This is an escape hatch. Most games should use single mode or
DisplayLayout.

### Display RAM Costs

Approximate costs for full-screen modes:

```
Mode 2  (text, 40 cols):     40 × rows bytes
Mode 4  (text, 40 cols):     40 × rows bytes
Mode 6  (text, 20 cols):     20 × rows bytes
Mode 8  (bitmap, 40×24):     240 bytes
Mode D  (bitmap, 160×192):   7,680 bytes
Mode E  (bitmap, 160×192):   7,680 bytes
Mode F  (bitmap, 320×192):   7,680 bytes
```

Plus character set RAM for text modes (1024 bytes for modes
2/4, 512 bytes for modes 6/7).

The engine exposes total display RAM usage at compile time:

```cpp
static_assert(Game::display_ram_usage < 8192,
    "Display uses too much RAM");
```

## Screen Management

### Declaring Screens

Each screen is a struct with a `display` layout and per-screen
flags:

```cpp
struct TitleScreen {
    using display = engine::DisplayLayout<
        engine::BitmapRegion<atari::Mode::BITMAP_E, 192>
    >;
    static constexpr bool sprites_active = false;
    static constexpr bool scroll_active  = false;
    static constexpr bool use_row_table  = false;
};
```

All screens are collected in `ScreenSet` within GameConfig.

### Memory Sharing

All screens share a single screen memory buffer, sized to the
largest screen's requirements. Only one screen is active at a
time. This mirrors what experienced Atari developers do
manually — reuse screen RAM across game states. See
DECISIONS.md ADR-014 for rationale.

### Switching Screens

```cpp
Game::set_screen<GameplayScreen>();
```

This single call:

1. Waits for the frame service (safe transition point)
2. Installs new display list
3. Clears screen memory
4. Reconfigures raster-hook chain (removes old hooks, installs new
   engine hooks per screen config)
5. Enables/disables sprites and scroll per screen config
6. Updates CHBASE if character set differs

### Screen Transition Callback

```cpp
Game::set_screen<GameplayScreen>([]() {
    // Runs after display is configured but before
    // first frame renders
    Game::init_charset(gameplay_tileset);
    draw_initial_level();
    init_player();
});
```

### Raster Hook Lifecycle Across Screen Changes

Non-persistent user raster hooks are cleared on screen change. The
engine reinstalls its own raster hooks (multiplex, scroll) based on
the new screen's configuration.

```cpp
// Cleared on screen change (default)
Game::interrupts.add_raster_hook(scanline, handler);

// Survives screen changes (rare, for global effects)
Game::interrupts.add_persistent_raster_hook(scanline, handler);
```

### Game Flow with Multiple Screens

**Pattern A: Single loop with state machine**

```cpp
enum class State : uint8_t { TITLE, PLAYING, HIGHSCORE };
static State state = State::TITLE;

Game::run([](const engine::Input& input) {
    switch (state) {
        case State::TITLE:
            if (input.start_pressed()) {
                state = State::PLAYING;
                Game::set_screen<GameplayScreen>([]() {
                    init_game();
                });
            }
            break;

        case State::PLAYING:
            update_game(input);
            render_game();
            if (game_over) {
                state = State::HIGHSCORE;
                Game::set_screen<HighScoreScreen>([]() {
                    draw_scores();
                });
            }
            break;

        case State::HIGHSCORE:
            if (input.start_pressed()) {
                state = State::TITLE;
                Game::set_screen<TitleScreen>([]() {
                    draw_title();
                });
            }
            break;
    }
});
```

**Pattern B: Per-screen run loops (cleaner)**

```cpp
void title_loop() {
    Game::set_screen<TitleScreen>([]() { draw_title(); });
    Game::run_until([](const engine::Input& input) -> bool {
        animate_title();
        return input.start_pressed();
    });
}

void game_loop() {
    Game::set_screen<GameplayScreen>([]() { init_game(); });
    Game::run_until([](const engine::Input& input) -> bool {
        update_game(input);
        render_game();
        return game_over;
    });
}

void highscore_loop() {
    Game::set_screen<HighScoreScreen>([]() { draw_scores(); });
    Game::run_until([](const engine::Input& input) -> bool {
        animate_scores();
        return input.start_pressed();
    });
}

int main() {
    Game::init();
    while (true) {
        title_loop();
        game_loop();
        highscore_loop();
    }
}
```

Pattern B is recommended — each screen is self-contained and
game flow reads as a sequence.

## Static Pools

### SlotPool<T, N>

Fixed-size object pool with stable indices. Uses a bitmap for
occupancy tracking. N must be 1-8 (use `SlotPool16` for 9-16).

#### Construction and Reset

```cpp
engine::SlotPool<Enemy, 8> enemies;  // all slots free

enemies.clear();  // marks all slots free, does not zero data
```

#### Acquiring Slots

```cpp
// Basic acquire — returns pointer or nullptr if full
Enemy* e = enemies.acquire();

// Acquire with index — reports which slot was assigned
uint8_t slot;
Enemy* e = enemies.acquire(slot);
```

Acquired slots have **undefined content**. The caller must
initialize every field before use. This is intentional — see
DECISIONS.md ADR-002.

#### Releasing Slots

```cpp
// Release by pointer (natural after acquire)
enemies.release(e);

// Release by index (natural in collision resolution)
enemies.release(slot);
```

#### Queries

```cpp
enemies.active(3)    // is slot 3 occupied?
enemies.count()      // how many occupied?
enemies.available()  // how many free?
enemies.full()       // no free slots?
enemies.empty()      // all slots free?
enemies.capacity()   // compile-time N
```

#### Direct Access

```cpp
enemies[3]           // direct access, no bounds/active check
```

#### Iteration

Range-for visits occupied slots only:

```cpp
for (auto& e : enemies) {
    e.x += e.speed;
}
```

Lambda callbacks (may generate tighter code under llvm-mos):

```cpp
// Without index
enemies.for_each([](Enemy& e) {
    e.x += e.speed;
});

// With index (needed for sprite mapping, collision)
enemies.for_each_indexed([](uint8_t idx, Enemy& e) {
    Game::sprite(idx, enemy_shape, e.x, e.y);
});
```

#### Memory Cost

- Storage: `N * sizeof(T)` bytes
- Overhead: 1 byte (uint8_t bitmap)
- Shared: `bit_mask[8]` table (8 bytes ROM, amortized)

### PackedPool<T, N>

Dense-packed object pool. Every element from index 0 to
count-1 is active. Release swaps with last element.

**Key property: indices are unstable.** Do not use PackedPool
for anything with external references (sprite hardware mapping,
collision targets). Use for iteration-only collections
(particles, visual effects, queued events).

#### Construction and Reset

```cpp
engine::PackedPool<Particle, 16> particles;

particles.clear();  // sets count to 0
```

#### Acquiring and Releasing

```cpp
Particle* p = particles.acquire();  // returns &data[count++]

particles.release(p);     // swaps with last, decrements count
particles.release(idx);   // same, by index
```

**Warning:** after `release(idx)`, the element previously at
`count-1` is now at `idx`. Any index held externally is invalid.

#### Iteration

Raw pointer iteration — zero overhead:

```cpp
for (auto& p : particles) {
    p.x += p.dx;  // every element is active
}

particles.for_each([](Particle& p) {
    p.x += p.dx;
});
```

#### Memory Cost

- Storage: `N * sizeof(T)` bytes
- Overhead: 1 byte (uint8_t count)

### SoA Pattern (Manual, Not a Pool Type)

When performance-critical loops need struct-of-arrays layout,
use parallel arrays with a SlotPool bitmap for lifecycle:

```cpp
uint8_t enemy_active;  // bitmap
uint8_t enemy_x[8];
uint8_t enemy_y[8];
uint8_t enemy_speed[8];

for (uint8_t i = 0; i < 8; i++) {
    if (enemy_active & engine::bit_mask[i]) {
        enemy_x[i] += enemy_speed[i];
    }
}
```

This is documented as a pattern, not an abstraction. The engine
does not provide a SoA pool type because it would fight C++
ergonomics for marginal benefit at small pool sizes.

## Input

### Snapshot Model

Input is captured once per frame during the frame service. The game receives
an immutable snapshot. No polling, no race conditions.

```cpp
Game::run([](const engine::Input& input) {
    // input is a snapshot of this frame's state
});
```

### Directional Input

```cpp
input.up()       // true if joystick pushed up (level)
input.down()
input.left()
input.right()
```

These return `bool`. They reflect the current state (held down).

### Fire Button

```cpp
input.fire()          // true if fire button held (level)
input.fire_pressed()  // true only on the frame fire was pressed (edge)
input.fire_released() // true only on the frame fire was released (edge)
```

Edge detection is handled internally by comparing current frame
to previous frame. No user bookkeeping needed.

### Keyboard (if applicable)

```cpp
input.key()           // last key pressed (scancode), or 0
input.key_pressed()   // true if a new key was pressed this frame
```

Keyboard input is optional and capability-gated. On platforms
without a keyboard, these methods exist but return 0/false.

### Console Keys (Atari-specific)

```cpp
input.start()         // START key
input.select()        // SELECT key
input.option()        // OPTION key
```

These are exposed through the input snapshot even though they're
Atari-specific, because they map to equivalent concepts on other
platforms (pause, menu, option).

### Multi-Joystick

```cpp
// Default: joystick port 0
input.up()

// Explicit port (for two-player games)
input.up(1)           // joystick port 1
input.fire(1)
input.fire_pressed(1)
```

Port count is capability-gated. Querying a port beyond the
platform's capacity returns false (no crash, no UB).

### Memory Cost

- 2 bytes per joystick port (current + previous frame state)
- 1 byte for keyboard scancode
- Total: 5-9 bytes depending on port count

## Sprites

### Logical Sprite Model

The game works with logical sprites identified by slot index.
The engine maps logical sprites to hardware P/M resources:

```cpp
// Set logical sprite 0 to player_shape at position (x, y)
Game::sprite(0, player_shape, player.x, player.y);

// Set logical sprite 3 to enemy_shape
Game::sprite(3, enemy_shape, e.x, e.y);
```

`Game::sprite()` updates the logical sprite's state (position,
shape) in a buffer. It does NOT write to P/M hardware — that
happens during the frame service to avoid tearing. See DECISIONS.md ADR-009.

The engine resolves logical-to-hardware mapping:

- On baseline Atari: Players 0-3 mapped to P/M hardware,
  additional sprites multiplexed via raster hooks
- On VBXE: all sprites rendered via blitter
- Software fallback available on platforms without hardware
  sprites

### Sprite Shapes

Defined at compile time via `constexpr`:

```cpp
constexpr auto my_sprite = Game::make_sprite<Width, Height>({
    // binary bitmap data, one byte per row
    0b00111100,
    0b01111110,
    // ...
});
```

Width is in bits (8 for P/M single-width, 16 for double-width).
Height is in scanlines. Data is converted to hardware format at
compile time.

### Missiles

Missiles are separate from players on Atari (4 available,
2 pixels wide each):

```cpp
Game::missile(index, x, y, height);
```

Index 0-3. Missiles are NOT multiplexed in the initial
implementation (see DECISIONS.md ADR-025). The 4 hardware
missiles are available directly. On platforms without
separate missiles, these map to narrow sprites or software
rendering.

### Sprite Visibility

```cpp
Game::sprite_hide(3);     // hide logical sprite 3
Game::sprite_hide_all();  // hide all sprites
```

Hidden sprites are not rendered, not included in collision
detection, and not assigned to multiplexer zones.

### Sprite Vertical Resolution

Sprite vertical resolution is configured per-screen (see ADR-023):

```cpp
struct GameplayScreen {
    // ...
    static constexpr auto pm_resolution = SpriteVerticalResolution::SingleLine;
};
```

`SpriteVerticalResolution::SingleLine`: 1-scanline Y precision, 256 bytes per
player, 1792 bytes P/M RAM total. Most games use this.

`SpriteVerticalResolution::DoubleLine`: 2-scanline Y steps, 128 bytes per
player, 896 bytes P/M RAM total. Lower fidelity, saves 896
bytes.

P/M memory is allocated at the maximum size needed by any
screen. The screen manager sets the DMACTL resolution bit
during `set_screen`.

### Multiplexing

When `GameConfig::max_sprites` exceeds 4 (hardware player
count), the engine automatically multiplexes. The screen is
divided into vertical zones (default max 4 zones = up to 16
logical sprites). The game author does not manage zones.

**How it works:**

Each frame during the frame service, the multiplexer:

1. Sorts active sprites by Y (insertion sort, ~80-120 cycles)
2. Groups sprites into zones of up to 4, placing zone
   boundaries in the gaps between sprite groups
3. Assigns each sprite in a zone to a hardware player (0-3)
4. Registers a raw raster hook at each zone boundary that writes
   HPOSP0-3 for the new zone (~53 cycles per hook)
5. The game's render phase pre-writes all sprite shape data
   into P/M memory at the correct Y offsets — the hook only
   changes horizontal positions, not shape data

**Querying multiplex state:**

```cpp
// How many zones this frame?
u8 zones = Game::multiplex.zone_count();

// Which logical sprites could be on this hardware player?
// Returns bitmask — may have multiple bits set when multiplexing
u16 candidates = Game::multiplex.sprites_on_player(hw_player);

// Which hardware player is this logical sprite on (in its zone)?
u8 hw = Game::multiplex.player_for_sprite(logical_index);

// What zone is this logical sprite in?
u8 zone = Game::multiplex.zone_for_sprite(logical_index);
```

**Zone limits:**

MaxZones is a template parameter on the SpriteManager
(default 4). Each zone costs one raster-hook slot (~53 cycles) and
9 bytes of zone state. With 4 zones and 4 players per zone,
the engine supports up to 16 multiplexed sprites. If sprites
can't fit in the available zones (too many overlapping
vertically), the lowest-priority sprites are dropped.

### Collision Detection

GTIA provides hardware collision registers, latched once per
frame during the frame service.

**Non-multiplexed games (≤ 4 sprites):**

Collision is straightforward — each hardware player maps to
exactly one logical sprite:

```cpp
auto col = Game::sprite_collisions();

col.sprite_to_sprite(0)    // did Player 0 hit any player?
col.projectile_to_sprite(2)   // did Missile 2 hit any player?
col.sprite_to_background(0) // did Player 0 hit playfield?
```

**Multiplexed games (> 4 sprites):**

GTIA collision registers accumulate across the whole frame
and don't distinguish zones. If Player 0 shows Sprite A in
zone 0 and Sprite E in zone 1, a P0PL collision could be
either. The engine provides a candidates bitmask:

```cpp
auto col = Game::sprite_collisions();

if (col.sprite_to_sprite(0)) {
    // Which logical sprites were on Player 0?
    u16 candidates = Game::multiplex.sprites_on_player(0);

    // Check each candidate with bounding-box overlap
    for (u8 i = 0; i < max_sprites; i++) {
        if (candidates & (1 << i)) {
            if (bbox_overlap(target, sprites[i])) {
                // Confirmed collision
            }
        }
    }
}
```

This is honest — the engine tells you what's possible, the
game confirms with geometry. The bounding-box check is a few
comparisons per candidate, which is cheap.

### Sprite Commit (Internal)

During the frame service, the sprite commit phase writes buffered positions
to P/M hardware:

1. Clear dirty ranges of player memory (tracked-range clear,
   see ADR-022, ~1250 cycles vs ~5000 for full clear)
2. For each active logical sprite: copy shape data into the
   assigned player's memory at the sprite's Y offset
3. Write HPOSP0-3 for zone 0 (the frame service sets first zone positions)
4. Update min/max Y tracking for next frame's clear

### Memory Cost

```
Logical sprites:  MaxSprites × 6 bytes   (9 × 6 = 54)
Zone info:        MaxZones × 9 bytes      (4 × 9 = 36)
Zone state:       2 bytes
Dirty tracking:   8 bytes (min/max Y per player)
P/M memory:       1792 bytes (single-line)
                  or 896 bytes (double-line)
Collision state:  ~8 bytes

Total (excl. P/M): ~108 bytes for 9 sprites, 4 zones
P/M memory:        896-1792 bytes (the dominant cost)
```

## Bitmap Drawing

Available on BitmapRegion or single-mode bitmap screens. These
are the minimum drawing primitives. Additional primitives
(line, rect, vline, masked blit) will be added in future
iterations.

### Core Primitives

```cpp
// Set a single pixel
field.plot(x, y, color);

// Read a single pixel's color value
uint8_t c = field.point(x, y);

// Clear entire bitmap region to a single color
field.clear(color);

// Horizontal line (fast — operates on whole bytes where
// possible, only masks at endpoints)
field.hline(x1, x2, y, color);

// Blit a rectangular bitmap to the screen
// src is packed pixel data in the same format as the screen mode
field.blit(x, y, src, width, height);
```

### Coordinate Systems

Coordinates are in pixels, not bytes. The engine handles the
translation from pixel coordinates to byte address and bit
position. This translation depends on the ANTIC mode:

```
Mode E:  160 pixels wide, 2 bits per pixel, 4 pixels per byte
Mode F:  320 pixels wide, 1 bit per pixel, 8 pixels per byte
Mode D:  160 pixels wide, 2 bits per pixel, 4 pixels per byte
```

### Color Values

Color indices are mode-dependent:

```
1 bpp modes (F):     0-1
2 bpp modes (D, E):  0-3
4 bpp modes (A):     0-15
```

Color index maps to ANTIC playfield color registers (COLPF0-3)
or GTIA color interpretation depending on mode.

### Row Address Table (Opt-In)

Pixel addressing requires computing `y * bytes_per_line`,
which is expensive on a 6502. The engine can precompute a
row address lookup table:

```cpp
struct GameplayScreen {
    // ...
    static constexpr bool use_row_table = true;
};
```

When enabled, `plot()`, `point()`, and `blit()` use the table
for fast row addressing. When disabled, the engine uses
shift-and-add computation (slower, saves ROM).

Cost: 2 bytes per scanline of ROM (e.g., 192 scanlines = 384
bytes). For games that plot many pixels per frame, this is
worthwhile. For games that occasionally plot a few pixels, the
shift-and-add path is adequate.

### Software Sprites in Bitmap Mode

P/M graphics overlay the bitmap display and work identically
to character mode — the sprite API (`Game::sprite()`,
`Game::missile()`) is independent of the background mode.

For additional software sprites rendered into the bitmap
itself, use `field.blit()`:

```cpp
field.blit(enemy_x, enemy_y, enemy_bitmap, 8, 16);
```

This modifies screen memory directly. The game is responsible
for erasing the previous position (typically by redrawing the
background underneath). The engine does not manage software
sprite lifecycle — this is intentional, because software
sprite strategies vary wildly between games (dirty rect,
full redraw, double buffer, etc.).

## Sound

### Channel Model

Sound is managed by channel. The game requests channels in
GameConfig; the engine allocates them from POKEY voices:

```cpp
Game::sound.play(sfx_shoot, 0);    // channel 0
Game::sound.play(sfx_explode, 1);  // channel 1
```

A playing sound effect runs to completion unless interrupted
by another `play()` on the same channel.

### Sound Effect Data

Defined at compile time as a sequence of frames:

```cpp
constexpr auto sfx_shoot = Game::make_sound({
    // {waveform, frequency, volume, duration_frames}
    {engine::audio::Waveform::Tone,  120, 8, 4},
    {engine::audio::Waveform::Tone,  200, 4, 2},
    {engine::audio::Waveform::Silent, 0,  0, 0},  // terminal entry
});
```

Data lives in ROM. The sound subsystem advances one entry per
N frames during the frame service, writing frequency and volume to POKEY
registers.

### Waveform Types

```cpp
engine::audio::Waveform::Tone        // pure tone
engine::audio::Waveform::Noise       // noise
engine::audio::Waveform::Buzz        // buzzy distortion
engine::audio::Waveform::Silent      // silence (terminal marker)
```

These map to POKEY AUDC distortion bits. Additional waveform
types may be available on PokeyMax.

### Music (Future)

Music playback is a separate subsystem layered on top of sound
channels. Not in initial scope. When added, it will reserve
channels and the game can allocate remaining channels for
sound effects.

### Channel Release for User Assembly

```cpp
Game::sound.release_channel(3);   // engine stops managing ch 3
Game::sound.reclaim_channel(3);   // engine resumes management
```

### Stereo and Extended Sound

On stereo POKEY platforms, channel allocation is extended:

```cpp
// Channels 0-3: left POKEY
// Channels 4-7: right POKEY
Game::sound.play(sfx_shoot, 4);  // play on right channel 0
```

On PokeyMax, additional synthesis modes are available through
capability-gated API extensions (detailed in platform-specific
documentation).

### Memory Cost

- Per channel: 4 bytes (pointer to current sound entry, frame
  counter, current frequency, current volume)
- Total: `sound_channels * 4` bytes

## Network (Implementation Deferred)

Multiplayer networking via Fujinet. Uses a state-sharing model:
one machine is the authoritative host, clients send input and
receive state. The API is designed; implementation is deferred.

Network is capability-gated. Games compiled without
`atari::Network::Fujinet` (or equivalent) do not have
`Game::net` available — referencing it is a compile error.
Zero cost when unused.

### Game-Defined Messages

The game defines its own state and input message structs.
The engine sends raw bytes — it doesn't impose a format.
Messages must be plain data (no pointers, no padding-sensitive
layouts):

```cpp
// Input sent from client to host
struct NetInput {
    uint8_t joy;       // packed: up/down/left/right/fire
    uint8_t sequence;  // frame counter for ordering
};

// State sent from host to clients
struct NetState {
    uint8_t player_x[4];
    uint8_t player_y[4];
    uint8_t player_lives[4];
    uint8_t enemy_count;
    uint8_t score_hi;
    uint8_t score_lo;
    uint8_t frame;
};

static_assert(sizeof(NetState) <= GameConfig::net_state_size,
    "NetState exceeds declared net_state_size");
static_assert(sizeof(NetInput) <= GameConfig::net_input_size,
    "NetInput exceeds declared net_input_size");
```

### Connection Management

```cpp
// Host a game — listen on specified port
Game::net.host(port);

// Join a game — connect to host
Game::net.join(host_address, port);

// Disconnect
Game::net.disconnect();

// Connection state
Game::net.connected()        // bool: active connection?
Game::net.is_host()          // bool: are we the host?
Game::net.player_count()     // connected player count
Game::net.local_player_id()  // our player index (0-based)
```

### Network I/O

Network I/O is polled explicitly by the game. The engine does
not poll automatically — the game controls when the (expensive)
SIO transaction happens:

```cpp
// Poll network — processes incoming, flushes outgoing
// Call once per frame (or every N frames to save cycles)
Game::net.poll();
```

### Sending

```cpp
// Host: broadcast state to all clients
Game::net.broadcast_state(state);

// Client: send input to host
Game::net.send_input(input);
```

The engine handles framing, addressing, and delivery. The game
provides the raw struct. Messages are queued internally and
flushed on the next `poll()`.

### Receiving

```cpp
// Host: read client inputs
while (auto msg = Game::net.receive_input()) {
    uint8_t from = msg->player_id;
    apply_remote_input(from, msg->data);
}

// Client: read latest state from host
if (auto state = Game::net.receive_state()) {
    apply_state(state->data);
}
```

`receive_state` returns the most recent state snapshot,
discarding older ones if multiple arrived since last poll.
This is intentional — stale state is useless, only the
latest matters.

### Host Game Loop

```cpp
void game_loop_host() {
    Game::set_screen<GameplayScreen>([]() { init_game(); });

    Game::run_until([](const engine::Input& input) -> bool {
        Game::net.poll();

        // Apply local input
        update_player(Game::net.local_player_id(), input);

        // Apply remote inputs
        while (auto msg = Game::net.receive_input()) {
            apply_remote_input(msg->player_id, msg->data);
        }

        // Run simulation (host is authoritative)
        update_enemies();
        check_collisions();

        // Send state to clients (every N frames)
        if (frame % GameConfig::net_send_interval == 0) {
            NetState state = build_state_snapshot();
            Game::net.broadcast_state(state);
        }

        render();
        frame++;
        return game_over;
    });
}
```

### Client Game Loop

```cpp
void game_loop_client() {
    Game::set_screen<GameplayScreen>([]() { init_game(); });

    Game::run_until([](const engine::Input& input) -> bool {
        Game::net.poll();

        // Send our input to host
        NetInput my_input{pack_joystick(input), frame_counter};
        Game::net.send_input(my_input);

        // Receive and apply latest state
        if (auto state = Game::net.receive_state()) {
            apply_state(state->data);
        }

        // Client only renders — no simulation
        render();
        frame_counter++;
        return disconnected;
    });
}
```

### Lobby / Connection Screen

```cpp
void lobby() {
    Game::set_screen<LobbyScreen>([]() { draw_lobby(); });

    if (hosting) {
        Game::net.host(5000);
    } else {
        Game::net.join(host_addr, 5000);
    }

    Game::run_until([](const engine::Input& input) -> bool {
        Game::net.poll();
        draw_player_count(Game::net.player_count());
        return input.start_pressed()
            && Game::net.player_count() >= 2;
    });
}
```

### Network Resource Release

For user assembly that needs direct Fujinet/SIO access:

```cpp
Game::net.suspend();    // engine stops network polling
// user can issue raw SIO commands to Fujinet

Game::net.resume();     // engine resumes management
```

### Memory Cost

All network memory is statically allocated:

- Send buffer: `net_send_buffer` bytes
- Receive buffer: `net_recv_buffer` bytes
- Connection state: ~8 bytes (addresses, player IDs, flags)
- Per-player state: ~2 bytes × `net_max_players`
- Total: `net_send_buffer + net_recv_buffer + 8 + 2*max_players`

Example: 32-byte buffers, 4 players = 80 bytes total.

### Bandwidth Budget

Approximate, assuming Fujinet over UDP:

- SIO transaction overhead: ~1ms base + ~0.1ms per byte
- 16-byte state packet: ~2.6ms per send
- 2-byte input packet: ~1.2ms per send
- At send_interval=2 (every other frame): ~1.3ms average
  per frame for host, ~0.6ms average for client

These are rough estimates. Actual performance depends on
Fujinet firmware, WiFi conditions, and SIO bus contention.

## Tiles and Screen

### Region-Scoped Text Operations

In a mixed display, text operations are scoped to text regions:

```cpp
auto& status = Game::region<0>();  // TextRegion

status.print(0, 0, "SCORE:");
status.print_num(6, 0, score, 5);
status.put_char(35, 0, HEART_TILE);
```

In a single-mode text game, convenience methods are available
directly on Game:

```cpp
Game::print(0, 0, "SCORE:");
Game::put_char(35, 0, HEART_TILE);
```

These convenience methods are only available when the display
is a single text mode. In a mixed display, the compiler
requires explicit region scoping to avoid ambiguity.

### Tileset / Character Set

Character sets apply to text regions:

```cpp
constexpr auto tileset = Game::make_charset({
    // 128 characters, 8 bytes each
});

Game::init(tileset);
```

In a mixed display with multiple text regions, all text regions
share the same character set (ANTIC has a single CHBASE
register). To use different character sets in different regions,
install a raster hook at the region boundary:

```cpp
Game::interrupts.add_raster_hook(status_end_scanline, [](auto& ctx) {
    ctx.set_charset_base(alt_charset_page);
});
```

### Tilemap (Scrolling Text/Character Games)

```cpp
constexpr auto level_map = Game::make_map<128, 32>({
    // tile indices
});

Game::tiles.set_viewport(scroll_x, scroll_y);
```

Tilemap scrolling is not available on bitmap regions in the
initial implementation. Bitmap scrolling uses hardware scroll
registers directly via the Scroll subsystem.

## Scroll

### Fine and Coarse Scroll

On Atari, ANTIC provides hardware scrolling in both axes:

```cpp
// Set scroll position (engine handles fine/coarse split)
Game::scroll.set(x, y);

// Relative scroll
Game::scroll.move(dx, dy);
```

The engine internally manages:

- ANTIC HSCROL/VSCROL registers (fine scroll, 0-15 pixels)
- Display list LMS address updates (coarse scroll, full tile)
- Buffer wrapping when the viewport crosses a tile boundary
- New row/column loading from the tilemap

### Split-Screen Scroll

For games with a scrolling playfield and a fixed status bar,
use a DisplayLayout with separate regions. The engine generates
a raster hook at the split point automatically.

### Scroll Suspension

```cpp
Game::scroll.suspend();   // engine stops writing scroll registers
Game::scroll.resume();    // engine resumes management
```

## Interrupts and Hooks

The interrupt manager uses a two-tier raster-hook dispatch model.
Understanding the overhead of each tier helps you choose the
right approach. See DECISIONS.md ADR-019/020/021 for rationale.

### Raster Hook Handler Tiers

**Tier 1: C++ handlers (~80 cycle overhead)**

The engine wraps the handler with automatic register save/
restore (A, X, Y), chain-to-next-handler setup, and RTI.
The wrapper uses a self-modifying JSR to call the handler.
Suitable for color changes, simple register writes, and other
work on scanlines with generous cycle budgets.

```cpp
// Handler must be a non-capturing lambda or function pointer.
// Any data the handler needs goes in static variables.
static u8 sky_color = 0x94;

Game::interrupts.add_raster_hook(scanline, []() {
    *atari::reg::COLBK = sky_color;
});
```

Handlers are non-capturing lambdas only (see ADR-020). The
compiler converts them to plain function pointers at zero
cost. Capturing lambdas would require per-slot capture storage
and passing overhead, which is unacceptable in interrupt
context.

Approximate cycle budget for C++ handlers:

```
raster-hook budget (typical):    ~100 cycles
Engine overhead:          ~80 cycles
Available for handler:    ~20 cycles (3-4 register writes)
```

For wider mode lines or blank lines, budgets are more generous
and more work fits in a C++ handler.

**Tier 2: Raw handlers (zero engine overhead)**

User manages register save/restore, VDSLST chaining, and RTI.
Full cycle budget available. Required for timing-critical work.

```cpp
extern "C" void my_dli_handler();  // defined in .s file
Game::interrupts.add_raw_raster_hook(scanline, my_dli_handler);
```

The raw handler contract:

- Save any registers you modify (PHA, TXA/PHA, TYA/PHA)
- Do your work
- Load the next handler address and store it in VDSLST
  ($0200/$0201). The engine provides a helper:
  `Game::interrupts.next_raster_addr()` returns the address
  of the next handler in the chain.
- Restore registers
- Execute RTI

### Raster Context Object

For C++ handlers that need typed register writes without
including platform headers, a RasterContext is available:

```cpp
Game::interrupts.add_raster_hook(scanline, [](RasterContext& ctx) {
    ctx.set_playfield_color<0>(0x2A);
    ctx.set_playfield_color<1>(0x84);
});
```

Each write method compiles to a single `LDA #imm / STA addr`
pair. The RasterContext struct has zero storage — it's purely a
namespace for these operations:

```cpp
ctx.set_playfield_color<N>(value)   // playfield colour N (0..3), a template arg
ctx.set_background_color(value)     // background colour
ctx.set_charset_base(value)         // character set base
ctx.set_fine_scroll_x(value)        // horizontal fine scroll
ctx.set_fine_scroll_y(value)        // vertical fine scroll
```

Note: when using RasterContext, the handler signature takes
`RasterContext&` as a parameter. The engine provides a static
instance; the reference adds no runtime cost.

### Raster Hook Persistence Across Screen Changes

```cpp
// Cleared on screen change (default)
Game::interrupts.add_raster_hook(scanline, handler);

// Survives screen changes
Game::interrupts.add_persistent_raster_hook(scanline, handler);
```

### Raster Hook Priority

When multiple raster hooks land on the same scanline, the interrupt
manager chains them in priority order:

1. Engine multiplex (sprite register reprogramming)
2. Engine scroll (scroll register updates)
3. User raster hooks (in registration order)

Engine raster hooks are raw handlers internally — the engine knows
exactly which registers they touch and saves only those,
keeping overhead to ~20-30 cycles.

### Interrupt Manager Configuration

MaxRasterHooks and MaxFrameHooks are template parameters on the
InterruptManager, not GameConfig fields:

```cpp
// Engine default: 12 raster-hook slots, 4 frame hooks
// Override if your game needs more or fewer:
using MyInterrupts = engine::InterruptManager<Platform, 16, 2>;
```

Memory cost: `MaxRasterHooks * 8 + MaxFrameHooks * 2 + 44` bytes RAM
plus 2 bytes of zero page. For defaults (12 raster hooks, 4 frame hooks):
approximately 152 bytes RAM.

### Frame Hooks

```cpp
extern "C" void my_vbi_work();
Game::interrupts.add_frame_hook(my_vbi_work);
```

User frame hooks run after all engine frame-service housekeeping (input
capture, sound tick, sprite commit, collision latch, multiplex
zone computation, raster-hook chain build). The user gets whatever
cycles remain in the frame-service period.

### Render Phase Hooks

```cpp
Game::hooks.pre_sprite_commit(my_modifier);
Game::hooks.post_render(my_effect);
```

## Game Loop

### run (Never Returns)

```cpp
Game::run([](const engine::Input& input) {
    update(input);
    render();
});
```

The engine synchronizes to the frame service. Each callback invocation is
one frame. On NTSC: 60 FPS. On PAL: 50 FPS.

### run_until (Returns on Condition)

```cpp
Game::run_until([](const engine::Input& input) -> bool {
    update(input);
    render();
    return should_exit;  // true = exit loop
});
```

Same frame synchronization as `run`, but returns when the
callback returns true. Used for per-screen game loops
(see Screen Management).

### Init

```cpp
Game::init(tileset);  // or Game::init() if no charset needed
```

Performs all hardware setup: display program, sprites, sound, the
frame-service handler, and input capture. Call once before `run`
or `run_until`.

### Frame Overrun Detection

```cpp
Game::run([](const engine::Input& input) {
    if (Game::frame_overrun()) {
        update(input);      // skip rendering to catch up
    } else {
        update(input);
        render();
    }
});
```

## Math Utilities

### Direction Tables

```cpp
engine::dir_x[4]  // { 0, 1, 0, -1 } as int8_t
engine::dir_y[4]  // { -1, 0, 1, 0 } as int8_t
```

### Lookup Tables

```cpp
engine::sin8[256]    // 8-bit sine (0-255 maps to 0-360°)
engine::cos8[256]    // 8-bit cosine
```

### Fixed-Point Helpers

```cpp
using fixed88 = engine::Fixed<8, 8>;

fixed88 speed = fixed88::from_int(2);
fixed88 half  = fixed88::from_raw(0x0080);
fixed88 result = speed + half;

uint8_t integer_part = result.integer();
uint8_t frac_part    = result.fraction();
```

### Random Number Generator

```cpp
uint8_t r = engine::random();  // 8-bit PRNG (LFSR)
```

## Compile-Time Queries

```cpp
// Total RAM used by engine + game pools
static_assert(Game::ram_usage < 32768,
    "Game exceeds 32KB RAM budget");

// Display RAM for largest screen
static_assert(Game::max_display_ram < 8192,
    "Display uses too much RAM");

// Available ZP after engine allocation
static_assert(Game::zp_remaining >= 8,
    "Not enough ZP for user assembly");

// User ZP base address (available in C++ and assembly)
constexpr uint8_t my_zp = Game::user_zp_base;

// Platform capability checks
static_assert(Platform::capabilities::sound_voices >= 2,
    "Game requires at least 2 sound voices");
```

## Resource Release Summary

For user assembly integration, any managed resource can be
released:

```cpp
Game::sound.release_channel(n);    // sound channel
Game::sprites.release_player(n);   // P/M player
Game::scroll.suspend();            // scroll registers
Game::net.suspend();               // network (Fujinet/SIO)

Game::sound.reclaim_channel(n);
Game::sprites.reclaim_player(n);
Game::scroll.resume();
Game::net.resume();
```

See ARCHITECTURE.md "User Extension Points" for full contracts.

## Register Access

For direct hardware access, the engine provides typed constants:

```cpp
#include <engine/platform/atari/registers.h>

*atari::reg::COLPF0 = 0x2A;
*atari::reg::AUDF1  = 120;
uint8_t vcount = *atari::reg::VCOUNT;
```

These are `volatile uint8_t*` constants pointing to hardware
addresses. Using them bypasses the engine entirely. Consequences
are documented in ARCHITECTURE.md "Direct Hardware Access."
