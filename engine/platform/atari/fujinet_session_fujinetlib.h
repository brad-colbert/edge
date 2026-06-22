#ifndef ENGINE_PLATFORM_ATARI_FUJINET_SESSION_FUJINETLIB_H
#define ENGINE_PLATFORM_ATARI_FUJINET_SESSION_FUJINETLIB_H

// platform/atari/fujinet_session_fujinetlib.h
//
// Stage 8C scaffolding only:
// - fixed-size TCP devicespec builder shared by tests and adapter
// - compile-gated session adapter state for optional fujinet-lib wiring
// - no real network_open/read/write wiring yet

#include "../../types.h"
#include "../../net_types.h"

namespace atari {
namespace fujinet_session {

using engine::u8;
using engine::u16;
using engine::i8;
using engine::i16;
using engine::net::NetError;
using engine::net::NetStatus;

static constexpr u16 kTcpDeviceSpecCapacity = 96;

#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
extern "C" {
#include <fujinet-network.h>
#include <fujinet-network-atari.h>
}
#endif

// Build "N:TCP://<host>:<port>/" into a fixed destination buffer.
inline NetStatus build_tcp_devicespec(const char* host,
                                      u16 port,
                                      char* dst,
                                      u16 dst_capacity,
                                      u16& out_len) {
    out_len = 0;
    if (dst == nullptr || dst_capacity == 0) return NetStatus::InvalidArgument;
    dst[0] = '\0';

    if (host == nullptr || host[0] == '\0' || port == 0) {
        return NetStatus::InvalidArgument;
    }

    const char prefix[] = "N:TCP://";
    const char suffix[] = "/";

    u16 host_len = 0;
    while (host[host_len] != '\0') ++host_len;

    // Max decimal digits for u16 port is 5.
    const u16 needed = static_cast<u16>((sizeof(prefix) - 1) + host_len + 1 + 5 + (sizeof(suffix) - 1));
    if (needed + 1 > dst_capacity) return NetStatus::InvalidArgument;

    u16 w = 0;
    for (u16 i = 0; i < sizeof(prefix) - 1; ++i) dst[w++] = prefix[i];
    for (u16 i = 0; i < host_len; ++i) dst[w++] = host[i];
    dst[w++] = ':';

    char digits[5] = {};
    u16 n = port;
    u8 dc = 0;
    do {
        digits[dc++] = static_cast<char>('0' + (n % 10));
        n = static_cast<u16>(n / 10);
    } while (n != 0 && dc < 5);
    while (dc > 0) dst[w++] = digits[--dc];

    for (u16 i = 0; i < sizeof(suffix) - 1; ++i) dst[w++] = suffix[i];
    dst[w] = '\0';
    out_len = w;
    return NetStatus::Ok;
}

// Pack FujiNet diagnostics into NetError.detail:
// high byte = fn_network_error, low byte = fn_device_error.
// If both globals are zero (not yet meaningful), preserve direct return code.
inline u16 pack_fujinet_detail(u8 fn_network_error,
                               u8 fn_device_error,
                               u16 fallback_detail) {
    if (fn_network_error == 0 && fn_device_error == 0) {
        return fallback_detail;
    }
    return static_cast<u16>((static_cast<u16>(fn_network_error) << 8) |
                            static_cast<u16>(fn_device_error));
}

struct FujinetLibSessionAdapter {
    static constexpr u16 device_spec_capacity() { return kTcpDeviceSpecCapacity; }

#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
    // Bulk-read staging buffer (Stage 5C). The engine session lane drains the RX
    // one byte at a time; routed straight to network_read_nb that is one full
    // network_status + sio_read SIO transaction PER BYTE — thousands of slow SIO
    // round-trips for one asset transfer, and over emulated NetSIO an occasional
    // dropped/garbled byte desynchronises the session framing (observed as a false
    // UnexpectedKind partway through). Staging one bulk read here and serving the
    // byte-at-a-time drain from it cuts SIO transactions ~30-90x: far more reliable
    // and much faster, with no change to the generic recv seam.
    static constexpr u16 kRxStageCapacity = 256;

    struct State {
        char devicespec[kTcpDeviceSpecCapacity] = {};
        u16 devicespec_len = 0;
        bool initialized = false;
        bool connected = false;
        NetError last_error{};
        u8 last_fn_error = 0;
        u8 last_device_error = 0;
        u16 last_bw = 0;
        u8 last_conn = 0;
        u8 rx_stage[kRxStageCapacity] = {};
        u16 rx_stage_len = 0;   // valid bytes currently staged
        u16 rx_stage_pos = 0;   // next byte to hand out
    };

    static State& state() {
        static State s{};
        return s;
    }

    // Helper: map FujiNet error code to NetStatus.
    static NetStatus map_fn_error(u8 fn_code) {
        switch (fn_code) {
            case FN_ERR_OK: return NetStatus::Ok;
            case FN_ERR_BAD_CMD: return NetStatus::InvalidArgument;
            case FN_ERR_IO_ERROR: return NetStatus::TransportError;
            default: return NetStatus::TransportError;
        }
    }

