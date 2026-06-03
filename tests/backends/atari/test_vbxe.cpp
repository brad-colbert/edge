// test_vbxe.cpp — unit tests for the VBXE access layer.
//
// Phase 2: register addressing off Config::reg_base, MEMAC window geometry /
// bank math (MEMAC-A and MEMAC-B), and the VRAM layout.
// Phase 3: BCB packing/builders, the XDL byte layout, and the default palette.
//
// Everything here is compile-time (static_assert) — these files are types and
// constants, so the checks must hold at build time. The same checks are mirrored
// as runtime CHECKs so the mos-sim harness sees a passing executable. The tests
// deliberately do NOT call the hardware-touching paths (MEMAC init/read/write,
// blitter submit, palette upload) — those drive real MMIO.

#include <stdio.h>

#include <engine/platform/atari/vbxe_blitter.h>
#include <engine/platform/atari/vbxe_config.h>
#include <engine/platform/atari/vbxe_layout.h>
#include <engine/platform/atari/vbxe_memac.h>
#include <engine/platform/atari/vbxe_palette.h>
#include <engine/platform/atari/vbxe_registers.h>
#include <engine/platform/atari/vbxe_xdl.h>

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

// ── 5. Blitter: BCB packing + builders ───────────────────────────────
static_assert(sizeof(v::BCB) == 21, "BCB is 21 bytes");

// pack_addr: 0x12345 -> {0x45, 0x23, 0x01} (19-bit LE).
struct AddrBytes { v::u8 b[3]; };
constexpr AddrBytes pack_addr_(engine::u32 a) { AddrBytes r{}; v::pack_addr(r.b, a); return r; }
static_assert(pack_addr_(0x12345u).b[0] == 0x45, "pack_addr lo");
static_assert(pack_addr_(0x12345u).b[1] == 0x23, "pack_addr mid");
static_assert(pack_addr_(0x12345u).b[2] == 0x01, "pack_addr hi");

// pack_step_y: signed 12-bit LE. -1 -> {0xFF,0x0F}; +160 -> {0xA0,0x00}.
struct StepBytes { v::u8 b[2]; };
constexpr StepBytes pack_step_(engine::i16 s) { StepBytes r{}; v::pack_step_y(r.b, s); return r; }
static_assert(pack_step_(-1).b[0] == 0xFF && pack_step_(-1).b[1] == 0x0F, "pack_step -1");
static_assert(pack_step_(160).b[0] == 0xA0 && pack_step_(160).b[1] == 0x00, "pack_step +160");

// bcb_clear: constant-source fill (and_mask 0, xor_mask = colour), COPY + NEXT.
constexpr v::BCB kClear = v::bcb_clear(0x10000u, 320, 240, 320, 0x2A);
static_assert(kClear.blt_and_mask == 0x00,            "clear: constant source");
static_assert(kClear.blt_xor_mask == 0x2A,            "clear: colour in xor_mask");
static_assert(kClear.blt_height == 239,               "clear: height-1");
static_assert(kClear.blt_width[0] == 0x3F && kClear.blt_width[1] == 0x01, "clear: width-1 = 319");
static_assert((kClear.blt_control & v::blt_mode::NEXT) != 0,           "clear: NEXT set");
static_assert((kClear.blt_control & 0x07) == v::blt_mode::COPY,        "clear: COPY mode");

// bcb_sprite: real source (and_mask 0xFF), TRANSPARENT + NEXT. step_y is the FULL
// stride (row-base advance), not stride-width: source 16, dest 320 (= 0x140).
constexpr v::BCB kSpr = v::bcb_sprite(0x20000u, 0x10000u, 16, 16, 16, 320);
static_assert(kSpr.blt_and_mask == 0xFF,                              "sprite: real source");
static_assert((kSpr.blt_control & 0x07) == v::blt_mode::TRANSPARENT,  "sprite: TRANSPARENT mode");
static_assert(kSpr.source_step_y[0] == 0x10 && kSpr.source_step_y[1] == 0x00,
                                                      "sprite: source step_y = src_stride 16");
static_assert(kSpr.dest_step_y[0] == 0x40 && kSpr.dest_step_y[1] == 0x01,
                                                      "sprite: dest step_y = dest_stride 320");

// bcb_sprite_colored: 1bpp-expanded sprite coloured via the AND-mask (set->color,
// 0->transparent), still TRANSPARENT mode.
constexpr v::BCB kSprC = v::bcb_sprite_colored(0x20000u, 0x10000u, 8, 8, 8, 320, 0x1E);
static_assert(kSprC.blt_and_mask == 0x1E,                            "colored sprite: and_mask = color");
static_assert(kSprC.blt_xor_mask == 0x00,                            "colored sprite: no xor");
static_assert((kSprC.blt_control & 0x07) == v::blt_mode::TRANSPARENT, "colored sprite: TRANSPARENT mode");
static_assert(kSprC.blt_width[0] == 0x07 && kSprC.blt_width[1] == 0x00, "colored sprite: width-1 = 7");
static_assert(kSprC.dest_step_y[0] == 0x40 && kSprC.dest_step_y[1] == 0x01,
                                                      "colored sprite: dest step_y = dest_stride 320");

