#ifndef ENGINE_NET_API_H
#define ENGINE_NET_API_H

// net_api.h — Stage-4 Game::net-facing API stubs.
//
// Exposes a dual-lane facade shape with no backend transport wiring yet:
//   - realtime lane: fixed packet send/recv API over Stage-3 packet rings
//   - session lane : framed byte-stream API over Stage-3 byte rings
//
// Host pointer inputs are transient. This stub stage does not retain borrowed
// host pointers in lane state.

#include "net_ring.h"
#include "net_types.h"
#include "attributes.h"
#include "config/capabilities.h"

namespace engine {
namespace net {

// Realtime lane stub (UDP-seq intent, no real transport yet).
template <typename Platform,
          u16 PacketBytes = default_realtime_packet_bytes,
          u8 RxPackets = default_realtime_rx_packets,
          u8 TxPackets = default_realtime_tx_packets>
class RealtimeLane {
public:
    static constexpr u16 packet_bytes() { return PacketBytes; }
    static constexpr u8 default_rx_capacity() { return RxPackets; }

    NetStatus open_udp_seq(const char* host, u16 remote_port, u16 local_port = 0) {
        const NetStatus st = Platform::hal::realtime_open_udp_seq(host, remote_port, local_port);
        active_ = (st == NetStatus::Ok);
        return set_status(st);
    }

    // Deferred API shape: kept present for call-site stability.
    NetStatus bind_udp_seq(u16 local_port) {
        return set_status(Platform::hal::realtime_bind_udp_seq(local_port));
    }

    void close() {
        Platform::hal::realtime_close();
        active_ = false;
        queues_.clear();
        set_status(NetStatus::Closed);
    }

    bool active() const { return active_; }

    // Drive nonblocking transport progression: seam poll, TX flush, RX drain.
    NetStatus poll() {
        if (!active()) return set_status(NetStatus::Closed);

        const NetStatus st = Platform::hal::realtime_poll();
        if (st != NetStatus::Ok && st != NetStatus::WouldBlock)
            return set_status(st);

        flush_tx_();
        drain_rx_();
        return set_status(NetStatus::Ok);
    }

    template <typename Packet>
    NetStatus send(const Packet& packet) {
        validate_packet_size<Packet, PacketBytes>();
        if (!active()) return set_status(NetStatus::Closed);
        queues_.tx_push(&packet);
        flush_tx_();
        return set_status(NetStatus::Ok);
    }

    // Copy fixed-size packet out of the RX ring.
    template <typename Packet>
    bool recv(Packet& packet) {
        validate_packet_size<Packet, PacketBytes>();
        if (!active()) {
            set_status(NetStatus::Closed);
            return false;
        }
        if (!queues_.rx_pop(&packet)) {
            set_status(NetStatus::WouldBlock);
            return false;
        }
        set_status(NetStatus::Ok);
        return true;
    }

    u8 rx_count() const { return queues_.rx_count(); }
    u8 rx_capacity() const { return queues_.rx_capacity(); }
    u16 rx_dropped() const { return queues_.rx_dropped(); }
    bool consume_rx_overflowed() { return queues_.consume_rx_overflowed(); }

    NetError last_error() const {
        if (last_error_.status == NetStatus::Ok) return Platform::hal::realtime_last_error();
        return last_error_;
    }

private:
    NetStatus set_status(NetStatus s, i16 detail = 0) {
        last_error_.status = s;
        last_error_.detail = detail;
        return s;
    }

    RealtimePacketQueues<PacketBytes, RxPackets, TxPackets> queues_{};
    NetError last_error_{};
    bool active_ = false;

    void flush_tx_() {
        u8 packet[PacketBytes] = {};
        while (queues_.tx_count() > 0) {
            if (!queues_.tx_peek(packet)) break;
            const NetStatus st = Platform::hal::realtime_send_nb(packet, PacketBytes);
            if (st == NetStatus::Ok) {
                queues_.tx_drop_oldest();
                continue;
            }
            if (st == NetStatus::WouldBlock) break;
            set_status(st);
            break;
        }
    }

