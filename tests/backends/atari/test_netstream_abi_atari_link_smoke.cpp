// tests/backends/atari/test_netstream_abi_atari_link_smoke.cpp
// Stage 9C.5 Atari-only link smoke.
//
// Purpose: force symbol resolution for EDGE Netstream ABI wrappers linked with
// upstream handler/netstream.s under the llvm-mos Atari target.
//
// This target is build-only (not run in tests) and must not perform runtime
// network validation.

#include <stdint.h>

#if !defined(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
#error "test_netstream_abi_atari_link_smoke requires EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON"
#endif

#include <engine/platform/atari/fujinet_netstream_realtime_abi.h>

// Stage 9F: force the low-risk diagnostic getters into the link by referencing
// the RAW handler ABI directly. These are not part of the edge_ns_* surface (no
// wrapper is added), so they must be declared here. Naming follows the existing
// upstream-compatible _ns_* export convention -- do not rename the handler
// exports. Build-only: never executed on hardware in this stage.
extern "C" {
    uint8_t  _ns_get_version(void);
    uint16_t _ns_get_base(void);       // upstream A:X; 0 in EDGE (no jump table)
    uint8_t  _ns_get_video_std(void);
    uint8_t  _ns_get_final_flags(void);
    uint8_t  _ns_get_final_audf3(void);
    uint8_t  _ns_get_final_audf4(void);
}

volatile uint8_t g_sink8 = 0;
volatile uint16_t g_sink16 = 0;

int main() {
    // Build/link smoke only. This enforces symbol references without requiring
    // runtime hardware execution in this stage.
    g_sink8 = _edge_ns_send_byte(0x00);
    g_sink16 = _edge_ns_recv_byte_packed();
    g_sink16 = _edge_ns_bytes_avail();
    g_sink8 = _edge_ns_get_status();

    _edge_ns_begin_stream();
    _edge_ns_end_stream();

    // Stage 9F diagnostic getters (raw _ns_* ABI).
    g_sink8 = _ns_get_version();
    g_sink16 = _ns_get_base();
    g_sink8 = _ns_get_video_std();
    g_sink8 = _ns_get_final_flags();
    g_sink8 = _ns_get_final_audf3();
    g_sink8 = _ns_get_final_audf4();

    return 0;
}
