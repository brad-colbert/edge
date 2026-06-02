// test_vbxe.cpp — unit tests for the VBXE Phase 2 access layer: register
// addressing off Config::reg_base, the MEMAC window geometry / bank math (both
// MEMAC-A and MEMAC-B), and the VRAM layout.
//
// Everything here is compile-time (static_assert) — these files are types and
// constants, so the checks must hold at build time. The same checks are mirrored
// as runtime CHECKs so the mos-sim harness sees a passing executable. The tests
// deliberately do NOT call init()/read()/write() (those drive real MMIO).

#include <stdio.h>

#include <engine/platform/atari/vbxe_config.h>
#include <engine/platform/atari/vbxe_layout.h>
#include <engine/platform/atari/vbxe_memac.h>
#include <engine/platform/atari/vbxe_registers.h>

namespace v = atari::vbxe;

// ── Config fixtures ──────────────────────────────────────────────────
using CfgDefault = v::Config<>;                          // D640, SR_320, single, MEMAC-A 4K
using CfgD740    = v::Config<v::Mode::SR_320, v::Buffers::Single, v::RegBase::D740>;
using Cfg8K      = v::Config<v::Mode::SR_320, v::Buffers::Single, v::RegBase::D640,
                             v::MEMAC_A_Cfg<0xB0, v::WindowSize::_8K>>;
using CfgB       = v::Config<v::Mode::SR_320, v::Buffers::Single, v::RegBase::D640, v::MEMAC_B>;
using CfgDouble  = v::Config<v::Mode::SR_320, v::Buffers::Double>;

// ── 1. Register addressing off reg_base ──────────────────────────────
static_assert(v::Regs<CfgDefault>::base == 0xD640,                 "default base $D640");
static_assert(v::Regs<CfgDefault>::VIDEO_CONTROL_ADDR == 0xD640,   "VIDEO_CONTROL @ base+0");
static_assert(v::Regs<CfgDefault>::XDL_ADR0_ADDR == 0xD641,        "XDL_ADR0 @ base+1");
static_assert(v::Regs<CfgDefault>::BLITTER_START_ADDR == 0xD653,   "BLITTER_START @ base+0x13");
static_assert(v::Regs<CfgDefault>::MEMAC_CONTROL_ADDR == 0xD65E,   "MEMAC_CONTROL @ base+0x1E");
static_assert(v::Regs<CfgDefault>::MEMAC_BANK_SEL_ADDR == 0xD65F,  "MEMAC_BANK_SEL @ base+0x1F");
// Read-side aliases share their write-side address.
static_assert(v::Regs<CfgDefault>::COLDETECT_ADDR == v::Regs<CfgDefault>::COLCLR_ADDR,
                                                                   "COLDETECT == COLCLR addr");
// D740 install shifts every register by $0100.
static_assert(v::Regs<CfgD740>::base == 0xD740,                    "D740 base");
static_assert(v::Regs<CfgD740>::VIDEO_CONTROL_ADDR == 0xD740,      "D740 VIDEO_CONTROL");
static_assert(v::Regs<CfgD740>::MEMAC_BANK_SEL_ADDR == 0xD75F,     "D740 MEMAC_BANK_SEL");

// ── 2. MEMAC-A window geometry + bank math ───────────────────────────
static_assert(v::MemacWindow<CfgDefault>::is_b == false,           "default is MEMAC-A");
static_assert(v::MemacWindow<CfgDefault>::cpu_base == 0xB000,      "MEMAC-A base $B000");
static_assert(v::MemacWindow<CfgDefault>::window_bytes == 4096,    "default 4K window");
static_assert(v::MemacWindow<CfgDefault>::Window::bank_shift == 12, "4K -> shift 12");
static_assert(v::MemacWindow<CfgDefault>::Window::offset_mask == 0x0FFF, "4K offset mask");
static_assert(v::MemacWindow<CfgDefault>::bank_or_mask == v::memac_bank_sel::MGE,
                                                                   "MEMAC-A OR-mask = MGE");
static_assert(v::MemacWindow<CfgDefault>::bank_sel_addr == 0xD65F, "A bank-sel = MEMAC_BANK_SEL");
// Sample linear VRAM address splits as the hardware expects (bank = addr/4096).
static_assert((0x12345u >> v::MemacWindow<CfgDefault>::Window::bank_shift) == 0x12,
                                                                   "0x12345 -> bank 0x12 @4K");
static_assert((0x12345u & v::MemacWindow<CfgDefault>::Window::offset_mask) == 0x345,
                                                                   "0x12345 -> offset 0x345 @4K");
