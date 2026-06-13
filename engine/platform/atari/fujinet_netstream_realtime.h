#ifndef ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_REALTIME_H
#define ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_REALTIME_H

// platform/atari/fujinet_netstream_realtime.h
//
// Stage 9B.5: Minimal realtime adapter state (safety audit + storage minimization).
// Scaffolding only: returns Unsupported until real Netstream I/O wiring (Stage 9C+).
// No host/port caching; no staging buffers (not used yet).
// Active flag always false (no false activation before wiring).
// sizeof(State) = ~6 bytes (1 bool + 3 NetError + 1 status byte + 1 padding).

#include "../../types.h"
#include "../../net_types.h"

namespace atari {
namespace fujinet_netstream {

using engine::u8;
using engine::u16;
using engine::net::NetError;
using engine::net::NetStatus;

struct NetstreamRealtimeAdapter {
#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
	struct State {
		// Safety-critical: active remains false until real Netstream init (Stage 9C+).
		bool active = false;

		// Error state cache (clear-on-read behavior, cached per poll).
		NetError last_error{NetStatus::Unsupported, 0};

		// Raw Netstream status byte cache (for ns_get_status result).
		// Future use: Stage 9D caches status to avoid clear-on-read volatility.
		u8 last_netstream_status = 0;
	};

	static State& state() {
		static State s{};
		return s;
	}

	// Stage 9B.5 SAFETY: Returns Unsupported until real Netstream init is wired (Stage 9C+).
	// Prevents false activation before ns_init_netstream + ns_begin_stream are called.
	// Caller correctly perceives realtime as not available during scaffolding.
	static NetStatus realtime_open_udp_seq(const char* host, u16 remote_port, u16 local_port) {
		State& s = state();

		// Validate inputs.
		if (host == nullptr || host[0] == '\0' || remote_port == 0) {
			s.last_error.status = NetStatus::InvalidArgument;
			s.last_error.detail = 0;
			s.active = false;
			return NetStatus::InvalidArgument;
		}

		// SAFETY: Do not cache host or ports; do not mark active.
		// Scaffolding phase: defer all state management to Stage 9C+.
		// TODO (Stage 9C): Implement real ns_init_netstream + ns_begin_stream calls.
		// TODO (Stage 9C): Cache host/ports if semantically required by handler.

		s.last_error.status = NetStatus::Unsupported;
		s.last_error.detail = 0;
		s.active = false;  // Critical: remain inactive during scaffolding.
		return NetStatus::Unsupported;
	}

	// Stage 9A discovery found: no separate bind primitive exists.
	// Port is fixed at NS_InitNetstream time and cannot be rebound.
	static NetStatus realtime_bind_udp_seq(u16 local_port) {
		State& s = state();
		s.last_error.status = NetStatus::Unsupported;
		s.last_error.detail = 0;
		return NetStatus::Unsupported;
	}

	static void realtime_close() {
		State& s = state();
		// Scaffolding phase: no real Netstream state to clean up.
		// TODO (Stage 9C): Implement real ns_end_stream call.
		// TODO (Stage 9C): Restore VIMIRQ vectors and POKEY state.
		s.active = false;
		s.last_error.status = NetStatus::Closed;
		s.last_error.detail = 0;
	}

	static bool realtime_active() {
		return state().active;
	}

	// Scaffolding: no real I/O wiring yet.
	static NetStatus realtime_poll() {
		State& s = state();

		if (!s.active) {
			// During scaffolding: report Unsupported to signal not available.
			// After Stage 9C real wiring: will return Ok/WouldBlock based on I/O.
			s.last_error.status = NetStatus::Unsupported;
			s.last_error.detail = 0;
			return NetStatus::Unsupported;
		}

		// TODO (Stage 9D): Implement real polling:
		// - Call ns_bytes_avail() to check RX pending
		// - Loop ns_send_byte() to drain pending TX
		// - Loop ns_recv_byte() to fill RX toward 16-byte packet boundary
		// - Cache ns_get_status() result (clear-on-read volatility)

		s.last_error.status = NetStatus::Ok;
		s.last_error.detail = 0;
		return NetStatus::Ok;
	}

	// Stage 9B.5: Unsupported during scaffolding; no real ns_send_byte yet.
	// Returns Ok for zero-length sends (no-op, always safe).
	// Returns Unsupported for nonzero sends (not wired yet).
	static NetStatus realtime_send_nb(const void* bytes, u16 size) {
		State& s = state();

		if (bytes == nullptr && size > 0) {
			s.last_error.status = NetStatus::InvalidArgument;
			s.last_error.detail = 0;
			return NetStatus::InvalidArgument;
		}

		if (size == 0) {
			// Zero-length send: always ok (no-op).
			s.last_error.status = NetStatus::Ok;
			s.last_error.detail = 0;
			return NetStatus::Ok;
		}

		// Nonzero send: scaffolding not wired yet.
		// TODO (Stage 9D): Loop ns_send_byte() into tx buffer, handle carries.
		s.last_error.status = NetStatus::Unsupported;
		s.last_error.detail = 0;
		return NetStatus::Unsupported;
	}

	// Stage 9B.5: Unsupported during scaffolding; no real ns_recv_byte yet.
	// Returns Ok for zero-length receives (no-op, always safe).
	// Returns Unsupported for nonzero receives (not wired yet).
	static NetStatus realtime_recv_nb(void* bytes, u16 size) {
		State& s = state();

		if (bytes == nullptr && size > 0) {
			s.last_error.status = NetStatus::InvalidArgument;
			s.last_error.detail = 0;
			return NetStatus::InvalidArgument;
		}

		if (size == 0) {
			// Zero-length receive: always ok (no-op).
			s.last_error.status = NetStatus::Ok;
			s.last_error.detail = 0;
			return NetStatus::Ok;
		}

		// Nonzero receive: scaffolding not wired yet.
		// TODO (Stage 9D–9E): Loop ns_recv_byte() into rx buffer, assemble 16-byte packets.
		s.last_error.status = NetStatus::Unsupported;
		s.last_error.detail = 0;
		return NetStatus::Unsupported;
	}

	static NetError realtime_last_error() {
		State& s = state();
		// Cache volatile ns_get_status result per poll cycle (future stage).
		// For now, return adapter state.
		return s.last_error;
	}

#else
	// OFF mode: no state, minimal stubs.
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
#endif
};

}  // namespace fujinet_netstream
}  // namespace atari

#endif  // ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_REALTIME_H