// ── 6. XDL byte layout (full-screen SR_320) ──────────────────────────
struct XdlResult { v::u8 buf[24]; v::u8 len; };
constexpr XdlResult build_xdl() {
    XdlResult r{};
    r.len = v::build_fullscreen_xdl<CfgDefault>(r.buf, 0x12345u, 320, 240);
    return r;
}
constexpr XdlResult kXdl = build_xdl();
// ctrl = GMON|RPTL|OVADR|ATT|END = 0x8862 (LE: 0x62, 0x88).
static_assert(kXdl.len == 10,        "SR_320 fullscreen XDL is 10 bytes");
static_assert(kXdl.buf[0] == 0x62,   "XDLC lo");
static_assert(kXdl.buf[1] == 0x88,   "XDLC hi");
static_assert(kXdl.buf[2] == 239,    "RPTL = height-1");
static_assert(kXdl.buf[3] == 0x45 && kXdl.buf[4] == 0x23 && kXdl.buf[5] == 0x01,
                                     "OVADR address bytes");
static_assert(kXdl.buf[6] == 0x40 && kXdl.buf[7] == 0x01, "OVSTEP = 320");
static_assert(kXdl.buf[8] == static_cast<v::u8>(v::ov_width::NORMAL | (v::ATT_OV_PALETTE << 4)),
                                     "ATT width NORMAL + OV palette 1");
static_assert(kXdl.buf[8] == 0x11,   "ATT byte1 = 0x11 (normal width, OV palette 1)");
static_assert(kXdl.buf[9] == 0xFF,   "ATT priority 255");

// ── 6b. XDL byte layout (full-screen Text_80) ────────────────────────
// Text mode adds TMON + CHBASE; the hardware advances OVADR by OVSTEP every 8
// scanlines (one record covers all rows). FX Core manual pp.9,14.
using CfgText = v::Config<v::Mode::Text_80>;
struct XdlTextResult { v::u8 buf[24]; v::u8 len; };
constexpr XdlTextResult build_text_xdl() {
    XdlTextResult r{};
    r.len = v::build_fullscreen_xdl<CfgText>(r.buf, 0x12345u, 160, 240);
    return r;
}
constexpr XdlTextResult kTextXdl = build_text_xdl();
// ctrl = TMON|RPTL|OVADR|CHBASE|ATT|END = 0x8961 (LE: 0x61, 0x89).
static_assert(kTextXdl.len == 11,      "Text_80 fullscreen XDL is 11 bytes (adds CHBASE)");
static_assert(kTextXdl.buf[0] == 0x61, "text XDLC lo (TMON set, GMON clear)");
static_assert(kTextXdl.buf[1] == 0x89, "text XDLC hi (CHBASE set)");
static_assert(kTextXdl.buf[2] == 239,  "text RPTL = height-1");
static_assert(kTextXdl.buf[6] == 0xA0 && kTextXdl.buf[7] == 0x00, "text OVSTEP = 160 (80 cols x 2)");
static_assert(kTextXdl.buf[8] == static_cast<v::u8>(v::VRAMLayout<CfgText>::fonts >> 11),
                                       "text CHBASE = fonts >> 11 (2K page)");
static_assert(kTextXdl.buf[9]  == 0x11, "text ATT byte1 = 0x11 (normal width, OV palette 1)");
static_assert(kTextXdl.buf[10] == 0xFF, "text ATT priority 255");

// ── 7. Default palette ───────────────────────────────────────────────
static_assert(sizeof(v::default_palette_0.entries) / sizeof(v::PaletteEntry) == 256,
                                     "default palette has 256 entries");
static_assert(v::default_palette_0.entries[0].r == 0x00 &&
              v::default_palette_0.entries[0].g == 0x00 &&
              v::default_palette_0.entries[0].b == 0x00, "palette[0] is black");
static_assert(v::default_palette_0.entries[1].r == 0x26 &&
              v::default_palette_0.entries[1].g == 0x26 &&
              v::default_palette_0.entries[1].b == 0x26, "palette[1] from laoo data");

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

static void test_blitter() {
    CHECK(sizeof(v::BCB) == 21);
    CHECK(kClear.blt_and_mask == 0x00);
    CHECK(kClear.blt_xor_mask == 0x2A);
    CHECK((kClear.blt_control & v::blt_mode::NEXT) != 0);
    CHECK(kSpr.blt_and_mask == 0xFF);

    // BlitterQueue push/full/reset (pure RAM, no MMIO).
    v::BlitterQueue<2> q;
    CHECK(q.empty());
    CHECK(q.push(kClear));
    CHECK(q.push(kSpr));
    CHECK(q.full());
    CHECK(!q.push(kClear));        // full -> rejected
    CHECK(q.count() == 2);
    q.reset();
    CHECK(q.empty());
}

static void test_xdl() {
    CHECK(kXdl.len == 10);
    CHECK(kXdl.buf[0] == 0x62 && kXdl.buf[1] == 0x88);
    CHECK(kXdl.buf[2] == 239);
    CHECK(kXdl.buf[9] == 0xFF);
}

static void test_palette() {
    CHECK(v::default_palette_0.entries[0].r == 0x00);
    CHECK(v::default_palette_0.entries[1].g == 0x26);
}

int main() {
    test_registers();
    test_memac_a();
    test_memac_b();
    test_layout();
    test_blitter();
    test_xdl();
    test_palette();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
