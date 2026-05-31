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

// VVBLKD — the OS deferred vertical-blank vector ($0224/$0225). Like VDSLST in
// dli_dispatch.h this is a RAM vector, not a chip register, so it lives here.
// The OS dispatches the deferred VBI through it; a deferred handler exits via
// XITVBV ($E462).
inline constexpr uint16_t VVBLKD_ADDR = 0x0224;

// XITVBV — the OS deferred-VBI exit routine ($E462). A deferred handler reached
// via JMP (VVBLKD) must leave through here (it pulls the A/X/Y the OS VBI
// prologue saved and RTIs); returning with RTS would corrupt the stack.
inline constexpr uint16_t XITVBV_ADDR = 0xE462;

// ── Deferred-VBI trampoline (the live ANTIC path) ─────────────────────
//
// engine::Core::vbi_service() is a normal C++ function: it ends in RTS and uses
// the llvm-mos zero-page "imaginary registers" ($80-$9F on the atari8-dos
// target — see its link.ld). The deferred VBI, though, is entered via
// JMP (VVBLKD) and must exit via JMP XITVBV, and it interrupts the main thread
// mid-computation — the main thread uses those same $80-$9F locations. This
// trampoline bridges both gaps: it saves $80-$9F, JSRs the service, restores
// $80-$9F, then JMP XITVBV. Hardware A/X/Y are saved by the OS VBI prologue and
// restored by XITVBV, so the trampoline may clobber them freely.
//
// Mirrors dli_dispatch.h: a naked routine whose JSR operand is self-modified by
// install_vbi() to point at the service. Never executed under mos-sim (no NMI).
extern "C" {
[[gnu::naked]] void edge_vbi_trampoline();
extern uint8_t edge_vbi_jsr;   // first byte of the JSR; operand begins at +1
}

