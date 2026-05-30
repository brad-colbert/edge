// test_input.cpp — unit tests for engine/input.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// These tests drive update() with simulated joystick/keyboard bytes (standing
// in for the HAL) and verify the level and edge queries.

#include <stdio.h>

#include <engine/input.h>

using engine::u8;
namespace joy = engine::joy;
namespace console = engine::console;

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Level queries ────────────────────────────────────────────────────

static void test_levels() {
    engine::Input in;
    u8 j[2] = { static_cast<u8>(joy::UP | joy::FIRE), 0 };
    in.update(j, 0);

    CHECK(in.up());            // held
    CHECK(in.fire());
    CHECK(!in.down());
    CHECK(!in.left());
    CHECK(!in.right());

    // Switch to right+down on port 0.
    j[0] = static_cast<u8>(joy::RIGHT | joy::DOWN);
    in.update(j, 0);
    CHECK(in.right());
    CHECK(in.down());
    CHECK(!in.up());
    CHECK(!in.fire());
}

// ── Fire edge detection ──────────────────────────────────────────────

static void test_fire_edges() {
    engine::Input in;
    u8 j[2] = { 0, 0 };

    // Frame 1: fire not pressed.
    in.update(j, 0);
    CHECK(!in.fire());
    CHECK(!in.fire_pressed());
    CHECK(!in.fire_released());

    // Frame 2: fire goes down -> pressed edge fires exactly once.
    j[0] = joy::FIRE;
    in.update(j, 0);
    CHECK(in.fire());
    CHECK(in.fire_pressed());      // true only on transition frame
    CHECK(!in.fire_released());

    // Frame 3: fire still held -> no pressed edge.
    in.update(j, 0);
    CHECK(in.fire());
    CHECK(!in.fire_pressed());     // held, not a new press
    CHECK(!in.fire_released());

    // Frame 4: fire released -> released edge fires exactly once.
    j[0] = 0;
    in.update(j, 0);
    CHECK(!in.fire());
    CHECK(!in.fire_pressed());
    CHECK(in.fire_released());

    // Frame 5: still released -> no edge.
    in.update(j, 0);
    CHECK(!in.fire_released());
}

// ── Multi-port queries ───────────────────────────────────────────────

static void test_multi_port() {
    engine::Input in;
    u8 j[2] = { joy::LEFT, static_cast<u8>(joy::RIGHT | joy::FIRE) };
    in.update(j, 0);

    // Port 0
    CHECK(in.left(0));
    CHECK(!in.right(0));
    CHECK(!in.fire(0));

    // Port 1
    CHECK(in.right(1));
    CHECK(in.fire(1));
    CHECK(!in.left(1));

    // Default arg == port 0.
    CHECK(in.left());

    // Out-of-range port returns false, no crash/UB.
    CHECK(!in.up(2));
    CHECK(!in.fire(7));
}

// ── Console keys (packed into port 0) ────────────────────────────────

static void test_console_keys() {
    engine::Input in;
    u8 j[2] = { static_cast<u8>(console::START | console::OPTION), 0 };
    in.update(j, 0);

    CHECK(in.start());
    CHECK(in.option());
    CHECK(!in.select());
}

// ── Keyboard edge detection (single byte) ────────────────────────────

static void test_keyboard() {
    engine::Input in;
    u8 j[2] = { 0, 0 };

    // No key.
    in.update(j, 0);
    CHECK(in.key() == 0);
    CHECK(!in.key_pressed());

    // New key 0x2A -> reported, pressed edge true.
    in.update(j, 0x2A);
    CHECK(in.key() == 0x2A);
    CHECK(in.key_pressed());

    // Same key held -> still reported, but not a new press.
    in.update(j, 0x2A);
    CHECK(in.key() == 0x2A);
    CHECK(!in.key_pressed());

    // Different key -> new press edge.
    in.update(j, 0x15);
    CHECK(in.key() == 0x15);
    CHECK(in.key_pressed());

    // Release.
    in.update(j, 0);
    CHECK(in.key() == 0);
    CHECK(!in.key_pressed());
}

// ── Port count is configurable ───────────────────────────────────────

static void test_port_count_template() {
    engine::InputState<4> in4;
    static_assert(engine::InputState<4>::port_count() == 4, "");
    u8 j[4] = { 0, 0, joy::UP, 0 };
    in4.update(j, 0);
    CHECK(in4.up(2));
    CHECK(!in4.up(0));
}

int main() {
    test_levels();
    test_fire_edges();
    test_multi_port();
    test_console_keys();
    test_keyboard();
    test_port_count_template();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
