// engine/platform/atari/fujinet_netstream_realtime_abi.h
//
// Stage 9C.5: EDGE ABI wrapper declarations for safe Netstream carry-flag operations.
//
// These wrappers convert 6502 carry-flag status into normal C return values:
// - ns_send_byte: 0 = sent, 1 = full
// - ns_recv_byte_packed: packed u16 return (low=data, high=status)
// - ns_bytes_avail: direct passthrough, returns u16
// - ns_get_status: direct passthrough, returns u8
// - ns_init_netstream: declared but deferred (marshaling not yet proven)
// - ns_begin_stream, ns_end_stream: void operations
//
// These are EDGE-owned symbols (edge_ns_* prefix) to avoid collisions.
// Only available when EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON.

#ifndef ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_REALTIME_ABI_H
#define ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_REALTIME_ABI_H

#include "../../types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)

// uint8_t edge_ns_send_byte(uint8_t byte)
// Sends a single byte to Netstream TX buffer.
// Returns: 0 if enqueued, 1 if TX buffer full.
// Replaces ns_send_byte carry-flag semantics with normal return value.
extern uint8_t _edge_ns_send_byte(uint8_t byte);

// uint16_t edge_ns_recv_byte_packed(void)
// Receives one byte from Netstream RX buffer, converting carry-status into a
// packed u16 return that is safe for C/C++ callers.
// Return format:
// - low byte  = received data byte (valid only when high byte is 0)
// - high byte = status (0 = success, 1 = empty)
extern uint16_t _edge_ns_recv_byte_packed(void);

// uint16_t edge_ns_bytes_avail(void)
// Returns number of bytes available in RX buffer.
// Direct passthrough; no carry-flag conversion.
extern uint16_t _edge_ns_bytes_avail(void);

// uint8_t edge_ns_get_status(void)
// Returns raw Netstream status byte.
// Direct passthrough; no carry-flag conversion.
extern uint8_t _edge_ns_get_status(void);

// edge_ns_init_netstream(const char* host, uint16_t flags, uint16_t port)
// DEFERRED / TEMPORARY (Stage 9E): NOT part of the usable edge_ns_* surface yet.
// Production code must not call this. Real marshaling to the handler init ABI is
// deferred until the llvm-mos parameter layout is proven (the upstream handler
// decodes cc65 fastcall args from c_sp at $82, which EDGE does not reproduce).
// The `void` return below is a PLACEHOLDER, not the long-term contract: init
// eventually needs to report success/failure, so this signature will change to
// return a status code once the marshaling is wired.
extern void _edge_ns_init_netstream(const char* host, uint16_t flags, uint16_t port);

// void edge_ns_begin_stream(void)
// Starts streaming operation.
extern void _edge_ns_begin_stream(void);

// void edge_ns_end_stream(void)
// Ends streaming operation and cleans up.
extern void _edge_ns_end_stream(void);

#endif // EDGE_ATARI_FUJINET_REALTIME_NETSTREAM

#ifdef __cplusplus
}
#endif

#endif // ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_REALTIME_ABI_H
