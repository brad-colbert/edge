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
//   2. saves the llvm-mos zero-page imaginary registers ($80-$9F): a C++ handler
//      is an ordinary function that uses them, and the DLI interrupts the main
//      thread mid-computation, which is using the very same locations. Without
//      this, a non-trivial handler (e.g. the multiplexer's zone-boundary hook)
//      corrupts the main thread's zero page and crashes it. Mirrors
//      edge_vbi_trampoline (hal.h), which closes the identical gap for the VBI.
//   3. loads the current handler address from handler_lo_/handler_hi_ indexed
//      by current_ and patches it into a JSR operand,
//   4. JSRs the handler (a non-capturing C++ lambda / function — ADR-020),
//   5. restores $80-$9F,
//   6. loads next_lo_/next_hi_[current_] and stores it into VDSLST ($0200/$0201)
//      so the next DLI of the frame enters correctly,
//   7. increments current_,
//   8. restores A/X/Y and RTIs.
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
extern uint8_t edge_dli_op_curx;   // LDX current_            (abs, 2-byte operand)
extern uint8_t edge_dli_op_hlo;    // LDA handler_lo_,X       (abs, 2-byte operand)
extern uint8_t edge_dli_op_hhi;    // LDA handler_hi_,X       (abs, 2-byte operand)
extern uint8_t edge_dli_op_curx2;  // LDX current_ (reload)   (abs, 2-byte operand)
extern uint8_t edge_dli_op_nlo;    // LDA next_lo_,X          (abs, 2-byte operand)
extern uint8_t edge_dli_op_nhi;    // LDA next_hi_,X          (abs, 2-byte operand)
extern uint8_t edge_dli_op_inc;    // INC current_            (abs, 2-byte operand)

// ── Lean multiplex DLI (the sprite multiplexer's zone-boundary hook) ──
//
// edge_multiplex_dli is a *raw* handler ANTIC enters directly (no C++ dispatcher,
// no $80-$9F save): the multiplexer pre-bakes each zone boundary's four HPOSP and
// four COLPM bytes into a flat table (engine/sprites.h, mux_table_), so the DLI is
// just eight LDA/STA pairs + an index bump — ~150 cycles (~1.3 scanlines) versus
// the C++ dispatcher's ~11. That length matters: a DLI is an unmaskable NMI and
// the dispatch path is not re-entrant, so a handler longer than the gap between
// two zone boundaries gets preempted mid-flight and corrupts the shared save
// buffer / chain index (it trashed the llvm-mos soft-stack pointer). Staying well
// under one mode line keeps boundaries that fall on adjacent lines safe.
//
// It still participates in the InterruptManager chain: after writing its
// registers it falls through to the generic dispatcher's tail (edge_dli_op_curx2)
// via JMP, which re-points VDSLST from next_*[current_] and bumps current_ — the
// same already-patched machinery, so user raster hooks compose normally.
[[gnu::naked]] void edge_multiplex_dli();
extern uint8_t edge_mux_op_index;  // LDA mux_index_   (abs, operand at +1)
extern uint8_t edge_mux_op_table;  // first LDA mux_table_+0,x; load k at +k*6
extern uint8_t edge_mux_op_inc;    // INC mux_index_   (abs, operand at +1)

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
    ; Save the llvm-mos imaginary registers $80-$9F before entering the C++
    ; handler — it uses them, and so does the main thread this DLI interrupted.
    ldx #31
.Ledge_dli_save:
    lda $80,x
    sta edge_dli_zp_save,x
    dex
    bpl .Ledge_dli_save
edge_dli_op_curx:
    ldx $ffff               ; X = current_  (abs operand patched)
edge_dli_op_hlo:
    lda $ffff,x             ; handler_lo_[current_]  (operand patched)
    sta edge_dli_jsr+1
edge_dli_op_hhi:
    lda $ffff,x             ; handler_hi_[current_]  (operand patched)
    sta edge_dli_jsr+2
edge_dli_jsr:
    jsr $ffff               ; call handler  (operand self-modified above)
    ; Restore the imaginary registers the handler clobbered, so the interrupted
    ; main thread resumes with its zero page intact.
    ldx #31
.Ledge_dli_restore:
    lda edge_dli_zp_save,x
    sta $80,x
    dex
    bpl .Ledge_dli_restore
edge_dli_op_curx2:
    ldx $ffff               ; reload current_  (abs operand patched)
edge_dli_op_nlo:
    lda $ffff,x             ; next_lo_[current_]  (operand patched)
    sta $0200               ; VDSLST low
