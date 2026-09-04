// test_interrupt.cpp — unit tests for engine/interrupt.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// The InterruptManager is exercised against a MOCK platform whose HAL returns
// sentinel dispatcher/terminal addresses, so the chain-building tables can be
// asserted exactly without depending on any backend hardware. The backend
// raster-delivery walker is tested separately (tests/backends/atari).

#include <stdint.h>
#include <stdio.h>

#include <engine/interrupt.h>

using engine::u8;
using engine::u16;

// ── Mock platform ─────────────────────────────────────────────────────

struct MockHal {
    static constexpr u16 DISPATCH = 0xD15A;
    static constexpr u16 TERMINAL = 0xD160;

    static u16 raster_dispatch_addr() { return DISPATCH; }
    static u16 raster_terminal_addr() { return TERMINAL; }

    // Raster delivery — recorded so prepare_chain's hardware arming is observable.
    // program_raster_lines is a no-op here; the backend walker is tested directly
    // (tests/backends/atari) against the backend HAL.
    static u16  raster_vector;
    static bool raster_enabled;
    static void program_raster_lines(u8*, u16, const u8*, u8) {}
    static void set_raster_vector(u16 a) { raster_vector = a; }
    static void enable_raster()  { raster_enabled = true; }
    static void disable_raster() { raster_enabled = false; }

    // Dispatcher install — record the args so arm_dispatch's wiring is observable.
    static bool dispatch_armed;
    static u16  arm_cur, arm_cnt, arm_hlo, arm_hhi, arm_nlo, arm_nhi;
    static void install_raster_dispatch(u16 cur, u16 count, u16 hlo, u16 hhi,
                                        u16 nlo, u16 nhi) {
        dispatch_armed = true;
        arm_cur = cur; arm_cnt = count;
        arm_hlo = hlo; arm_hhi = hhi; arm_nlo = nlo; arm_nhi = nhi;
    }

    // RasterContext stores — stubbed (RasterContext is compiled, not exercised).
    static u8 last_colpf0;
    static void write_colpf0(u8 v) { last_colpf0 = v; }
    static void write_colpf1(u8) {}
    static void write_colpf2(u8) {}
    static void write_colpf3(u8) {}
    static void write_colbk (u8) {}
    static void set_charset_base(u8) {}
    static void set_fine_scroll_x(u8) {}
    static void set_fine_scroll_y(u8) {}
};
u8   MockHal::last_colpf0    = 0;
u16  MockHal::raster_vector         = 0;
bool MockHal::raster_enabled    = false;
bool MockHal::dispatch_armed = false;
u16  MockHal::arm_cur = 0, MockHal::arm_cnt = 0, MockHal::arm_hlo = 0,
     MockHal::arm_hhi = 0, MockHal::arm_nlo = 0, MockHal::arm_nhi = 0;

struct MockPlatform {
    using hal = MockHal;
};

using IM = engine::InterruptManager<MockPlatform>;   // defaults: 12 raster hooks, 4 frame hooks

static u16 faddr(void (*f)()) {
    return static_cast<u16>(reinterpret_cast<uintptr_t>(f));
}

// ── Dummy handlers (distinct addresses) ───────────────────────────────

static void h_a() {}
static void h_b() {}
static void h_c() {}
static void v_1() {}
static void v_2() {}

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Sort by scanline + table construction ─────────────────────────────

