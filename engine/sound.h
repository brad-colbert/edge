#ifndef ENGINE_SOUND_H
#define ENGINE_SOUND_H

// sound.h — portable sound-effect subsystem.
//
// This is the Sound subsystem (ARCHITECTURE.md "Sound", API_DESIGN.md "Sound").
// A sound effect is a constexpr ROM table of frames; each frame holds a
// waveform, frequency, volume, and a duration in game frames (ADR-008 —
// compile-time assets, no runtime conversion). Once per frame the frame service
// calls tick(): it writes the active note to the audio HAL and counts the current
// frame's duration down; when it expires the channel advances to the next frame,
// stopping when it reaches a Silent terminal.
//
// Like every engine header it reaches hardware ONLY through the `Platform`
// template parameter (Dependency Rule 2) — never by including a platform header.
// Notes are described with the backend-neutral engine::audio::Waveform vocabulary
// (audio_defs.h); the backend HAL maps each Waveform to its concrete control byte
// via set_voice_control, so no hardware register values appear here.
//
// Depends on types.h and audio_defs.h.

#include "audio_defs.h"
#include "types.h"

namespace engine {

// ── SoundFrame ────────────────────────────────────────────────────────
//
// One step of a sound effect: a note (waveform + frequency + volume) held for
// `duration` game frames. 4 bytes, ROM-resident as part of a SoundEffect.
struct SoundFrame {
    audio::Waveform waveform;
    u8              frequency;
    u8              volume;
    u8              duration;
};
static_assert(sizeof(SoundFrame) == 4, "SoundFrame must be 4 bytes");

// ── SoundEffect ───────────────────────────────────────────────────────
//
// A constexpr array of frames terminated by a Silent frame. This is the value
// `Game::make_sound` returns; it lives in ROM and is referenced by pointer.
template <u8 N>
struct SoundEffect {
    SoundFrame frames[N];
};

// Build a SoundEffect from a braced array of frames, appending the Silent
// terminal automatically (the eventual Game::make_sound). Mirrors make_sprite
// in sprites.h. Authors write only the real frames; an explicitly-written
// Silent terminal is harmless (tick() stops at the first one).
template <u8 N>
constexpr SoundEffect<N + 1> make_sound(const SoundFrame (&in)[N]) {
    SoundEffect<N + 1> e{};
    for (u8 i = 0; i < N; ++i) e.frames[i] = in[i];
    e.frames[N] = SoundFrame{audio::Waveform::Silent, 0, 0, 0};
    return e;
}

// ── ChannelState ──────────────────────────────────────────────────────
//
// One channel's playback state. `current` points at the frame playing right
// now (nullptr ⇒ inactive); freq/vol cache that frame's note so tick() can
// rewrite the voice each frame without re-reading ROM. ~5-6 bytes (pointer = 2).
struct ChannelState {
    const SoundFrame* current = nullptr;
    u8                frame_counter = 0;
    u8                freq = 0;
    u8                vol = 0;
};

// ── SoundManager ──────────────────────────────────────────────────────
//
// MaxChannels independent channels mapped onto the backend's audio voices. The
// game starts effects with play(); the engine advances them in tick() during the
// frame service. A channel the user has released (release_channel) is left
// entirely alone so user assembly can drive that voice directly (ARCHITECTURE.md
// "Resource Release", ADR-006).
template <typename Platform, u8 MaxChannels>
class SoundManager {
    static_assert(MaxChannels >= 1, "need at least one sound channel");
    static_assert(MaxChannels <= 8, "released_mask_ is one byte");

public:
    // Start `sfx` on `channel`, interrupting whatever was playing. A released
    // channel is owned by the user, so play() ignores it. An effect whose first
    // frame is already SILENT plays nothing.
    template <typename Effect>
    void play(const Effect& sfx, u8 channel) {
        if (released(channel)) return;
        load(channels_[channel], &sfx.frames[0]);
        if (channels_[channel].current == nullptr) stop(channel);
    }

    // Silence a channel and mark it inactive.
    void stop(u8 channel) {
        channels_[channel].current = nullptr;
        Platform::hal::silence_voice(channel);
    }

    // Advance every engine-managed channel by one game frame. Call once per
    // frame from the frame service. When the current frame's duration is spent,
    // advance to the next frame first (stopping on the Silent terminal), then write
    // the active note to the voice. Advancing *before* the write means each note
    // holds for its full duration on hardware — the value written on a tick is what
    // sounds until the next tick — and the terminal silence lands on the tick
    // after the last note frame, not on top of it.
    void tick() {
        for (u8 ch = 0; ch < MaxChannels; ++ch) {
            if (released(ch)) continue;
            ChannelState& c = channels_[ch];
            if (c.current == nullptr) continue;

            if (c.frame_counter == 0) {
                const SoundFrame* next = c.current + 1;
                if (next->waveform == audio::Waveform::Silent) { stop(ch); continue; }
                load(c, next);
            }

            Platform::hal::set_voice_freq(ch, c.freq);
            Platform::hal::set_voice_control(ch, c.current->waveform, c.vol);
            --c.frame_counter;
        }
    }

    // Release / reclaim a channel for direct user access to that hardware voice.
    // A released channel is skipped by tick() and ignored by play().
    void release_channel(u8 n) { released_mask_ |= bit_mask[n]; }
    void reclaim_channel(u8 n) { released_mask_ &= static_cast<u8>(~bit_mask[n]); }

    // True if the channel is currently playing an effect.
    bool active(u8 channel) const { return channels_[channel].current != nullptr; }

private:
    bool released(u8 n) const { return (released_mask_ & bit_mask[n]) != 0; }

    // Point a channel at `frame` and cache its note. A Silent frame leaves the
    // channel inactive (current = nullptr).
    static void load(ChannelState& c, const SoundFrame* frame) {
        if (frame->waveform == audio::Waveform::Silent) {
            c.current = nullptr;
            return;
        }
        c.current       = frame;
        c.frame_counter = frame->duration;
        c.freq          = frame->frequency;
        c.vol           = frame->volume;
    }

    ChannelState channels_[MaxChannels] = {};
    u8           released_mask_         = 0;
};

} // namespace engine

#endif // ENGINE_SOUND_H
