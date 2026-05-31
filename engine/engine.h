#ifndef ENGINE_ENGINE_H
#define ENGINE_ENGINE_H

// engine.h — umbrella include for game code.
//
// Game code includes only <engine/...> headers, never platform/* directly
// (see docs/ARCHITECTURE.md "Dependency Rules"). This header pulls in the
// public engine API. Subsystems are added here as they are implemented.

#include "types.h"
#include "math.h"
#include "pool.h"
#include "input.h"

// Display + subsystems.
#include "display.h"
#include "screen.h"
#include "sprites.h"
#include "sound.h"
#include "scroll.h"
#include "tiles.h"
#include "interrupt.h"

// Integration layer.
#include "hooks.h"
#include "loop.h"
#include "core.h"

#endif // ENGINE_ENGINE_H
