#ifndef ENGINE_TYPES_H
#define ENGINE_TYPES_H

// types.h — common fixed-width types and shared utility tables.
//
// This is the bottom of the engine dependency graph: it depends on nothing
// else (see docs/ARCHITECTURE.md "Dependency Rules"). Everything else builds
// on top of it.
//
// CONSTRAINTS.md bans bare `int` (ambiguous width on 6502) and floating point.
// Use the fixed-width types below for all engine and game data.

#include <stdint.h>

namespace engine {

// ── Fixed-width aliases ──────────────────────────────────────────────
//
// Convenience names for the standard fixed-width integers. These document
// intent ("one byte", "two bytes") and keep declarations terse. Game code
// may use either these or the underlying <stdint.h> names interchangeably.

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;

// ── bit_mask ─────────────────────────────────────────────────────────
//
// Single-byte occupancy-bitmap helper: bit_mask[i] == (1u << i).
//
// SlotPool tracks up to 8 slots in one byte and tests/sets/clears bits with
// this table rather than computing shifts at runtime (a variable shift is
// expensive on the 6502). The table is 8 bytes of ROM, shared and amortized
// across every SlotPool instance (see API_DESIGN.md "Memory Cost" and
// DECISIONS.md ADR-001). It is also the table game code uses directly for the
// manual struct-of-arrays pattern (API_DESIGN.md "SoA Pattern").
//
// `inline constexpr` gives it a single ODR-safe definition usable from any
// translation unit that includes this header.

inline constexpr u8 bit_mask[8] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
};

} // namespace engine

#endif // ENGINE_TYPES_H
