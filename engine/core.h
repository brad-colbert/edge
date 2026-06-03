#ifndef ENGINE_CORE_H
#define ENGINE_CORE_H

// core.h — the integration layer that wires every subsystem into one user-facing
// API (ARCHITECTURE.md "Engine Subsystems", API_DESIGN.md north-star sketch).
//
// engine::Core<Platform, GameConfig> owns one instance of each subsystem and
// exposes the flat surface games use through a `using Game = engine::Core<...>`
// alias: Game::init(), Game::sprite(), Game::print(), Game::run(), the sub-object
// accessors (Game::sound, Game::scroll, Game::interrupts, Game::multiplex,
// Game::hooks), Game::sprite_collisions(), and the compile-time queries. Core is
// an all-static singleton — every subsystem is a `static inline` member and the
// convenience entry points are static — because the API is used unqualified by an
// instance (Game::init(), Game::sprite(...)).
//
// Core also defines frame_service(): the per-frame sequence the backend's frame
// interrupt runs (ARCHITECTURE.md "Data Flow Per Frame"). It is a non-capturing
// static function so its address installs as the frame handler via
// Platform::hal::install_frame_isr().
//
// Depends on the subsystem headers and the Platform template parameter only
// (Dependency Rule 2) — never a platform header by name.

#include "types.h"

#include "config/capabilities.h"
#include "display.h"
#include "gfx.h"
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

// gfx_region: GameConfig::bitmap_region if present, else void. Gives the bitmap
// subsystem (engine/gfx.h) its baseline software-view type; void = blitter-only.
template <typename C, typename = void>
struct gfx_region { using type = void; };
template <typename C>
struct gfx_region<C, void_t<typename C::bitmap_region>> {
    using type = typename C::bitmap_region;
};

// Scalar config fields with engine defaults when the game omits them.
template <typename C, typename = void>
struct max_raster_hooks { static constexpr u8 value = 12; };
template <typename C>
struct max_raster_hooks<C, void_t<decltype(C::max_raster_hooks)>> {
    static constexpr u8 value = C::max_raster_hooks;
};

template <typename C, typename = void>
struct max_frame_hooks { static constexpr u8 value = 4; };
template <typename C>
struct max_frame_hooks<C, void_t<decltype(C::max_frame_hooks)>> {
    static constexpr u8 value = C::max_frame_hooks;
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

// ── SpriteCollisionState ────────────────────────────────────────────────
//
// The four hardware collision banks, latched by frame_service() each frame. Each
// byte is a 4-bit mask (bit n set => collision with object n). Query methods name
// the pairing the way API_DESIGN.md "Collision Detection" presents it: sprites
// and projectiles against each other and against the background.
struct SpriteCollisionState {
    u8 s_bg[4]  = {};   // sprite     -> background
    u8 s_s[4]   = {};   // sprite     -> sprite
    u8 p_bg[4]  = {};   // projectile -> background
    u8 p_s[4]   = {};   // projectile -> sprite

    u8 sprite_to_background(u8 s)     const { return s_bg[s]; }
    u8 sprite_to_sprite(u8 s)         const { return s_s[s]; }
    u8 projectile_to_background(u8 p) const { return p_bg[p]; }
    u8 projectile_to_sprite(u8 p)     const { return p_s[p]; }
};

// ── Core ────────────────────────────────────────────────────────────────
template <typename Platform, typename GameConfig>
class Core {
public:
    using screens       = typename GameConfig::screens;
    using InitialScreen = typename cdetail::initial_screen<GameConfig, screens>::type;

    static constexpr u8  kPorts          = cdetail::ports<Platform>::value;
    static constexpr u8  kMaxRasterHooks = cdetail::max_raster_hooks<GameConfig>::value;
    static constexpr u8  kMaxFrameHooks  = cdetail::max_frame_hooks<GameConfig>::value;
    static constexpr u16 kPmBytes        = Platform::hal::sprite_area_bytes;
    static constexpr u16 kCharsetBytes   = 1024;   // largest charset (Charset1K)

