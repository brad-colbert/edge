#ifndef ENGINE_NET_RING_H
#define ENGINE_NET_RING_H

// net_ring.h — fixed-size byte and packet rings (generic, backend-neutral).
//
// No heap usage, no exceptions, and constexpr capacities. PacketRing uses the
// realtime RX overflow policy required by Stage 3: drop oldest, increment drop
// counter, and set a sticky overflow flag consumable by the caller.

#include "types.h"
#include "net_types.h"

namespace engine {
namespace net {

// Byte FIFO ring with fixed compile-time capacity.
template <u16 Capacity>
class ByteRing {
    static_assert(Capacity > 0, "ByteRing capacity must be > 0");

public:
    static constexpr u16 capacity() { return Capacity; }

    u16 count() const { return count_; }
    u16 free_space() const { return static_cast<u16>(Capacity - count_); }
    bool empty() const { return count_ == 0; }
    bool full() const { return count_ == Capacity; }

    void clear() {
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        overflowed_ = false;
    }

    // Push one byte. Returns false on overflow and sets the sticky overflow flag.
    bool push(u8 v) {
        if (full()) {
            overflowed_ = true;
            return false;
        }
        buf_[head_] = v;
        head_ = next(head_);
        ++count_;
        return true;
    }

    // Pop one byte into out. Returns false if empty.
    bool pop(u8& out) {
        if (empty()) return false;
        out = buf_[tail_];
        tail_ = next(tail_);
        --count_;
        return true;
    }

    // Peek oldest byte without consuming it.
    bool peek(u8& out) const {
        if (empty()) return false;
        out = buf_[tail_];
        return true;
    }

    // Peek at `offset` bytes from the oldest element.
    bool peek_at(u16 offset, u8& out) const {
        if (offset >= count_) return false;
        u16 idx = static_cast<u16>(tail_ + offset);
        if (idx >= Capacity) idx = static_cast<u16>(idx - Capacity);
        out = buf_[idx];
        return true;
    }

    // Drop one byte (oldest). Returns false if empty.
    bool drop_oldest() {
        if (empty()) return false;
        tail_ = next(tail_);
        --count_;
        return true;
    }

    bool overflowed() const { return overflowed_; }

    // Sticky flag: returns current overflow state and clears it.
    bool consume_overflowed() {
        const bool v = overflowed_;
        overflowed_ = false;
        return v;
    }

private:
    static u16 next(u16 i) {
        ++i;
        return (i == Capacity) ? 0 : i;
    }

    u8  buf_[Capacity] = {};
    u16 head_ = 0;
    u16 tail_ = 0;
    u16 count_ = 0;
    bool overflowed_ = false;
};

// Fixed-size packet FIFO ring.
//
// Overflow policy for push():
//   1) drop oldest packet
//   2) increment dropped counter
//   3) set sticky overflow flag
//   4) enqueue the new packet
//
// This matches realtime RX semantics where packets are disposable state data.
template <u16 PacketBytes, u8 PacketCount>
class PacketRing {
    static_assert(PacketBytes > 0, "PacketRing packet bytes must be > 0");
    static_assert(PacketCount > 0, "PacketRing packet count must be > 0");

public:
    static constexpr u16 packet_bytes() { return PacketBytes; }
    static constexpr u8 capacity() { return PacketCount; }

    u8 count() const { return count_; }
    bool empty() const { return count_ == 0; }
    bool full() const { return count_ == PacketCount; }

    void clear() {
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        dropped_ = 0;
        overflowed_ = false;
    }

    bool push(const void* packet) {
        if (packet == nullptr) return false;
        if (full()) {
            drop_oldest_unchecked();
            ++dropped_;
            overflowed_ = true;
        }
        copy_in(slot_ptr(head_), static_cast<const u8*>(packet));
        head_ = next(head_);
        ++count_;
        return true;
    }

    bool pop(void* out_packet) {
        if (out_packet == nullptr || empty()) return false;
        copy_out(static_cast<u8*>(out_packet), slot_ptr(tail_));
        tail_ = next(tail_);
        --count_;
        return true;
    }

    // Copy oldest packet without consuming it.
    bool peek(void* out_packet) const {
        if (out_packet == nullptr || empty()) return false;
        copy_out(static_cast<u8*>(out_packet), slot_ptr(tail_));
        return true;
    }

    bool drop_oldest() {
        if (empty()) return false;
        drop_oldest_unchecked();
        return true;
    }

    template <typename Packet>
    bool push_packet(const Packet& packet) {
        validate_packet_size<Packet, PacketBytes>();
        return push(&packet);
    }

    template <typename Packet>
    bool pop_packet(Packet& packet) {
        validate_packet_size<Packet, PacketBytes>();
        return pop(&packet);
    }

    u16 dropped() const { return dropped_; }

    // Sticky overflow flag: returns current state and clears it.
    bool consume_overflowed() {
        const bool v = overflowed_;
        overflowed_ = false;
        return v;
    }

private:
    static u8 next(u8 i) {
        ++i;
        return (i == PacketCount) ? 0 : i;
    }

    void drop_oldest_unchecked() {
        tail_ = next(tail_);
        --count_;
    }

    static void copy_in(u8* dst, const u8* src) {
        for (u16 i = 0; i < PacketBytes; ++i) dst[i] = src[i];
    }

    static void copy_out(u8* dst, const u8* src) {
        for (u16 i = 0; i < PacketBytes; ++i) dst[i] = src[i];
    }

    u8* slot_ptr(u8 slot) { return &storage_[static_cast<u16>(slot) * PacketBytes]; }
    const u8* slot_ptr(u8 slot) const {
        return &storage_[static_cast<u16>(slot) * PacketBytes];
    }

    u8  storage_[static_cast<u16>(PacketBytes) * PacketCount] = {};
    u8  head_ = 0;
    u8  tail_ = 0;
    u8  count_ = 0;
    u16 dropped_ = 0;
    bool overflowed_ = false;
};

// Generic realtime packet queues (no transport wiring).
template <u16 PacketBytes = default_realtime_packet_bytes,
          u8 RxPackets = default_realtime_rx_packets,
          u8 TxPackets = default_realtime_tx_packets>
class RealtimePacketQueues {
public:
    static constexpr u16 packet_bytes() { return PacketBytes; }
    static constexpr u8 rx_capacity() { return RxPackets; }
    static constexpr u8 tx_capacity() { return TxPackets; }

    bool rx_push(const void* packet) { return rx_.push(packet); }
    bool rx_pop(void* packet) { return rx_.pop(packet); }
    bool tx_push(const void* packet) { return tx_.push(packet); }
    bool tx_pop(void* packet) { return tx_.pop(packet); }
    bool tx_peek(void* packet) const { return tx_.peek(packet); }
    bool tx_drop_oldest() { return tx_.drop_oldest(); }

    u8 rx_count() const { return rx_.count(); }
    u8 tx_count() const { return tx_.count(); }
    u16 rx_dropped() const { return rx_.dropped(); }

    // Sticky overflow diagnostic for RX path.
    bool consume_rx_overflowed() { return rx_.consume_overflowed(); }

    void clear() {
        rx_.clear();
        tx_.clear();
    }

private:
    PacketRing<PacketBytes, RxPackets> rx_{};
    PacketRing<PacketBytes, TxPackets> tx_{};
};

} // namespace net
} // namespace engine

#endif // ENGINE_NET_RING_H
