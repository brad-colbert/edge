#ifndef ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_REALTIME_H
#define ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_REALTIME_H

// platform/atari/fujinet_netstream_realtime.h — Netstream realtime adapter.
//
// Stage 9R.1: production LIFECYCLE wiring. The adapter is now an ops-policy template
// NetstreamRealtimeAdapterT<Ops>; the default alias NetstreamRealtimeAdapter binds the
// real backend (RealNetstreamOps -> the 9Q.2 ABI). open->init->begin, close->end, active,
// poll(status), last_error are wired. The byte<->packet DATA PATH (send_nb/recv_nb) is
// deliberately NOT pumped here (it stays a benign WouldBlock); that lands in Stage 9R.2.
//
// Why a template: the netstream-ON unit tests build under mos-sim where SIOV / the OS
// vector-page + POKEY + IRQEN writes in begin cannot execute, and add_engine_test targets
// do not link the handler/ABI. Tests therefore instantiate NetstreamRealtimeAdapterT<FakeOps>
// to exercise the lifecycle state machine deterministically. RealNetstreamOps (the ABI) is
// linked only by the Atari .xex / Altirra probe targets; no mos-sim CTest instantiates it.
//
// Engine-owned framing: RealtimeLane (engine/net_api.h) + RealtimePacketQueues
// (engine/net_ring.h) own the RX/TX packet rings and call realtime_send_nb/recv_nb a whole
// 16-byte packet at a time. The adapter adds NO wire framing.

#include "../../types.h"
#include "../../net_types.h"

#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
#include "fujinet_netstream_realtime_abi.h"  // edge_ns_init_netstream / _edge_ns_* (ON only)
#endif

namespace atari {
namespace fujinet_netstream {

using engine::u8;
using engine::u16;
using engine::net::NetError;
using engine::net::NetStatus;

#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)

// ── Adapter policy: nominal baud / init flags / port byte order (internal) ───────────
//
// The public realtime API (Game::net.realtime.open_udp_seq) carries no baud/flags, so the
// adapter picks them. These are internal constants (no user-facing API change); retune here.
//
// Values match the proven-working upstream reference
// (fujinet-atari-netstream/examples/udp-sequence/atari_udp_sequence.c: flags 0x26, baud 31250),
// validated against fujinet-pc + NetSIO in 9R.3. The earlier 0x20 / 19200 (internal TX clock)
// never transmitted: FujiNet/NetSIO drives an EXTERNAL transmit clock, so TX_EXT (0x04) is required
// or POKEY never clocks the serial output and the output-ready IRQ never fires.
//
// kNetstreamNominalBaud: a BaudTable entry (31250 = row 0x7A12, AUDF3=21).
// kNetstreamFlags: UDP (bit0=0) + UDP-seq (0x20, the lane is open_udp_seq) + TX external clock
//   (0x04) + register (0x02). RX stays internal (0x08 clear). The PAL bit 0x10 is left 0 in the
//   seed — ns_init_prepare derives it from DetectPAL; do NOT pre-set it. The handler maps
//   flags & 0x0c == 0x04 -> SKCTL 0x10 (RX int, TX ext).
inline constexpr u16 kNetstreamNominalBaud = 31250;
inline constexpr u8  kNetstreamFlags       = 0x26;

// Host-order remote port -> the byte-swapped value edge_ns_init_netstream() expects in
// port_swapped (low byte -> DCB DAUX1, high byte -> DAUX2). This is the adapter's single,
// deterministic conversion. The Mode-A probe used port 0, so whether FujiNet accepts this
// exact ordering is a Stage 9R.3 Mode-B validation item — but the implementation is fixed.
static constexpr u16 to_netstream_port_arg(u16 remote_port) {
    return static_cast<u16>(((remote_port & 0x00ffu) << 8) |
                            ((remote_port & 0xff00u) >> 8));
}

// Frames to wait AFTER begin before the first transmit, so FujiNet/NetSIO can renegotiate the
// external SIO clock to the netstream baud. Validated against fujinet-pc in 9R.3: sending
// immediately after begin corrupts the first ~5-9 bytes (the firmware reports the new peer
// baudrate ~475 ms after the baud switch); ~30 frames (~0.5 s NTSC) makes the stream byte-perfect.
inline constexpr u8 kNetstreamSettleFrames = 30;

// ── Real backend ops: the 9Q.2 ABI. Linked only where handler.S/abi.s are linked ────────
// (the Atari .xex / Altirra probe). ODR-used only when NetstreamRealtimeAdapterT<RealNetstreamOps>
// methods that touch the backend are instantiated — never by FakeOps tests.
struct RealNetstreamOps {
    // 0 = success, 1 = failure (carry mapped by _edge_ns_init_run).
    static u8 init(const char* host, u8 flags, u16 baud, u16 port) {
        return edge_ns_init_netstream(host, flags, baud, port);
    }
    static void begin() { _edge_ns_begin_stream(); }
    static void end()   { _edge_ns_end_stream(); }
    static u8   status(){ return _edge_ns_get_status(); }
    // Wait kNetstreamSettleFrames OS frames (RTCLOK low byte $14) for the external TX clock to
    // renegotiate after begin, before the first transmit. Atari-specific; only compiled into
    // the real backend (FakeOps::settle is a no-op, so unit tests never block here).
    static void settle() {
        volatile u8* const rtclok = (volatile u8*)0x0014;
        u8 last = *rtclok, frames = 0;
        while (frames < kNetstreamSettleFrames) {
            const u8 v = *rtclok, d = (u8)(v - last);
            if (d) { frames = (u8)(frames + d); last = v; }
        }
    }
    // 9R.2 data path.
    static u8   send_byte(u8 b) { return _edge_ns_send_byte(b); }       // 0 sent / 1 full
    static u16  recv_packed()   { return _edge_ns_recv_byte_packed(); } // lo=data, hi=status
    static u16  bytes_avail()   { return _edge_ns_bytes_avail(); }      // RX bytes buffered
    static u8   tx_space()      { return _edge_ns_tx_space(); }         // TX free 0..128
};

// ── Ops-policy realtime adapter ─────────────────────────────────────────────────────────
template <class Ops = RealNetstreamOps>
struct NetstreamRealtimeAdapterT {
    // Minimal cached state (≤ 8 bytes; field names are asserted by test_netstream_state_size).
    struct State {
        bool    active = false;
        NetError last_error{NetStatus::Unsupported, 0};
        u8      last_netstream_status = 0;
    };

