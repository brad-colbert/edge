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

#include "../../audio_defs.h"
#include "antic.h"
#include "registers.h"
#include "nmi.h"
#include "os.h"
#include "dli_dispatch.h"

namespace atari {

// The OS deferred-VBI vector (os::VVBLKD, $0224/$0225) and the deferred-VBI exit
// routine (os::XITVBV, $E462) live in os.h. install_vbi points VVBLKD at the
// trampoline below; a deferred handler reached via JMP (VVBLKD) must leave
// through XITVBV (it pulls the A/X/Y the OS VBI prologue saved and RTIs);
// returning with RTS would corrupt the stack.

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
//
// RE-ENTRY GUARD (edge_vbi_busy): the deferred VBI is an NMI, and a heavy service
// (e.g. the 9-sprite sprite multiplexer) can overrun a frame. The next frame's VBI
// NMI would then re-enter this trampoline *while it is still running* — and the
// inner save loop would overwrite edge_vbi_zp_save with the outer call's
// already-allocated $80-$9F. On unwind the main thread's llvm-mos soft-stack
// pointer (__rc0/__rc1 at $80/$81) comes back one service frame lower than it went
// in; that drift accumulates every overrun until the pointer walks down into the
// code and the next service's stack writes corrupt it → crash. The guard makes a
// re-entrant VBI a no-op (straight to XITVBV): one frame's service is skipped (a
// cosmetic hiccup) instead of corrupting the soft stack. A in-service flag, not a
// disable of NMIs, because the VBI NMI itself must keep being delivered.
extern "C" {
[[gnu::naked]] void edge_vbi_trampoline();
extern uint8_t edge_vbi_jsr;   // first byte of the JSR; operand begins at +1
}

asm(R"(
    .globl edge_vbi_trampoline
    .globl edge_vbi_jsr
edge_vbi_trampoline:
    lda edge_vbi_busy
    bne .Ledge_vbi_skip        ; already mid-service → don't re-enter; just exit
    inc edge_vbi_busy
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
    dec edge_vbi_busy
.Ledge_vbi_skip:
    jmp $e462                  ; XITVBV
edge_vbi_busy:
    .byte 0
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
    static void set_charset_base(uint8_t v) {
        *os::CHBAS  = v;
        *reg::CHBASE = v;
    }
    static void set_fine_scroll_x(uint8_t v) { *reg::HSCROL = v; }
    static void set_fine_scroll_y(uint8_t v) { *reg::VSCROL = v; }

    // ── DLI dispatcher addresses ──
    //
    // Addresses the InterruptManager writes into the next-pointer table: the
    // dispatcher entry for C++ handlers and the no-op terminal for the last
    // slot. Defined in dli_dispatch.h.
    static uint16_t raster_dispatch_addr() {
        return static_cast<uint16_t>(
            reinterpret_cast<uintptr_t>(&edge_dli_dispatch));
    }
    static uint16_t raster_terminal_addr() {
        return static_cast<uint16_t>(
            reinterpret_cast<uintptr_t>(&edge_dli_terminal));
    }

    // Patch the C++ DLI dispatcher's operands with the InterruptManager's table
    // and current_ addresses. Called once from engine::Core::init via
    // InterruptManager::arm_dispatch (the single manager instance never moves).
    static void install_raster_dispatch(uint16_t cur,
                                     uint16_t handler_lo, uint16_t handler_hi,
                                     uint16_t next_lo, uint16_t next_hi) {
        install_dispatch(cur, handler_lo, handler_hi, next_lo, next_hi);
    }

    // The raw zone-boundary handler the sprite multiplexer registers as its
    // dynamic raster hook (engine/sprites.h), and the patch step that binds it to
    // the multiplexer's flat position/colour table. Defined in dli_dispatch.h.
    static void (*multiplex_dli())() { return &edge_multiplex_dli; }
    static void install_multiplex_dli(uint16_t table, uint16_t index) {
        install_multiplex(table, index);
    }

    // ── Display list / ANTIC DMA ──
    //
    // The screen manager (engine/screen.h) reaches ANTIC only through these
    // (Dependency Rule 2). All writes go to the OS shadows (os.h), not the chip
    // registers: the OS deferred VBI copies the shadows to hardware each frame
    // before the engine's service runs, so a direct register write would be
    // overwritten.
    //
    // SDMCTL carries bits owned by two subsystems — the screen manager (DL-enable
    // + playfield width) and the sprite system (P/M DMA + line resolution). Since
    // SDMCTL is readable RAM, each side read-modify-writes only its own bits:
    // antic_dma_enable rewrites the DL/playfield portion while preserving the P/M
    // bits, and pm_dma_enable (below) ORs in the P/M bits. antic_dma_disable
    // clears just DL+playfield (keeping P/M) so P/M DMA survives the
    // disable→set_display_list→enable swap a screen change performs.
    static void display_dma_disable() { *os::SDMCTL &= dmactl::PM_DMA_MASK; }
    static void display_dma_enable(uint8_t mode_bits = dmactl::PLAYFIELD_NORMAL) {
        *os::SDMCTL = static_cast<uint8_t>((*os::SDMCTL & dmactl::PM_DMA_MASK) |
                                           dmactl::DL_ENABLE | mode_bits);
    }
    // Toggle ONLY the ANTIC playfield fetch (the character/bitmap DMA), preserving
    // the DL-enable and P/M bits. Under an opaque VBXE overlay the ANTIC playfield
    // is fully hidden, yet its per-scanline VRAM-bus DMA starves the blitter's
    // VRAM-read restore copies — collapsing the overlay compositor's per-frame
    // budget (sprites-over-bitmap ran the game loop at ~8 Hz instead of 60).
    // Dropping the playfield fetch frees the bus; the overlay still drives the
    // display via its XDL, and DL/P-M DMA keep running. `on` restores normal width.
    static void set_antic_playfield_dma(bool on) {
        *os::SDMCTL = static_cast<uint8_t>(
            (*os::SDMCTL & static_cast<uint8_t>(~dmactl::PLAYFIELD_MASK)) |
            (on ? dmactl::PLAYFIELD_NORMAL : 0u));
    }
    static void set_display_program(const uint8_t* dl) {
        const uint16_t a = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(dl));
        *os::SDLSTL = static_cast<uint8_t>(a & 0xFF);
        *os::SDLSTH = static_cast<uint8_t>(a >> 8);
    }

