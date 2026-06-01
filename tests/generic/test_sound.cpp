// test_sound.cpp — unit tests for engine/sound.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// The SoundManager is exercised against a MOCK platform whose HAL records the
// per-channel voice writes in backend-neutral terms (frequency + Waveform + volume),
// so playback advancement, channel release, and interruption can be asserted
// without any hardware register values. The concrete Waveform→register encoding is
// the backend's job and is tested separately (tests/backends/atari).

#include <stdint.h>
#include <stdio.h>

#include <engine/sound.h>

using engine::u8;
using engine::u16;

using engine::SoundFrame;
using engine::SoundManager;
using engine::make_sound;
using engine::audio::Waveform;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records, per channel, the last frequency / waveform / volume written and a
// count of voice writes (set_voice_control + silence_voice), so a released
// channel can be asserted to receive none. `silenced` records the last action.
struct MockHal {
    static u8       freq[4];
    static Waveform wave[4];
    static u8       vol[4];
    static bool     silenced[4];
    static unsigned voice_writes[4];

    static void set_voice_freq(u8 channel, u8 f) { freq[channel] = f; }
    static void set_voice_control(u8 channel, Waveform w, u8 v) {
        wave[channel] = w;
        vol[channel]  = v;
        silenced[channel] = false;
        ++voice_writes[channel];
    }
    static void silence_voice(u8 channel) {
        silenced[channel] = true;
        ++voice_writes[channel];
    }

    static void reset() {
        for (u8 i = 0; i < 4; ++i) {
            freq[i] = 0; wave[i] = Waveform::Silent; vol[i] = 0;
            silenced[i] = false; voice_writes[i] = 0;
        }
    }
};
u8       MockHal::freq[4]         = {};
Waveform MockHal::wave[4]         = {};
u8       MockHal::vol[4]          = {};
bool     MockHal::silenced[4]     = {};
unsigned MockHal::voice_writes[4] = {};

struct MockPlatform {
    using hal = MockHal;
};

using Sound = engine::SoundManager<MockPlatform, 2>;

// The reference effect: Tone 100/8 for 3 frames, Tone 150/4 for 2 frames, then
// the auto-appended Silent terminal — three note frames total.
static constexpr auto g_sfx = make_sound({
    {Waveform::Tone, 100, 8, 3},
    {Waveform::Tone, 150, 4, 2},
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

    // Ticks 1-3: first note (Tone, freq 100, vol 8).
    for (u8 t = 0; t < 3; ++t) {
        snd.tick();
        CHECK(snd.active(0));
        CHECK(MockHal::freq[0] == 100);
        CHECK(MockHal::wave[0] == Waveform::Tone);
        CHECK(MockHal::vol[0] == 8);
    }

    // Ticks 4-5: second note (Tone, freq 150, vol 4). Both ticks must produce a
    // clean note write — each note holds for its full duration.
    for (u8 t = 0; t < 2; ++t) {
        snd.tick();
        CHECK(snd.active(0));
        CHECK(MockHal::freq[0] == 150);
        CHECK(MockHal::wave[0] == Waveform::Tone);
        CHECK(MockHal::vol[0] == 4);
    }

    // The five note frames (3 + 2) are spent. The next tick reaches the Silent
    // terminal and silences the channel.
    snd.tick();
    CHECK(!snd.active(0));
    CHECK(MockHal::silenced[0]);

    // Further ticks do nothing to an inactive channel.
    const unsigned writes_before = MockHal::voice_writes[0];
    snd.tick();
    CHECK(MockHal::voice_writes[0] == writes_before);
}

// ── Released channel is untouched; reclaim resumes management ──────────

static void test_release_reclaim() {
    MockHal::reset();
    static Sound snd;

    snd.release_channel(1);
    snd.play(g_sfx, 1);              // ignored — user owns channel 1
    snd.tick();
    CHECK(!snd.active(1));
    CHECK(MockHal::voice_writes[1] == 0);   // engine wrote nothing to channel 1

    snd.reclaim_channel(1);
    snd.play(g_sfx, 1);
    snd.tick();
    CHECK(snd.active(1));
    CHECK(MockHal::voice_writes[1] > 0);    // writes resume
    CHECK(MockHal::freq[1] == 100);
    CHECK(MockHal::wave[1] == Waveform::Tone);
    CHECK(MockHal::vol[1] == 8);
}

// ── Playing a new sound interrupts the current one ─────────────────────

static void test_interrupt() {
    MockHal::reset();
    static Sound snd;

    // A different effect: Noise, freq 40, vol 12.
    static constexpr auto sfx_b = make_sound({
        {Waveform::Noise, 40, 12, 4},
    });

    snd.play(g_sfx, 0);
    snd.tick();                      // partway through note 1 of g_sfx
    CHECK(MockHal::freq[0] == 100);

    snd.play(sfx_b, 0);              // interrupt before g_sfx finishes
    snd.tick();
    CHECK(MockHal::freq[0] == 40);
    CHECK(MockHal::wave[0] == Waveform::Noise);
    CHECK(MockHal::vol[0] == 12);
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
