#ifndef ENGINE_INPUT_H
#define ENGINE_INPUT_H

// input.h — portable input snapshot and edge-detection logic.
//
// This is the data structure and the per-frame edge logic ONLY. Actual
// hardware polling (joystick reads, keyboard scan, system console keys) is the
// platform HAL's job and comes later. The HAL calls update() once per frame with
// already-normalised state; game code reads the snapshot via the query methods
// (API_DESIGN.md "Input").
//
// State is captured once per frame and presented immutably to game code, so
// there are no races and no polling (DECISIONS.md / ARCHITECTURE.md data flow).
//
// Depends only on types.h.

#include "types.h"

namespace engine {

// Joystick direction + fire bits within a port's state byte.
namespace joy {
inline constexpr u8 UP    = 0x01;
inline constexpr u8 DOWN  = 0x02;
inline constexpr u8 LEFT  = 0x04;
inline constexpr u8 RIGHT = 0x08;
inline constexpr u8 FIRE  = 0x10;
} // namespace joy

// System console keys are a single global set (the backend's console buttons).
// To stay within the "2 bytes per port" budget they are packed into the high
// bits of port 0's state byte rather than costing extra storage.
namespace syskey {
inline constexpr u8 PRIMARY   = 0x20;
inline constexpr u8 SECONDARY = 0x40;
inline constexpr u8 OPTION    = 0x80;
} // namespace syskey

// InputState<NumPorts> — current + previous frame state for NumPorts joystick
// ports plus a one-byte keyboard field.
//
// Memory cost (API_DESIGN.md "Input"): 2 bytes per port (current + previous)
// + 1 byte keyboard. NumPorts is a template parameter, not hardcoded.
template <u8 NumPorts = 2>
class InputState {
    static_assert(NumPorts >= 1, "need at least one joystick port");
    static_assert(NumPorts <= 4, "at most 4 joystick ports are supported");

public:
    static constexpr u8 port_count() { return NumPorts; }

    // ── Internal interface (called by the HAL each frame, not by game code) ──
    //
    // Shifts current -> previous for every port, latches the new joystick
    // bytes, and updates the keyboard field. `joy` points to NumPorts bytes
    // (each a bitwise OR of joy::* and, for port 0, syskey::* bits). `key`
    // is the current scancode (0..127), or 0 if no key is down.
    void update(const u8* joy, u8 key) {
        for (u8 p = 0; p < NumPorts; ++p) {
            prev_[p] = cur_[p];
            cur_[p]  = joy[p];
        }
        update_key(key);
    }

    // ── Directional queries (level / held) ──
    bool up   (u8 port = 0) const { return level(port, joy::UP); }
    bool down (u8 port = 0) const { return level(port, joy::DOWN); }
    bool left (u8 port = 0) const { return level(port, joy::LEFT); }
    bool right(u8 port = 0) const { return level(port, joy::RIGHT); }

    // ── Fire button (level + edges) ──
    bool fire         (u8 port = 0) const { return level(port, joy::FIRE); }
    bool fire_pressed (u8 port = 0) const { return edge_pressed(port, joy::FIRE); }
    bool fire_released(u8 port = 0) const { return edge_released(port, joy::FIRE); }

    // ── System console keys (level; global, read from port 0) ──
    bool system_primary()   const { return level(0, syskey::PRIMARY); }
    bool system_secondary() const { return level(0, syskey::SECONDARY); }
    bool system_option()    const { return level(0, syskey::OPTION); }

    // ── Keyboard ──
    // key() returns the current scancode (0 if none). key_pressed() is true
    // only on the frame a *new* scancode appears (edge), not while held.
    u8   key() const { return static_cast<u8>(key_ & 0x7F); }
    bool key_pressed() const { return (key_ & 0x80) != 0; }

private:
    bool level(u8 port, u8 mask) const {
        if (port >= NumPorts) return false;        // out-of-range -> false
        return (cur_[port] & mask) != 0;
    }
    bool edge_pressed(u8 port, u8 mask) const {
        if (port >= NumPorts) return false;
        return (cur_[port] & mask) && !(prev_[port] & mask);
    }
    bool edge_released(u8 port, u8 mask) const {
        if (port >= NumPorts) return false;
        return !(cur_[port] & mask) && (prev_[port] & mask);
    }

    // Pack the scancode into the low 7 bits; bit 7 flags "new this frame".
    // Computed against the previously latched scancode so a held key reads as
    // pressed only once — keeps the whole keyboard state in a single byte.
    void update_key(u8 key) {
        const u8 code = static_cast<u8>(key & 0x7F);
        const bool fresh = (code != 0) && (code != static_cast<u8>(key_ & 0x7F));
        key_ = fresh ? static_cast<u8>(code | 0x80) : code;
    }

    u8 cur_[NumPorts]  = {};
    u8 prev_[NumPorts] = {};
    u8 key_ = 0;
};

// Default game-facing alias: two joystick ports (API_DESIGN.md uses
// `engine::Input`). Games needing more ports use InputState<N> directly.
using Input = InputState<2>;

} // namespace engine

#endif // ENGINE_INPUT_H
