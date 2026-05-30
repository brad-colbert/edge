// test_sound.cpp — unit tests for engine/sound.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// The SoundManager is exercised against a MOCK platform whose HAL records the
// AUDF/AUDC writes per channel, so playback advancement, channel release, and
// interruption can be asserted exactly without POKEY hardware. The live POKEY
// path is verified separately on Altirra/Fujisan.

#include <stdint.h>
#include <stdio.h>

#include <engine/sound.h>

using engine::u8;
using engine::u16;

using engine::SoundFrame;
using engine::SoundManager;
using engine::make_sound;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records the last AUDF/AUDC value written per channel, plus a count of AUDC
// writes per channel (so a released channel can be asserted to receive none).
struct MockHal {
    static u8 audf[4];
    static u8 audc[4];
    static unsigned audc_writes[4];

    static void write_audf(u8 channel, u8 freq) { audf[channel] = freq; }
    static void write_audc(u8 channel, u8 ctrl) {
        audc[channel] = ctrl;
        ++audc_writes[channel];
    }

    static void reset() {
        for (u8 i = 0; i < 4; ++i) { audf[i] = 0; audc[i] = 0; audc_writes[i] = 0; }
    }
};
u8 MockHal::audf[4] = {};
u8 MockHal::audc[4] = {};
unsigned MockHal::audc_writes[4] = {};

struct MockPlatform {
    using hal = MockHal;
};

using Sound = engine::SoundManager<MockPlatform, 2>;

// The reference effect: PURE 100/8 for 3 frames, PURE 150/4 for 2 frames, then
// the auto-appended SILENT terminal — three frames total, matching the spec.
static constexpr auto g_sfx = make_sound({
    {engine::pokey::PURE, 100, 8, 3},
    {engine::pokey::PURE, 150, 4, 2},
});

// ── Harness ────────────────────────────────────────────────────────────

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Playback: 3 frames of note 1, 2 frames of note 2, then silence ─────

static void test_playback_advance() {
    MockHal::reset();
    static Sound snd;
    snd.play(g_sfx, 0);

    // Ticks 1-3: first note (PURE, freq 100, vol 8).
    for (u8 t = 0; t < 3; ++t) {
        snd.tick();
        CHECK(snd.active(0));
        CHECK(MockHal::audf[0] == 100);
        CHECK(MockHal::audc[0] == static_cast<u8>(engine::pokey::PURE | 8));
    }

    // Ticks 4-5: second note (PURE, freq 150, vol 4). Both ticks must produce a
    // clean note write — each note holds for its full duration.
    for (u8 t = 0; t < 2; ++t) {
        snd.tick();
        CHECK(snd.active(0));
        CHECK(MockHal::audf[0] == 150);
        CHECK(MockHal::audc[0] == static_cast<u8>(engine::pokey::PURE | 4));
    }

    // The five note frames (3 + 2) are spent. The next tick reaches the SILENT
    // terminal and silences the channel.
    snd.tick();
    CHECK(!snd.active(0));
    CHECK(MockHal::audc[0] == 0);

    // Further ticks do nothing to an inactive channel.
    const unsigned writes_before = MockHal::audc_writes[0];
    snd.tick();
    CHECK(MockHal::audc_writes[0] == writes_before);
}

// ── Released channel is untouched; reclaim resumes management ──────────

static void test_release_reclaim() {
    MockHal::reset();
    static Sound snd;

    snd.release_channel(1);
    snd.play(g_sfx, 1);              // ignored — user owns channel 1
    snd.tick();
    CHECK(!snd.active(1));
    CHECK(MockHal::audc_writes[1] == 0);   // engine wrote nothing to channel 1

    snd.reclaim_channel(1);
    snd.play(g_sfx, 1);
    snd.tick();
    CHECK(snd.active(1));
    CHECK(MockHal::audc_writes[1] > 0);    // writes resume
    CHECK(MockHal::audf[1] == 100);
    CHECK(MockHal::audc[1] == static_cast<u8>(engine::pokey::PURE | 8));
}

// ── Playing a new sound interrupts the current one ─────────────────────

static void test_interrupt() {
    MockHal::reset();
    static Sound snd;

    // A different effect: NOISE, freq 40, vol 12.
    static constexpr auto sfx_b = make_sound({
        {engine::pokey::NOISE, 40, 12, 4},
    });

    snd.play(g_sfx, 0);
    snd.tick();                      // partway through note 1 of g_sfx
    CHECK(MockHal::audf[0] == 100);

    snd.play(sfx_b, 0);              // interrupt before g_sfx finishes
    snd.tick();
    CHECK(MockHal::audf[0] == 40);
    CHECK(MockHal::audc[0] == static_cast<u8>(engine::pokey::NOISE | 12));
}

int main() {
    test_playback_advance();
    test_release_reclaim();
    test_interrupt();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