    // ── DLI delivery (display-list bits + VDSLST + NMIEN) ──
    //
    // The portable InterruptManager (engine/interrupt.h) builds the chain tables
    // but cannot touch the display list (it must not include ANTIC headers), so it
    // hands the raw list down here. program_dli_lines walks the list and sets the
    // DLI ($80) bit on the mode line that displays each requested scanline,
    // clearing every other DLI bit so a stale chain doesn't fire. Scanlines are
    // display-list-relative (line 0 = first instruction; the leading blank lines
    // count). The walk handles blank-line instructions (low nibble 0, variable
    // count), mode lines with/without an LMS prefix (1 vs 3 bytes), and stops at
    // the jump/JVB terminator (low nibble 1).
    static void program_raster_lines(uint8_t* dl, uint16_t dl_size,
                                  const uint8_t* scanlines, uint8_t count) {
        uint16_t scan = 0;
        uint16_t p    = 0;
        while (p < dl_size) {
            const uint8_t low = static_cast<uint8_t>(dl[p] & 0x0F);
            if (low == 0x01) break;                         // JMP / JVB terminator
            if (low == 0x00) {                              // blank-line instruction
                dl[p] = static_cast<uint8_t>(dl[p] & ~DL_DLI);
                scan = static_cast<uint16_t>(scan + ((dl[p] >> 4) & 0x07) + 1);
                ++p;
                continue;
            }
            // Mode line: clear any stale DLI, then set it if a requested scanline
            // falls within this line's [scan, scan+height) span.
            uint8_t instr = static_cast<uint8_t>(dl[p] & ~DL_DLI);
            const uint8_t h =
                scanlines_per_line(static_cast<Mode>(low));
            for (uint8_t k = 0; k < count; ++k) {
                if (scanlines[k] >= scan && scanlines[k] < scan + h) {
                    instr |= DL_DLI;
                    break;
                }
            }
            dl[p] = instr;
            p = static_cast<uint16_t>(p + ((instr & DL_LMS) ? 3 : 1));
            scan = static_cast<uint16_t>(scan + h);
        }
    }

    // Point the OS DLI vector (os::VDSLST, $0200/1) at the chain head the VBI
    // computed; the dispatcher rewrites it mid-frame as it walks the chain.
    static void set_raster_vector(uint16_t a) {
        os::VDSLST[0] = static_cast<uint8_t>(a & 0xFF);
        os::VDSLST[1] = static_cast<uint8_t>(a >> 8);
    }