asm(R"(
    .globl edge_vbi_trampoline
    .globl edge_vbi_jsr
edge_vbi_trampoline:
    ldx #31
.Ledge_vbi_save:
    lda $80,x
    sta edge_vbi_zp_save,x
    dex
    bpl .Ledge_vbi_save
edge_vbi_jsr:
    jsr $ffff                  ; → vbi_service (operand patched by install_vbi)
    ldx #31
.Ledge_vbi_restore:
    lda edge_vbi_zp_save,x
    sta $80,x
    dex
    bpl .Ledge_vbi_restore
    jmp $e462                  ; XITVBV
edge_vbi_zp_save:
    .fill 32
)");

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

    // ── POKEY sound ──
    //
    // POKEY voice registers interleave AUDFn/AUDCn from $D200 (AUDF1=$D200,
    // AUDC1=$D201, AUDF2=$D202, …), so a channel's frequency register is
    // AUDF1[channel*2] and its control register is AUDC1[channel*2]. The sound
    // subsystem (engine/sound.h) writes these during the VBI tick.
    static void write_audf(uint8_t channel, uint8_t freq) { reg::AUDF1[channel * 2] = freq; }
    static void write_audc(uint8_t channel, uint8_t ctrl) { reg::AUDC1[channel * 2] = ctrl; }

    // ── Input capture ──
    //
    // The Core VBI service (engine/core.h) calls these once per frame and feeds
    // the result to engine::InputState::update(). The returned bytes are already
    // in the portable `engine::joy::` / `engine::console::` bit layout
    // (engine/input.h) so the engine side stays platform-free: bit 0 up, 1 down,
    // 2 left, 3 right, 4 fire; for port 0 the high bits carry the console keys
    // (0x20 START, 0x40 SELECT, 0x80 OPTION). Hardware direction bits and the
    // trigger/console registers are all active-low, so each is inverted here.
    static uint8_t read_joystick(uint8_t port) {
        const uint8_t pa  = *reg::PORTA;                       // sticks 0/1 nibbles
        const uint8_t nib = (port == 0) ? (pa & 0x0F)
                                        : static_cast<uint8_t>(pa >> 4);
        uint8_t state = static_cast<uint8_t>(~nib) & 0x0F;     // 1 = direction held
        const uint8_t trig = (port == 0) ? *reg::TRIG0 : *reg::TRIG1;
        if ((trig & 0x01) == 0) state |= 0x10;                 // FIRE held
        if (port == 0) {
            const uint8_t c = *reg::CONSOL;                    // 0 = pressed
            if ((c & 0x01) == 0) state |= 0x20;                // START
            if ((c & 0x02) == 0) state |= 0x40;                // SELECT
            if ((c & 0x04) == 0) state |= 0x80;                // OPTION
        }
        return state;
    }
    // Current keyboard scancode (low 7 bits), 0 if none. A full implementation
    // also consults SKSTAT/IRQ for key-down; for now the latest KBCODE is
    // returned and InputState's edge logic dedupes held keys.
    static uint8_t read_keyboard() { return static_cast<uint8_t>(*reg::KBCODE & 0x7F); }

    // ── P/M collision reads (GTIA read side) ──
    //
    // The collision registers accumulate across the frame; the Core VBI service
    // latches them and then clears them with clear_collisions() for the next
    // frame. The four register banks are contiguous, so the player/missile index
    // is a direct subscript (registers.h).
    static uint8_t coll_player_playfield(uint8_t p) { return reg::P0PF[p]; }
    static uint8_t coll_player_player(uint8_t p)    { return reg::P0PL[p]; }
    static uint8_t coll_missile_playfield(uint8_t m){ return reg::M0PF[m]; }
    static uint8_t coll_missile_player(uint8_t m)   { return reg::M0PL[m]; }
    static void    clear_collisions() { *reg::HITCLR = 0; }

    // ── Player/Missile DMA setup ──
    //
    // pm_area_bytes is the single-line P/M window size; engine::Core sizes its
    // (alignment-constrained) P/M buffer from it without including antic.h.
    // set_pm_base points ANTIC at that buffer (high byte); pm_dma_enable arms the
    // GTIA P/M latches. Full DMACTL P/M-DMA bit setup arrives with the live
    // display path.
    static constexpr uint16_t pm_area_bytes = 2048;
    static void set_pm_base(uint8_t page) { *reg::PMBASE = page; }
    static void pm_dma_enable()  { *reg::GRACTL = 0x03; }   // players + missiles
    static void pm_dma_disable() { *reg::GRACTL = 0x00; }

    // ── Deferred VBI install ──
    //
    // Point the OS deferred-VBI vector at `service` and enable the VBI NMI. The
    // engine's service routine runs the per-frame sequence (engine/core.h) and,
    // on real hardware, must exit via XITVBV. Like the DLI dispatcher this seam
    // is never executed under `mos-sim` (no NMI) — it exists for compile coverage
    // and the live ANTIC path.
    static void install_vbi(void (*service)()) {
        // Patch the trampoline's JSR target to the engine VBI service, then
        // point the OS deferred-VBI vector at the trampoline (not the service
        // directly — see edge_vbi_trampoline above).
        const uint16_t a =
            static_cast<uint16_t>(reinterpret_cast<uintptr_t>(service));
        (&edge_vbi_jsr)[1] = static_cast<uint8_t>(a & 0xFF);
        (&edge_vbi_jsr)[2] = static_cast<uint8_t>(a >> 8);

        const uint16_t t =
            static_cast<uint16_t>(reinterpret_cast<uintptr_t>(&edge_vbi_trampoline));
        volatile uint8_t* v =
            reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(VVBLKD_ADDR));

        // NMIEN is write-only (reads return NMIST), so it can't be RMW'd: blank
        // all NMIs while the two-byte vector is swapped to avoid a VBI firing on
        // a half-written vector, then re-enable the VBI NMI (bit 6). The DLI NMI
        // (bit 7) is armed separately by code that installs a DLI.
        *reg::NMIEN = 0x00;
        v[0] = static_cast<uint8_t>(t & 0xFF);
        v[1] = static_cast<uint8_t>(t >> 8);
        *reg::NMIEN = 0x40;
    }
};

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_HAL_H
