#ifndef ENGINE_PLATFORM_ATARI_PLATFORM_H
#define ENGINE_PLATFORM_ATARI_PLATFORM_H

// platform/atari/platform.h — the Atari platform type.
//
// A platform composes a machine identity with independent hardware extension
// axes (CONSTRAINTS.md "Platform Configuration Model", DECISIONS.md ADR-005).
// Each axis is an enum; any combination is valid. The Platform type carries
// the capability profile and the HAL, both resolved at compile time:
//
//     using P = atari::Platform<atari::Machine::XL, atari::RAM::Rambo256,
//                               atari::gfx::Baseline, atari::Sound::Stereo>;
//     using caps = P::capabilities;          // queried via if constexpr
//
// Capabilities are composed by starting from baseline Atari values and folding
// in axis-specific overrides with constexpr expressions on the axis enums. The
// Graphics axis is a *type* (gfx::Baseline / gfx::VBXE<Config>) rather than an
// enum, so the VBXE variant can carry a compile-time configuration payload.

#include "../../config/capabilities.h"
#include "../../types.h"
#include "display_list.h"
#include "display_traits.h"
#include "hal.h"
#include "vbxe_config.h"
#include "vbxe_overlay.h"

namespace atari {

using engine::u8;
using engine::u16;
using engine::u32;

// ── Independent configuration axes ───────────────────────────────────

enum class Machine  : u8 { A400, A800, XL, XE };
enum class RAM      : u8 { Baseline, XE128, Rambo256, U1MB };
enum class Sound    : u8 { Mono, Stereo, PokeyMax };
enum class Network  : u8 { None, Fujinet };
enum class TV       : u8 { NTSC, PAL };

// ── Graphics axis (a type, not an enum) ──────────────────────────────
//
// Unlike the other axes, Graphics is a type so the VBXE variant can carry a
// compile-time configuration (vbxe::Config). Baseline is stock ANTIC+GTIA.
namespace gfx {
    struct Baseline {};                                  // stock ANTIC+GTIA
    template <typename Config = vbxe::DefaultConfig>
    struct VBXE { using config = Config; };
}

namespace detail {

// is_vbxe<Gfx>::value — true iff the Graphics axis is a gfx::VBXE<...>.
// Uses the engine's `static constexpr bool value` trait idiom (cf.
// detail::same in screen.h); no <type_traits> dependency.
template <typename G> struct is_vbxe { static constexpr bool value = false; };
template <typename C> struct is_vbxe<gfx::VBXE<C>> { static constexpr bool value = true; };

// overlay_hal_for<Gfx>::type — the backend overlay HAL seam the Platform folds
// into Platform::hal. VBXE supplies OverlayHal<Config>; everything else gets the
// no-op NullOverlay (both defined in vbxe_overlay.h). This keeps every VBXE type
// in the backend: the generic engine only ever calls the neutral overlay_* seams.
template <typename G> struct overlay_hal_for            { using type = NullOverlay; };
template <typename C> struct overlay_hal_for<gfx::VBXE<C>> { using type = OverlayHal<C>; };

// Extended (bank-switched) RAM total for each memory axis, in bytes.
constexpr u32 ext_ram_bytes(RAM r) {
    switch (r) {
        case RAM::XE128:    return 65536u;     // 130XE: +64K
        case RAM::Rambo256: return 262144u;    // Rambo: +256K
        case RAM::U1MB:     return 1048576u;   // Ultimate 1MB
        case RAM::Baseline: return 0u;
    }
    return 0u;
}

// POKEY voice count for each sound axis.
constexpr u8 voice_count(Sound s) {
    switch (s) {
        case Sound::Stereo:   return 8;   // dual POKEY, 4+4
        case Sound::PokeyMax: return 8;   // FPGA POKEY (stereo-class base)
        case Sound::Mono:     return 4;   // single POKEY
    }
    return 4;
}

// Timing constants per TV standard (CONSTRAINTS.md "TV Standard", ADR-018).
// PAL and NTSC builds are separate binaries with different timing budgets.
constexpr u32 tv_cpu_frequency(TV tv) {        // Hz
    return (tv == TV::PAL) ? 1773447u : 1789773u;
}
constexpr u8 tv_frames_per_second(TV tv) {
    return (tv == TV::PAL) ? 50 : 60;
}
constexpr u16 tv_cycles_per_frame(TV tv) {
    return (tv == TV::PAL) ? 35280 : 29780;
}

} // namespace detail

// ── Composed Atari capability profile ────────────────────────────────
//
// Derives from engine::Capabilities so any field not set here keeps its
// "absent" default. Baseline Atari (400/800/XL/XE) always has ANTIC+GTIA P/M
// graphics, 4 players + 4 missiles, hardware scroll, a programmable display
// list, and DLI/VBI interrupts; the extension axes layer on top.
template <Machine M, RAM R, typename Gfx, Sound S, TV Tv, Network N>
struct AtariCaps : engine::Capabilities {
    // ── Graphics ──
    static constexpr bool has_hardware_sprites = true;
    static constexpr u8   max_hardware_sprites = 4;     // Players 0-3
    static constexpr bool has_missiles         = true;
    static constexpr u8   max_missiles         = 4;
    static constexpr bool has_hardware_scroll  = true;
    static constexpr bool has_blitter          = detail::is_vbxe<Gfx>::value;
    static constexpr bool has_display_list     = true;  // ANTIC
    static constexpr bool has_raster_interrupt = true;  // DLI