// 8K window changes geometry only.
static_assert(v::MemacWindow<Cfg8K>::window_bytes == 8192,         "8K window");
static_assert(v::MemacWindow<Cfg8K>::Window::bank_shift == 13,     "8K -> shift 13");

// ── 3. MEMAC-B window geometry + bank math ───────────────────────────
static_assert(v::is_memac_b<v::MEMAC_B>::value == true,            "is_memac_b<MEMAC_B>");
static_assert(v::MemacWindow<CfgB>::is_b == true,                  "CfgB is MEMAC-B");
static_assert(v::MemacWindow<CfgB>::cpu_base == 0x4000,            "MEMAC-B base $4000");
static_assert(v::MemacWindow<CfgB>::window_bytes == 16384,         "MEMAC-B 16K window");
static_assert(v::MemacWindow<CfgB>::Window::bank_shift == 14,      "16K -> shift 14");
static_assert(v::MemacWindow<CfgB>::bank_sel_addr == 0xD65D,       "B bank-sel = MEMAC_B_CONTROL");
static_assert(v::MemacWindow<CfgB>::bank_or_mask == v::memac_b_control::MBCE,
                                                                   "MEMAC-B OR-mask = MBCE");
static_assert((0x12345u >> v::MemacWindow<CfgB>::Window::bank_shift) == 0x4,
                                                                   "0x12345 -> bank 4 @16K");

// ── 4. VRAM layout ───────────────────────────────────────────────────
using LDef = v::VRAMLayout<CfgDefault>;
using LDbl = v::VRAMLayout<CfgDouble>;

// NB: 320u*240u would overflow 16-bit `unsigned int` on the mos target; use a
// literal large enough to be typed as a 32-bit unsigned.
static_assert(CfgDefault::fb_bytes == 76800u,             "SR_320 framebuffer 76800 bytes");
static_assert(LDef::fb_a == 0,                            "fb_a at vram_offset 0");
static_assert(LDef::fb_b_size == 0,                       "single-buffer: no fb_b");
static_assert(LDef::fb_b == LDef::shapes,                 "single-buffer: fb_b coincides w/ shapes");
static_assert(LDef::xdl < LDef::bcb_queue && LDef::bcb_queue < LDef::fonts,
                                                          "tail order xdl<bcb_queue<fonts");
static_assert(LDef::fonts + LDef::fonts_size <= LDef::vram_size, "layout fits in 512KB");
// Double-buffering reserves a second framebuffer and pushes shapes/user up.
static_assert(LDbl::fb_b_size == CfgDouble::fb_bytes,     "double-buffer: fb_b sized");
static_assert(LDbl::shapes == LDef::shapes + CfgDouble::fb_bytes,
                                                          "double-buffer shifts shapes by fb");
static_assert(LDbl::shapes_size < LDef::shapes_size,      "double-buffer shrinks shapes area");

// ── Runtime mirror (for the harness) ─────────────────────────────────

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static void test_registers() {
    CHECK(v::Regs<CfgDefault>::VIDEO_CONTROL_ADDR == 0xD640);
    CHECK(v::Regs<CfgDefault>::MEMAC_BANK_SEL_ADDR == 0xD65F);
    CHECK(v::Regs<CfgD740>::VIDEO_CONTROL_ADDR == 0xD740);
}

static void test_memac_a() {
    CHECK(v::MemacWindow<CfgDefault>::is_b == false);
    CHECK(v::MemacWindow<CfgDefault>::cpu_base == 0xB000);
    CHECK(v::MemacWindow<CfgDefault>::window_bytes == 4096);
    CHECK(v::MemacWindow<Cfg8K>::window_bytes == 8192);
}

static void test_memac_b() {
    CHECK(v::MemacWindow<CfgB>::is_b == true);
    CHECK(v::MemacWindow<CfgB>::cpu_base == 0x4000);
    CHECK(v::MemacWindow<CfgB>::window_bytes == 16384);
    CHECK(v::MemacWindow<CfgB>::bank_sel_addr == 0xD65D);
}

static void test_layout() {
    CHECK(LDef::fb_b_size == 0);
    CHECK(LDef::fb_b == LDef::shapes);
    CHECK(LDef::fonts + LDef::fonts_size <= LDef::vram_size);
    CHECK(LDbl::fb_b_size == CfgDouble::fb_bytes);
    CHECK(LDbl::shapes_size < LDef::shapes_size);
}

int main() {
    test_registers();
    test_memac_a();
    test_memac_b();
    test_layout();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