    // Arm/disarm the DLI NMI. NMIEN is write-only (reads return NMIST) so it can't
    // be RMW'd; the engine's VBI NMI is always armed once install_vbi ran, so the
    // correct absolute value is VBI|DLI to enable and VBI alone to disable.
    // Route NMIEN changes through nmien_set so the write-only register's shadow
    // (nmi.h) stays accurate — the VBXE critical section restores from it.
    static void enable_raster()  { nmien_set(nmien::VBI | nmien::DLI); }
    static void disable_raster() { nmien_set(nmien::VBI); }

    // ── Player/Missile graphics ──
    //
    // Horizontal-position registers (HPOSP0-3 at $D000-3, HPOSM0-3 at $D004-7
    // are contiguous, so a player/missile index is a direct subscript). The
    // sprite manager (engine/sprites.h) writes zone-0 player positions during
    // the VBI commit and per-zone positions from raw DLIs through these.
    static void set_sprite_x(uint8_t player,  uint8_t x) { reg::HPOSP0[player]  = x; }
    static void set_projectile_x(uint8_t missile, uint8_t x) { reg::HPOSM0[missile] = x; }

    // Player colour straight to the GTIA hardware register (COLPM0-3 contiguous).
    // Unlike set_color_pm (which writes the os::PCOLR0 shadow for OS-managed
    // colours), the sprite multiplexer drives COLPM directly: zone-0 colours from
    // the VBI commit, later zones from the boundary DLI, so a sprite keeps its
    // colour on whatever player slot it lands in.
    static void set_sprite_color(uint8_t player, uint8_t color) { reg::COLPM0[player] = color; }

    // Player object width (SIZEP0-3 are contiguous, like HPOSP0). `size` is one
    // of atari::sizep:: (NORMAL/DOUBLE/QUAD).
    static void set_player_size(uint8_t player, uint8_t size) { reg::SIZEP0[player] = size; }

    // Sprite memory layout for the commit phase. `res` is the vertical resolution
    // as a plain byte (0 = single-line, 1 = double-line), matching the underlying
    // value of engine::SpriteVerticalResolution so hal.h needs no engine include.
    // Returns the byte offset of sprite `player`'s strip from the P/M base; the
    // caller adds the (resolution-scaled) scanline within the strip.
    static uint16_t sprite_strip_offset(uint8_t res, uint8_t player) {
        const bool single = (res == 0);
        return static_cast<uint16_t>(atari::pm_player_base(single) +
                                     player * atari::pm_strip_size(single));
    }
    static uint16_t sprite_strip_size(uint8_t res) {
        return atari::pm_strip_size(res == 0);
    }

    // Byte offset of the single shared missile strip from the P/M base. Unlike
    // players (one strip each), all four missiles live in one strip: each scanline
    // byte packs the four missiles two bits apiece (missile m in bits 2m..2m+1).
    // The commit phase adds the (resolution-scaled) scanline within the strip, then
    // read-modify-writes that 2-bit field. `res` matches sprite_strip_offset.
    static uint16_t missile_strip_offset(uint8_t res) {
        return atari::pm_missile_base(res == 0);
    }

    // ── POKEY sound ──
    //
    // POKEY voice registers interleave AUDFn/AUDCn from $D200 (AUDF1=$D200,
    // AUDC1=$D201, AUDF2=$D202, …), so a channel's frequency register is
    // AUDF1[channel*2] and its control register is AUDC1[channel*2]. The sound
    // subsystem (engine/sound.h) drives these during the VBI tick through the
    // backend-neutral set_voice_freq / set_voice_control / silence_voice seam; the
    // Waveform→AUDC distortion mapping lives here (POKEY AUDC high-nibble bits).
    static constexpr uint8_t audc_distortion(engine::audio::Waveform w) {
        switch (w) {
            case engine::audio::Waveform::Tone:   return 0xA0;  // pure tone
            case engine::audio::Waveform::Noise:  return 0x80;  // noise
            case engine::audio::Waveform::Buzz:   return 0xC0;  // buzzy distortion
            case engine::audio::Waveform::Silent: return 0x10;  // never reaches POKEY
        }
        return 0x10;
    }
    static void set_voice_freq(uint8_t channel, uint8_t freq) {
        reg::AUDF1[channel * 2] = freq;
    }
    static void set_voice_control(uint8_t channel, engine::audio::Waveform w, uint8_t vol) {
        reg::AUDC1[channel * 2] = static_cast<uint8_t>(audc_distortion(w) | vol);
    }
    static void silence_voice(uint8_t channel) { reg::AUDC1[channel * 2] = 0; }