    static NetStatus session_connect_tcp(const char* host, u16 port) {
        State& s = state();

        // Validate and build devicespec.
        NetStatus st = build_tcp_devicespec(host, port,
                                            s.devicespec,
                                            kTcpDeviceSpecCapacity,
                                            s.devicespec_len);
        if (st != NetStatus::Ok) {
            s.connected = false;
            s.last_error.status = st;
            s.last_error.detail = 0;
            return st;
        }

        // Initialize network subsystem once.
        if (!s.initialized) {
            u8 fn_code = network_init();
            if (fn_code != FN_ERR_OK) {
                s.initialized = false;
                s.connected = false;
                s.last_fn_error = fn_code;
                s.last_device_error = fn_device_error;
                s.last_error.status = map_fn_error(fn_code);
                s.last_error.detail = pack_fujinet_detail(fn_network_error,
                                                          s.last_device_error,
                                                          fn_code);
                return s.last_error.status;
            }
            s.initialized = true;
        }

        // Open TCP connection.
        u8 fn_code = network_open(s.devicespec, OPEN_MODE_RW, OPEN_TRANS_NONE);
        if (fn_code != FN_ERR_OK) {
            s.connected = false;
            s.last_fn_error = fn_code;
            s.last_device_error = fn_device_error;
            s.last_error.status = map_fn_error(fn_code);
            s.last_error.detail = pack_fujinet_detail(fn_network_error,
                                                      s.last_device_error,
                                                      fn_code);
            return s.last_error.status;
        }

        s.connected = true;
        s.last_fn_error = FN_ERR_OK;
        s.last_device_error = fn_device_error;
        s.last_error.status = NetStatus::Ok;
        s.last_error.detail = 0;
        s.rx_stage_len = 0;          // fresh connection: drop any staged bytes
        s.rx_stage_pos = 0;
        return NetStatus::Ok;
    }

    static void session_close() {
        State& s = state();

        if (!s.connected && !s.initialized) {
            s.last_error.status = NetStatus::Closed;
            s.last_error.detail = 0;
            return;
        }

        if (s.connected) {
            u8 fn_code = network_close(s.devicespec);
            s.last_fn_error = fn_code;
            s.last_device_error = fn_device_error;

            if (fn_code != FN_ERR_OK) {
                // Keep close status behavior unchanged (Closed), but preserve
                // transport diagnostics in detail when close fails.
                s.last_error.detail = pack_fujinet_detail(fn_network_error,
                                                          s.last_device_error,
                                                          fn_code);
            }
        }

        s.connected = false;
        s.last_error.status = NetStatus::Closed;
        s.last_error.detail = 0;
        s.rx_stage_len = 0;
        s.rx_stage_pos = 0;
    }

    static bool session_connected() {
        return state().connected;
    }

    // Keep poll lightweight in 8C: no status I/O calls yet.
    static NetStatus session_poll() {
        State& s = state();
        if (s.connected) {
            s.last_error.status = NetStatus::Ok;
            s.last_error.detail = 0;
            return NetStatus::Ok;
        } else {
            s.last_error.status = NetStatus::Closed;
            s.last_error.detail = 0;
            return NetStatus::Closed;
        }
    }

    static NetStatus session_send_nb(const void* bytes, u16 size) {
        State& s = state();

        if (bytes == nullptr && size > 0) {
            s.last_error.status = NetStatus::InvalidArgument;
            s.last_error.detail = 0;
            return NetStatus::InvalidArgument;
        }

        // Zero-length send is always ok (no-op); no I/O.
        if (size == 0) {
            s.last_error.status = NetStatus::Ok;
            s.last_error.detail = 0;
            return NetStatus::Ok;
        }

        // Not connected: cannot send.
        if (!s.connected) {
            s.last_error.status = NetStatus::Closed;
            s.last_error.detail = 0;
            return NetStatus::Closed;
        }

        // NOTE (Stage 8F): network_write may block or perform a full CIO/FujiNet
        // transaction. This call is only safe on the session/control lane where
        // occasional frame stalls are acceptable. Do NOT use this path for
        // realtime traffic; the realtime lane must use Netstream/UDP-seq when
        // that stage is implemented. Avoid large writes during timing-critical
        // frames; prefer small, bounded messages on the session lane.
        u8 fn_code = network_write(s.devicespec,
                                   static_cast<const u8*>(bytes),
                                   size);
        s.last_fn_error = fn_code;
        s.last_device_error = fn_device_error;
        s.last_error.status = map_fn_error(fn_code);
        if (fn_code != FN_ERR_OK) {
            s.last_error.detail = pack_fujinet_detail(fn_network_error,
                                                      s.last_device_error,
                                                      fn_code);
        } else {
            s.last_error.detail = 0;
        }
        return s.last_error.status;
    }