    // Subsystem types.
    using Screen    = ScreenManager<Platform, GameConfig>;
    using Sprites   = SpriteManager<Platform, GameConfig::max_sprites>;
    using Sound     = SoundManager<Platform, GameConfig::sound_channels>;
    using Scroll    = ScrollManager<Platform>;
    using Tiles     = TileManager<Platform>;
    using Interrupts = InterruptManager<Platform, kMaxRasterHooks, kMaxFrameHooks>;
    using GfxRegion = typename cdetail::gfx_region<GameConfig>::type;
    using Gfx       = BitmapOps<Platform, GfxRegion>;

    // ── Subsystem instances (static singletons) ──
    static inline Screen     screen{};
    static inline Sprites    sprites{};
    static inline Sound      sound{};
    static inline Scroll     scroll{};
    static inline Tiles      tiles{};
    static inline Interrupts interrupts{};
    static inline Gfx        gfx_{};
    static inline InputState<kPorts> input{};
    static inline Hooks      hooks{};

    // Documented sub-object alias: the multiplex queries live on the sprite
    // manager (zone_count / sprites_on_player / player_for_sprite).
    static inline Sprites& multiplex = sprites;

    // Frame-ready flag set by frame_service(), polled by the loop (loop.h).
    static inline volatile bool frame_ready_ = false;

