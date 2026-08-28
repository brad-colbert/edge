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

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
