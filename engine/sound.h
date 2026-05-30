#ifndef ENGINE_SOUND_H
#define ENGINE_SOUND_H

// sound.h — portable sound-effect subsystem.
//
// This is the Sound subsystem (ARCHITECTURE.md "Sound", API_DESIGN.md "Sound").
// A sound effect is a constexpr ROM table of frames; each frame holds a
// waveform, frequency, volume, and a duration in game frames (ADR-008 —
// compile-time assets, no runtime conversion). Once per frame the VBI calls
// tick(): it writes the active note to POKEY and counts the current frame's
// duration down; when it expires the channel advances to the next frame,
// stopping when it reaches a SILENT terminal.
//
// Like every engine header it reaches hardware ONLY through the `Platform`
// template parameter (Dependency Rule 2) — never by including a platform
// header. POKEY register writes go through Platform::hal::write_audf /
// write_audc, mirroring how sprites.h reaches the P/M registers.
//
// One deliberate exception to "no hardware values in engine headers": the
// pokey::* waveform constants below are the actual POKEY AUDC distortion bits.
// tick() passes `waveform | volume` straight to write_audc (the AUDC byte is
// distortion in the high nibble, volume in the low nibble), so the waveform
// value must already be the hardware bit pattern. The audible three values are
// tuned against Altirra/Fujisan; SILENT is only ever a terminal sentinel (it is
// never written to POKEY — tick() stops the channel instead), so it just needs
// to be distinct.
//
// Depends only on types.h.

#include "types.h"

namespace engine {

// ── Waveform constants (POKEY AUDC distortion bits) ───────────────────
//
// API_DESIGN.md uses these unqualified as `pokey::PURE` etc., so the namespace
// is top-level. Values are the AUDC high-nibble distortion patterns.
namespace pokey {
inline constexpr u8 PURE   = 0xA0;  // pure tone
inline constexpr u8 NOISE  = 0x80;  // noise
inline constexpr u8 BUZZ   = 0xC0;  // buzzy distortion
inline constexpr u8 SILENT = 0x10;  // terminal marker (never written to POKEY)
} // namespace pokey

// ── SoundFrame ────────────────────────────────────────────────────────
//
// One step of a sound effect: a note (waveform + frequency + volume) held for
// `duration` game frames. 4 bytes, ROM-resident as part of a SoundEffect.
struct SoundFrame {
    u8 waveform;
    u8 frequency;
    u8 volume;
    u8 duration;
};
static_assert(sizeof(SoundFrame) == 4, "SoundFrame must be 4 bytes");

// ── SoundEffect ───────────────────────────────────────────────────────
//
// A constexpr array of frames terminated by a SILENT frame. This is the value
// `Game::make_sound` returns; it lives in ROM and is referenced by pointer.
template <u8 N>
struct SoundEffect {
    SoundFrame frames[N];
};

// Build a SoundEffect from a braced array of frames, appending the SILENT
// terminal automatically (the eventual Game::make_sound). Mirrors make_sprite
// in sprites.h. Authors write only the real frames; an explicitly-written
// SILENT terminal is harmless (tick() stops at the first one).
template <u8 N>
constexpr SoundEffect<N + 1> make_sound(const SoundFrame (&in)[N]) {
    SoundEffect<N + 1> e{};
    for (u8 i = 0; i < N; ++i) e.frames[i] = in[i];
    e.frames[N] = SoundFrame{pokey::SILENT, 0, 0, 0};
    return e;
}

// ── ChannelState ──────────────────────────────────────────────────────
//
// One channel's playback state. `current` points at the frame playing right
// now (nullptr ⇒ inactive); freq/vol cache that frame's note so tick() can
// rewrite POKEY each frame without re-reading ROM. ~5-6 bytes (pointer = 2).
struct ChannelState {
    const SoundFrame* current = nullptr;
    u8                frame_counter = 0;
    u8                freq = 0;
    u8                vol = 0;
};

// ── SoundManager ──────────────────────────────────────────────────────
//
// MaxChannels independent channels mapped onto POKEY voices. The game starts
// effects with play(); the engine advances them in tick() during the VBI.
// A channel the user has released (release_channel) is left entirely alone so
// user assembly can drive that POKEY voice directly (ARCHITECTURE.md "Resource
// Release", ADR-006).
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
        Platform::hal::write_audc(channel, 0);
    }

    // Advance every engine-managed channel by one game frame. Call once per
    // frame from the VBI. When the current frame's duration is spent, advance to
    // the next frame first (stopping on the SILENT terminal), then write the
    // active note to POKEY. Advancing *before* the write means each note holds
    // for its full duration on hardware — the value written on a tick is what
    // sounds until the next tick — and the terminal silence lands on the tick
    // after the last note frame, not on top of it.
    void tick() {
        for (u8 ch = 0; ch < MaxChannels; ++ch) {
            if (released(ch)) continue;
            ChannelState& c = channels_[ch];
            if (c.current == nullptr) continue;

            if (c.frame_counter == 0) {
                const SoundFrame* next = c.current + 1;
                if (next->waveform == pokey::SILENT) { stop(ch); continue; }
                load(c, next);
            }

            Platform::hal::write_audf(ch, c.freq);
            Platform::hal::write_audc(
                ch, static_cast<u8>(c.current->waveform | c.vol));
            --c.frame_counter;
        }
    }

    // Release / reclaim a channel for direct user POKEY access. A released
    // channel is skipped by tick() and ignored by play().
    void release_channel(u8 n) { released_mask_ |= bit_mask[n]; }
    void reclaim_channel(u8 n) { released_mask_ &= static_cast<u8>(~bit_mask[n]); }

    // True if the channel is currently playing an effect.
    bool active(u8 channel) const { return channels_[channel].current != nullptr; }

private:
    bool released(u8 n) const { return (released_mask_ & bit_mask[n]) != 0; }

    // Point a channel at `frame` and cache its note. A SILENT frame leaves the
    // channel inactive (current = nullptr).
    static void load(ChannelState& c, const SoundFrame* frame) {
        if (frame->waveform == pokey::SILENT) {
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
