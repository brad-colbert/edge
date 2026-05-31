#ifndef ENGINE_CORE_H
#define ENGINE_CORE_H

// core.h — the integration layer that wires every subsystem into one user-facing
// API (ARCHITECTURE.md "Engine Subsystems", API_DESIGN.md north-star sketch).
//
// engine::Core<Platform, GameConfig> owns one instance of each subsystem and
// exposes the flat surface games use through a `using Game = engine::Core<...>`
// alias: Game::init(), Game::sprite(), Game::print(), Game::run(), the sub-object
// accessors (Game::sound, Game::scroll, Game::interrupts, Game::multiplex,
// Game::hooks), Game::pm_collisions(), and the compile-time queries. Core is an
// all-static singleton — every subsystem is a `static inline` member and the
// convenience entry points are static — because the API is used unqualified by an
// instance (Game::init(), Game::sprite(...)).
//
// Core also defines vbi_service(): the per-frame sequence the deferred VBI runs
// (ARCHITECTURE.md "Data Flow Per Frame"). It is a non-capturing static function
// so its address installs as the VBI handler via Platform::hal::install_vbi().
//
// Depends on the subsystem headers and the Platform template parameter only
// (Dependency Rule 2) — never a platform header by name.

#include "types.h"

#include "display.h"
#include "hooks.h"
#include "input.h"
#include "interrupt.h"
#include "loop.h"
#include "screen.h"
#include "scroll.h"
#include "sound.h"
#include "sprites.h"
#include "tiles.h"

namespace engine {

namespace cdetail {

// Minimal detection idiom (no <type_traits> — CONSTRAINTS.md keeps the engine off
// the standard library where a tiny local helper does).
template <typename...> using void_t = void;

// initial_screen: GameConfig::initial_screen if present, else the first screen.
template <typename C, typename Screens, typename = void>
struct initial_screen { using type = typename Screens::template screen_at<0>; };
template <typename C, typename Screens>
struct initial_screen<C, Screens, void_t<typename C::initial_screen>> {
    using type = typename C::initial_screen;
};

// Scalar config fields with engine defaults when the game omits them.
template <typename C, typename = void>
struct max_dlis { static constexpr u8 value = 12; };
template <typename C>
struct max_dlis<C, void_t<decltype(C::max_dlis)>> {
    static constexpr u8 value = C::max_dlis;
};

template <typename C, typename = void>
struct max_vbi_hooks { static constexpr u8 value = 4; };
template <typename C>
struct max_vbi_hooks<C, void_t<decltype(C::max_vbi_hooks)>> {
    static constexpr u8 value = C::max_vbi_hooks;
};

template <typename C, typename = void>
struct user_zp_bytes { static constexpr u8 value = 0; };
template <typename C>
struct user_zp_bytes<C, void_t<decltype(C::user_zp_bytes)>> {
    static constexpr u8 value = C::user_zp_bytes;
};

// Joystick port count: Platform::capabilities::joystick_ports if present, else 2.
template <typename P, typename = void>
struct ports { static constexpr u8 value = 2; };
template <typename P>
struct ports<P, void_t<decltype(P::capabilities::joystick_ports)>> {
    static constexpr u8 value = P::capabilities::joystick_ports;
};

} // namespace cdetail

// ── PMCollisions ──────────────────────────────────────────────────────
//
// The four GTIA collision banks, latched by vbi_service() each frame. Each byte
// is a 4-bit mask (bit n set => collision with object n). Query methods name the
// pairing the way API_DESIGN.md "Collision Detection" presents it.
struct PMCollisions {
    u8 p_pf[4] = {};   // player  -> playfield
    u8 p_pl[4] = {};   // player  -> player
    u8 m_pf[4] = {};   // missile -> playfield
    u8 m_pl[4] = {};   // missile -> player

    u8 player_to_playfield(u8 p)  const { return p_pf[p]; }
    u8 player_to_player(u8 p)     const { return p_pl[p]; }
    u8 missile_to_playfield(u8 m) const { return m_pf[m]; }
    u8 missile_to_player(u8 m)    const { return m_pl[m]; }
};

// ── Core ────────────────────────────────────────────────────────────────
template <typename Platform, typename GameConfig>
class Core {
public:
    using screens       = typename GameConfig::screens;
    using InitialScreen = typename cdetail::initial_screen<GameConfig, screens>::type;

