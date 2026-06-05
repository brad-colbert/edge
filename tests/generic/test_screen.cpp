// test_screen.cpp — generic unit tests for engine/display.h and engine/screen.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// These assertions are backend-neutral: the DisplayLayout / ScreenSet compile-time
// geometry, the typed-view pixel/char packing, and the set_screen view-rebind +
// callback behaviour (driven through a MOCK HAL so no real display is touched). The
// backend display-program byte encoding (opcodes, 4K-crossing reloads, DMA bits) is
// asserted separately in tests/backends/atari/test_atari_display.cpp.
//
// The Atari backend is the only one that exists today, so its mode tokens
// (atari::Mode) and display-program builder are named here as the concrete backend
// — but every assertion below is about the engine's portable behaviour, not any
// ANTIC byte.

#include <stdint.h>
#include <stdio.h>

#include <engine/display.h>
#include <engine/screen.h>
#include <engine/platform/atari/platform.h>   // backend mode trait + display-program builder

using engine::u8;
using engine::u16;

using engine::DisplayLayout;
using engine::TextRegion;
using engine::BitmapRegion;
using engine::OverlayRegion;
using engine::TextRegionView;
using engine::BitmapRegionView;
using engine::ScreenSet;

namespace M = atari;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records the installed display-program address and that DMA was cycled, so
// set_screen's portable steps can be checked without a real display backend.
struct MockHal {
    static const u8* last_program;
    static bool      dma_enabled;
    static unsigned  enable_calls;
    static unsigned  disable_calls;

    static void display_dma_disable() { dma_enabled = false; ++disable_calls; }
    static void display_dma_enable()  { dma_enabled = true;  ++enable_calls; }
    static void set_display_program(const u8* p) { last_program = p; }
};
const u8* MockHal::last_program  = nullptr;
bool      MockHal::dma_enabled   = false;
unsigned  MockHal::enable_calls  = 0;
unsigned  MockHal::disable_calls = 0;

struct MockPlatform {
    using hal = MockHal;
    template <typename Layout>
    using display_program = atari::DisplayProgram<Layout>;
};

// ── Test screens / config ─────────────────────────────────────────────

struct ScreenText {
    using display = DisplayLayout<TextRegion<M::Mode::MODE_2, 24>>;
};

struct ScreenMixed {
    using display = DisplayLayout<
        TextRegion<M::Mode::MODE_2, 1>,
        BitmapRegion<M::Mode::BITMAP_E, 180>,
        TextRegion<M::Mode::MODE_2, 1>
    >;
};

struct GameConfig {
    using screens = ScreenSet<ScreenText, ScreenMixed>;
};

// Overlay screens for the DMA-dispatch tests. Kept in a SEPARATE config/manager
// so the existing GameConfig/g_sm and their geometry static_asserts are untouched.
struct ScreenOverlay {        // pure overlay: ANTIC fully disabled
    using display = DisplayLayout<OverlayRegion<M::Mode::VBXE_SR, 240>>;
};
struct ScreenMixedOverlay {   // overlay + ANTIC text: has_overlay, not pure
    using display = DisplayLayout<
        OverlayRegion<M::Mode::VBXE_SR, 200>,
        TextRegion<M::Mode::MODE_2, 3>>;
};
struct OverlayConfig {
    using screens = ScreenSet<ScreenOverlay, ScreenMixedOverlay>;
};

// A pure-overlay-ONLY config has max_screen_ram == 0; ScreenManager must clamp
// the buffer to at least 1 byte so screen_buffer_ is never a zero-length array.
struct PureOverlayOnlyConfig {
    using screens = ScreenSet<ScreenOverlay>;
};
static_assert(
    engine::ScreenManager<MockPlatform, PureOverlayOnlyConfig>::buffer_size == 1,
    "pure-overlay-only ScreenSet clamps the screen buffer to 1 byte");

// ── Compile-time checks: DisplayLayout geometry ───────────────────────

// 1. Single full-screen text region: 40 cols x 24 rows = 960 bytes, 1 region.
using TextLayout = ScreenText::display;
static_assert(TextLayout::region_count == 1, "text layout has 1 region");
static_assert(TextLayout::total_ram == 960,  "40x24 text = 960 bytes");