    static NetStatus session_recv_nb(void* bytes, u16 capacity) {
        State& s = state();

        if (bytes == nullptr && capacity > 0) {
            s.last_error.status = NetStatus::InvalidArgument;
            s.last_error.detail = 0;
            return NetStatus::InvalidArgument;
        }

        if (!s.connected) {
            s.last_error.status = NetStatus::Closed;
            s.last_error.detail = 0;
            return NetStatus::Closed;
        }

        if (capacity == 0) {
            s.last_error.status = NetStatus::Ok;
            s.last_error.detail = 0;
            return NetStatus::Ok;
        }

        // Refill the stage with ONE bulk SIO read when it runs dry. This is the
        // only place that talks to fujinet-lib; the engine's per-byte drain is
        // served from the stage, so a 92-byte message costs ~1 SIO read, not 92.
        if (s.rx_stage_pos >= s.rx_stage_len) {
            s.rx_stage_pos = 0;
            s.rx_stage_len = 0;
            i16 result = network_read_nb(s.devicespec, s.rx_stage, kRxStageCapacity);
            s.last_fn_error = fn_network_error;
            s.last_device_error = fn_device_error;
            s.last_bw = fn_network_bw;
            s.last_conn = fn_network_conn;

            if (result > 0) {
                s.rx_stage_len = static_cast<u16>(result);
            } else if (result == 0) {
                s.last_error.status = NetStatus::WouldBlock;
                s.last_error.detail = 0;
                return NetStatus::WouldBlock;
            } else {
                u8 err_code = -static_cast<i8>(result);
                s.last_error.status = map_fn_error(err_code);
                s.last_error.detail = pack_fujinet_detail(s.last_fn_error,
                                                          s.last_device_error,
                                                          err_code);
                return s.last_error.status;
            }
        }

        // Serve up to `capacity` bytes from the stage (the engine asks for 1).
        u16 avail = static_cast<u16>(s.rx_stage_len - s.rx_stage_pos);
        u16 n = (capacity < avail) ? capacity : avail;
        u8* dst = static_cast<u8*>(bytes);
        for (u16 i = 0; i < n; ++i) dst[i] = s.rx_stage[s.rx_stage_pos++];

        s.last_error.status = NetStatus::Ok;
        s.last_error.detail = 0;
        return NetStatus::Ok;
    }

    static NetError session_last_error() {
        return state().last_error;
    }

    static u8 session_diag_fn_error() {
        return state().last_fn_error;
    }

    static u8 session_diag_device_error() {
        return state().last_device_error;
    }

    static u8 session_diag_conn() {
        return state().last_conn;
    }

    static u16 session_diag_bw() {
        return state().last_bw;
    }

    static constexpr u16 state_size_bytes() { return static_cast<u16>(sizeof(State)); }

    // Stage 8D: compile-only declaration/signature smoke for optional dependency.
    static void declaration_smoke_check() {
        uint8_t (*p_init)(void) = &network_init;
        uint8_t (*p_open)(const char*, uint8_t, uint8_t) = &network_open;
        uint8_t (*p_close)(const char*) = &network_close;
        int16_t (*p_read_nb)(const char*, uint8_t*, uint16_t) = &network_read_nb;
        uint8_t (*p_write)(const char*, const uint8_t*, uint16_t) = &network_write;
        uint8_t (*p_status)(const char*, uint16_t*, uint8_t*, uint8_t*) = &network_status;
        uint8_t (*p_status_unit)(uint8_t, uint16_t*, uint8_t*, uint8_t*) = &network_status_unit;

        uint8_t* g_net_err = &fn_network_error;
        uint8_t* g_dev_err = &fn_device_error;
        uint16_t* g_bw = &fn_network_bw;
        uint8_t* g_conn = &fn_network_conn;

        uint8_t fn_ok = FN_ERR_OK;
        uint8_t fn_io = FN_ERR_IO_ERROR;
        uint8_t fn_bad = FN_ERR_BAD_CMD;
        uint8_t mode_rw = OPEN_MODE_RW;
        uint8_t trans_none = OPEN_TRANS_NONE;

        (void)p_init; (void)p_open; (void)p_close;
        (void)p_read_nb; (void)p_write; (void)p_status; (void)p_status_unit;
        (void)g_net_err; (void)g_dev_err; (void)g_bw; (void)g_conn;
        (void)fn_ok; (void)fn_io; (void)fn_bad; (void)mode_rw; (void)trans_none;
    }
#else
    static NetStatus session_connect_tcp(const char*, u16) {
        return NetStatus::Unsupported;
    }
    static void session_close() {}
    static bool session_connected() { return false; }
    static NetStatus session_poll() { return NetStatus::WouldBlock; }
    static NetStatus session_send_nb(const void*, u16) { return NetStatus::Unsupported; }
    static NetStatus session_recv_nb(void*, u16) { return NetStatus::WouldBlock; }
    static NetError session_last_error() {
        return NetError{NetStatus::Unsupported, 0};
    }

    static u8 session_diag_fn_error() { return 0; }
    static u8 session_diag_device_error() { return 0; }
    static u8 session_diag_conn() { return 0; }
    static u16 session_diag_bw() { return 0; }

    static constexpr u16 state_size_bytes() { return 0; }

    static void declaration_smoke_check() {}
#endif
};

} // namespace fujinet_session
} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_FUJINET_SESSION_FUJINETLIB_H