    static constexpr u8  kPorts       = cdetail::ports<Platform>::value;
    static constexpr u8  kMaxDLIs     = cdetail::max_dlis<GameConfig>::value;
    static constexpr u8  kMaxVBIHooks = cdetail::max_vbi_hooks<GameConfig>::value;
    static constexpr u16 kPmBytes     = Platform::hal::pm_area_bytes;
    static constexpr u16 kCharsetBytes = 1024;   // largest charset (Charset2)

    // Subsystem types.
    using Screen    = ScreenManager<Platform, GameConfig>;
    using Sprites   = SpriteManager<Platform, GameConfig::max_sprites>;
    using Sound     = SoundManager<Platform, GameConfig::sound_channels>;
    using Scroll    = ScrollManager<Platform>;
    using Tiles     = TileManager<Platform>;
    using Interrupts = InterruptManager<Platform, kMaxDLIs, kMaxVBIHooks>;

    // ── Subsystem instances (static singletons) ──
    static inline Screen     screen{};
    static inline Sprites    sprites{};
    static inline Sound      sound{};
    static inline Scroll     scroll{};
    static inline Tiles      tiles{};
    static inline Interrupts interrupts{};
    static inline InputState<kPorts> input{};
    static inline Hooks      hooks{};

    // Documented sub-object alias: the multiplex queries live on the sprite
    // manager (zone_count / sprites_on_player / player_for_sprite).
    static inline Sprites& multiplex = sprites;

    // Frame-ready flag set by vbi_service(), polled by the loop (loop.h).
    static inline volatile bool frame_ready_ = false;

    // ── Initialisation ──
    //
    // With a charset: enable P/M, bring up the initial screen, load the charset
    // into the engine's char-set buffer and point CHBASE at it, then install the
    // VBI service. The no-charset form leaves CHBASE at its power-on default.
    template <typename Charset>
    static void init(const Charset& cs) {
        setup_pm();
        set_screen<InitialScreen>([] {});
        tiles.init_charset(cs, charset_buffer_);
        tiles.set_chbase(page_of(charset_buffer_));
        interrupts.arm_dispatch();
        Platform::hal::install_vbi(&vbi_service);
    }
    static void init() {
        setup_pm();
        set_screen<InitialScreen>([] {});
        interrupts.arm_dispatch();
        Platform::hal::install_vbi(&vbi_service);
    }

    // ── Sprite / missile delegators ──
    template <typename Shape>
    static void sprite(u8 slot, const Shape& shape, u8 x, u8 y) {
        sprites.sprite(slot, shape, x, y);
    }
    static void missile(u8 index, u8 x, u8 y, u8 height) {
        sprites.missile(index, x, y, height);
    }
    static void sprite_hide(u8 slot) { sprites.sprite_hide(slot); }
    static void sprite_hide_all()    { sprites.sprite_hide_all(); }

    // P/M collision state latched at the last VBI.
    static const PMCollisions& pm_collisions() { return collisions_; }

    // ── Text delegators (single-screen shorthand: region 0) ──
    static void print(u8 col, u8 row, const char* s) {
        screen.template region<0>().print(col, row, s);
    }
    static void put_char(u8 col, u8 row, u8 tile) {
        screen.template region<0>().put_char(col, row, tile);
    }
    static void print_num(u8 col, u8 row, u16 value, u8 digits) {
        screen.template region<0>().print_num(col, row, value, digits);
    }

    // ── Region access ──
    template <u8 N>
    static auto& region() { return screen.template region<N>(); }
    template <typename S, u8 N>
    static auto& region() { return screen.template region<S, N>(); }

    // ── Screen transition ──
    template <typename S, typename Cb>
    static void set_screen(Cb cb) { screen.template set_screen<S>(cb); }

    // ── Game loop forwarders (loop.h) ──
    template <typename Cb>
    [[noreturn]] static void run(Cb cb) { engine::run<Core>(cb); }
    template <typename Cb>
    static void run_until(Cb cb) { engine::run_until<Core>(cb); }
    static bool frame_overrun() { return engine::frame_overrun<Core>(); }

