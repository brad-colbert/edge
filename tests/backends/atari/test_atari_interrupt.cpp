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

int main() {
    test_dli_program();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
