#ifndef ENGINE_NET_TYPES_H
#define ENGINE_NET_TYPES_H

// net_types.h — generic networking support types (no backend wiring).
//
// Stage-3 generic layer only: status/error vocabulary, fixed packet wrapper,
// session message view, and packet-size validation helpers. No transport code,
// no platform headers, and no dynamic allocation.

#include "types.h"

namespace engine {
namespace net {

// Default realtime lane capacities (bytes/packets).
inline constexpr u8 default_realtime_packet_bytes = 16;
inline constexpr u8 default_realtime_rx_packets   = 8;
inline constexpr u8 default_realtime_tx_packets   = 2;

// Default session lane capacities (bytes). Sized for the download-a-playfield
// case; a game with a narrower protocol overrides them per direction through
// GameConfig (session_rx_bytes / session_tx_bytes / session_max_message).
inline constexpr u16 default_session_rx_bytes    = 256;
inline constexpr u16 default_session_tx_bytes    = 256;
inline constexpr u16 default_session_max_message = 128;

// Which transport lanes a game intends to use.
//
// A platform's capability profile says which lanes the hardware *can* offer;
// this says which ones the game actually wants storage and code for. On
// transports where the lanes are mutually exclusive (one lane taking the serial
// vectors the other needs), a game that runs them in sequence rather than
// together can name the single lane a given binary needs and let the linker drop
// the other. Selecting a lane the platform does not provide yields no lane —
// capability still gates availability, this only ever narrows.
enum class NetLanes : u8 {
    Realtime,   // realtime lane only
    Session,    // session lane only
    Both,       // both concurrently (the default)
};

// Generic non-throwing status codes for network operations.
enum class NetStatus : u8 {
    Ok = 0,
    WouldBlock,
    Closed,
    Overflow,
    InvalidArgument,
    BadConfig,
    Unsupported,
    TransportError,
};

// Error payload: status plus backend-specific detail code (if any).
struct NetError {
    NetStatus status = NetStatus::Ok;
    i16       detail = 0;

    bool ok() const { return status == NetStatus::Ok; }
};

// Fixed 16-byte realtime payload wrapper.
struct RealtimePacket16 {
    u8 bytes[default_realtime_packet_bytes] = {};
};
static_assert(sizeof(RealtimePacket16) == default_realtime_packet_bytes,
              "RealtimePacket16 must be exactly 16 bytes");

// View into a session message stored in engine-owned buffers.
// Lifetime is controlled by the owning lane/buffer.
struct SessionMessageView {
    u8        kind = 0;
    const u8* data = nullptr;
    u16       size = 0;
};

// Optional view type for APIs that return packet pointers instead of copy-out.
struct RealtimePacketView {
    const u8* data = nullptr;
    u16       size = 0;
};

// Packet-size validation helpers for fixed-size realtime APIs.
template <typename Packet, u16 PacketBytes>
struct packet_size_valid {
    static constexpr bool value = (sizeof(Packet) == PacketBytes);
};

template <typename Packet, u16 PacketBytes>
inline constexpr bool packet_size_valid_v = packet_size_valid<Packet, PacketBytes>::value;

template <typename Packet, u16 PacketBytes>
constexpr void validate_packet_size() {
    static_assert(packet_size_valid_v<Packet, PacketBytes>,
                  "Packet type size does not match configured realtime packet size");
}

} // namespace net
} // namespace engine

#endif // ENGINE_NET_TYPES_H
