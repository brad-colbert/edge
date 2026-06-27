// tests/backends/atari/test_netstream_adapter_lifecycle.cpp
// Stage 9R.1: realtime adapter lifecycle state-machine self-test (FakeOps; no SIOV).
//
// Drives NetstreamRealtimeAdapterT<FakeOps> — the ops-policy adapter with a SCRIPTED
// backend — so the lifecycle (open->init->begin, close->end, active, poll/status,
// last_error, port byte-order conversion) is validated deterministically WITHOUT real
// SIOV / begin / the OS-vector-page writes. RealNetstreamOps (the real ABI) is never
// instantiated here, so this links and runs under mos-sim with no handler.S/abi.s.
//
// Built with EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1 (forced on the target) so the
// templated adapter exists; the FakeOps adapter has its own static State, independent of
// the production RealNetstreamOps adapter.

#include <stdint.h>
#include <stdio.h>

#include <engine/platform/atari/fujinet_netstream_realtime.h>

namespace nsr = atari::fujinet_netstream;
namespace n = engine::net;

// Scripted backend ops: record the exact init args, script init/status returns, count calls.
struct FakeOps {
    static const char* last_host;
    static uint8_t  last_flags;
    static uint16_t last_baud;
    static uint16_t last_port;
    static uint8_t  init_rc;      // scripted init result (0 = success, 1 = failure)
    static uint8_t  status_val;   // scripted get_status byte
    static int init_calls, begin_calls, end_calls, status_calls, settle_calls;

    // 9R.2 data-path model.
    static uint8_t  tx_free;          // scripted tx_space() return (0..128)
    static uint8_t  tx_captured[64];  // bytes written via send_byte, in order
    static int      tx_count;         // number captured
    static int      send_fail_at;     // -1 = never; else send_byte returns full once
                                      // tx_count >= send_fail_at (mid-packet failure)
    static uint16_t rx_avail;         // scripted bytes_avail() return
    static uint8_t  rx_data[64];      // RX bytes to deliver via recv_packed()
    static int      rx_pos;           // next index into rx_data (bytes consumed so far)
    static int      rx_empty_at;      // -1 = never; else recv_packed() returns empty once
                                      // rx_pos >= rx_empty_at (mid-packet empty)

    static uint8_t init(const char* host, uint8_t flags, uint16_t baud, uint16_t port) {
        last_host = host; last_flags = flags; last_baud = baud; last_port = port;
        ++init_calls; return init_rc;
    }
    static void    begin()  { ++begin_calls; }
    static void    end()    { ++end_calls; }
    static void    settle() { ++settle_calls; }   // no-op delay stand-in (must not block tests)
    static uint8_t status() { ++status_calls; return status_val; }

    static uint8_t  tx_space()    { return tx_free; }
    static uint8_t  send_byte(uint8_t b) {
        if (send_fail_at >= 0 && tx_count >= send_fail_at) return 1;  // full
        tx_captured[tx_count++] = b;
        return 0;  // accepted
    }
    static uint16_t bytes_avail() { return rx_avail; }
    static uint16_t recv_packed() {
        if (rx_empty_at >= 0 && rx_pos >= rx_empty_at) return (uint16_t)(1u << 8);  // empty
        const uint8_t d = rx_data[rx_pos++];
        return d;  // status byte (high) = 0
    }
};
const char* FakeOps::last_host = nullptr;
uint8_t  FakeOps::last_flags = 0;
uint16_t FakeOps::last_baud = 0;
uint16_t FakeOps::last_port = 0;
uint8_t  FakeOps::init_rc = 0;
uint8_t  FakeOps::status_val = 0;
int FakeOps::init_calls = 0, FakeOps::begin_calls = 0, FakeOps::end_calls = 0,
    FakeOps::status_calls = 0, FakeOps::settle_calls = 0;
