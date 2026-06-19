// test_net_session_loader.cpp — Stage 5B transport-coordinator tests
// (demo/tank/net_session_loader.h) driven by a FAKE session (the live FujiNet
// path cannot run without fujinet-lib). Built for mos-sim, run under CTest.

#include <stdint.h>
#include <stdio.h>

#include "demo/tank/net_session_loader.h"
#include "demo/tank/asset_protocol.h"
#include "demo/tank/playfield_geometry.h"

using engine::u8;
using engine::u16;
using engine::net::NetStatus;
using engine::net::SessionMessageView;
namespace P = tank::proto;
using tank::NetState;
using tank::NetTransportError;

static unsigned g_failures = 0;
#define CHECK(c) do { if(!(c)){ printf("FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); ++g_failures; } } while(0)

// ── Reference assets + Stage 5A payload builder (server side) ────────────────
static u8 g_ts[1024];
static u8 g_ck[4][960];
static void make_ref() {
    for (u16 i = 0; i < 1024; ++i) g_ts[i] = static_cast<u8>((i * 5 + 1) & 0xFF);
    for (u8 k = 0; k < 4; ++k) for (u16 i = 0; i < 960; ++i)
        g_ck[k][i] = static_cast<u8>((k * 40 + (i & 0x3F)) & 0xFF);
}
// payload index: 0 manifest, 1..16 tileset, 17..64 chunk pairs, 65 complete.
static u16 build_payload(u8 idx, u8 xfer, u8* out) {
    if (idx == 0) return P::build_manifest(out, xfer);
    if (idx <= 16) { u16 b = static_cast<u16>(idx - 1);
        return P::build_tileset_block(out, xfer, static_cast<u16>(b * 64), &g_ts[b * 64], 64); }
    if (idx <= 64) { u8 j = static_cast<u8>(idx - 17); u8 ch = static_cast<u8>(j / 12);
        u8 pr = static_cast<u8>(j % 12); u8 cx = static_cast<u8>(ch & 1), cy = static_cast<u8>(ch >> 1);
        u8 s = static_cast<u8>(pr * 2);
        return P::build_chunk_rows(out, xfer, cx, cy, s, 2, &g_ck[cy * 2 + cx][s * 40]); }
    return P::build_complete(out, xfer);
}
static constexpr u8 kPayloadCount = 66;

// ── Fake session: a scriptable server-in-a-box ──────────────────────────────
struct FakeSession {
    // Behaviour knobs.
    NetStatus connect_result = NetStatus::Ok;
    NetStatus poll_result    = NetStatus::Ok;
    bool      connected_     = false;
    u8        wrong_xfer     = 0;     // if non-zero, stamp payloads with this id
    bool      wrong_kind     = false; // send first payload with a bad session kind
    u8        close_after    = 255;   // drop the connection after N delivered messages

    // Server transfer state.
    int  credit = 0;
    u8   next   = 0;        // next payload index to enqueue
    u8   adopted_xfer = 0;
    u8   max_depth = 0;     // peak queued messages (in-flight) seen
    u8   requests = 0, credits = 0;
    u8   last_req_credit = 0;

    // Pending session messages (kind + payload), a tiny queue.
    struct Q { u8 kind; u16 size; u8 data[128]; };
    Q    q[8];
    u8   qhead = 0, qtail = 0, qcount = 0;
    u8   delivered = 0;

    NetStatus connect_tcp(const char*, u16) { connected_ = (connect_result == NetStatus::Ok); return connect_result; }
    bool connected() const { return connected_; }
    void close() { connected_ = false; }

    NetStatus send_bytes(const void* data, u16 size, u8 kind) {
        const u8* p = static_cast<const u8*>(data);
        if (kind == tank::sess::kRequest) { ++requests; adopted_xfer = p[1]; credit = p[3]; last_req_credit = p[3]; next = 0; }
        else if (kind == tank::sess::kCredit) { ++credits; credit += p[1]; }
        (void)size;
        return NetStatus::Ok;
    }

    NetStatus poll() {
        if (poll_result != NetStatus::Ok) return poll_result;
        // Server sends up to `credit` asset messages while queue has room.
        while (credit > 0 && next < kPayloadCount && qcount < 8) {
            Q& slot = q[qtail];
            // Mis-stamp only non-manifest payloads, so the manifest adopts the
            // correct id and a later block trips the loader's WrongTransfer check.
            const u8 stamp = (wrong_xfer && next > 0) ? wrong_xfer : adopted_xfer;
            slot.size = build_payload(next, stamp, slot.data);
            slot.kind = (wrong_kind && next == 0) ? 99 : tank::sess::kAssetPayload;
            qtail = static_cast<u8>((qtail + 1) & 7); ++qcount;
            if (qcount > max_depth) max_depth = qcount;
            --credit; ++next;
        }
        return NetStatus::Ok;
    }

