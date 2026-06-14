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
    static int init_calls, begin_calls, end_calls, status_calls;

    static uint8_t init(const char* host, uint8_t flags, uint16_t baud, uint16_t port) {
        last_host = host; last_flags = flags; last_baud = baud; last_port = port;
        ++init_calls; return init_rc;
    }
    static void    begin()  { ++begin_calls; }
    static void    end()    { ++end_calls; }
    static uint8_t status() { ++status_calls; return status_val; }
};
const char* FakeOps::last_host = nullptr;
uint8_t  FakeOps::last_flags = 0;
uint16_t FakeOps::last_baud = 0;
uint16_t FakeOps::last_port = 0;
uint8_t  FakeOps::init_rc = 0;
uint8_t  FakeOps::status_val = 0;
int FakeOps::init_calls = 0, FakeOps::begin_calls = 0, FakeOps::end_calls = 0,
    FakeOps::status_calls = 0;

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

    // ----- open success: init+begin, active, exact args (incl. converted port) -----
    FakeOps::init_calls = FakeOps::begin_calls = 0;
    FakeOps::init_rc = 0;
    static const char host[] = "host";
    CHECK(A::realtime_open_udp_seq(host, 0x1234, 5000) == n::NetStatus::Ok);
    CHECK(A::realtime_active());
    CHECK(FakeOps::init_calls == 1);
    CHECK(FakeOps::begin_calls == 1);
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
        CHECK(A::realtime_open_udp_seq(host, 1234, 1) == n::NetStatus::TransportError);
        CHECK(!A::realtime_active());
        CHECK(FakeOps::begin_calls == begin_before);    // begin not entered on init failure
        CHECK(A::realtime_last_error().status == n::NetStatus::TransportError);
    }

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED (Netstream adapter lifecycle 9R.1)\n");
    } else {
        printf("\n%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
