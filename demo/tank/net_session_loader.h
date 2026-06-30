#ifndef DEMO_TANK_NET_SESSION_LOADER_H
#define DEMO_TANK_NET_SESSION_LOADER_H

// net_session_loader.h — Stage 5B live session-lane transport coordinator
// (demo-local). Drives a reliable session connection, requests one asset
// transfer with credit-windowed pacing, forwards each session asset payload to
// the Stage 5A AssetLoader, and reports a frame-driven connection/loading state
// machine. Templated on the Session type so it runs over the real
// `Game::net.session` AND a fake session in host tests (the live FujiNet path
// cannot run without fujinet-lib).
//
// Two layers stay distinct:
//   * Session transport (EDGE session lane): TCP, reliable ordered bytes, the
//     session frame [kind][size_lo][size_hi][payload], RX/TX rings.
//   * Tank asset protocol (asset_protocol.h): the bytes inside an asset payload.
//
// Pacing rationale: the session RX ring (256 B) DROPS bytes + flags Overflow when
// full (engine/net_api.h drain_rx_), so there is no safe natural backpressure. A
// max asset session frame is 3+89 = 92 B; a credit window of 2 (<=184 B) fits the
// ring with margin. The client grants credit; the server sends at most that many
// messages, so in-flight bytes are bounded.

#include <engine/net_types.h>
#include <engine/attributes.h>

#include "asset_loader.h"
#include "asset_protocol.h"

namespace tank {

using engine::u8;
using engine::u16;
using engine::net::NetStatus;
using engine::net::SessionMessageView;

// ── Session message kinds (the session-frame `kind` byte; transport layer) ──
namespace sess {
inline constexpr u8 kAssetPayload = 1;   // server -> client: payload is a Stage 5A asset message
inline constexpr u8 kRequest      = 2;   // client -> server: start a transfer
inline constexpr u8 kCredit       = 3;   // client -> server: grant more messages
}  // namespace sess

// Client control payloads (little-endian, byte-by-byte; mirrored in the server).
//   Request: [version][transfer_id][asset_set][credit]   (4 bytes)
//   Credit : [transfer_id][credit]                        (2 bytes)
inline constexpr u8 kAssetSet      = 1;
inline constexpr u8 kCreditWindow  = 2;   // <= 2 max asset frames in flight (<=184 B < 256 RX)
inline constexpr u8 kDrainPerFrame = 4;   // bounded messages handled per loading frame

inline u16 build_request(u8* out, u8 transfer_id, u8 credit) {
    out[0] = proto::kVersion; out[1] = transfer_id; out[2] = kAssetSet; out[3] = credit;
    return 4;
}
inline u16 build_credit(u8* out, u8 transfer_id, u8 credit) {
    out[0] = transfer_id; out[1] = credit;
    return 2;
}

enum class NetState : u8 { Starting, Connecting, Requesting, Receiving, Ready, Failed };

enum class NetTransportError : u8 {
    None = 0, ConnectFailed, ConnectTimeout, ClosedEarly, RecvError, SendError,
    RxOverflow, UnexpectedKind, InactivityTimeout, LoaderFailed,
};

template <typename Session>
class NetAssetClient {
public:
    void begin(Session* session, PhysicalMap* map, u8* tileset_dest,
               const char* host, u16 port, u8 transfer_id,
               u16 connect_timeout_frames, u16 inactivity_timeout_frames) {
        session_ = session;
        host_ = host; port_ = port;
        transfer_id_ = transfer_id;
        connect_timeout_ = connect_timeout_frames;
        inactivity_timeout_ = inactivity_timeout_frames;
        state_ = NetState::Starting;
        terr_ = NetTransportError::None;
        timer_ = 0;
        outstanding_ = 0;
        msgs_received_ = 0; bytes_received_ = 0; last_kind_ = 0; overflow_seen_ = false;
        loader_.begin(map, tileset_dest);
    }

    // Drive one loading frame. Connect -> request -> receive chain so a frame
    // makes forward progress; the Receiving drain is still bounded per frame.
    EDGE_COLD void update() {
        if (state_ == NetState::Starting || state_ == NetState::Connecting) step_connect();
        if (state_ == NetState::Requesting) step_request();
        if (state_ == NetState::Receiving) step_receive();
    }

