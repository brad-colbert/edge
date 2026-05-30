#ifndef ENGINE_PLATFORM_ATARI_DLI_DISPATCH_H
#define ENGINE_PLATFORM_ATARI_DLI_DISPATCH_H

// platform/atari/dli_dispatch.h — the Atari DLI dispatcher (assembly).
//
// The portable InterruptManager (engine/interrupt.h) builds the indexed
// handler_/next_ tables; this file provides the Atari-specific code that ANTIC
// actually enters on a Display List Interrupt for a C++ (engine-wrapped)
// handler. See DECISIONS.md ADR-019 (two-tier dispatch) and ADR-021
// (self-modifying JSR).
//
// edge_dli_dispatch() is the wrapper for C++ handlers. On entry (from ANTIC via
// VDSLST) it:
//   1. saves A/X/Y,
//   2. loads the current handler address from handler_lo_/handler_hi_ indexed
//      by current_ and patches it into a JSR operand,
//   3. JSRs the handler (a non-capturing C++ lambda / function — ADR-020),
//   4. loads next_lo_/next_hi_[current_] and stores it into VDSLST ($0200/$0201)
//      so the next DLI of the frame enters correctly,
//   5. increments current_,
//   6. restores A/X/Y and RTIs.
//
// The table base addresses and the current_ ZP location are not known until the
// single InterruptManager instance is placed in RAM, so they are patched into
// the instruction operands by install_dispatch() — itself a self-modifying-code
// step, consistent with ADR-021. Each patch site is exported as a global label
// positioned at the instruction's first byte; the operand is at label+1.
//
// NOTE: this routine is never executed under `mos-sim` (there is no ANTIC); it
// exists for compile coverage and to be wired up by engine::Core later. Raw
// handlers (engine multiplex/scroll, user raw DLIs) never enter here — they
// chain themselves via InterruptManager::next_dli_addr().

#include <stdint.h>

namespace atari {

// VDSLST — the OS DLI vector ($0200/$0201). This is a RAM vector, not a chip
// register, so it lives here rather than in registers.h.
inline constexpr uint16_t VDSLST_ADDR = 0x0200;

extern "C" {

// The dispatcher and the no-op terminal handler. Defined in the asm block below.
[[gnu::naked]] void edge_dli_dispatch();
[[gnu::naked]] void edge_dli_terminal();

// Patch-site labels: each sits on the first byte of an instruction whose operand
// install_dispatch() rewrites. The operand begins at (label + 1).
extern uint8_t edge_dli_op_curx;   // LDX current_            (zp,  1-byte operand)
extern uint8_t edge_dli_op_hlo;    // LDA handler_lo_,X       (abs, 2-byte operand)
extern uint8_t edge_dli_op_hhi;    // LDA handler_hi_,X       (abs, 2-byte operand)
extern uint8_t edge_dli_op_curx2;  // LDX current_ (reload)   (zp,  1-byte operand)
extern uint8_t edge_dli_op_nlo;    // LDA next_lo_,X          (abs, 2-byte operand)
extern uint8_t edge_dli_op_nhi;    // LDA next_hi_,X          (abs, 2-byte operand)
extern uint8_t edge_dli_op_inc;    // INC current_            (zp,  1-byte operand)

} // extern "C"

// The dispatcher body. Placeholder operands ($00 / $FFFF) are overwritten by
// install_dispatch(). VDSLST is the only fixed address.
asm(R"(
    .globl edge_dli_dispatch
    .globl edge_dli_terminal
    .globl edge_dli_op_curx
    .globl edge_dli_op_hlo
    .globl edge_dli_op_hhi
    .globl edge_dli_op_curx2
    .globl edge_dli_op_nlo
    .globl edge_dli_op_nhi
    .globl edge_dli_op_inc

edge_dli_dispatch:
    pha
    txa
    pha
    tya
    pha
edge_dli_op_curx:
    ldx $00                 ; X = current_  (operand patched)
edge_dli_op_hlo:
    lda $ffff,x             ; handler_lo_[current_]  (operand patched)
    sta edge_dli_jsr+1
edge_dli_op_hhi:
    lda $ffff,x             ; handler_hi_[current_]  (operand patched)
    sta edge_dli_jsr+2
edge_dli_jsr:
    jsr $ffff               ; call handler  (operand self-modified above)
edge_dli_op_curx2:
    ldx $00                 ; reload current_  (operand patched)
edge_dli_op_nlo:
    lda $ffff,x             ; next_lo_[current_]  (operand patched)
    sta $0200               ; VDSLST low
edge_dli_op_nhi:
    lda $ffff,x             ; next_hi_[current_]  (operand patched)
    sta $0201               ; VDSLST high
edge_dli_op_inc:
    inc $00                 ; ++current_  (operand patched)
    pla
    tay
    pla
    tax
    pla
    rti

edge_dli_terminal:
    rti                     ; no-op terminal: nothing left to chain
)");

// install_dispatch — patch the table/ZP addresses into the dispatcher operands.
// Called once by the engine after the InterruptManager instance is placed (not
// exercised by the simulator tests). `cur` is the ZP address of current_; the
// remaining arguments are the RAM addresses of the parallel tables.
inline void install_dispatch(uint8_t cur,
                             uint16_t handler_lo, uint16_t handler_hi,
                             uint16_t next_lo, uint16_t next_hi) {
    // Zero-page single-byte operands.
    (&edge_dli_op_curx)[1]  = cur;
    (&edge_dli_op_curx2)[1] = cur;
    (&edge_dli_op_inc)[1]   = cur;

    // Absolute two-byte operands (little-endian).
    auto patch16 = [](uint8_t* opcode, uint16_t addr) {
        opcode[1] = static_cast<uint8_t>(addr & 0xFF);
        opcode[2] = static_cast<uint8_t>(addr >> 8);
    };
    patch16(&edge_dli_op_hlo, handler_lo);
    patch16(&edge_dli_op_hhi, handler_hi);
    patch16(&edge_dli_op_nlo, next_lo);
    patch16(&edge_dli_op_nhi, next_hi);
}

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_DLI_DISPATCH_H
