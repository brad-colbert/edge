// test_atari_interrupt.cpp — Atari-backend tests for raster (DLI) delivery.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// The portable raster-hook chain builder is tested in tests/generic/test_interrupt.cpp.
// This file covers the Atari-specific delivery walker: atari::Hal::program_raster_lines
// sets the DLI bit on the ANTIC display-list mode line that displays each requested
// scanline, clearing every stale DLI bit. It also pulls in the platform header for
// compile coverage of the dispatcher asm (never executed under the simulator).
//
// It further covers Hal::set_raster_vector's atomicity against the DLI: the two
// VDSLST bytes must be written with NMI masked, or a DLI landing between them takes
// a hybrid address neither writer produced. See the block above main().

#include <stdint.h>
#include <stdio.h>

#include <engine/platform/atari/platform.h>

using engine::u8;
using engine::u16;

namespace A = atari;

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Display-list DLI-bit programming (the ANTIC walker) ────────────────

static void test_dli_program() {
    namespace A = atari;
    // 3 blank-8 lines (24 scanlines), then 3 Mode-2 lines (8 each): the first
    // carries an LMS prefix (+2 address bytes), the third starts with a stale DLI
    // bit. A JVB terminates. Scanline spans: [24,32) [32,40) [40,48).
    u8 dl[] = {
        A::dl_blank(8), A::dl_blank(8), A::dl_blank(8),
        static_cast<u8>(A::dl_mode_byte(A::Mode::MODE_2) | A::DL_LMS), 0x00, 0x40,
        A::dl_mode_byte(A::Mode::MODE_2),
        static_cast<u8>(A::dl_mode_byte(A::Mode::MODE_2) | A::DL_DLI),
        A::DL_JVB, 0x00, 0x40,
    };
    const u8 lines[] = {24, 35};   // -> line @ idx3 [24,32) and line @ idx6 [32,40)
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 2);

    CHECK(dl[3] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_LMS | A::DL_DLI)); // 0xC2
    CHECK(dl[4] == 0x00);                                   // LMS address untouched
    CHECK(dl[5] == 0x40);
    CHECK(dl[6] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_DLI));             // 0x82
    CHECK(dl[7] == A::dl_mode_byte(A::Mode::MODE_2));       // stale DLI cleared (0x02)
    CHECK(dl[8] == A::DL_JVB);                              // terminator untouched

    // Empty chain clears every DLI bit.
    A::Hal::program_raster_lines(dl, sizeof(dl), nullptr, 0);
    CHECK(dl[3] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_LMS));   // 0x42
    CHECK(dl[6] == A::dl_mode_byte(A::Mode::MODE_2));                 // 0x02
    CHECK(dl[7] == A::dl_mode_byte(A::Mode::MODE_2));                 // 0x02
}


// ── Multi-hook corpus ────────────────────────────────────────────────
//
// The single test above covers two hooks on a simple list. Hand-assembling this
// walker is on the table as a size reserve, so this corpus exists to be the
// ORACLE for such a rewrite: any reimplementation must reproduce every byte
// asserted here. The cases are chosen to pin the walker's decisions rather than
// one happy path — line heights, LMS stride, blank-line accounting, list
// termination, and the several-hooks-per-line and no-match edges.

// Heights: MODE_2 = 8, MODE_3 = 10, MODE_5 = 16, BITMAP_F = 1.
// Two hooks landing inside ONE mode line must set that line's DLI exactly once
// and must not leak a DLI onto the neighbouring line.
static void test_two_hooks_one_line() {
    u8 dl[] = {
        A::dl_blank(8),                                  // idx0: scan 0..7
        A::dl_mode_byte(A::Mode::MODE_2),                // idx1: scan 8..15
        A::dl_mode_byte(A::Mode::MODE_2),                // idx2: scan 16..23
        A::DL_JVB, 0x00, 0x40,
    };
    const u8 lines[] = {9, 14};      // both inside idx1
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 2);
    CHECK(dl[1] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_DLI));
    CHECK(dl[2] == A::dl_mode_byte(A::Mode::MODE_2));
}

