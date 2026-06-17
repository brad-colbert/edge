// test_net_rings.cpp — generic tests for engine/net types and fixed-size rings.
//
// Stage-3 scope only: ByteRing, PacketRing, default realtime queue sizing,
// overflow diagnostics, and packet-size validation helpers.

#include <stdio.h>

#include <engine/net.h>

using engine::u8;
using engine::u16;

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

namespace n = engine::net;

// Packet-size helper compile-time checks.
struct P16 { u8 b[16]; };
struct P8  { u8 b[8]; };

static_assert(n::packet_size_valid<P16, 16>::value, "P16 must validate for 16 bytes");
static_assert(!n::packet_size_valid<P8, 16>::value, "P8 must not validate for 16 bytes");

// Default realtime settings from the API contract.
static_assert(n::default_realtime_packet_bytes == 16, "default packet bytes must be 16");
static_assert(n::default_realtime_rx_packets == 8, "default RX packets must be 8");
static_assert(n::default_realtime_tx_packets == 2, "default TX packets must be 2");

static void test_bytering_push_pop() {
    n::ByteRing<4> r;
    CHECK(r.empty());
    CHECK(r.capacity() == 4);

    CHECK(r.push(1));
    CHECK(r.push(2));
    CHECK(r.push(3));
    CHECK(r.count() == 3);

    u8 v = 0;
    CHECK(r.pop(v)); CHECK(v == 1);
    CHECK(r.pop(v)); CHECK(v == 2);
    CHECK(r.pop(v)); CHECK(v == 3);
    CHECK(r.empty());
}

static void test_bytering_wraparound() {
    n::ByteRing<4> r;
    u8 v = 0;

    CHECK(r.push(10));
    CHECK(r.push(11));
    CHECK(r.push(12));
    CHECK(r.pop(v)); CHECK(v == 10);
    CHECK(r.pop(v)); CHECK(v == 11);

    // Wrap write index to start.
    CHECK(r.push(13));
    CHECK(r.push(14));
    CHECK(r.push(15));

    CHECK(r.count() == 4);
    CHECK(r.pop(v)); CHECK(v == 12);
    CHECK(r.pop(v)); CHECK(v == 13);
    CHECK(r.pop(v)); CHECK(v == 14);
    CHECK(r.pop(v)); CHECK(v == 15);
    CHECK(r.empty());
}

static void test_bytering_overflow() {
    n::ByteRing<2> r;
    CHECK(r.push(1));
    CHECK(r.push(2));
    CHECK(!r.push(3));
    CHECK(r.full());
    CHECK(r.overflowed());
    CHECK(r.consume_overflowed());
    CHECK(!r.consume_overflowed());
}

static void test_packetring_push_pop() {
    n::PacketRing<4, 4> r;
    u8 p0[4] = {1, 2, 3, 4};
    u8 p1[4] = {5, 6, 7, 8};
    u8 out[4] = {};

    CHECK(r.push(p0));
    CHECK(r.push(p1));
    CHECK(r.count() == 2);

    CHECK(r.pop(out));
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4);
    CHECK(r.pop(out));
    CHECK(out[0] == 5 && out[1] == 6 && out[2] == 7 && out[3] == 8);
    CHECK(r.empty());
}

static void test_packetring_wraparound() {
    n::PacketRing<2, 3> r;
    u8 a[2] = {1, 1};
    u8 b[2] = {2, 2};
    u8 c[2] = {3, 3};
    u8 d[2] = {4, 4};
    u8 out[2] = {};

    CHECK(r.push(a));
    CHECK(r.push(b));
    CHECK(r.push(c));
    CHECK(r.pop(out)); CHECK(out[0] == 1);
    CHECK(r.pop(out)); CHECK(out[0] == 2);

    // Wrap push positions.
    CHECK(r.push(d));

    CHECK(r.pop(out)); CHECK(out[0] == 3);
    CHECK(r.pop(out)); CHECK(out[0] == 4);
    CHECK(r.empty());
}

static void test_packetring_preserves_multiple_packets() {
    n::PacketRing<2, 4> r;
    u8 p0[2] = {10, 0};
    u8 p1[2] = {20, 0};
    u8 p2[2] = {30, 0};
    u8 out[2] = {};

    CHECK(r.push(p0));
    CHECK(r.push(p1));
    CHECK(r.push(p2));
    CHECK(r.count() == 3);

    CHECK(r.pop(out)); CHECK(out[0] == 10);
    CHECK(r.pop(out)); CHECK(out[0] == 20);
    CHECK(r.pop(out)); CHECK(out[0] == 30);
}

static void test_packetring_drop_oldest_overflow() {
    n::PacketRing<2, 2> r;
    u8 a[2] = {1, 0};
    u8 b[2] = {2, 0};
    u8 c[2] = {3, 0};
    u8 out[2] = {};

    CHECK(r.push(a));
    CHECK(r.push(b));
    CHECK(r.full());

    // Overflow: drop oldest (a), keep (b), then enqueue (c).
    CHECK(r.push(c));
    CHECK(r.count() == 2);
    CHECK(r.dropped() == 1);
    CHECK(r.consume_overflowed());
    CHECK(!r.consume_overflowed());

    CHECK(r.pop(out)); CHECK(out[0] == 2);
    CHECK(r.pop(out)); CHECK(out[0] == 3);
}

static void test_realtime_queue_rx_overflow_diagnostics() {
    n::RealtimePacketQueues<> q;
    u8 p[16] = {};
    u8 out[16] = {};

    // Fill RX ring to capacity.
    for (u8 i = 0; i < q.rx_capacity(); ++i) {
        p[0] = i;
        CHECK(q.rx_push(p));
    }
    CHECK(q.rx_count() == q.rx_capacity());

    // Overflow once.
    p[0] = 99;
    CHECK(q.rx_push(p));
    CHECK(q.rx_dropped() == 1);
    CHECK(q.consume_rx_overflowed());
    CHECK(!q.consume_rx_overflowed());

    // Oldest packet (0) should have been dropped.
    CHECK(q.rx_pop(out)); CHECK(out[0] == 1);
}

static void test_packet_size_validation_helpers() {
    // Runtime-adjacent compile check via template API usage.
    n::PacketRing<16, 2> r;
    P16 p{};
    CHECK(r.push_packet(p));

    P16 out{};
    CHECK(r.pop_packet(out));
}

int main() {
    test_bytering_push_pop();
    test_bytering_wraparound();
    test_bytering_overflow();

    test_packetring_push_pop();
    test_packetring_wraparound();
    test_packetring_preserves_multiple_packets();
    test_packetring_drop_oldest_overflow();
    test_realtime_queue_rx_overflow_diagnostics();
    test_packet_size_validation_helpers();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
