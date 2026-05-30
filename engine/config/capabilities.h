#ifndef ENGINE_CONFIG_CAPABILITIES_H
#define ENGINE_CONFIG_CAPABILITIES_H

// config/capabilities.h — the capability trait contract.
//
// A platform exposes a nested `capabilities` type whose `static constexpr`
// members describe what the hardware can do. The engine queries these by
// feature name via `if constexpr` — never by platform identity (DECISIONS.md
// ADR-004, ARCHITECTURE.md "Capability Profile Structure").
//
// `Capabilities` below is the reference profile: it lists every field the
// engine may query, all defaulted to "absent / zero". Platform capability
// profiles derive from it and override the members that apply, so a profile
// that forgets a field degrades to "feature off" rather than failing to
// compile. Fields are grouped by subsystem per ARCHITECTURE.md.
//
// This header is platform-agnostic (engine layer); it depends only on types.h.

#include "../types.h"

namespace engine {

// Network transport kind, used by the network capability fields.
enum class NetworkTransport : u8 {
    None,
    UDP,
    TCP,
    Serial,
};

struct Capabilities {
    // ── Graphics ──
    static constexpr bool has_hardware_sprites = false;
    static constexpr u8   max_hardware_sprites = 0;
    static constexpr bool has_missiles         = false;
    static constexpr u8   max_missiles         = 0;
    static constexpr bool has_hardware_scroll  = false;
    static constexpr bool has_blitter          = false;
    static constexpr bool has_display_list     = false;
    static constexpr bool has_raster_interrupt = false;   // shared with timing

    // ── Sound ──
    static constexpr bool has_dedicated_sound   = false;
    static constexpr u8   sound_voices          = 0;
    static constexpr bool has_stereo            = false;
    static constexpr bool has_noise_generator   = false;
    static constexpr bool has_filter            = false;
    static constexpr bool has_extended_sound    = false;
    static constexpr u8   extended_sound_voices = 0;

    // ── Memory ──
    static constexpr u32 main_ram_bytes     = 0;
    static constexpr bool has_extended_ram  = false;
    static constexpr u32 extended_ram_bytes = 0;
    static constexpr u16 extended_bank_size = 0;
    static constexpr u8  zp_available       = 0;

    // ── Input ──
    static constexpr u8   joystick_ports = 0;
    static constexpr bool has_keyboard   = false;
    static constexpr bool has_paddle     = false;

    // ── Timing ──
    static constexpr u32  cpu_frequency     = 0;   // Hz
    static constexpr u8   frames_per_second = 0;
    static constexpr u16  cycles_per_frame  = 0;
    static constexpr bool has_vbi           = false;

    // ── Network ──
    static constexpr bool             has_network         = false;
    static constexpr NetworkTransport network_transport   = NetworkTransport::None;
    static constexpr bool             network_reliable    = false;
    static constexpr u16              network_max_payload = 0;
    static constexpr u16              network_latency_ms  = 0;
};

} // namespace engine

#endif // ENGINE_CONFIG_CAPABILITIES_H
