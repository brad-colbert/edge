#ifndef DEMO_TANK_ASSET_PROTOCOL_H
#define DEMO_TANK_ASSET_PROTOCOL_H

// asset_protocol.h — Stage 5A demo-local network-asset protocol (transport
// neutral). Shared by the loader (asset_loader.h), the simulated message feed in
// the demo, and the host tests. The Python server (tools/net/edge_tank_asset_
// server.py) mirrors these exact byte layouts. See demo/tank/ASSET_PROTOCOL.md.
//
// This is a DEMO-LOCAL protocol for loading the tank demo's fixed asset set
// (1x1024-byte tileset + 4x960-byte map chunks) — NOT a generic EDGE asset
// protocol. All multi-byte integers are little-endian; messages do not serialize
// C++ structs (every field is read/written byte-by-byte), so struct padding is
// irrelevant. Every message fits inside the 128-byte session-lane payload.

#include <engine/types.h>

namespace tank {
namespace proto {

using engine::u8;
using engine::u16;

// ── Versions / kinds ────────────────────────────────────────────────────────
inline constexpr u8 kVersion = 1;

enum Kind : u8 {
    kManifest     = 1,   // server -> client: geometry to validate before any data
    kTilesetBlock = 2,   // server -> client: a 64-byte aligned slice of the tileset
    kChunkRows    = 3,   // server -> client: 1-2 rows (40 B each) of one map chunk
    kComplete     = 4,   // server -> client: end of transfer (requires full coverage)
    kError        = 5,   // server -> client: abort with a code (loader -> Failed)
};

// ── Fixed asset geometry (must match PlayfieldGeometry) ─────────────────────
inline constexpr u16 kTilesetBytes      = 1024;
inline constexpr u8  kTilesetBlockSize  = 64;            // fixed -> simple u16 coverage mask
inline constexpr u8  kTilesetBlockCount = 16;            // 1024 / 64
inline constexpr u8  kChunkColumns      = 2;
inline constexpr u8  kChunkGridRows     = 2;            // chunk grid rows (kChunkRows is a Kind)
inline constexpr u8  kChunkWidth        = 40;
inline constexpr u8  kChunkHeight       = 24;            // rows per chunk
inline constexpr u8  kChunkCount        = 4;
inline constexpr u8  kRowBytes          = 40;            // tile codes per row
inline constexpr u8  kMaxRowsPerMsg     = 2;             // 2*40 + 9 hdr = 89 B < 128
inline constexpr u16 kSessionPayloadMax = 128;           // session-lane limit

// ── Common prefix (3 bytes) ─────────────────────────────────────────────────
//   [0] protocol version   [1] message kind   [2] transfer id
inline constexpr u8 kPrefixBytes = 3;

// ── Message sizes ───────────────────────────────────────────────────────────
inline constexpr u16 kManifestBytes     = kPrefixBytes + 7;            // 10
inline constexpr u16 kTilesetBlockHdr    = kPrefixBytes + 4;           // 7  (off u16 + len u16)
inline constexpr u16 kTilesetBlockBytes  = kTilesetBlockHdr + kTilesetBlockSize;  // 71
inline constexpr u16 kChunkRowsHdr       = kPrefixBytes + 6;           // 9 (cx,cy,start,count,len u16)
inline constexpr u16 kChunkRowsMaxBytes  = kChunkRowsHdr + kMaxRowsPerMsg * kRowBytes; // 89
inline constexpr u16 kCompleteBytes      = kPrefixBytes;               // 3
inline constexpr u16 kErrorBytes         = kPrefixBytes + 1;           // 4
inline constexpr u16 kMaxMessageBytes    = kChunkRowsMaxBytes;         // 89

static_assert(kMaxMessageBytes <= kSessionPayloadMax, "messages must fit the session payload");

// ── Little-endian byte helpers (no struct serialization) ────────────────────
inline u16 rd_u16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline void wr_u16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v & 0xFF);
    p[1] = static_cast<u8>(v >> 8);
}

// ── Message builders (write into a caller buffer, return byte count) ────────
// Used by the demo's simulated feed and the host tests; mirrored in Python.

inline u16 build_manifest(u8* out, u8 transfer_id) {
    out[0] = kVersion; out[1] = kManifest; out[2] = transfer_id;
    wr_u16(out + 3, kTilesetBytes);   // [3,4] little-endian 1024
    out[5] = kChunkColumns;
    out[6] = kChunkGridRows;
    out[7] = kChunkWidth;
    out[8] = kChunkHeight;
    out[9] = kChunkCount;
    return kManifestBytes;
}

inline u16 build_tileset_block(u8* out, u8 transfer_id, u16 dest_offset,
                               const u8* src, u16 length) {
    out[0] = kVersion; out[1] = kTilesetBlock; out[2] = transfer_id;
    wr_u16(out + 3, dest_offset);
    wr_u16(out + 5, length);
    for (u16 i = 0; i < length; ++i) out[7 + i] = src[i];
    return static_cast<u16>(kTilesetBlockHdr + length);
}

inline u16 build_chunk_rows(u8* out, u8 transfer_id, u8 chunk_x, u8 chunk_y,
                            u8 start_row, u8 row_count, const u8* rows) {
    out[0] = kVersion; out[1] = kChunkRows; out[2] = transfer_id;
    out[3] = chunk_x;
    out[4] = chunk_y;
    out[5] = start_row;
    out[6] = row_count;
    const u16 plen = static_cast<u16>(row_count * kRowBytes);
    wr_u16(out + 7, plen);
    for (u16 i = 0; i < plen; ++i) out[9 + i] = rows[i];
    return static_cast<u16>(kChunkRowsHdr + plen);
}

inline u16 build_complete(u8* out, u8 transfer_id) {
    out[0] = kVersion; out[1] = kComplete; out[2] = transfer_id;
    return kCompleteBytes;
}

inline u16 build_error(u8* out, u8 transfer_id, u8 code) {
    out[0] = kVersion; out[1] = kError; out[2] = transfer_id;
    out[3] = code;
    return kErrorBytes;
}

}  // namespace proto
}  // namespace tank

#endif  // DEMO_TANK_ASSET_PROTOCOL_H