    // ── Extended graphics (VBXE) ──
    // VBXE adds 512KB VRAM, a 256-colour overlay plane (incl. an 80-column
    // text mode and overlay/playfield raster collision), and four 256-colour
    // hardware palettes. The neutral capability fields are declared in
    // engine::Capabilities; the VBXE-specific values live here in the backend.
    static constexpr bool has_vram              = detail::is_vbxe<Gfx>::value;
    static constexpr u32  vram_bytes            = detail::is_vbxe<Gfx>::value ? 524288u : 0u;
    static constexpr bool has_overlay           = detail::is_vbxe<Gfx>::value;
    static constexpr u16  overlay_colors        = detail::is_vbxe<Gfx>::value ? 256 : 0;
    static constexpr bool has_overlay_text_mode = detail::is_vbxe<Gfx>::value;
    static constexpr bool has_overlay_collision = detail::is_vbxe<Gfx>::value;
    static constexpr bool has_palette           = detail::is_vbxe<Gfx>::value;
    static constexpr u8   palette_count         = detail::is_vbxe<Gfx>::value ? 4 : 0;
    static constexpr u16  colors_per_palette    = detail::is_vbxe<Gfx>::value ? 256 : 0;

    // ── Sound ──
    static constexpr bool has_dedicated_sound   = true;          // POKEY
    static constexpr u8   sound_voices          = detail::voice_count(S);
    static constexpr bool has_stereo            = (S == Sound::Stereo) ||
                                                  (S == Sound::PokeyMax);
    static constexpr bool has_noise_generator   = true;          // POKEY noise
    static constexpr bool has_filter            = true;          // POKEY high-pass
    static constexpr bool has_extended_sound    = (S == Sound::PokeyMax);
    static constexpr u8   extended_sound_voices = (S == Sound::PokeyMax) ? 8 : 0;

    // ── Memory ──
    static constexpr u32  main_ram_bytes     = 49152u;           // 48K usable
    static constexpr bool has_extended_ram   = (R != RAM::Baseline);
    static constexpr u32  extended_ram_bytes = detail::ext_ram_bytes(R);
    static constexpr u16  extended_bank_size = (R == RAM::Baseline) ? 0u : 16384u;
    static constexpr u8   zp_available       = 128;              // after OS
    static constexpr u16  screen_buffer_alignment = 4096;

    // ── Input ──
    // XL/XE expose 2 joystick ports; 400/800 expose 4.
    static constexpr u8   joystick_ports = (M == Machine::XL || M == Machine::XE) ? 2 : 4;
    static constexpr bool has_keyboard   = true;
    static constexpr bool has_paddle     = true;

    // ── Timing (derived from the TV axis; ADR-018) ──
    static constexpr u32  cpu_frequency     = detail::tv_cpu_frequency(Tv);
    static constexpr u8   frames_per_second = detail::tv_frames_per_second(Tv);
    static constexpr u16  cycles_per_frame  = detail::tv_cycles_per_frame(Tv);
    static constexpr bool has_vbi           = true;

    // ── Network ──
    static constexpr bool has_network = (N == Network::Fujinet);
    static constexpr engine::NetworkTransport network_transport =
        (N == Network::Fujinet) ? engine::NetworkTransport::UDP
                                : engine::NetworkTransport::None;
    static constexpr bool network_reliable    = false;          // UDP best-effort
    static constexpr u16  network_max_payload = (N == Network::Fujinet) ? 512 : 0;
    static constexpr u16  network_latency_ms  = (N == Network::Fujinet) ? 2 : 0;
};

// ── Platform type ────────────────────────────────────────────────────

// TV is a required axis (no default — the machine determines it; ADR-018) and
// is placed before Network so Network can keep its None default. (C++ forbids
// a defaulted template parameter before a non-defaulted one.)
template <Machine M, RAM R, typename Gfx, Sound S, TV Tv, Network Net = Network::None>
struct Platform {
    // Axis values, queryable at compile time. Graphics is a type axis, so it is
    // exposed as a nested type rather than a constexpr value.
    static constexpr Machine  machine  = M;
    static constexpr RAM      ram       = R;
    using graphics                      = Gfx;
    static constexpr Sound    sound    = S;
    static constexpr TV       tv       = Tv;
    static constexpr Network  network  = Net;

    using capabilities = AtariCaps<M, R, Gfx, S, Tv, Net>;

    // The HAL the engine talks to: the baseline Atari Hal plus the graphics-axis
    // overlay seam (OverlayHal<Config> for VBXE, NullOverlay otherwise). Both are
    // all-static, so multiple inheritance just unifies their static methods under
    // one Platform::hal:: — existing call sites are unaffected.
    struct HalBundle : Hal, detail::overlay_hal_for<Gfx>::type {};
    using hal          = HalBundle;

    // The backend display-program builder for a portable layout. engine::Screen-
    // Manager builds each screen's display list as Platform::display_program<Layout>,
    // so the generic screen manager names nothing ANTIC-specific.
    template <typename Layout>
    using display_program = atari::DisplayProgram<Layout>;
};

// ── Common configurations ────────────────────────────────────────────
//
// PAL/NTSC are separate binaries, so the TV axis is spelled out per alias.
// Any alias fixed to NTSC has a PAL counterpart available by passing TV::PAL
// to the Platform template directly.

using StockXL_NTSC = Platform<
    Machine::XL, RAM::Baseline, gfx::Baseline, Sound::Mono, TV::NTSC>;

using StockXL_PAL = Platform<
    Machine::XL, RAM::Baseline, gfx::Baseline, Sound::Mono, TV::PAL>;

// NTSC by default; for PAL use the full Platform template with TV::PAL.
using ExpandedXE = Platform<
    Machine::XE, RAM::XE128, gfx::Baseline, Sound::Mono, TV::NTSC>;

// NTSC by default; for PAL use the full Platform template with TV::PAL.
using FullUpgrade = Platform<
    Machine::XL, RAM::U1MB, gfx::VBXE<>, Sound::PokeyMax, TV::NTSC, Network::Fujinet>;

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_PLATFORM_H