    bool ready()  const { return state_ == NetState::Ready; }
    bool failed() const { return state_ == NetState::Failed; }
    NetState          state()        const { return state_; }
    NetTransportError net_error()    const { return terr_; }
    LoadError         loader_error() const { return loader_.error(); }
    const AssetLoader& loader()      const { return loader_; }

    // Compact debug surface.
    u8  messages_received() const { return msgs_received_; }
    u16 bytes_received()    const { return bytes_received_; }
    u8  last_kind()         const { return last_kind_; }
    u8  outstanding()       const { return outstanding_; }
    bool overflow_seen()    const { return overflow_seen_; }

private:
    EDGE_COLD NetState fail(NetTransportError e) {
        terr_ = e;
        if (session_) session_->close();
        return state_ = NetState::Failed;
    }

    EDGE_COLD void step_connect() {
        const NetStatus st = session_->connect_tcp(host_, port_);
        if (st == NetStatus::Ok && session_->connected()) {
            state_ = NetState::Requesting; timer_ = 0; return;
        }
        if (st == NetStatus::WouldBlock) {                 // async connect: wait, with timeout
            state_ = NetState::Connecting;
            if (++timer_ > connect_timeout_) fail(NetTransportError::ConnectTimeout);
            return;
        }
        fail(NetTransportError::ConnectFailed);
    }

    EDGE_COLD void step_request() {
        u8 buf[8];
        const u16 n = build_request(buf, transfer_id_, kCreditWindow);
        if (session_->send_bytes(buf, n, sess::kRequest) != NetStatus::Ok) {
            fail(NetTransportError::SendError); return;
        }
        outstanding_ = kCreditWindow;
        state_ = NetState::Receiving; timer_ = 0;
    }

    EDGE_COLD void step_receive() {
        const NetStatus pst = session_->poll();
        if (pst == NetStatus::Overflow) { overflow_seen_ = true; fail(NetTransportError::RxOverflow); return; }

        bool got = false;
        SessionMessageView view;
        for (u8 i = 0; i < kDrainPerFrame; ++i) {
            if (!session_->recv(view)) break;              // WouldBlock / no complete frame
            got = true;
            last_kind_ = view.kind;
            if (view.kind != sess::kAssetPayload) { fail(NetTransportError::UnexpectedKind); return; }
            loader_.consume(view.data, view.size);
            if (outstanding_) --outstanding_;
            ++msgs_received_;
            bytes_received_ = static_cast<u16>(bytes_received_ + view.size);
            if (loader_.failed())   { fail(NetTransportError::LoaderFailed); return; }
            if (loader_.complete()) { state_ = NetState::Ready; return; }
        }

        // Connection dropped before completion?
        if (!session_->connected()) { fail(NetTransportError::ClosedEarly); return; }

        // Replenish the credit window so the server keeps sending (bounded in-flight).
        if (outstanding_ < kCreditWindow) {
            u8 buf[4];
            const u16 n = build_credit(buf, transfer_id_, static_cast<u8>(kCreditWindow - outstanding_));
            if (session_->send_bytes(buf, n, sess::kCredit) != NetStatus::Ok) {
                fail(NetTransportError::SendError); return;
            }
            outstanding_ = kCreditWindow;
        }

        if (got) timer_ = 0;
        else if (++timer_ > inactivity_timeout_) fail(NetTransportError::InactivityTimeout);
    }

    Session*    session_ = nullptr;
    const char* host_ = nullptr;
    u16         port_ = 0;
    u8          transfer_id_ = 0;
    u16         connect_timeout_ = 0;
    u16         inactivity_timeout_ = 0;
    u16         timer_ = 0;
    u8          outstanding_ = 0;
    NetState    state_ = NetState::Starting;
    NetTransportError terr_ = NetTransportError::None;
    AssetLoader loader_;
    // Debug counters.
    u8   msgs_received_ = 0;
    u16  bytes_received_ = 0;
    u8   last_kind_ = 0;
    bool overflow_seen_ = false;
};

}  // namespace tank

#endif  // DEMO_TANK_NET_SESSION_LOADER_H