// Mixed heights must advance `scan` by each line's own height, not a constant.
static void test_mixed_mode_heights() {
    u8 dl[] = {
        A::dl_mode_byte(A::Mode::MODE_3),                // idx0: scan 0..9   (10)
        A::dl_mode_byte(A::Mode::MODE_5),                // idx1: scan 10..25 (16)
        A::dl_mode_byte(A::Mode::BITMAP_F),              // idx2: scan 26     (1)
        A::dl_mode_byte(A::Mode::MODE_2),                // idx3: scan 27..34 (8)
        A::DL_JVB, 0x00, 0x40,
    };
    const u8 lines[] = {5, 25, 26, 34};   // one in each line
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 4);
    CHECK(dl[0] == (A::dl_mode_byte(A::Mode::MODE_3)   | A::DL_DLI));
    CHECK(dl[1] == (A::dl_mode_byte(A::Mode::MODE_5)   | A::DL_DLI));
    CHECK(dl[2] == (A::dl_mode_byte(A::Mode::BITMAP_F) | A::DL_DLI));
    CHECK(dl[3] == (A::dl_mode_byte(A::Mode::MODE_2)   | A::DL_DLI));
}

// An LMS line carries two address bytes; the walk must step 3, not 1, or every
// later line is decoded from an address byte.
static void test_lms_stride() {
    u8 dl[] = {
        static_cast<u8>(A::dl_mode_byte(A::Mode::MODE_2) | A::DL_LMS), 0x34, 0x12,  // idx0: scan 0..7
        static_cast<u8>(A::dl_mode_byte(A::Mode::MODE_2) | A::DL_LMS), 0x78, 0x56,  // idx3: scan 8..15
        A::dl_mode_byte(A::Mode::MODE_2),                          // idx6: scan 16..23
        A::DL_JVB, 0x00, 0x40,
    };
    const u8 lines[] = {17};
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 1);
    CHECK(dl[6] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_DLI));
    // Address bytes must be untouched even where they alias a mode/DLI pattern.
    CHECK(dl[1] == 0x34);  CHECK(dl[2] == 0x12);
    CHECK(dl[4] == 0x78);  CHECK(dl[5] == 0x56);
    CHECK(dl[0] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_LMS));
    CHECK(dl[3] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_LMS));
}

// Blank instructions advance scan by ((b >> 4) & 7) + 1 scanlines and never
// carry a DLI, including when a requested scanline falls inside one.
static void test_blank_lines_take_no_dli() {
    u8 dl[] = {
        static_cast<u8>(A::dl_blank(8) | A::DL_DLI),   // idx0: stale DLI, scan 0..7
        A::dl_blank(4),                    // idx1: scan 8..11
        A::dl_mode_byte(A::Mode::MODE_2),  // idx2: scan 12..19
        A::DL_JVB, 0x00, 0x40,
    };
    const u8 lines[] = {3, 10, 15};   // two land in blanks, one in the mode line
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 3);
    CHECK(dl[0] == A::dl_blank(8));    // stale DLI cleared, none re-set
    CHECK(dl[1] == A::dl_blank(4));
    CHECK(dl[2] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_DLI));
}

// A scanline past the end of the list matches nothing and must not wrap onto an
// early line.
static void test_out_of_range_scanline() {
    u8 dl[] = {
        static_cast<u8>(A::dl_mode_byte(A::Mode::MODE_2) | A::DL_DLI),   // stale
        A::dl_mode_byte(A::Mode::MODE_2),
        A::DL_JVB, 0x00, 0x40,
    };
    const u8 lines[] = {200};
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 1);
    CHECK(dl[0] == A::dl_mode_byte(A::Mode::MODE_2));   // cleared, not re-set
    CHECK(dl[1] == A::dl_mode_byte(A::Mode::MODE_2));
}

// Scanline 0 is a boundary the >= test must include.
static void test_scanline_zero() {
    u8 dl[] = {
        A::dl_mode_byte(A::Mode::MODE_2),
        A::dl_mode_byte(A::Mode::MODE_2),
        A::DL_JVB, 0x00, 0x40,
    };
    const u8 lines[] = {0};
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 1);
    CHECK(dl[0] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_DLI));
    CHECK(dl[1] == A::dl_mode_byte(A::Mode::MODE_2));
}

// A full-capacity chain (12 slots, the engine default) on consecutive lines.
static void test_full_capacity_chain() {
    u8 dl[16];
    for (u8 i = 0; i < 12; ++i) dl[i] = A::dl_mode_byte(A::Mode::BITMAP_F);  // 1 scanline each
    dl[12] = A::DL_JVB; dl[13] = 0x00; dl[14] = 0x40; dl[15] = 0x00;
    const u8 lines[] = {0,1,2,3,4,5,6,7,8,9,10,11};
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 12);
    for (u8 i = 0; i < 12; ++i)
        CHECK(dl[i] == (A::dl_mode_byte(A::Mode::BITMAP_F) | A::DL_DLI));
}