    static State& state() {
        static State s{};
        return s;
    }

    // open -> init -> begin. Active only after BOTH succeed; otherwise fail closed.
    static NetStatus realtime_open_udp_seq(const char* host, u16 remote_port, u16 local_port) {
        (void)local_port;  // no separate bind primitive; port is fixed at init (see bind).
        State& s = state();

        if (host == nullptr || host[0] == '\0' || remote_port == 0) {
            s.last_error = NetError{NetStatus::InvalidArgument, 0};
            return NetStatus::InvalidArgument;  // leave active unchanged
        }
        if (s.active) {
            s.last_error = NetError{NetStatus::BadConfig, 0};  // already active; no re-init
            return NetStatus::BadConfig;
        }

        const u8 rc = Ops::init(host, kNetstreamFlags, kNetstreamNominalBaud,
                                to_netstream_port_arg(remote_port));
        if (rc != 0) {
            s.active = false;  // fail closed; nsFinal* policy handled in the backend
            s.last_error = NetError{NetStatus::TransportError, 0};
            return NetStatus::TransportError;
        }

        // begin is void (no failure channel; Altirra-validated). Active after init+begin.
        Ops::begin();
        // Settle the external TX clock before any transmit (9R.3); without this the first
        // ~5-9 stream bytes are corrupted. FakeOps::settle is a no-op (tests don't block).
        Ops::settle();
        s.active = true;
        s.last_error = NetError{NetStatus::Ok, 0};
        return NetStatus::Ok;
    }

    // No separate bind: the remote port is fixed at init time (Stage 9A discovery).
    static NetStatus realtime_bind_udp_seq(u16 /*local_port*/) {
        return NetStatus::Unsupported;
    }

    // end only if active; last_error is deterministically Closed either way.
    static void realtime_close() {
        State& s = state();
        if (s.active) {
            Ops::end();
            s.active = false;
        }
        s.last_error = NetError{NetStatus::Closed, 0};
    }

    static bool realtime_active() { return state().active; }

    // Read serial status once. Overflow (bit 0x10) is recorded WITHOUT conflating it with an
    // init failure (distinct NetStatus::Overflow). 9R.1 does not drain bytes here.
    static NetStatus realtime_poll() {
        State& s = state();
        if (!s.active) {
            s.last_error = NetError{NetStatus::Closed, 0};
            return NetStatus::Closed;
        }
        const u8 st = Ops::status();
        s.last_netstream_status = st;
        if (st & 0x10) {
            s.last_error = NetError{NetStatus::Overflow, st};  // detail = raw status byte
        } else {
            s.last_error = NetError{NetStatus::Ok, 0};
        }
        return NetStatus::Ok;
    }