static void test_sort_and_tables() {
    IM im;
    im.add_raster_hook(40, h_a);
    im.add_raster_hook(80, h_b);
    im.add_raster_hook(20, h_c);
    im.prepare_chain();

    // Sorted ascending by scanline: 20(h_c), 40(h_a), 80(h_b).
    CHECK(im.raster_hook_count() == 3);
    CHECK(im.slot(0).scanline == 20);
    CHECK(im.slot(1).scanline == 40);
    CHECK(im.slot(2).scanline == 80);
    CHECK(im.slot(0).handler == h_c);
    CHECK(im.slot(1).handler == h_a);
    CHECK(im.slot(2).handler == h_b);

    // handler_*[i] holds each slot's user function address in sorted order.
    CHECK(im.handler_lo(0) == (faddr(h_c) & 0xFF));
    CHECK(im.handler_hi(0) == (faddr(h_c) >> 8));
    CHECK(im.handler_lo(1) == (faddr(h_a) & 0xFF));
    CHECK(im.handler_hi(2) == (faddr(h_b) >> 8));

    // next[i] = entry(slot i+1); all three are C++ handlers, so the first two
    // chain to the dispatcher and the last to the terminal.
    CHECK(im.next_lo(0) == (MockHal::DISPATCH & 0xFF));
    CHECK(im.next_hi(0) == (MockHal::DISPATCH >> 8));
    CHECK(im.next_lo(1) == (MockHal::DISPATCH & 0xFF));
    CHECK(im.next_hi(1) == (MockHal::DISPATCH >> 8));
    CHECK(im.next_lo(2) == (MockHal::TERMINAL & 0xFF));
    CHECK(im.next_hi(2) == (MockHal::TERMINAL >> 8));

    // First raster hook of the frame enters via the dispatcher (slot 0 is C++).
    CHECK(im.first_handler_addr() == MockHal::DISPATCH);
}

// ── Raw vs C++ next-entry: raw next points straight at the raw handler ─

static void test_raw_next_entry() {
    IM im;
    im.add_raster_hook(30, h_a);        // C++
    im.add_raw_raster_hook(60, h_b);    // raw
    im.prepare_chain();

    CHECK(im.slot(0).scanline == 30);
    CHECK(im.slot(1).scanline == 60);
    // next[0] = entry(slot 1) and slot 1 is raw -> its handler address directly.
    CHECK(im.next_lo(0) == (faddr(h_b) & 0xFF));
    CHECK(im.next_hi(0) == (faddr(h_b) >> 8));
    // Slot 0 is C++, so the frame's first entry is the dispatcher.
    CHECK(im.first_handler_addr() == MockHal::DISPATCH);
}

// ── Priority ordering on a shared scanline ─────────────────────────────

static void test_priority_ordering() {
    IM im;
    im.add_raster_hook(50, h_a);            // static, priority USER (2)
    im.begin_dynamic();
    im.add_dynamic_raster_hook(50, h_b);    // dynamic, priority MULTIPLEX (0)
    im.prepare_chain();

    CHECK(im.raster_hook_count() == 2);
    // Same scanline -> priority 0 (multiplex) sorts before priority 2 (user).
    CHECK(im.slot(0).handler == h_b);
    CHECK((im.slot(0).flags & engine::raster::PRIO_MASK) == 0);
    CHECK(im.slot(0).flags & engine::raster::FLAG_RAW);   // dynamic is always raw
    CHECK(im.slot(1).handler == h_a);
    CHECK(((im.slot(1).flags & engine::raster::PRIO_MASK) >> engine::raster::PRIO_SHIFT)
          == engine::raster::PRIO_USER);
}

// ── clear_transient keeps persistent raster hooks ──────────────────────────────

static void test_clear_transient() {
    IM im;
    im.add_raster_hook(30, h_a);              // transient
    im.add_persistent_raster_hook(60, h_b);   // persistent
    CHECK(im.raster_hook_count() == 2);

    im.clear_transient();
    CHECK(im.raster_hook_count() == 1);
    CHECK(im.slot(0).handler == h_b);
    CHECK(im.slot(0).flags & engine::raster::FLAG_PERSISTENT);
}

// ── Dynamic raster hooks merge with static and reset on begin_dynamic ──────────

