#ifndef ENGINE_PLATFORM_ATARI_HAL_H
#define ENGINE_PLATFORM_ATARI_HAL_H

// platform/atari/hal.h — the Atari hardware abstraction layer.
//
// The HAL provides concrete functions for register access, interrupt
// installation, display construction, and DMA configuration. The engine calls
// it through the Platform's `hal` type via static dispatch (ARCHITECTURE.md
// "Platform HAL", DECISIONS.md ADR-007 — never virtual).
//
// Only a partial HAL exists today. The interrupt-manager seam is implemented:
// the DLIContext register-store helpers and the dispatcher/terminal addresses
// the portable engine/interrupt.h reaches through Platform::hal. The rest
// (display build, DMA setup, per-axis specialisation) comes in later steps.

#include "antic.h"
#include "registers.h"
#include "dli_dispatch.h"

namespace atari {

// All-static HAL (no instance state); the engine calls it via Platform::hal
// (DECISIONS.md ADR-007 — static dispatch, never virtual).
struct Hal {
    // ── DLIContext register stores ──
    //
    // Backing for engine::DLIContext<Platform>. Each is a single LDA #imm /
    // STA abs against the GTIA/ANTIC register. `engine::u8` is uint8_t.
    static void write_colpf0(uint8_t v) { *reg::COLPF0 = v; }
    static void write_colpf1(uint8_t v) { *reg::COLPF1 = v; }
    static void write_colpf2(uint8_t v) { *reg::COLPF2 = v; }
    static void write_colpf3(uint8_t v) { *reg::COLPF3 = v; }
    static void write_colbk (uint8_t v) { *reg::COLBK  = v; }
    static void write_chbase(uint8_t v) { *reg::CHBASE = v; }
    static void write_hscrol(uint8_t v) { *reg::HSCROL = v; }
    static void write_vscrol(uint8_t v) { *reg::VSCROL = v; }

    // ── DLI dispatcher addresses ──
    //
    // Addresses the InterruptManager writes into the next-pointer table: the
    // dispatcher entry for C++ handlers and the no-op terminal for the last
    // slot. Defined in dli_dispatch.h.
    static uint16_t dli_dispatch_addr() {
        return static_cast<uint16_t>(
            reinterpret_cast<uintptr_t>(&edge_dli_dispatch));
    }
    static uint16_t dli_terminal_addr() {
        return static_cast<uint16_t>(
            reinterpret_cast<uintptr_t>(&edge_dli_terminal));
    }

    // ── Display list / ANTIC DMA ──
    //
    // The screen manager (engine/screen.h) reaches ANTIC only through these
    // (Dependency Rule 2). Disabling DMA blanks the screen for a safe display-
    // list swap; set_display_list points ANTIC at the active list; enabling DMA
    // restores it. DMACTL standard value: $20 (display-list DMA fetch) | $02
    // (normal-width playfield).
    static constexpr uint8_t DMACTL_NORMAL = 0x22;

    static void antic_dma_disable() { *reg::DMACTL = 0x00; }
    static void antic_dma_enable(uint8_t dmactl = DMACTL_NORMAL) {
        *reg::DMACTL = dmactl;
    }
    static void set_display_list(const uint8_t* dl) {
        const uint16_t a = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(dl));
        *reg::DLISTL = static_cast<uint8_t>(a & 0xFF);
        *reg::DLISTH = static_cast<uint8_t>(a >> 8);
    }

    // ── Player/Missile graphics ──
    //
    // Horizontal-position registers (HPOSP0-3 at $D000-3, HPOSM0-3 at $D004-7
    // are contiguous, so a player/missile index is a direct subscript). The
    // sprite manager (engine/sprites.h) writes zone-0 player positions during
    // the VBI commit and per-zone positions from raw DLIs through these.
    static void write_hposp(uint8_t player,  uint8_t x) { reg::HPOSP0[player]  = x; }
    static void write_hposm(uint8_t missile, uint8_t x) { reg::HPOSM0[missile] = x; }

    // P/M memory layout for the sprite-commit phase. `res` is the resolution as
    // a plain byte (0 = single-line, 1 = double-line), matching the underlying
    // value of engine::PMRes so hal.h needs no engine include. Returns the byte
    // offset of player `player`'s strip from the P/M base; the caller adds the
    // (resolution-scaled) scanline within the strip.
    static uint16_t pm_player_offset(uint8_t res, uint8_t player) {
        const bool single = (res == 0);
        return static_cast<uint16_t>(atari::pm_player_base(single) +
                                     player * atari::pm_strip_size(single));
    }
    static uint16_t pm_strip_size(uint8_t res) {
        return atari::pm_strip_size(res == 0);
    }

    // TODO: interrupt install, scroll setup.
};

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_HAL_H