// 2. Mixed layout: 40 + 7200 + 40 = 7280, offsets 0 / 40 / 7240.
using MixedLayout = ScreenMixed::display;
static_assert(MixedLayout::region_count == 3, "mixed layout has 3 regions");
static_assert(MixedLayout::total_ram == 7280, "40 + 7200 + 40 = 7280");
static_assert(MixedLayout::offset(0) == 0,    "region 0 at offset 0");
static_assert(MixedLayout::offset(1) == 40,   "region 1 after the 40-byte header");
static_assert(MixedLayout::offset(2) == 7240, "region 2 after header + bitmap");

// 3. ScreenSet shared buffer is sized to the largest screen.
using Screens = GameConfig::screens;
static_assert(Screens::screen_count == 2,      "two screens");
static_assert(Screens::max_screen_ram == 7280, "max(960, 7280) == 7280");

// ── Compile-time checks: OverlayRegion + overlay-aware DisplayLayout ────
//
// VBXE overlay regions are VRAM-backed (ram_bytes == 0), carry the wide (u16)
// 320-byte VBXE_SR line, and flip the layout's overlay queries. None of these
// touch the screen buffer.

// 4. OverlayRegion geometry: VRAM-backed, 320 bytes/line (u16, not truncated).
using OverlaySR = OverlayRegion<M::Mode::VBXE_SR, 240>;
static_assert(OverlaySR::ram_bytes      == 0,    "overlay is VRAM-backed, 0 screen RAM");
static_assert(OverlaySR::is_overlay     == true, "OverlayRegion::is_overlay");
static_assert(OverlaySR::bytes_per_line == 320,  "VBXE_SR = 320 bytes/line (u16)");
static_assert(OverlaySR::height         == 240,  "overlay height passes through");

// 5. Pure-overlay layout: ANTIC can be fully disabled, costs 0 screen RAM.
using PureOverlay = DisplayLayout<OverlayRegion<M::Mode::VBXE_SR, 240>>;
static_assert(PureOverlay::is_pure_overlay    == true,  "single overlay is pure");
static_assert(PureOverlay::has_overlay        == true,  "pure overlay has an overlay");
static_assert(PureOverlay::antic_region_count == 0,     "no ANTIC regions");
static_assert(PureOverlay::total_ram          == 0,     "overlay-only = 0 screen RAM");

// 6. Mixed layout: overlay over a 3-row text region. Only the text costs RAM.
using MixedOverlay = DisplayLayout<
    OverlayRegion<M::Mode::VBXE_SR, 200>,
    TextRegion<M::Mode::MODE_2, 3>
>;
static_assert(MixedOverlay::is_pure_overlay    == false, "a text region breaks purity");
static_assert(MixedOverlay::has_overlay        == true,  "still has an overlay");
static_assert(MixedOverlay::antic_region_count == 1,     "one ANTIC (text) region");
static_assert(MixedOverlay::total_ram          == 120,   "40x3 text only; overlay = 0");
static_assert(MixedOverlay::overlay_region_index() == 0, "overlay is region 0");

// 7. Traditional layout: zero overlays. is_pure_overlay must NOT be vacuously true.
static_assert(TextLayout::has_overlay     == false, "no overlay in a pure-text layout");
static_assert(TextLayout::is_pure_overlay == false, "no overlay => not pure overlay");
static_assert(TextLayout::antic_region_count == 1,  "the text region is an ANTIC region");
static_assert(TextLayout::overlay_region_index() == TextLayout::region_count,
              "no overlay => index past the end");

// NOTE (negative test, kept disabled): instantiating OverlayRegion on a non-VBXE
// mode must fail the is_vbxe static_assert. Verified manually, e.g.
//   using BadOverlay = OverlayRegion<M::Mode::MODE_2, 24>;  // static_assert fires
// Left commented out so the suite still builds.

// ── Runtime harness ────────────────────────────────────────────────────

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static u16 addr_of(const void* p) {
    return static_cast<u16>(reinterpret_cast<uintptr_t>(p));
}

// ── TextRegionView::put_char writes the right offset ───────────────────

static void test_text_view_put_char() {
    u8 buf[256] = {};
    TextRegionView<M::Mode::MODE_2, 24> view{buf};

    // (col=5, row=2) -> 2*40 + 5 = 85.
    view.put_char(5, 2, 0x41);
    CHECK(buf[85] == 0x41);
    CHECK(view.get_char(5, 2) == 0x41);

    // (col=0, row=0) -> 0.
    view.put_char(0, 0, 0x21);
    CHECK(buf[0] == 0x21);

    // print converts ASCII to screen codes through the backend trait ('A' -> 0x21).
    view.print(0, 1, "A");
    CHECK(buf[40] == engine::display::traits<M::Mode>::to_screen_code('A'));
}

