#ifndef DEMO_TANK_ASSET_LOADER_H
#define DEMO_TANK_ASSET_LOADER_H

// asset_loader.h — Stage 5A transport-neutral asset loader (demo-local).
//
// A fixed-memory state machine that consumes already-framed protocol payloads
// (asset_protocol.h) and writes:
//   * map chunk rows DIRECTLY into the final 88x48 physical map (no staging
//     buffer, no second map), using the established stride/left-padding formula;
//   * tileset bytes into ONE caller-owned 1024-byte buffer (network mode's
//     accepted writable duplicate; installed via the public charset API by the
//     demo once the transfer completes).
//
// It knows nothing of TCP / FujiNet / session rings / polling — it only consumes
// complete protocol payloads and validates every field. No heap, no exceptions,
// no strings, no engine API. Shared by the demo's simulated feed and the host
// test. Coverage is tracked with fixed masks (no dynamic bitsets).

#include <engine/types.h>
#include <engine/attributes.h>

#include "asset_protocol.h"
#include "playfield_geometry.h"

namespace tank {

using engine::u8;
using engine::u16;
using engine::u32;

enum class LoadState : u8 { Idle, AwaitManifest, Receiving, Complete, Failed };

enum class LoadError : u8 {
    None = 0,
    BadVersion,        // protocol version mismatch
    BadKind,           // unknown message kind
    BadLength,         // truncated / wrong-size message
    BadState,          // message not valid in the current state
    ManifestMismatch,  // manifest geometry != compiled demo
    WrongTransfer,     // transfer id != the manifest's
    TilesetRange,      // tileset offset/length out of range or misaligned
    ChunkCoord,        // chunk_x / chunk_y out of range
    ChunkRowRange,     // start_row / row_count out of range
    ChunkPayloadSize,  // chunk payload length != row_count * 40
    PrematureComplete, // COMPLETE before full coverage
    ServerError,       // received an ERROR message
};

struct LoadProgress {
    u8 tileset_blocks;   // 0..16
    u8 chunk_rows;       // 0..96 (rows across all four chunks)
};

class AssetLoader {
public:
    // Bind destinations and reset. `tileset_dest` is a writable 1024-byte buffer;
    // `map` is the final physical map (chunk rows land directly in it).
    void begin(PhysicalMap* map, u8* tileset_dest) {
        map_ = map;
        tileset_ = tileset_dest;
        reset();
    }
    // Reset transfer state for a clean retry (keeps the bound destinations).
    void reset() {
        state_ = LoadState::AwaitManifest;
        error_ = LoadError::None;
        transfer_id_ = 0;
        tileset_mask_ = 0;
        for (u8 i = 0; i < proto::kChunkCount; ++i) chunk_rows_[i] = 0;
    }

    LoadState consume(const u8* p, u16 size);

    bool      complete() const { return state_ == LoadState::Complete; }
    bool      failed()   const { return state_ == LoadState::Failed; }
    LoadState state()    const { return state_; }
    LoadError error()    const { return error_; }

    LoadProgress progress() const {
        LoadProgress pr{};
        pr.tileset_blocks = popcount16(tileset_mask_);
        u8 rows = 0;
        for (u8 i = 0; i < proto::kChunkCount; ++i)
            rows = static_cast<u8>(rows + popcount24(chunk_rows_[i]));
        pr.chunk_rows = rows;
        return pr;
    }

private:
    LoadState fail(LoadError e) { state_ = LoadState::Failed; error_ = e; return state_; }

    bool coverage_done() const {
        if (tileset_mask_ != 0xFFFFu) return false;
        for (u8 i = 0; i < proto::kChunkCount; ++i)
            if ((chunk_rows_[i] & 0x00FFFFFFu) != 0x00FFFFFFu) return false;
        return true;
    }

    static u8 popcount16(u16 v) {
        u8 n = 0;
        while (v) { n = static_cast<u8>(n + (v & 1)); v = static_cast<u16>(v >> 1); }
        return n;
    }
    static u8 popcount24(u32 v) {
        v &= 0x00FFFFFFu;
        u8 n = 0;
        while (v) { n = static_cast<u8>(n + (v & 1u)); v >>= 1; }
        return n;
    }

    LoadState handle_manifest(const u8* p, u16 size, u8 xfer);
    LoadState handle_tileset_block(const u8* p, u16 size, u8 xfer);
    LoadState handle_chunk_rows(const u8* p, u16 size, u8 xfer);
    LoadState handle_complete(u16 size, u8 xfer);

