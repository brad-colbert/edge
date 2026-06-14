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

#include <stdint.h>   // uint8_t / uint16_t / uintptr_t (init wrapper marshaling)

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

// uint8_t edge_ns_tx_space(void)
// Stage 9R.2: bytes free in the TX output ring (range 0..128 -- treat as unsigned, it can
// equal 128). Used by the realtime adapter to pre-check room for a whole 16-byte packet
// before an all-or-nothing send. Direct passthrough; no carry-flag conversion.
extern uint8_t _edge_ns_tx_space(void);

// uint8_t edge_ns_get_status(void)
// Returns raw Netstream status byte.
// Direct passthrough; no carry-flag conversion.
extern uint8_t _edge_ns_get_status(void);

// Stage 9Q.2 init surface. The raw handler entry (_ns_init_netstream) is parameterless
// and reads its arguments from the staging .bss below; the C wrapper marshals the 4 args
// into that .bss and then dispatches through the asm carry-to-status shim. The obsolete
// 3-arg deferred `_edge_ns_init_netstream(const char*, uint16_t, uint16_t)` shim has been
// REMOVED (no production caller yet -- the realtime adapter is wired in Stage 9R), so the
// only init entry point is edge_ns_init_netstream(...) below.

// Staged init inputs (handler .bss; exported in production by fujinet_netstream_handler.S).
// The wrapper writes these; the raw _ns_init_netstream reads them.
extern uint8_t nsHostPtrLo, nsHostPtrHi;   // hostname pointer lo/hi
extern uint8_t nsInitFlags;                // caller flags seed (derived -> nsFinalFlags)
extern uint8_t nsNominalBaudLo, nsNominalBaudHi;  // select_baud key
extern uint8_t nsPortLo, nsPortHi;         // DCB daux1/daux2

// uint8_t edge_ns_init_run(void)
// Parameterless asm carry-to-status dispatcher: runs the raw init hardware path
// (ns_init_prepare -> DCB fill -> SIOV -> DSTATS check) and returns 0 = success,
// 1 = failure. Drives the real SIOV path -- do NOT call from mos-sim tests.
extern uint8_t _edge_ns_init_run(void);

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

#if defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
// Stage 9Q.2 safe 4-arg C wrapper. Marshaling stays in C (the asm never decodes a
// multi-arg ABI): edge_ns_init_marshal writes the staged init inputs into the handler
// .bss; edge_ns_init_netstream marshals then dispatches through _edge_ns_init_run.
// Split so the marshaling can be unit-tested under mos-sim WITHOUT executing SIOV
// (call edge_ns_init_marshal only). Both live under the same Netstream feature guard
// as the rest of this ABI.

// Marshal the 4 init args into the handler staging .bss. Pure stores; no hardware,
// no SIOV. The caller is responsible for any host/network byte-order swap (hence
// `port_swapped`); this performs a straight lo/hi split into daux1/daux2.
static inline void edge_ns_init_marshal(const char* host, uint8_t flags,
                                        uint16_t nominal_baud, uint16_t port_swapped) {
    uintptr_t p = (uintptr_t)host;
    nsHostPtrLo = (uint8_t)(p & 0xff);
    nsHostPtrHi = (uint8_t)((p >> 8) & 0xff);
    nsInitFlags = flags;
    nsNominalBaudLo = (uint8_t)(nominal_baud & 0xff);
    nsNominalBaudHi = (uint8_t)((nominal_baud >> 8) & 0xff);
    nsPortLo = (uint8_t)(port_swapped & 0xff);
    nsPortHi = (uint8_t)((port_swapped >> 8) & 0xff);
}

// Full init: marshal the args, then run the raw hardware path (prepare -> DCB -> SIOV
// -> DSTATS check). Returns 0 = success, 1 = failure. Drives SIOV -- hardware/Altirra
// only; do NOT call from mos-sim tests.
static inline uint8_t edge_ns_init_netstream(const char* host, uint8_t flags,
                                             uint16_t nominal_baud, uint16_t port_swapped) {
    edge_ns_init_marshal(host, flags, nominal_baud, port_swapped);
    return _edge_ns_init_run();
}
#endif // EDGE_ATARI_FUJINET_REALTIME_NETSTREAM

#endif // ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_REALTIME_ABI_H
