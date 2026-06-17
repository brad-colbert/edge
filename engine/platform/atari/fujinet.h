#ifndef ENGINE_PLATFORM_ATARI_FUJINET_H
#define ENGINE_PLATFORM_ATARI_FUJINET_H

// platform/atari/fujinet.h — Atari network HAL seam stubs.
//
// Stage-6 scope: expose the seam surface the portable engine net layer will
// eventually call. No fujinet-lib headers, no Netstream headers, and no real
// transport behavior yet.

#include "../../net_types.h"
#include "fujinet_session_fujinetlib.h"
#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
#include "fujinet_netstream_realtime.h"
#endif

namespace atari {

using engine::u8;
using engine::u16;
using engine::net::NetError;
using engine::net::NetStatus;

// Network seam for non-networked builds.
struct NullNetwork {
	// Realtime lane seam.
	static NetStatus realtime_open_udp_seq(const char*, u16, u16) {
		return NetStatus::Unsupported;
	}
	static NetStatus realtime_bind_udp_seq(u16) { return NetStatus::Unsupported; }
	static void realtime_close() {}
	static bool realtime_active() { return false; }
	static NetStatus realtime_poll() { return NetStatus::WouldBlock; }
	static NetStatus realtime_send_nb(const void*, u16) { return NetStatus::Unsupported; }
	static NetStatus realtime_recv_nb(void*, u16) { return NetStatus::WouldBlock; }
	static NetError realtime_last_error() {
		return NetError{NetStatus::Unsupported, 0};
	}

	// Session lane seam.
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
};

// Fujinet-capable seam stub.
//
// Real transport wiring points (deferred):
// - realtime_*: Netstream / UDP-seq nonblocking path (when EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
// - session_* : fujinet-lib / CIO N: TCP client nonblocking path
struct FujinetNetwork {
	// Realtime lane seam.
	static NetStatus realtime_open_udp_seq(const char* host, u16 remote_port, u16 local_port) {
#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
		return fujinet_netstream::NetstreamRealtimeAdapter::realtime_open_udp_seq(host, remote_port, local_port);
#else
		if (host == nullptr || host[0] == '\0' || remote_port == 0)
			return NetStatus::InvalidArgument;
		return NetStatus::Ok;
#endif
	}
	static NetStatus realtime_bind_udp_seq(u16 local_port) {
#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
		return fujinet_netstream::NetstreamRealtimeAdapter::realtime_bind_udp_seq(local_port);
#else
		return NetStatus::Unsupported;
#endif
	}
	static void realtime_close() {
#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
		fujinet_netstream::NetstreamRealtimeAdapter::realtime_close();
#endif
	}
	static bool realtime_active() {
#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
		return fujinet_netstream::NetstreamRealtimeAdapter::realtime_active();
#else
		return false;
#endif
	}
	static NetStatus realtime_poll() {
#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
		return fujinet_netstream::NetstreamRealtimeAdapter::realtime_poll();
#else
		return NetStatus::WouldBlock;
#endif
	}
	static NetStatus realtime_send_nb(const void* bytes, u16 size) {
#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
		return fujinet_netstream::NetstreamRealtimeAdapter::realtime_send_nb(bytes, size);
#else
		if (bytes == nullptr && size > 0) return NetStatus::InvalidArgument;
		return NetStatus::Ok;
#endif
	}
	static NetStatus realtime_recv_nb(void* bytes, u16 size) {
#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
		return fujinet_netstream::NetstreamRealtimeAdapter::realtime_recv_nb(bytes, size);
#else
		if (bytes == nullptr && size > 0) return NetStatus::InvalidArgument;
		return NetStatus::WouldBlock;
#endif
	}
	static NetError realtime_last_error() {
#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
		return fujinet_netstream::NetstreamRealtimeAdapter::realtime_last_error();
#else
		return NetError{NetStatus::WouldBlock, 0};
#endif
	}

	// Session lane seam.
	static NetStatus session_connect_tcp(const char* host, u16 port) {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		return fujinet_session::FujinetLibSessionAdapter::session_connect_tcp(host, port);
	#else
		if (host == nullptr || host[0] == '\0' || port == 0)
			return NetStatus::InvalidArgument;
		return NetStatus::Ok;
	#endif
	}
	static void session_close() {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		fujinet_session::FujinetLibSessionAdapter::session_close();
	#endif
	}
	static bool session_connected() {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		return fujinet_session::FujinetLibSessionAdapter::session_connected();
	#else
		return false;
	#endif
	}
	static NetStatus session_poll() {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		return fujinet_session::FujinetLibSessionAdapter::session_poll();
	#else
		return NetStatus::WouldBlock;
	#endif
	}
	static NetStatus session_send_nb(const void* bytes, u16 size) {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		return fujinet_session::FujinetLibSessionAdapter::session_send_nb(bytes, size);
	#else
		if (bytes == nullptr && size > 0) return NetStatus::InvalidArgument;
		return NetStatus::Ok;
	#endif
	}
	static NetStatus session_recv_nb(void* bytes, u16 capacity) {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		return fujinet_session::FujinetLibSessionAdapter::session_recv_nb(bytes, capacity);
	#else
		if (bytes == nullptr && capacity > 0) return NetStatus::InvalidArgument;
		return NetStatus::WouldBlock;
	#endif
	}
	static NetError session_last_error() {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		return fujinet_session::FujinetLibSessionAdapter::session_last_error();
	#else
		return NetError{NetStatus::WouldBlock, 0};
	#endif
	}
	static u8 session_diag_fn_error() {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		return fujinet_session::FujinetLibSessionAdapter::session_diag_fn_error();
	#else
		return 0;
	#endif
	}
	static u8 session_diag_device_error() {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		return fujinet_session::FujinetLibSessionAdapter::session_diag_device_error();
	#else
		return 0;
	#endif
	}
	static u8 session_diag_conn() {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		return fujinet_session::FujinetLibSessionAdapter::session_diag_conn();
	#else
		return 0;
	#endif
	}
	static u16 session_diag_bw() {
	#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
		return fujinet_session::FujinetLibSessionAdapter::session_diag_bw();
	#else
		return 0;
	#endif
	}
};

} // namespace atari

#endif // ENGINE_PLATFORM_ATARI_FUJINET_H
