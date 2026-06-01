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
//                               atari::Graphics::Baseline, atari::Sound::Stereo>;
//     using caps = P::capabilities;          // queried via if constexpr
//
// Capabilities are composed by starting from baseline Atari values and folding
// in axis-specific overrides with constexpr expressions on the axis enums.

#include "../../config/capabilities.h"
#include "../../types.h"
#include "display_list.h"
#include "display_traits.h"
#include "hal.h"

namespace atari {

using engine::u8;
using engine::u16;
using engine::u32;

// ── Independent configuration axes ───────────────────────────────────

enum class Machine  : u8 { A400, A800, XL, XE };
enum class RAM      : u8 { Baseline, XE128, Rambo256, U1MB };
enum class Graphics : u8 { Baseline, VBXE };
enum class Sound    : u8 { Mono, Stereo, PokeyMax };
enum class Network  : u8 { None, Fujinet };
enum class TV       : u8 { NTSC, PAL };

namespace detail {

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
template <Machine M, RAM R, Graphics G, Sound S, TV Tv, Network N>
struct AtariCaps : engine::Capabilities {
    // ── Graphics ──
    static constexpr bool has_hardware_sprites = true;
    static constexpr u8   max_hardware_sprites = 4;     // Players 0-3
    static constexpr bool has_missiles         = true;
    static constexpr u8   max_missiles         = 4;
    static constexpr bool has_hardware_scroll  = true;
    static constexpr bool has_blitter          = (G == Graphics::VBXE);
    static constexpr bool has_display_list     = true;  // ANTIC
    static constexpr bool has_raster_interrupt = true;  // DLI

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
template <Machine M, RAM R, Graphics G, Sound S, TV Tv, Network Net = Network::None>
struct Platform {
    // Axis values, queryable at compile time.
    static constexpr Machine  machine  = M;
    static constexpr RAM      ram       = R;
    static constexpr Graphics graphics = G;
    static constexpr Sound    sound    = S;
    static constexpr TV       tv       = Tv;
    static constexpr Network  network  = Net;

    using capabilities = AtariCaps<M, R, G, S, Tv, Net>;
    using hal          = Hal;

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
    Machine::XL, RAM::Baseline, Graphics::Baseline, Sound::Mono, TV::NTSC>;

using StockXL_PAL = Platform<
    Machine::XL, RAM::Baseline, Graphics::Baseline, Sound::Mono, TV::PAL>;

// NTSC by default; for PAL use the full Platform template with TV::PAL.
using ExpandedXE = Platform<
    Machine::XE, RAM::XE128, Graphics::Baseline, Sound::Mono, TV::NTSC>;

// NTSC by default; for PAL use the full Platform template with TV::PAL.
using FullUpgrade = Platform<
    Machine::XL, RAM::U1MB, Graphics::VBXE, Sound::PokeyMax, TV::NTSC, Network::Fujinet>;

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_PLATFORM_H