    void drain_rx_() {
        u8 packet[PacketBytes] = {};
        for (;;) {
            const NetStatus st = Platform::hal::realtime_recv_nb(packet, PacketBytes);
            if (st == NetStatus::Ok) {
                queues_.rx_push(packet);
                continue;
            }
            if (st == NetStatus::WouldBlock) break;
            set_status(st);
            break;
        }
    }
};

// Session lane stub (TCP intent, no real transport yet).
//
// recv(view) fills a view into engine-owned storage. The view lifetime is valid
// only until the next session.poll(), session.recv(), or session buffer
// mutation.
template <typename Platform,
          u16 RxBytes = 256, u16 TxBytes = 256, u16 MaxMessageBytes = 128>
class SessionLane {
    static_assert(MaxMessageBytes > 0, "Session max message bytes must be > 0");

public:
    static constexpr u16 max_message_bytes() { return MaxMessageBytes; }

    EDGE_COLD NetStatus connect_tcp(const char* host, u16 port) {
        const NetStatus st = Platform::hal::session_connect_tcp(host, port);
        connected_ = (st == NetStatus::Ok);
        rx_.clear();
        tx_.clear();
        invalidate_view();
        return set_status(st);
    }

    void close() {
        Platform::hal::session_close();
        connected_ = false;
        rx_.clear();
        tx_.clear();
        invalidate_view();
        set_status(NetStatus::Closed);
    }

    bool connected() const { return connected_; }

    // Nonblocking seam poll + TX flush + RX drain.
    EDGE_COLD NetStatus poll() {
        if (!connected()) return set_status(NetStatus::Closed);
        invalidate_view();
        const NetStatus st = Platform::hal::session_poll();
        if (st == NetStatus::Closed) connected_ = false;
        if (st != NetStatus::Ok && st != NetStatus::WouldBlock)
            return set_status(st);
        flush_tx_();
        drain_rx_();
        return set_status(NetStatus::Ok);
    }

    template <typename Message>
    NetStatus send(const Message& message) {
        return send_bytes(&message, static_cast<u16>(sizeof(Message)), 0);
    }

    EDGE_COLD NetStatus send_bytes(const void* data, u16 size, u8 kind = 0) {
        if (!connected()) return set_status(NetStatus::Closed);
        if ((data == nullptr && size > 0) || size > MaxMessageBytes)
            return set_status(NetStatus::InvalidArgument);

        // Framed as: kind(1), size_lo(1), size_hi(1), payload(size).
        const u16 total = static_cast<u16>(3 + size);
        if (tx_.free_space() < total) return set_status(NetStatus::Overflow);

        tx_.push(kind);
        tx_.push(static_cast<u8>(size & 0xFF));
        tx_.push(static_cast<u8>(size >> 8));

        const u8* bytes = static_cast<const u8*>(data);
        for (u16 i = 0; i < size; ++i) tx_.push(bytes[i]);

        flush_tx_();
        return set_status(NetStatus::Ok);
    }

    EDGE_COLD bool recv(SessionMessageView& view) {
        invalidate_view();
        view.kind = 0;
        view.data = nullptr;
        view.size = 0;

        if (!connected()) {
            set_status(NetStatus::Closed);
            return false;
        }

        // Need at least framing header: kind + 16-bit size.
        if (rx_.count() < 3) {
            set_status(NetStatus::WouldBlock);
            return false;
        }

        u8 kind = 0;
        u8 sz_lo = 0;
        u8 sz_hi = 0;
        rx_.peek_at(0, kind);
        rx_.peek_at(1, sz_lo);
        rx_.peek_at(2, sz_hi);
        const u16 msg_size = static_cast<u16>(sz_lo | (static_cast<u16>(sz_hi) << 8));

        if (msg_size > MaxMessageBytes) {
            set_status(NetStatus::Overflow);
            // Drop malformed frame header to keep the stream moving.
            u8 sink = 0;
            rx_.pop(sink); rx_.pop(sink); rx_.pop(sink);
            return false;
        }

        if (rx_.count() < static_cast<u16>(3 + msg_size)) {
            set_status(NetStatus::WouldBlock);
            return false;
        }

        // Consume header and payload into engine-owned storage.
        u8 sink = 0;
        rx_.pop(sink);
        rx_.pop(sink);
        rx_.pop(sink);
        for (u16 i = 0; i < msg_size; ++i) {
            rx_.pop(message_storage_[i]);
        }

        current_view_.kind = kind;
        current_view_.data = message_storage_;
        current_view_.size = msg_size;
        view = current_view_;
        set_status(NetStatus::Ok);
        return true;
    }

