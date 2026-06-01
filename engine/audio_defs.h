#ifndef ENGINE_AUDIO_DEFS_H
#define ENGINE_AUDIO_DEFS_H

// audio_defs.h — backend-neutral audio vocabulary for the sound subsystem.
//
// The portable sound subsystem (engine/sound.h) describes a note's timbre with
// engine::audio::Waveform rather than a raw hardware register value. The backend
// HAL maps each Waveform to its concrete control byte. `Silent` is a terminal
// sentinel: a SoundEffect ends with a Silent frame and the engine stops the
// channel on it — it is never played, so its only requirement is to be a distinct
// value.
//
// Depends only on types.h.

#include "types.h"

namespace engine {
namespace audio {

enum class Waveform : u8 {
    Tone,     // pure tone
    Noise,    // noise
    Buzz,     // buzzy distortion
    Silent,   // terminal sentinel (never played)
};

} // namespace audio
} // namespace engine

#endif // ENGINE_AUDIO_DEFS_H