static void test_dynamic_lifecycle() {
    IM im;
    im.add_raster_hook(10, h_a);              // static

    im.begin_dynamic();
    im.add_dynamic_raster_hook(20, h_b);
    im.add_dynamic_raster_hook(30, h_c);
    CHECK(im.static_raster_hook_count() == 1);
    CHECK(im.raster_hook_count() == 3);       // static + 2 dynamic

    im.prepare_chain();
    CHECK(im.slot(0).scanline == 10);
    CHECK(im.slot(1).scanline == 20);
    CHECK(im.slot(2).scanline == 30);

    // Next frame: dynamic tail is discarded, static survives.
    im.begin_dynamic();
    CHECK(im.raster_hook_count() == 1);
    CHECK(im.static_raster_hook_count() == 1);
}

// ── prepare_chain arms the raster vector + delivery (via MockHal) ──────

static void test_prepare_chain_arms() {
    u8 dummy_dl[] = { 0x41, 0x00, 0x40 };   // content irrelevant (mock no-op)

    IM im;
    im.add_raster_hook(40, h_a);
    im.add_raster_hook(80, h_b);
    im.prepare_chain(dummy_dl, sizeof(dummy_dl));
    CHECK(MockHal::raster_vector == im.first_handler_addr());
    CHECK(MockHal::raster_vector == MockHal::DISPATCH);     // slot 0 is a C++ handler
    CHECK(MockHal::raster_enabled == true);

    // Empty chain: the raster vector points at the terminal and delivery is off.
    IM im2;
    im2.prepare_chain(dummy_dl, sizeof(dummy_dl));
    CHECK(MockHal::raster_vector == MockHal::TERMINAL);
    CHECK(MockHal::raster_enabled == false);
}

// ── arm_dispatch forwards the table/current_ addresses to the HAL ──────

static void test_arm_dispatch() {
    MockHal::dispatch_armed = false;
    IM im;
    im.arm_dispatch();
    CHECK(MockHal::dispatch_armed);
    // The handler/next table bases, current_, and total_count_ are distinct addrs.
    CHECK(MockHal::arm_cur != 0);
    CHECK(MockHal::arm_cnt != 0);
    CHECK(MockHal::arm_cnt != MockHal::arm_cur);   // total_count_ vs current_
    CHECK(MockHal::arm_hlo != 0);
    CHECK(MockHal::arm_hlo != MockHal::arm_hhi);   // handler_lo_ vs handler_hi_
    CHECK(MockHal::arm_nlo != MockHal::arm_nhi);   // next_lo_   vs next_hi_
    CHECK(MockHal::arm_hlo != MockHal::arm_nlo);   // handler vs next tables
}

// ── Frame hooks: add 2, remove 1 ───────────────────────────────────────

static void test_frame_hooks() {
    IM im;
    im.add_frame_hook(v_1);
    im.add_frame_hook(v_2);
    CHECK(im.frame_hook_count() == 2);

    im.remove_frame_hook(v_1);
    CHECK(im.frame_hook_count() == 1);
}

// ── Unused next_ entries hold the terminal, never zero ────────────────
//
// The backend's chain tail is shared by RAW handlers, and a raw handler — unlike the
// C++ dispatcher, which resyncs an out-of-range index at its head — is entered by the
// hardware directly and indexes next_*[current_] with no check. current_ sits one past
// the end whenever a raster interrupt fires before the frame service re-armed the
// chain. Left zero, that entry made the tail install 0000 and the next interrupt
// jumped to address zero (observed on ATank, 2026-08-30, with next_[1] == 0 read out
// of the wedged machine). Every entry the chain does not use must be the terminal.

static bool is_terminal(const IM& im, u8 i) {
    return im.next_lo(i) == (MockHal::TERMINAL & 0xFF) &&
           im.next_hi(i) == (MockHal::TERMINAL >> 8);
}