uint8_t  FakeOps::tx_free = 0;
uint8_t  FakeOps::tx_captured[64] = {};
int      FakeOps::tx_count = 0;
int      FakeOps::send_fail_at = -1;
uint16_t FakeOps::rx_avail = 0;
uint8_t  FakeOps::rx_data[64] = {};
int      FakeOps::rx_pos = 0;
int      FakeOps::rx_empty_at = -1;

using A = nsr::NetstreamRealtimeAdapterT<FakeOps>;

static unsigned g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

int main() {
    // ----- bad args: rejected before any init; active unchanged -----
    FakeOps::init_calls = 0;
    CHECK(A::realtime_open_udp_seq(nullptr, 1234, 1) == n::NetStatus::InvalidArgument);
    CHECK(A::realtime_open_udp_seq("", 1234, 1) == n::NetStatus::InvalidArgument);
    CHECK(A::realtime_open_udp_seq("host", 0, 1) == n::NetStatus::InvalidArgument);
    CHECK(FakeOps::init_calls == 0);
    CHECK(!A::realtime_active());
    CHECK(A::realtime_last_error().status == n::NetStatus::InvalidArgument);

    // ----- open success: init+begin+settle, active, exact args (incl. converted port) -----
    FakeOps::init_calls = FakeOps::begin_calls = FakeOps::settle_calls = 0;
    FakeOps::init_rc = 0;
    static const char host[] = "host";
    CHECK(A::realtime_open_udp_seq(host, 0x1234, 5000) == n::NetStatus::Ok);
    CHECK(A::realtime_active());
    CHECK(FakeOps::init_calls == 1);
    CHECK(FakeOps::begin_calls == 1);
    CHECK(FakeOps::settle_calls == 1);    // settle runs once on a successful open
    CHECK(FakeOps::last_host == host);                 // exact host pointer
    CHECK(FakeOps::last_flags == nsr::kNetstreamFlags);
    CHECK(FakeOps::last_baud == nsr::kNetstreamNominalBaud);
    CHECK(FakeOps::last_port == nsr::to_netstream_port_arg(0x1234));  // 0x1234 -> 0x3412
    CHECK(FakeOps::last_port == 0x3412);
    CHECK(A::realtime_last_error().status == n::NetStatus::Ok);

    // ----- open while active: BadConfig, init NOT called again -----
    {
        const int before = FakeOps::init_calls;
        CHECK(A::realtime_open_udp_seq(host, 0x1234, 5000) == n::NetStatus::BadConfig);
        CHECK(FakeOps::init_calls == before);          // no second init
        CHECK(A::realtime_active());                   // still active
    }

    // ----- poll active, status 0x00 -> Ok, last_error Ok -----
    FakeOps::status_val = 0x00;
    CHECK(A::realtime_poll() == n::NetStatus::Ok);
    CHECK(A::realtime_last_error().status == n::NetStatus::Ok);
    CHECK(A::state().last_netstream_status == 0x00);

    // ----- poll active, status 0x10 -> Ok, last_error {Overflow, 0x10} -----
    FakeOps::status_val = 0x10;
    CHECK(A::realtime_poll() == n::NetStatus::Ok);
    {
        const n::NetError e = A::realtime_last_error();
        CHECK(e.status == n::NetStatus::Overflow);
        CHECK(e.detail == 0x10);
        CHECK(A::state().last_netstream_status == 0x10);
    }

    // ----- send/recv (9R.1 non-pumping): zero -> Ok, nonzero -> WouldBlock -----
    {
        uint8_t buf[16] = {};
        CHECK(A::realtime_send_nb(buf, 0) == n::NetStatus::Ok);
        CHECK(A::realtime_send_nb(buf, 16) == n::NetStatus::WouldBlock);
        CHECK(A::realtime_recv_nb(buf, 0) == n::NetStatus::Ok);
        CHECK(A::realtime_recv_nb(buf, 16) == n::NetStatus::WouldBlock);
        CHECK(A::realtime_send_nb(nullptr, 1) == n::NetStatus::InvalidArgument);
        CHECK(A::realtime_recv_nb(nullptr, 1) == n::NetStatus::InvalidArgument);
    }

    // ----- bind unsupported -----
    CHECK(A::realtime_bind_udp_seq(5000) == n::NetStatus::Unsupported);

    // ----- close while active: end called, inactive, last_error Closed -----
    {
        const int before = FakeOps::end_calls;
        A::realtime_close();
        CHECK(FakeOps::end_calls == before + 1);
        CHECK(!A::realtime_active());
        CHECK(A::realtime_last_error().status == n::NetStatus::Closed);
    }

    // ----- close while inactive: end NOT called, last_error still Closed -----
    {
        const int before = FakeOps::end_calls;
        A::realtime_close();
        CHECK(FakeOps::end_calls == before);            // end not called again
        CHECK(!A::realtime_active());
        CHECK(A::realtime_last_error().status == n::NetStatus::Closed);
    }

    // ----- poll while inactive -> Closed -----
    CHECK(A::realtime_poll() == n::NetStatus::Closed);

    // ----- open init failure: TransportError, inactive, begin NOT called -----
    {
        FakeOps::init_rc = 1;
        const int begin_before = FakeOps::begin_calls;
        const int settle_before = FakeOps::settle_calls;
        CHECK(A::realtime_open_udp_seq(host, 1234, 1) == n::NetStatus::TransportError);
        CHECK(!A::realtime_active());
        CHECK(FakeOps::begin_calls == begin_before);    // begin not entered on init failure
        CHECK(FakeOps::settle_calls == settle_before);  // settle not entered either
        CHECK(A::realtime_last_error().status == n::NetStatus::TransportError);
    }

    // ================= 9R.2 data path (TX all-or-nothing / RX full-packet) =================
    uint8_t pkt[16];
    for (int i = 0; i < 16; ++i) pkt[i] = static_cast<uint8_t>(0xA0 + i);
    uint8_t out[16] = {};

    // ----- send/recv while inactive -> Closed (state still inactive from init-fail above) -----
    CHECK(!A::realtime_active());
    CHECK(A::realtime_send_nb(pkt, 16) == n::NetStatus::Closed);
    CHECK(A::realtime_recv_nb(out, 16) == n::NetStatus::Closed);

    // activate for the data-path cases
    FakeOps::init_rc = 0;
    CHECK(A::realtime_open_udp_seq(host, 0x1234, 0) == n::NetStatus::Ok);
    CHECK(A::realtime_active());

    // ----- arg validation (active): null / zero / oversize -----
    // The lane drives the packet size (its compile-time PacketBytes); the adapter
    // honours any size up to the 128-byte NS ring and rejects only oversize.
    CHECK(A::realtime_send_nb(nullptr, 16) == n::NetStatus::InvalidArgument);
    CHECK(A::realtime_send_nb(pkt, 0) == n::NetStatus::Ok);
    CHECK(A::realtime_send_nb(pkt, 129) == n::NetStatus::InvalidArgument);   // > kMaxPacketBytes
    CHECK(A::realtime_recv_nb(nullptr, 16) == n::NetStatus::InvalidArgument);
    CHECK(A::realtime_recv_nb(out, 0) == n::NetStatus::Ok);
    CHECK(A::realtime_recv_nb(out, 129) == n::NetStatus::InvalidArgument);   // > kMaxPacketBytes

    // ----- TX: tx_space < 16 -> WouldBlock, NOTHING written -----
    FakeOps::tx_count = 0; FakeOps::send_fail_at = -1; FakeOps::tx_free = 15;
    CHECK(A::realtime_send_nb(pkt, 16) == n::NetStatus::WouldBlock);
    CHECK(FakeOps::tx_count == 0);

    // ----- TX: tx_space == 16 -> Ok, EXACTLY 16 bytes in order -----
    FakeOps::tx_count = 0; FakeOps::tx_free = 16;
    CHECK(A::realtime_send_nb(pkt, 16) == n::NetStatus::Ok);
    CHECK(FakeOps::tx_count == 16);
    { int ok = 1; for (int i = 0; i < 16; ++i) if (FakeOps::tx_captured[i] != pkt[i]) ok = 0;
      CHECK(ok); }

    // ----- TX: tx_space > 16 -> Ok, EXACTLY 16 bytes -----
    FakeOps::tx_count = 0; FakeOps::tx_free = 100;
    CHECK(A::realtime_send_nb(pkt, 16) == n::NetStatus::Ok);
    CHECK(FakeOps::tx_count == 16);

    // ----- TX: unexpected mid-packet full after pre-check -> TransportError (NOT Ok) -----
    FakeOps::tx_count = 0; FakeOps::tx_free = 16; FakeOps::send_fail_at = 8;
    {
        const n::NetStatus r = A::realtime_send_nb(pkt, 16);
        CHECK(r == n::NetStatus::TransportError);
        CHECK(r != n::NetStatus::Ok);          // never reported as success
        CHECK(FakeOps::tx_count == 8);         // 8 accepted before the (impossible) failure
    }
    FakeOps::send_fail_at = -1;

    // RX data fixture: 0x10,0x11,...
    for (int i = 0; i < 32; ++i) FakeOps::rx_data[i] = static_cast<uint8_t>(0x10 + i);

    // ----- RX: bytes_avail < 16 -> WouldBlock, NOTHING consumed -----
    FakeOps::rx_pos = 0; FakeOps::rx_empty_at = -1; FakeOps::rx_avail = 15;
    for (int i = 0; i < 16; ++i) out[i] = 0;
    CHECK(A::realtime_recv_nb(out, 16) == n::NetStatus::WouldBlock);
    CHECK(FakeOps::rx_pos == 0);

    // ----- RX: bytes_avail == 16 -> Ok, EXACTLY 16 copied -----
    FakeOps::rx_pos = 0; FakeOps::rx_avail = 16;
    CHECK(A::realtime_recv_nb(out, 16) == n::NetStatus::Ok);
    CHECK(FakeOps::rx_pos == 16);
    { int ok = 1; for (int i = 0; i < 16; ++i) if (out[i] != FakeOps::rx_data[i]) ok = 0;
      CHECK(ok); }

    // ----- RX: bytes_avail > 16 -> Ok, first 16 copied, REMAINING preserved -----
    FakeOps::rx_pos = 0; FakeOps::rx_avail = 20;
    CHECK(A::realtime_recv_nb(out, 16) == n::NetStatus::Ok);
    CHECK(FakeOps::rx_pos == 16);                                  // exactly 16 consumed
    CHECK(FakeOps::rx_data[16] == static_cast<uint8_t>(0x10 + 16)); // 17th byte preserved
    { int ok = 1; for (int i = 0; i < 16; ++i) if (out[i] != FakeOps::rx_data[i]) ok = 0;
      CHECK(ok); }

    // ----- RX: unexpected mid-packet empty after pre-check -> TransportError -----
    FakeOps::rx_pos = 0; FakeOps::rx_avail = 16; FakeOps::rx_empty_at = 8;
    CHECK(A::realtime_recv_nb(out, 16) == n::NetStatus::TransportError);
    CHECK(FakeOps::rx_pos == 8);
    FakeOps::rx_empty_at = -1;

    // ----- poll overflow status survives a send (send/recv do not clobber last_error) -----
    FakeOps::status_val = 0x10;
    CHECK(A::realtime_poll() == n::NetStatus::Ok);
    CHECK(A::realtime_last_error().status == n::NetStatus::Overflow);
    FakeOps::tx_count = 0; FakeOps::tx_free = 16;
    CHECK(A::realtime_send_nb(pkt, 16) == n::NetStatus::Ok);
    CHECK(A::realtime_last_error().status == n::NetStatus::Overflow);  // unchanged by send_nb

    A::realtime_close();

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream adapter lifecycle + data path 9R.2)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