    LoadState state_  = LoadState::Idle;
    LoadError error_  = LoadError::None;
    u8  transfer_id_  = 0;
    u16 tileset_mask_ = 0;             // bit i = 64-byte block i received (16 blocks)
    u32 chunk_rows_[proto::kChunkCount] = {};  // low 24 bits = rows received per chunk
    PhysicalMap* map_     = nullptr;
    u8*          tileset_ = nullptr;
};

// ── consume(): validate the common prefix, then dispatch by kind ────────────
EDGE_COLD inline LoadState AssetLoader::consume(const u8* p, u16 size) {
    if (state_ == LoadState::Failed || state_ == LoadState::Complete)
        return fail(LoadError::BadState);                 // require reset() to retry
    if (size < proto::kPrefixBytes) return fail(LoadError::BadLength);
    if (p[0] != proto::kVersion)    return fail(LoadError::BadVersion);

    const u8 kind = p[1];
    const u8 xfer = p[2];
    switch (kind) {
        case proto::kManifest:     return handle_manifest(p, size, xfer);
        case proto::kTilesetBlock: return handle_tileset_block(p, size, xfer);
        case proto::kChunkRows:    return handle_chunk_rows(p, size, xfer);
        case proto::kComplete:     return handle_complete(size, xfer);
        case proto::kError:
            if (size != proto::kErrorBytes) return fail(LoadError::BadLength);
            return fail(LoadError::ServerError);
        default:                   return fail(LoadError::BadKind);
    }
}

EDGE_COLD inline LoadState AssetLoader::handle_manifest(const u8* p, u16 size, u8 xfer) {
    if (state_ != LoadState::AwaitManifest) return fail(LoadError::BadState);
    if (size != proto::kManifestBytes)      return fail(LoadError::BadLength);
    if (proto::rd_u16(p + 3) != proto::kTilesetBytes ||
        p[5] != proto::kChunkColumns || p[6] != proto::kChunkGridRows ||
        p[7] != proto::kChunkWidth   || p[8] != proto::kChunkHeight ||
        p[9] != proto::kChunkCount)
        return fail(LoadError::ManifestMismatch);
    transfer_id_ = xfer;
    state_ = LoadState::Receiving;
    return state_;
}

EDGE_COLD inline LoadState AssetLoader::handle_tileset_block(const u8* p, u16 size, u8 xfer) {
    if (state_ != LoadState::Receiving) return fail(LoadError::BadState);
    if (xfer != transfer_id_)           return fail(LoadError::WrongTransfer);
    if (size < proto::kTilesetBlockHdr) return fail(LoadError::BadLength);
    const u16 off = proto::rd_u16(p + 3);
    const u16 len = proto::rd_u16(p + 5);
    if (size != static_cast<u16>(proto::kTilesetBlockHdr + len)) return fail(LoadError::BadLength);
    // Fixed 64-byte aligned blocks -> simple coverage mask.
    if (len != proto::kTilesetBlockSize || (off % proto::kTilesetBlockSize) != 0 ||
        static_cast<u16>(off + len) > proto::kTilesetBytes)
        return fail(LoadError::TilesetRange);
    for (u16 i = 0; i < len; ++i) tileset_[off + i] = p[7 + i];   // duplicate = idempotent
    tileset_mask_ = static_cast<u16>(tileset_mask_ | (1u << (off / proto::kTilesetBlockSize)));
    return state_;
}

EDGE_COLD inline LoadState AssetLoader::handle_chunk_rows(const u8* p, u16 size, u8 xfer) {
    if (state_ != LoadState::Receiving) return fail(LoadError::BadState);
    if (xfer != transfer_id_)           return fail(LoadError::WrongTransfer);
    if (size < proto::kChunkRowsHdr)    return fail(LoadError::BadLength);
    const u8  cx        = p[3];
    const u8  cy        = p[4];
    const u8  start_row = p[5];
    const u8  row_count = p[6];
    const u16 plen      = proto::rd_u16(p + 7);
    if (cx >= proto::kChunkColumns || cy >= proto::kChunkGridRows) return fail(LoadError::ChunkCoord);
    if (row_count < 1 || row_count > proto::kMaxRowsPerMsg ||
        start_row >= proto::kChunkHeight ||
        static_cast<u16>(start_row + row_count) > proto::kChunkHeight)
        return fail(LoadError::ChunkRowRange);
    if (plen != static_cast<u16>(row_count * proto::kRowBytes)) return fail(LoadError::ChunkPayloadSize);
    if (size != static_cast<u16>(proto::kChunkRowsHdr + plen))  return fail(LoadError::BadLength);

    using G = PlayfieldGeometry;
    const u16 base_col = static_cast<u16>(G::physical_left_padding + cx * G::chunk_width);
    for (u8 r = 0; r < row_count; ++r) {
        const u16 logical_y = static_cast<u16>(cy * G::chunk_height + start_row + r);
        const u16 dst = static_cast<u16>(logical_y * G::physical_width + base_col);
        const u16 src = static_cast<u16>(9 + r * proto::kRowBytes);
        for (u8 c = 0; c < proto::kRowBytes; ++c) map_->cells[dst + c] = p[src + c];
    }
    const u8 idx = static_cast<u8>(cy * proto::kChunkColumns + cx);
    for (u8 r = 0; r < row_count; ++r)
        chunk_rows_[idx] |= (static_cast<u32>(1) << (start_row + r));   // 32-bit shift (u is 16-bit on 6502)
    return state_;
}

EDGE_COLD inline LoadState AssetLoader::handle_complete(u16 size, u8 xfer) {
    if (state_ != LoadState::Receiving) return fail(LoadError::BadState);
    if (xfer != transfer_id_)           return fail(LoadError::WrongTransfer);
    if (size != proto::kCompleteBytes)  return fail(LoadError::BadLength);
    if (!coverage_done())               return fail(LoadError::PrematureComplete);
    state_ = LoadState::Complete;
    return state_;
}

}  // namespace tank

#endif  // DEMO_TANK_ASSET_LOADER_H