    // ── Input capture ──
    //
    // The Core frame service (engine/core.h) calls these once per frame and feeds
    // the result to engine::InputState::update(). The returned bytes are already
    // in the portable `engine::joy::` / `engine::syskey::` bit layout
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
    // sprite_area_bytes is the single-line P/M window size; engine::Core sizes its
    // (alignment-constrained) sprite buffer from it without including antic.h.
    // set_sprite_base points ANTIC at that buffer (high byte); sprite_dma_enable
    // arms the GTIA P/M latches. Full DMACTL P/M-DMA bit setup arrives with the
    // live display path.
    static constexpr uint16_t sprite_area_bytes = 2048;
    static void set_sprite_base(uint8_t page) { *reg::PMBASE = page; }
    // Latch the GTIA P/M DMA (GRACTL, a chip register the OS does not shadow) and
    // OR the P/M DMA bits into SDMCTL alongside whatever DL/playfield bits the
    // screen manager already placed there. `single_line` selects single-scanline
    // sprite resolution (PM_SINGLE_LINE); double-line leaves it clear.
    static void sprite_dma_enable(bool single_line) {
        *reg::GRACTL = gractl::PLAYER_LATCH | gractl::MISSILE_LATCH;
        uint8_t bits = dmactl::PLAYER_DMA | dmactl::MISSILE_DMA;
        if (single_line) bits |= dmactl::PM_SINGLE_LINE;
        *os::SDMCTL = static_cast<uint8_t>(*os::SDMCTL | bits);
    }
    static void sprite_dma_disable() {
        *reg::GRACTL = 0x00;
        *os::SDMCTL &= static_cast<uint8_t>(~dmactl::PM_DMA_MASK);
    }

    // ── OS shadow colours + system ──
    //
    // Persistent (non-DLI) colours are written to the OS shadows so they survive
    // the OS VBI copy; the per-scanline DLI register stores above hit the chip
    // registers directly (they run after the copy). PCOLR0[player] shadows
    // COLPM0-3; COLOR0[field] shadows COLPF0-3 then COLBK at field 4.
    static void set_color_pm(uint8_t player, uint8_t color) { os::PCOLR0[player] = color; }
    static void set_color_pf(uint8_t field,  uint8_t color) { os::COLOR0[field]  = color; }

    // Zero the attract-mode timer each frame (from the VBI service) to stop the
    // OS dimming/cycling colours after a few minutes of no input.
    static void suppress_idle_dim() { *os::ATRACT = 0; }

    // ── Deferred VBI install ──
    //
    // Point the OS deferred-VBI vector at `service` and enable the VBI NMI. The
    // engine's service routine runs the per-frame sequence (engine/core.h) and,
    // on real hardware, must exit via XITVBV. Like the DLI dispatcher this seam
    // is never executed under `mos-sim` (no NMI) — it exists for compile coverage
    // and the live ANTIC path.
    static void install_frame_isr(void (*service)()) {
        // Patch the trampoline's JSR target to the engine VBI service, then
        // point the OS deferred-VBI vector at the trampoline (not the service
        // directly — see edge_vbi_trampoline above).
        const uint16_t a =
            static_cast<uint16_t>(reinterpret_cast<uintptr_t>(service));
        (&edge_vbi_jsr)[1] = static_cast<uint8_t>(a & 0xFF);
        (&edge_vbi_jsr)[2] = static_cast<uint8_t>(a >> 8);

        const uint16_t t =
            static_cast<uint16_t>(reinterpret_cast<uintptr_t>(&edge_vbi_trampoline));

        // NMIEN is write-only (reads return NMIST), so it can't be RMW'd: blank
        // all NMIs while the two-byte os::VVBLKD vector is swapped to avoid a VBI
        // firing on a half-written vector, then re-enable the VBI NMI. The DLI
        // NMI (nmien::DLI) is armed separately by code that installs a DLI.
        *reg::NMIEN = 0x00;                 // raw blank during the vector swap
        os::VVBLKD[0] = static_cast<uint8_t>(t & 0xFF);
        os::VVBLKD[1] = static_cast<uint8_t>(t >> 8);
        nmien_set(nmien::VBI);              // records the intended mask in the shadow
    }
};

// Enable or disable playfield DMA. Call this on an opaque overlay screen to
// prevent ANTIC bus contention with the blitter. Any set_screen() on a
// non-pure-overlay layout re-enables it automatically.
inline void set_playfield_dma(bool enable) { Hal::set_antic_playfield_dma(enable); }

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_HAL_H