// The walk stops at the terminator: bytes beyond it keep whatever they held,
// including bytes that would otherwise decode as DLI-bearing mode lines.
static void test_terminator_stops_walk() {
    u8 dl[] = {
        A::dl_mode_byte(A::Mode::MODE_2),
        A::DL_JVB, 0x00, 0x40,
        static_cast<u8>(A::dl_mode_byte(A::Mode::MODE_2) | A::DL_DLI),  // past the end
    };
    const u8 lines[] = {4};
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 1);
    CHECK(dl[4] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_DLI));  // untouched
    CHECK(dl[1] == A::DL_JVB);
}

// A hook sitting EXACTLY on a line boundary belongs to the line that starts
// there, not to the one that ends there. Pins the half-open [scan, scan+h) test:
// an inclusive end would light both lines.
static void test_line_boundary_is_half_open() {
    u8 dl[] = {
        A::dl_mode_byte(A::Mode::MODE_2),   // idx0: scan 0..7
        A::dl_mode_byte(A::Mode::MODE_2),   // idx1: scan 8..15
        A::DL_JVB, 0x00, 0x40,
    };
    const u8 lines[] = {8};                 // first scanline of idx1
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 1);
    CHECK(dl[0] == A::dl_mode_byte(A::Mode::MODE_2));                  // NOT set
    CHECK(dl[1] == (A::dl_mode_byte(A::Mode::MODE_2) | A::DL_DLI));    // set
}

// Blank-line height is ((b >> 4) & 7) + 1 scanlines. Two blank-8s must push the
// following mode line to scan 16; a hook at 15 then falls in neither. Losing the
// +1 would start that mode line at 14 and wrongly light it.
static void test_blank_height_accounting() {
    u8 dl[] = {
        A::dl_blank(8),                     // scan 0..7   (8 lines)
        A::dl_blank(8),                     // scan 8..15  (8 lines)
        A::dl_mode_byte(A::Mode::MODE_2),   // scan 16..23
        A::DL_JVB, 0x00, 0x40,
    };
    const u8 lines[] = {15};                // last scanline of the second blank
    A::Hal::program_raster_lines(dl, sizeof(dl), lines, 1);
    CHECK(dl[2] == A::dl_mode_byte(A::Mode::MODE_2));   // no DLI anywhere
    CHECK(dl[0] == A::dl_blank(8));
    CHECK(dl[1] == A::dl_blank(8));
}

// ── set_raster_vector is atomic against the DLI ────────────────────────
//
// VDSLST is two bytes and its reader is an unmaskable NMI, so an unguarded pair of
// stores can be split by a DLI: the interrupt then takes a HYBRID vector, the low
// byte of one address with the high byte of the other. Field capture (ATank,
// 2026-08-30): VDSLST = $4574 = lo(edge_dli_terminal $4474) : hi(the game's raw hook
// $452B), a value neither writer can produce, and the machine wedged inside whatever
// function that address landed in. The write must therefore run with NMI masked.
//
// Independent checks, because none alone is sufficient under a simulator with no NMI:
// behavioural witnesses that the mask happened at all and that it defers to an
// enclosing guard, and a structural one that it BRACKETS the pair rather than merely
// surrounding one end of it, with a store that really does mask the DLI.

// Runtime-valued argument so the vector cannot be constant-folded away.
static volatile u16 g_probe_vector = 0x1234;

[[gnu::noinline]] static void probe_set_raster_vector() {
    A::Hal::set_raster_vector(g_probe_vector);
}

// Behavioural witness. The guard restores NMIEN from the SHADOW, not from whatever
// the register held on entry — so poking the register behind the shadow's back
// leaves a fingerprint only a guarded write can erase. NMIEN is write-only on real
// hardware (a read returns NMIST); under mos-sim $D40E is plain RAM, which is what
// makes this observable at all.
static void test_raster_vector_masks_nmi() {
    A::nmien_set(A::nmien::VBI | A::nmien::DLI);   // shadow and register agree
    *A::reg::NMIEN = 0x55;                         // register only — shadow untouched
    A::os::VDSLST[0] = 0x00;
    A::os::VDSLST[1] = 0x00;

    g_probe_vector = 0x1234;
    probe_set_raster_vector();

    // Masked and then restored from the shadow, so the poke is gone. An unguarded
    // write never touches NMIEN and 0x55 would survive.
    CHECK(*A::reg::NMIEN == (A::nmien::VBI | A::nmien::DLI));
    CHECK(A::g_nmi_guard_depth == 0);              // entry/exit balanced
    // ...and the vector itself still landed, both bytes.
    CHECK(A::os::VDSLST[0] == 0x34);
    CHECK(A::os::VDSLST[1] == 0x12);

    A::nmien_set(A::nmien::VBI);                   // leave the shadow as found
}