    u16 bytes_pending() const { return rx_.count(); }
    NetError last_error() const {
        if (last_error_.status == NetStatus::Ok) return Platform::hal::session_last_error();
        return last_error_;
    }

private:
    void invalidate_view() {
        current_view_.kind = 0;
        current_view_.data = nullptr;
        current_view_.size = 0;
    }

    NetStatus set_status(NetStatus s, i16 detail = 0) {
        last_error_.status = s;
        last_error_.detail = detail;
        return s;
    }

    bool connected_ = false;
    ByteRing<RxBytes> rx_{};
    ByteRing<TxBytes> tx_{};
    u8 message_storage_[MaxMessageBytes] = {};
    SessionMessageView current_view_{};
    NetError last_error_{};

    EDGE_COLD void flush_tx_() {
        // Session framing: [kind(1)] [size_lo(1)] [size_hi(1)] [payload(size)]
        // Read the frame header first to determine total frame size, then send
        // the entire frame as one unit to session_send_nb().
        if (tx_.count() < 3) return;  // Not enough for header yet

        u8 kind = 0, size_lo = 0, size_hi = 0;
        if (!tx_.peek_at(0, kind) || !tx_.peek_at(1, size_lo) || !tx_.peek_at(2, size_hi)) {
            return;  // Shouldn't happen if count >= 3, but be defensive
        }

        u16 payload_size = static_cast<u16>(size_lo | (size_hi << 8));
        u16 frame_size = static_cast<u16>(3 + payload_size);

        // Only try to send if we have the complete frame buffered
        if (tx_.count() < frame_size) return;

        // Read the entire frame into a temporary buffer
        u8 frame_buf[MaxMessageBytes + 3] = {};
        for (u16 i = 0; i < frame_size; ++i) {
            if (!tx_.peek_at(i, frame_buf[i])) {
                set_status(NetStatus::InvalidArgument);
                return;
            }
        }

        // Try to send the complete frame
        const NetStatus st = Platform::hal::session_send_nb(frame_buf, frame_size);
        if (st == NetStatus::Ok) {
            // Only pop bytes if the send succeeded
            for (u16 i = 0; i < frame_size; ++i) {
                u8 unused = 0;
                tx_.pop(unused);
            }
        } else if (st == NetStatus::WouldBlock) {
            // Don't break; let the next flush retry the entire frame
        } else {
            set_status(st);
        }
    }

    void drain_rx_() {
        u8 byte = 0;
        for (;;) {
            const NetStatus st = Platform::hal::session_recv_nb(&byte, 1);
            if (st == NetStatus::Ok) {
                if (!rx_.push(byte)) {
                    set_status(NetStatus::Overflow);
                    break;
                }
                continue;
            }
            if (st == NetStatus::WouldBlock) break;
            set_status(st);
            break;
        }
    }
};

namespace ndetail {

template <typename...> using void_t = void;

// Realtime packet size: GameConfig::realtime_packet_bytes if the demo defines it, else
// the engine default. Lets a demo widen its realtime frame (e.g. to pack several
// entities per packet) without touching the engine default or other lane users.
template <typename C, typename = void>
struct realtime_packet_bytes_or_default {
    static constexpr u16 value = default_realtime_packet_bytes;
};
template <typename C>
struct realtime_packet_bytes_or_default<C, void_t<decltype(C::realtime_packet_bytes)>> {
    static constexpr u16 value = C::realtime_packet_bytes;
};

template <typename Platform, typename GameConfig, bool Enabled>
struct realtime_facet { };

template <typename Platform, typename GameConfig>
struct realtime_facet<Platform, GameConfig, true> {
    RealtimeLane<Platform, realtime_packet_bytes_or_default<GameConfig>::value> realtime{};
};

template <typename Platform, bool Enabled>
struct session_facet { };

template <typename Platform>
struct session_facet<Platform, true> {
    SessionLane<Platform> session{};
};

} // namespace ndetail

// Game-facing network facade: owns both lanes.
template <typename Platform, typename GameConfig,
          bool HasRealtime = engine::caps_of_t<Platform>::has_network_realtime,
          bool HasSession  = engine::caps_of_t<Platform>::has_network_session>
struct NetManager : ndetail::realtime_facet<Platform, GameConfig, HasRealtime>,
                    ndetail::session_facet<Platform, HasSession> {
    void close_all() {
        if constexpr (HasRealtime) this->realtime.close();
        if constexpr (HasSession)  this->session.close();
    }
};

} // namespace net
} // namespace engine

#endif // ENGINE_NET_API_H