// ── BitmapRegionView::plot packs 2bpp pixels correctly ─────────────────

static void test_bitmap_view_plot() {
    u8 buf[64] = {};
    BitmapRegionView<M::Mode::BITMAP_E, 180> view{buf};

    // 2bpp, 4 pixels/byte, leftmost pixel in the high bits.
    // plot(0,0,3): top 2 bits -> 0b11000000 = 0xC0.
    view.plot(0, 0, 3);
    CHECK(buf[0] == 0xC0);

    // plot(1,0,1): next 2 bits -> 0b00010000, OR'd in -> 0xD0.
    view.plot(1, 0, 1);
    CHECK(buf[0] == 0xD0);

    // Read-back of both pixels.
    CHECK(view.point(0, 0) == 3);
    CHECK(view.point(1, 0) == 1);

    // A pixel one byte over: plot(4,0,2) -> byte 1 high bits = 0b10000000 = 0x80.
    view.plot(4, 0, 2);
    CHECK(buf[1] == 0x80);
}

// ── ScreenManager::set_screen builds the program and rebinds views ─────

static engine::ScreenManager<MockPlatform, GameConfig> g_sm;

static void test_set_screen() {
    bool callback_ran = false;
    g_sm.set_screen<ScreenMixed>([&]() { callback_ran = true; });
    CHECK(callback_ran);

    const u8* program = g_sm.active_dl();
    const u16 buf     = addr_of(g_sm.buffer());

    // The portable steps ran: DMA re-enabled, a display program installed, and the
    // active program is what the HAL received.
    CHECK(MockHal::dma_enabled);
    CHECK(MockHal::last_program == program);
    CHECK(program != nullptr);
    CHECK(g_sm.active_dl_size() > 0);

    // Typed views bind to their buffer slices (text vs bitmap is compile-time):
    // region 0 at buffer+0, region 1 at buffer+40 (after the 40-byte header).
    auto& header = g_sm.region<ScreenMixed, 0>();
    auto& field  = g_sm.region<ScreenMixed, 1>();
    CHECK(addr_of(header.ptr) == buf);
    CHECK(addr_of(field.ptr) == static_cast<u16>(buf + 40));

    // And the views actually draw into the shared buffer at the right place.
    field.plot(0, 0, 3);
    CHECK(g_sm.buffer()[40] == 0xC0);
}

// ── set_screen drives ANTIC DMA from the layout's overlay properties ───

static engine::ScreenManager<MockPlatform, OverlayConfig> g_sm_overlay;

// A pure-overlay screen disables ANTIC DMA and never re-enables it: the program
// (the 3-byte JVB stub) is still installed as a DLISTL safety pointer.
static void test_pure_overlay_no_dma_enable() {
    MockHal::enable_calls  = 0;
    MockHal::disable_calls = 0;
    g_sm_overlay.set_screen<ScreenOverlay>([]() {});

    CHECK(MockHal::disable_calls == 1);
    CHECK(MockHal::enable_calls  == 0);   // ANTIC stays off for a pure overlay
    CHECK(!MockHal::dma_enabled);
    CHECK(MockHal::last_program == g_sm_overlay.active_dl());
    CHECK(g_sm_overlay.active_dl_size() == 3);   // JVB stub
}

// A mixed overlay+ANTIC screen DOES re-enable DMA (the ANTIC text region needs
// the playfield fetch); the overlay blank lines just cost DL DMA.
static void test_mixed_overlay_enables_dma() {
    MockHal::enable_calls  = 0;
    MockHal::disable_calls = 0;
    g_sm_overlay.set_screen<ScreenMixedOverlay>([]() {});

    CHECK(MockHal::disable_calls == 1);
    CHECK(MockHal::enable_calls  == 1);   // ANTIC region present => enabled
    CHECK(MockHal::dma_enabled);
}

int main() {
    test_text_view_put_char();
    test_bitmap_view_plot();
    test_set_screen();
    test_pure_overlay_no_dma_enable();
    test_mixed_overlay_enables_dma();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