// Behavioural witness for nesting. The write is a leaf guard (NmiLeafGuard): it keeps
// no count of its own and restores only when no NmiGuard encloses it. Called under an
// open NmiGuard it must leave NMI masked -- restoring there would re-arm NMI inside
// the OUTER critical section -- and it must not disturb that guard's count, so the
// outer scope still restores on exit.
static void test_raster_vector_defers_to_enclosing_guard() {
    A::nmien_set(A::nmien::VBI | A::nmien::DLI);
    A::os::VDSLST[0] = 0x00;
    A::os::VDSLST[1] = 0x00;
    {
        A::NmiGuard outer;
        CHECK(*A::reg::NMIEN == 0x00);
        CHECK(A::g_nmi_guard_depth == 1);

        g_probe_vector = 0xBEEF;
        probe_set_raster_vector();

        CHECK(*A::reg::NMIEN == 0x00);             // still masked for the outer scope
        CHECK(A::g_nmi_guard_depth == 1);          // count untouched
        CHECK(A::os::VDSLST[0] == 0xEF);
        CHECK(A::os::VDSLST[1] == 0xBE);
    }
    CHECK(*A::reg::NMIEN == (A::nmien::VBI | A::nmien::DLI));  // outer restored
    CHECK(A::g_nmi_guard_depth == 0);

    A::nmien_set(A::nmien::VBI);
}

// Structural oracle. The body is decoded as 6502 instructions from its entry point,
// which is an instruction boundary by definition, so every match below is a real
// opcode and never an operand byte that happens to look like one.

// NMOS 6502 instruction length, from the aaabbbcc opcode layout: cc selects the
// group, bbb the addressing mode. Undocumented opcodes follow their group's modes,
// which is all a length decoder needs.
static u8 insn_len(u8 op) {
    static constexpr u8 g1[8] = {2, 2, 2, 3, 2, 2, 3, 3};  // cc=01/11: (zp,X) zp # abs (zp),Y zp,X abs,Y abs,X
    static constexpr u8 g2[8] = {2, 2, 1, 3, 1, 2, 1, 3};  // cc=10:    #    zp A  abs  --    zp,X  impl  abs,X
    static constexpr u8 g0[8] = {1, 2, 1, 3, 2, 2, 1, 3};  // cc=00:    *    zp impl abs  rel  zp,X  impl  abs,X
    const u8 cc = op & 3, bbb = (op >> 2) & 7;
    if (cc == 1 || cc == 3) return g1[bbb];
    if (cc == 2) return bbb == 0 ? ((op & 0x80) ? 2 : 1) : g2[bbb];
    if (bbb == 0) return op == 0x20 ? 3 : ((op & 0x80) ? 2 : 1);  // JSR abs; BRK/RTI/RTS; #imm
    return g0[bbb];
}

// Anything that can leave the fall-through path: branches, JMP, JSR, RTS, RTI, BRK.
static bool is_control_flow(u8 op) {
    return (op & 0x1F) == 0x10 || op == 0x4C || op == 0x6C || op == 0x20 ||
           op == 0x60 || op == 0x40 || op == 0x00;
}

// STA/STX/STY absolute (8D/8E/8C) and zero-page (85/86/84): stores write memory and
// leave every register alone, so they can sit between a load and the store it feeds.
static bool is_store(u8 op) {
    return op == 0x8D || op == 0x8E || op == 0x8C || op == 0x85 || op == 0x86 || op == 0x84;
}

static bool abs_store_to(const u8* p, u16 addr) {
    const u8 op = p[0];
    if (op != 0x8D && op != 0x8E && op != 0x8C) return false;
    return static_cast<u16>(p[1] | (static_cast<u16>(p[2]) << 8)) == addr;
}

// The immediate load that feeds a store: LDA/LDX/LDY # are A9/A2/A0, paired with
// STA/STX/STY abs by register.
static u8 load_imm_for(u8 store_op) {
    return store_op == 0x8D ? 0xA9 : store_op == 0x8E ? 0xA2 : 0xA0;
}