// The one-hook chain — the exact shape that sent the walk to address zero.
static void test_next_table_one_hook_is_not_zero() {
    IM im;
    im.add_raster_hook(50, h_a);
    im.prepare_chain();

    CHECK(is_terminal(im, 0));                              // the only live slot
    CHECK((im.next_lo(1) | im.next_hi(1)) != 0);            // NOT address zero
    CHECK(is_terminal(im, 1));                              // the tail's stale index
    // ...and so is every remaining entry, up to and INCLUDING the spare one past
    // capacity, which is the highest index a raw tail can reach.
    for (u8 i = 1; i <= IM::capacity(); ++i) CHECK(is_terminal(im, i));
}

// No hooks at all: nothing is live, so index 0 is a terminal too.
static void test_next_table_empty_chain() {
    IM im;
    im.prepare_chain();
    for (u8 i = 0; i <= IM::capacity(); ++i) CHECK(is_terminal(im, i));
}

// A chain filled to capacity still terminal-fills the spare entry past the last slot
// — the case a `< MaxRasterHooks` bound would leave at zero.
static void test_next_table_full_capacity_spare() {
    IM im;
    for (u8 i = 0; i < IM::capacity(); ++i)
        im.add_raster_hook(static_cast<u8>(10 + i * 4), h_a);
    im.prepare_chain();

    CHECK(im.raster_hook_count() == IM::capacity());
    CHECK(is_terminal(im, static_cast<u8>(IM::capacity() - 1)));  // last live slot
    CHECK(is_terminal(im, IM::capacity()));                       // the spare
}

// A SHRINKING chain must not leave the previous build's live entries behind. A stale
// dispatcher address at a reachable index is the same defect wearing a plausible
// value instead of zero.
static void test_next_table_shrink_clears_stale() {
    IM im;
    im.add_raster_hook(10, h_a);
    im.add_raster_hook(20, h_b);
    im.add_raster_hook(30, h_c);
    im.prepare_chain();
    CHECK(im.next_lo(0) == (MockHal::DISPATCH & 0xFF));   // slots 0,1 chain onward
    CHECK(im.next_lo(1) == (MockHal::DISPATCH & 0xFF));

    im.remove_raster_hook(20);
    im.remove_raster_hook(30);
    im.prepare_chain();

    CHECK(im.raster_hook_count() == 1);
    CHECK(is_terminal(im, 0));
    CHECK(is_terminal(im, 1));                            // was DISPATCH
    CHECK(is_terminal(im, 2));
}

// next_raster_addr() — the portable mirror of the raw tail. In range it walks the
// chain; at or past the live count it yields the terminal and the index stops
// climbing, so a hook that keeps firing can never walk off the table.
static void test_next_raster_addr_clamps() {
    IM im;
    im.add_raster_hook(10, h_a);        // C++
    im.add_raw_raster_hook(20, h_b);    // raw
    im.prepare_chain();
    CHECK(im.chain_index() == 0);

    CHECK(im.next_raster_addr() == faddr(h_b));         // slot 0 -> the raw slot 1
    CHECK(im.next_raster_addr() == MockHal::TERMINAL);  // slot 1 -> end of chain

    // Now out of range: the first such call parks the index one past the live count,
    // and every call after it reads the same terminal entry and leaves it there.
    CHECK(im.next_raster_addr() == MockHal::TERMINAL);
    const u8 saturated = im.chain_index();
    for (u8 k = 0; k < 8; ++k) {
        CHECK(im.next_raster_addr() == MockHal::TERMINAL);
        CHECK(im.chain_index() == saturated);           // index no longer climbs
    }
}

int main() {
    test_sort_and_tables();
    test_raw_next_entry();
    test_priority_ordering();
    test_clear_transient();
    test_dynamic_lifecycle();
    test_prepare_chain_arms();
    test_arm_dispatch();
    test_frame_hooks();
    test_next_table_one_hook_is_not_zero();
    test_next_table_empty_chain();
    test_next_table_full_capacity_spare();
    test_next_table_shrink_clears_stale();
    test_next_raster_addr_clamps();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