    // ── Per-frame VBI service ──
    //
    // The deferred VBI runs this once per frame in the ARCHITECTURE.md
    // "Data Flow Per Frame" order. Non-capturing so its address is a plain
    // void(*)() for install_vbi().
    static void vbi_service() {
        // 0. Suppress attract mode (the OS would otherwise dim/cycle colours
        //    after a few minutes of no console/keyboard input).
        Platform::hal::suppress_attract();

        // 1. Input capture.
        u8 joy[kPorts];
        for (u8 p = 0; p < kPorts; ++p) joy[p] = Platform::hal::read_joystick(p);
        input.update(joy, Platform::hal::read_keyboard());

        // 2. Sound envelopes.
        sound.tick();

        // 3. Commit buffered sprite state (pre-commit hook first).
        if (hooks.pre_sprite_commit) hooks.pre_sprite_commit();
        sprites.commit(pm_buffer_);

        // 4. Latch this frame's collisions, then clear for the next.
        for (u8 i = 0; i < 4; ++i) {
            collisions_.p_pf[i] = Platform::hal::coll_player_playfield(i);
            collisions_.p_pl[i] = Platform::hal::coll_player_player(i);
            collisions_.m_pf[i] = Platform::hal::coll_missile_playfield(i);
            collisions_.m_pl[i] = Platform::hal::coll_missile_player(i);
        }
        Platform::hal::clear_collisions();

        // 5. Recompute multiplex zones for the next commit.
        sprites.update_zones();

        // 6. Rebuild the DLI chain (build_dli_handlers calls begin_dynamic()),
        //    then complete delivery: prepare_chain sets the DLI bits on the
        //    active display list and re-arms VDSLST/NMIEN for the frame.
        sprites.build_dli_handlers(interrupts);
        interrupts.prepare_chain(screen.active_dl(), screen.active_dl_size());

        // 7. User VBI hooks.
        interrupts.run_vbi_hooks();

        // 8. Release the loop's frame.
        frame_ready_ = true;
    }

    // ── Compile-time queries (API_DESIGN.md "Compile-Time Queries") ──
    //
    // Static-inline members do not contribute to sizeof(Core), so sum the
    // subsystem and buffer sizes explicitly.
    static constexpr u16 max_display_ram = Screen::buffer_size;
    static constexpr u16 ram_usage =
        static_cast<u16>(sizeof(Screen) + sizeof(Sprites) + sizeof(Sound) +
                         sizeof(Scroll) + sizeof(Tiles) + sizeof(Interrupts) +
                         sizeof(InputState<kPorts>) + kPmBytes + kCharsetBytes);

    // Zero-page accounting is approximate until a global ZP allocator exists:
    // the engine reserves a fixed slice at the base of the $80-$FF user page, the
    // game's own ZP follows, and the remainder is free. (Documented placeholder.)
    static constexpr u8 kEngineZpBytes = 8;
    static constexpr u8 user_zp_base =
        static_cast<u8>(0x80 + kEngineZpBytes);
    static constexpr u8 zp_remaining =
        static_cast<u8>(128 - kEngineZpBytes - cdetail::user_zp_bytes<GameConfig>::value);

private:
    static void setup_pm() {
        Platform::hal::set_pm_base(page_of(pm_buffer_));
        // Pass the sprite manager's P/M resolution so the HAL sets PM_SINGLE_LINE
        // in SDMCTL to match the layout sprites.commit() writes into pm_buffer_.
        Platform::hal::pm_dma_enable(sprites.resolution() == PMRes::SingleLine);
    }
    static u8 page_of(const void* p) {
        return static_cast<u8>(
            static_cast<u16>(reinterpret_cast<uintptr_t>(p)) >> 8);
    }

    // Player/Missile graphics memory. Single-line P/M wants 2K alignment so the
    // high byte alone selects the region (PMBASE).
    alignas(2048) static inline u8 pm_buffer_[kPmBytes] = {};
    // Char-set destination (page-aligned so CHBASE is just the high byte).
    alignas(256)  static inline u8 charset_buffer_[kCharsetBytes] = {};

    static inline PMCollisions collisions_{};
};

} // namespace engine

#endif // ENGINE_CORE_H