    // ── Initialisation ──
    //
    // With a charset: enable sprites, bring up the initial screen, load the
    // charset into the engine's char-set buffer and bind the backend's charset
    // base to it, then install the frame service. The no-charset form leaves the
    // charset base at its power-on default.
    template <typename Charset>
    static void init(const Charset& cs) {
        using caps = engine::caps_of_t<Platform>;
        if constexpr (caps::has_blitter) {
            // VBXE overlay bring-up (MEMAC window + full-screen XDL + enable).
            Platform::hal::overlay_init();
        } else {
            setup_sprites();
        }
        set_screen<InitialScreen>([] {});
        // The ANTIC charset buffer is only used by the baseline tile path; the
        // VBXE overlay-font upload to VRAM lands with the 4b text path.
        if constexpr (!caps::has_blitter) {
            tiles.init_charset(cs, charset_buffer_);
            tiles.bind_charset_page(page_of(charset_buffer_));
        }
        interrupts.arm_dispatch();
        Platform::hal::install_frame_isr(&frame_service);
    }
    static void init() {
        using caps = engine::caps_of_t<Platform>;
        if constexpr (caps::has_blitter) {
            Platform::hal::overlay_init();
        } else {
            setup_sprites();
        }
        set_screen<InitialScreen>([] {});
        interrupts.arm_dispatch();
        Platform::hal::install_frame_isr(&frame_service);
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
    static void sprite_color(u8 slot, u8 color) { sprites.sprite_color(slot, color); }

    // Sprite collision state latched at the last frame service.
    static const SpriteCollisionState& sprite_collisions() { return collisions_; }

    // ── Overlay (extended-graphics) frame controls ──
    //
    // Double-buffered overlays present + flip automatically each frame service (the
    // backend shows the page composed last frame, then composes the next); there is
    // no explicit flip() — the engine owns the cadence.
    //
    // Set the overlay's opaque background colour (the field sprites are composed
    // over and erased back to). 0 = transparent. No-op on platforms without an
    // overlay. Call after init().
    static void set_overlay_background(u8 color) {
        Platform::hal::overlay_set_background(color);
    }

    // ── Overlay text mode (VBXE Text_80; no-op on platforms without it) ──
    // A dedicated text surface separate from the baseline ANTIC TextRegion API.
    // Chars are raw font indices (no screen-code remap); `attr` is the cell colour
    // attribute (foreground index in b0-b6; b7=1 = opaque background). Call after
    // init(), on a Text_80 overlay config.
    static void overlay_text_font(const u8* glyphs, u16 bytes) {
        Platform::hal::overlay_text_font(glyphs, bytes);
    }
    static void overlay_text_clear(u8 ch, u8 attr) {
        Platform::hal::overlay_text_clear(ch, attr);
    }
    static void overlay_put_char(u8 col, u8 row, u8 ch, u8 attr) {
        Platform::hal::overlay_text_put(col, row, ch, attr);
    }
    static void overlay_print(u8 col, u8 row, const char* s, u8 attr) {
        Platform::hal::overlay_text_print(col, row, s, attr);
    }
    // Overlay collision latched at the last frame service (overlay vs PF/PMG, and
    // the blitter's overlay-vs-overlay code). Zero on platforms without overlays.
    static u8 overlay_collision()      { return overlay_collision_; }
    static u8 overlay_blit_collision() { return overlay_blit_collision_; }

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

    // ── Bitmap drawing ──
    // The bitmap canvas: the VBXE overlay framebuffer on blitter platforms, else a
    // BitmapRegion (GameConfig::bitmap_region) via its software view. Call after
    // init(); on baseline the view is bound to the screen buffer by set_screen().
    static Gfx& gfx() { return gfx_; }

    // ── Screen transition ──
    // Deactivate scroll on every transition; a scroll screen re-arms it via
    // scroll_map() once the game has bound its map buffer.
    template <typename S, typename Cb>
    static void set_screen(Cb cb) {
        scroll.deactivate();
        screen.template set_screen<S>(cb);
        // Baseline: bind the bitmap canvas to S's first bitmap region (if any), so
        // Game::gfx() draws into the live screen buffer. Blitter platforms draw the
        // VRAM overlay through the seams and ignore this.
        if constexpr (!engine::caps_of_t<Platform>::has_blitter) {
            using Layout = typename S::display;
            constexpr u8 bidx = Layout::bitmap_region_index();
            if constexpr (bidx < Layout::region_count)
                gfx_.attach(screen.buffer() + Layout::offset(bidx));
        }
    }

    // ── Scroll map binding ──
    //
    // Bind a game-held map (an engine::TileMap, or any value exposing `tiles[]`
    // and a `width` constant) as screen S's scroll source and start scrolling.
    // Call after init(). The map's width/height must match the screen's scroll
    // region (checked at compile time). The frame service then drives the fine
    // registers and the scroll-region load addresses from Game::scroll's position
    // every frame.
    template <typename Map, typename S = InitialScreen>
    static void scroll_map(Map& m) {
        using Layout = typename S::display;
        static constexpr u8 idx = Layout::scroll_region_index();
        static_assert(Layout::region_map_width[idx] == Map::width,
                      "scroll map width does not match the screen's scroll region");
        static_assert(Layout::region_map_height[idx] == Map::height,
                      "scroll map height does not match the screen's scroll region");
        screen.template bind_scroll_map<S>(scroll, &m.tiles[0], Map::width);
    }

    // ── Game loop forwarders (loop.h) ──
    template <typename Cb>
    [[noreturn]] static void run(Cb cb) { engine::run<Core>(cb); }
    template <typename Cb>
    static void run_until(Cb cb) { engine::run_until<Core>(cb); }
    static bool frame_overrun() { return engine::frame_overrun<Core>(); }

    // ── Per-frame service ──
    //
    // The backend's frame interrupt runs this once per frame in the
    // ARCHITECTURE.md "Data Flow Per Frame" order. Non-capturing so its address is
    // a plain void(*)() for install_frame_isr().
    static void frame_service() {
        using caps = engine::caps_of_t<Platform>;

        // 0. Suppress idle-dim (the backend would otherwise dim/cycle colours
        //    after a few minutes of no console/keyboard input).
        Platform::hal::suppress_idle_dim();

        // 1. Input capture.
        u8 joy[kPorts];
        for (u8 p = 0; p < kPorts; ++p) joy[p] = Platform::hal::read_joystick(p);
        input.update(joy, Platform::hal::read_keyboard());

        // 1b. Scroll: write the fine registers and repoint the scroll-region load
        //     addresses for the current viewport, and keep the tile viewport
        //     coherent. No-op unless a scroll map is bound (the screen owns the
        //     display-program bytes).
        screen.apply_scroll(scroll);
        tiles.set_viewport(scroll.x(), scroll.y());

        // 2. Sound envelopes.
        sound.tick();

        // 3. Commit buffered sprite state (pre-commit hook first). The path
        //    diverges by backend: VBXE composes the overlay via the blitter and
        //    has its own collision model; baseline writes P/M hardware sprites.
        if (hooks.pre_sprite_commit) hooks.pre_sprite_commit();

        // Commit buffered sprites — self-dispatching by backend.
        if constexpr (caps::has_blitter) {
            // VBXE: present first — show the page composed last frame (which had a
            // full frame's display time to finish) and flip so we now compose the
            // hidden page. Then commit this frame's blits (commit_blitter clears the
            // back page and queues a blit per active sprite) and start them async.
            // The blit is deliberately NOT waited on here: a synchronous wait makes
            // the VBI outrun a frame, letting the next VBI NMI re-enter this handler
            // (stack overflow). The present's wait is for LAST frame's blit, already
            // finished, so it returns at once. Single-buffer present is a no-op (it
            // composes the visible page in place, dirty-rect).
            Platform::hal::overlay_present();
            sprites.commit(pm_buffer_);
            Platform::hal::overlay_submit();

            // 4. Latch overlay collisions (overlay↔PF/PMG and overlay↔overlay).
            if constexpr (caps::has_overlay_collision) {
                overlay_collision_      = Platform::hal::overlay_collision();
                overlay_blit_collision_ = Platform::hal::overlay_blit_collision();
            }
        } else {
            // Baseline P/M path: write P/M memory, then latch + clear collisions.
            sprites.commit(pm_buffer_);
            for (u8 i = 0; i < 4; ++i) {
                collisions_.s_bg[i] = Platform::hal::coll_player_playfield(i);
                collisions_.s_s[i]  = Platform::hal::coll_player_player(i);
                collisions_.p_bg[i] = Platform::hal::coll_missile_playfield(i);
                collisions_.p_s[i]  = Platform::hal::coll_missile_player(i);
            }
            Platform::hal::clear_collisions();
        }

        // 5. Recompute multiplex zones for the next commit (harmless on VBXE,
        //    where no sprites are committed yet — the Y-sort over zero active
        //    sprites yields no zones).
        sprites.update_zones();

        // 6. Rebuild the raster-hook chain and arm per-line delivery. VBXE needs
        //    no zone-boundary DLIs (no hardware players to reposition), so the
        //    hook rebuild is baseline-only; prepare_chain still runs for both.
        if constexpr (!caps::has_blitter) {
            sprites.build_raster_hooks(interrupts);
        }
        interrupts.prepare_chain(screen.active_dl(), screen.active_dl_size());

        // 7. User frame hooks.
        interrupts.run_frame_hooks();

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
    static void setup_sprites() {
        Platform::hal::set_sprite_base(page_of(pm_buffer_));
        // Pass the sprite manager's vertical resolution so the HAL sets single-line
        // sprite DMA to match the layout sprites.commit() writes into pm_buffer_.
        Platform::hal::sprite_dma_enable(
            sprites.resolution() == SpriteVerticalResolution::SingleLine);
    }
    static u8 page_of(const void* p) {
        return static_cast<u8>(
            static_cast<u16>(reinterpret_cast<uintptr_t>(p)) >> 8);
    }

    // Hardware-sprite graphics memory. Single-line resolution wants 2K alignment
    // so the high byte alone selects the region (the sprite base register).
    alignas(2048) static inline u8 pm_buffer_[kPmBytes] = {};
    // Char-set destination (page-aligned so the charset base is just the high byte).
    alignas(256)  static inline u8 charset_buffer_[kCharsetBytes] = {};

    static inline SpriteCollisionState collisions_{};

    // ── Overlay frame state (neutral; meaningful only on extended-graphics
    //    platforms — a few bytes on baseline, never named with VBXE identity) ──
    static inline u8   overlay_collision_      = 0;
    static inline u8   overlay_blit_collision_ = 0;
};

} // namespace engine

#endif // ENGINE_CORE_H
