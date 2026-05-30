#ifndef ENGINE_PLATFORM_ATARI_HAL_H
#define ENGINE_PLATFORM_ATARI_HAL_H

// platform/atari/hal.h — the Atari hardware abstraction layer.
//
// The HAL provides concrete functions for register access, interrupt
// installation, display construction, and DMA configuration. The engine calls
// it through the Platform's `hal` type via static dispatch (ARCHITECTURE.md
// "Platform HAL", DECISIONS.md ADR-007 — never virtual).
//
// Only a placeholder type exists today: it gives `atari::Platform::hal`
// something to alias. Real HAL functions (and per-axis specialisation) come in
// a later step.

#include "registers.h"

namespace atari {

// Placeholder HAL. Methods will be added as subsystems are implemented; they
// will be `static` and operate through atari::reg::* (no instance state).
struct Hal {
    // TODO: register access, interrupt install, display build, DMA setup.
};

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_HAL_H