    // Fixed realtime packet size (engine::net::default_realtime_packet_bytes). The lane only
    // ever calls send_nb/recv_nb with exactly this many bytes; any other nonzero size is a
    // defensive InvalidArgument.
    static constexpr u16 kPacketBytes = 16;

    // Stage 9R.2: ALL-OR-NOTHING TX. Send a whole 16-byte packet or nothing -- the engine
    // RealtimeLane drops the packet only on Ok, retries on WouldBlock, so a partial write
    // would corrupt the implicit fixed-length packet boundary. Pre-check tx_space() so the
    // 16 byte sends cannot hit a full ring (the output IRQ only DRAINS the ring, so free
    // space can only grow during the loop). send_nb/recv_nb do NOT touch state.last_error
    // (it stays owned by open/poll, keeping init/serial errors distinct from backpressure).
    static NetStatus realtime_send_nb(const void* bytes, u16 size) {
        if (!state().active) return NetStatus::Closed;
        if (bytes == nullptr && size > 0) return NetStatus::InvalidArgument;
        if (size == 0) return NetStatus::Ok;
        if (size != kPacketBytes) return NetStatus::InvalidArgument;
        if (Ops::tx_space() < kPacketBytes) return NetStatus::WouldBlock;  // write nothing
        const u8* p = static_cast<const u8*>(bytes);
        for (u16 i = 0; i < kPacketBytes; ++i) {
            if (Ops::send_byte(p[i]) != 0) {
                // Impossible under the concurrency model (free space only grows after the
                // pre-check); a bug/race. Bytes may already have been accepted, so this is
                // NOT a clean write-nothing -- surface it, never report success.
                return NetStatus::TransportError;
            }
        }
        return NetStatus::Ok;  // exactly 16 bytes committed, in order
    }

    // Stage 9R.2: FULL-PACKET RX. Deliver a whole 16-byte packet or nothing. Pre-check
    // bytes_avail() so the 16 reads cannot underrun (the input IRQ only ADDS bytes; this is
    // the only consumer). Bytes beyond 16 stay in the RX ring for the next drain iteration.
    static NetStatus realtime_recv_nb(void* bytes, u16 size) {
        if (!state().active) return NetStatus::Closed;
        if (bytes == nullptr && size > 0) return NetStatus::InvalidArgument;
        if (size == 0) return NetStatus::Ok;
        if (size != kPacketBytes) return NetStatus::InvalidArgument;
        if (Ops::bytes_avail() < kPacketBytes) return NetStatus::WouldBlock;  // consume nothing
        u8* p = static_cast<u8*>(bytes);
        for (u16 i = 0; i < kPacketBytes; ++i) {
            const u16 packed = Ops::recv_packed();   // low = data, high = status (0 ok/1 empty)
            if ((packed >> 8) != 0) {
                // Impossible after the pre-check (only this consumer drains RX); a bug/race.
                return NetStatus::TransportError;
            }
            p[i] = static_cast<u8>(packed & 0xff);
        }
        return NetStatus::Ok;  // exactly 16 bytes copied
    }

    static NetError realtime_last_error() { return state().last_error; }
};

// Default production adapter: the real backend. The seam (fujinet.h) and the size test use
// this alias, so neither changes.
using NetstreamRealtimeAdapter = NetstreamRealtimeAdapterT<RealNetstreamOps>;

#else  // !EDGE_ATARI_FUJINET_REALTIME_NETSTREAM

// OFF mode: no state, minimal stubs (the header is only included by fujinet.h when ON, so
// this is defensive for any direct include in an OFF build).
struct NetstreamRealtimeAdapter {
    static NetStatus realtime_open_udp_seq(const char*, u16, u16) {
        return NetStatus::Unsupported;
    }
    static NetStatus realtime_bind_udp_seq(u16) { return NetStatus::Unsupported; }
    static void realtime_close() {}
    static bool realtime_active() { return false; }
    static NetStatus realtime_poll() { return NetStatus::WouldBlock; }
    static NetStatus realtime_send_nb(const void*, u16) { return NetStatus::Unsupported; }
    static NetStatus realtime_recv_nb(void*, u16) { return NetStatus::WouldBlock; }
    static NetError realtime_last_error() { return NetError{NetStatus::WouldBlock, 0}; }
};

#endif  // EDGE_ATARI_FUJINET_REALTIME_NETSTREAM

}  // namespace fujinet_netstream
}  // namespace atari

#endif  // ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_REALTIME_H