    bool recv(SessionMessageView& v) {
        if (qcount == 0) return false;
        Q& slot = q[qhead];
        v.kind = slot.kind; v.data = slot.data; v.size = slot.size;
        qhead = static_cast<u8>((qhead + 1) & 7); --qcount;
        if (++delivered >= close_after) connected_ = false;   // simulate early close
        return true;
    }
};

static FakeSession g_fs;
static tank::PhysicalMap g_map;
static u8 g_tsdest[1024];
static tank::NetAssetClient<FakeSession> g_client;

static void begin(u8 xfer) {
    g_client.begin(&g_fs, &g_map, g_tsdest, "host", 9000, xfer,
                   /*connect_to*/ 60, /*inactivity_to*/ 60);
}
static void run(int frames) { for (int i = 0; i < frames && !g_client.ready() && !g_client.failed(); ++i) g_client.update(); }

int main() {
    make_ref();

    // 1 connection state progression: Starting -> Requesting -> Receiving -> Ready.
    g_fs = FakeSession{}; begin(0x11);
    CHECK(g_client.state() == NetState::Starting);
    g_client.update();                              // connect -> request
    CHECK(g_fs.requests == 1);                      // 2 client transfer request encoded/sent
    CHECK(g_fs.adopted_xfer == 0x11);              // request carried our transfer id
    CHECK(g_fs.last_req_credit == tank::kCreditWindow);  // 13 credit initialized
    CHECK(g_client.state() == NetState::Receiving);
    run(300);
    CHECK(g_client.ready());                        // 10 complete loader -> Ready
    // 3 asset kind accepted + 5 payload forwarded unchanged + 6 multi-frame drain.
    CHECK(g_client.loader().complete());
    CHECK(g_client.messages_received() == kPayloadCount);
    // verify the tileset + a chunk byte round-tripped to the destinations.
    CHECK(g_tsdest[0] == g_ts[0] && g_tsdest[1023] == g_ts[1023]);
    CHECK(g_map.cells[4] == g_ck[0][0]);
    // 15 server never exceeded the granted window.
    CHECK(g_fs.max_depth <= tank::kCreditWindow);
    // 14 credit replenished over the transfer.
    CHECK(g_fs.credits > 0);

    // 7 per-frame drain bound respected (<= kDrainPerFrame messages per update).
    {
        g_fs = FakeSession{}; g_fs.credit = 0; begin(0x22);
        g_client.update();                          // -> Receiving, request grants window
        g_fs.credit = 100;                          // server allowed to flood its queue
        g_fs.poll();                                // fill queue to depth 8
        const u8 before = g_client.messages_received();
        g_client.update();                          // one frame
        const u8 drained = static_cast<u8>(g_client.messages_received() - before);
        CHECK(drained <= tank::kDrainPerFrame);
    }

    // 4 unexpected session kind rejected.
    g_fs = FakeSession{}; g_fs.wrong_kind = true; begin(0x33);
    run(50);
    CHECK(g_client.failed() && g_client.net_error() == NetTransportError::UnexpectedKind);

    // 8 transport close before completion fails (ClosedEarly).
    g_fs = FakeSession{}; g_fs.close_after = 5; begin(0x44);
    run(300);
    CHECK(g_client.failed() && g_client.net_error() == NetTransportError::ClosedEarly);

    // 9 loader failure becomes loading failure (17 stale transfer payload rejected).
    g_fs = FakeSession{}; g_fs.wrong_xfer = 0x99; begin(0x44);   // payloads stamped 0x99 != 0x44
    run(50);
    CHECK(g_client.failed() && g_client.net_error() == NetTransportError::LoaderFailed);
    CHECK(g_client.loader_error() == tank::LoadError::WrongTransfer);

    // 11 connection timeout (connect keeps returning WouldBlock).
    g_fs = FakeSession{}; g_fs.connect_result = NetStatus::WouldBlock; begin(0x55);
    run(200);
    CHECK(g_client.failed() && g_client.net_error() == NetTransportError::ConnectTimeout);

    // connect failed (Unsupported, as the stub returns).
    g_fs = FakeSession{}; g_fs.connect_result = NetStatus::Unsupported; begin(0x56);
    g_client.update();
    CHECK(g_client.failed() && g_client.net_error() == NetTransportError::ConnectFailed);

    // 12 inactivity timeout (connected, request sent, but no data ever arrives).
    g_fs = FakeSession{}; begin(0x57);
    g_client.update();                              // -> Receiving
    g_fs.credit = 0; g_fs.next = kPayloadCount;     // server sends nothing
    run(200);
    CHECK(g_client.failed() && g_client.net_error() == NetTransportError::InactivityTimeout);

    // 16 RX overflow state causes failure.
    g_fs = FakeSession{}; begin(0x58);
    g_client.update();                              // -> Receiving
    g_fs.poll_result = NetStatus::Overflow;
    g_client.update();
    CHECK(g_client.failed() && g_client.net_error() == NetTransportError::RxOverflow);

    if (g_failures == 0) printf("ALL TESTS PASSED\n");
    else                 printf("%u FAILURES\n", g_failures);
    return g_failures != 0;
}