edge_dli_op_nhi:
    lda $ffff,x             ; next_hi_[current_]  (operand patched)
    sta $0201               ; VDSLST high
edge_dli_op_inc:
    inc $ffff               ; ++current_  (abs operand patched)
    pla
    tay
    pla
    tax
    pla
    rti

edge_dli_terminal:
    rti                     ; no-op terminal: nothing left to chain

edge_dli_zp_save:
    .fill 32                ; saved $80-$9F (llvm-mos imaginary registers)
)");

// The lean multiplex DLI. Placeholder operands ($FFFF) are overwritten by
// install_multiplex(); the GTIA register stores are fixed. Each LDA/STA pair is
// 6 bytes, so the operand of table-load k is at (edge_mux_op_table + k*6 + 1).
asm(R"(
    .globl edge_multiplex_dli
    .globl edge_mux_op_index
    .globl edge_mux_op_table
    .globl edge_mux_op_inc

edge_multiplex_dli:
    pha
    txa
    pha
    tya
    pha
edge_mux_op_index:
    lda $ffff               ; A = mux_index_  (abs operand patched)
    asl
    asl
    asl                     ; X = mux_index_ * 8  (row offset into the flat table)
    tax
edge_mux_op_table:
    lda $ffff,x             ; mux_table_+0,x   (operand patched)
    sta $d000               ; HPOSP0
    lda $ffff,x             ; mux_table_+1,x
    sta $d001               ; HPOSP1
    lda $ffff,x             ; mux_table_+2,x
    sta $d002               ; HPOSP2
    lda $ffff,x             ; mux_table_+3,x
    sta $d003               ; HPOSP3
    lda $ffff,x             ; mux_table_+4,x
    sta $d012               ; COLPM0
    lda $ffff,x             ; mux_table_+5,x
    sta $d013               ; COLPM1
    lda $ffff,x             ; mux_table_+6,x
    sta $d014               ; COLPM2
    lda $ffff,x             ; mux_table_+7,x
    sta $d015               ; COLPM3
edge_mux_op_inc:
    inc $ffff               ; ++mux_index_  (abs operand patched)
    jmp edge_dli_op_curx2   ; share the generic chain tail + epilogue (RTI)
)");

// install_dispatch — patch the table/current_ addresses into the dispatcher
// operands. Called once by the engine after the InterruptManager instance is
// placed (not exercised by the simulator tests). `cur` is the address of
// current_; the remaining arguments are the RAM addresses of the parallel tables.
// Every patched site uses absolute (2-byte) addressing — current_ lives in BSS,
// not a reserved zero-page byte, so it is addressed absolutely like the tables.
inline void install_dispatch(uint16_t cur,
                             uint16_t handler_lo, uint16_t handler_hi,
                             uint16_t next_lo, uint16_t next_hi) {
    // Absolute two-byte operands (little-endian).
    auto patch16 = [](uint8_t* opcode, uint16_t addr) {
        opcode[1] = static_cast<uint8_t>(addr & 0xFF);
        opcode[2] = static_cast<uint8_t>(addr >> 8);
    };
    patch16(&edge_dli_op_curx,  cur);
    patch16(&edge_dli_op_curx2, cur);
    patch16(&edge_dli_op_inc,   cur);
    patch16(&edge_dli_op_hlo, handler_lo);
    patch16(&edge_dli_op_hhi, handler_hi);
    patch16(&edge_dli_op_nlo, next_lo);
    patch16(&edge_dli_op_nhi, next_hi);
}

// install_multiplex — patch the lean multiplex DLI's operands with the addresses
// of the multiplexer's flat position/colour table and its per-frame fire index
// (both members of the single SpriteManager instance, so the addresses are
// stable). The eight table loads index mux_table_ + 0..7, six bytes apart.
inline void install_multiplex(uint16_t table, uint16_t index) {
    auto patch16 = [](uint8_t* opcode, uint16_t addr) {
        opcode[1] = static_cast<uint8_t>(addr & 0xFF);
        opcode[2] = static_cast<uint8_t>(addr >> 8);
    };
    patch16(&edge_mux_op_index, index);
    patch16(&edge_mux_op_inc,   index);
    uint8_t* tbl = &edge_mux_op_table;
    for (uint8_t k = 0; k < 8; ++k)
        patch16(tbl + k * 6, static_cast<uint16_t>(table + k));
}

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_DLI_DISPATCH_H