// Both VDSLST stores must lie strictly between a store to NMIEN and a LATER store to
// NMIEN. That is the property — which of the two VDSLST bytes goes first does not
// matter, only that no NMI can be taken while one of them has landed and the other
// has not. Deleting the guard, or moving it so it no longer spans the pair, breaks
// exactly this.
//
// Position alone is not the whole property, so three more rules make it one:
//  * nothing before the mask store can branch, so the mask is on EVERY path in;
//  * the register that store writes was loaded with an immediate whose DLI bit is
//    clear -- a guard that wrote the shadow on the way in would bracket the pair just
//    as well and leave the DLI armed. With no branch before it, the last load into
//    that register on the straight line IS the value stored;
//  * nothing between the mask and the later VDSLST store can branch, so no path
//    leaves the window with one byte landed;
//  * at least one instruction separates the mask store from the first VDSLST store.
//    A mask that lands too late in the line cannot withdraw a DLI already asserted,
//    and the CPU takes that NMI one instruction after the store -- so with no settle
//    it lands between the pair, exactly the tear the mask exists to prevent (see
//    NmiLeafGuard). No simulator can show this; the tear probe measured it on both
//    emulators, and this rule keeps the settle from being optimised or tidied away.
static void test_raster_vector_write_is_bracketed() {
    // The guarded body is ~24 bytes; the window is generous enough to absorb codegen
    // changes and tight enough that a missing guard runs out of function rather than
    // finding a store in unrelated code. The walk stops at the first complete match.
    // It reads Hal::set_raster_vector itself -- the out-of-line body every caller
    // jumps to -- rather than a wrapper that would only hold the call.
    constexpr u16 kWindow = 64;
    const u8* code = reinterpret_cast<const u8*>(
        reinterpret_cast<uintptr_t>(&A::Hal::set_raster_vector));

    constexpr u8 kMaxInsns = 32;
    u8 at[kMaxInsns];                     // offsets of the decoded instructions
    int n = 0, mask = -1, vlo = -1, vhi = -1, restore = -1;
    int mask_insn = -1, first_vector_insn = -1;   // instruction indices, for the settle
    bool flow_before_mask = false, flow_in_pair = false;
    for (u16 i = 0; i < kWindow && n < kMaxInsns && restore < 0;
         i = static_cast<u16>(i + insn_len(code[i]))) {
        at[n++] = static_cast<u8>(i);
        const bool pair_open = mask >= 0 && (vlo < 0 || vhi < 0);
        const bool vector_store = abs_store_to(code + i, A::os::VDSLST_ADDR) ||
                                  abs_store_to(code + i, A::os::VDSLST_ADDR + 1);
        if (vector_store && first_vector_insn < 0) first_vector_insn = n - 1;
        if (abs_store_to(code + i, A::reg::NMIEN_ADDR)) {
            if (mask < 0)                       { mask = i; mask_insn = n - 1; }
            else if (vlo >= 0 && vhi >= 0)      restore = i;
        } else if (vlo < 0 && abs_store_to(code + i, A::os::VDSLST_ADDR)) {
            vlo = i;
        } else if (vhi < 0 && abs_store_to(code + i, A::os::VDSLST_ADDR + 1)) {
            vhi = i;
        } else if (is_control_flow(code[i])) {
            if (mask < 0) flow_before_mask = true;
            if (pair_open) flow_in_pair = true;
        }
    }

    CHECK(mask >= 0);                    // the mask store exists
    CHECK(vlo >= 0 && vhi >= 0);         // both vector bytes are written
    CHECK(vlo > mask && vhi > mask);     // ...after the mask went down
    CHECK(restore > vlo && restore > vhi);  // ...and the unmask comes after both
    CHECK(!flow_before_mask);            // the mask is unconditional
    CHECK(!flow_in_pair);                // and no path leaves with one byte landed
    CHECK(mask_insn >= 0 && first_vector_insn > mask_insn + 1);  // a settle instruction

    // The mask's value: step back over stores (which leave registers alone) to the
    // instruction that last set a register. It must be the immediate load into the
    // store's own register. Any other shape fails -- the safe direction for a codegen
    // change to break this.
    int load = -1;
    for (int k = n - 1; mask >= 0 && k >= 0; --k) {
        if (at[k] >= mask) continue;
        if (is_store(code[at[k]])) continue;
        if (code[at[k]] == load_imm_for(code[mask])) load = at[k];
        break;
    }
    CHECK(load >= 0);                                             // an immediate mask value
    CHECK(load >= 0 && (code[load + 1] & A::nmien::DLI) == 0);    // ...that masks the DLI
}

int main() {
    test_dli_program();
    test_two_hooks_one_line();
    test_mixed_mode_heights();
    test_lms_stride();
    test_blank_lines_take_no_dli();
    test_out_of_range_scanline();
    test_scanline_zero();
    test_full_capacity_chain();
    test_terminator_stops_walk();
    test_line_boundary_is_half_open();
    test_blank_height_accounting();
    test_raster_vector_masks_nmi();
    test_raster_vector_defers_to_enclosing_guard();
    test_raster_vector_write_is_bracketed();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
