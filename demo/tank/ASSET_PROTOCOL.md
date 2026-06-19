# Tank demo network-asset protocol (Stage 5A)

A **demo-local** protocol for loading the tank demo's fixed asset set over a
reliable, message-framed transport. It is **not** a generic EDGE asset protocol.

- Source of truth: [`asset_protocol.h`](asset_protocol.h) (C++) and
  [`../../tools/net/edge_tank_asset_server.py`](../../tools/net/edge_tank_asset_server.py) (Python).
- Loader: [`asset_loader.h`](asset_loader.h). Transport integration is **Stage 5B**.

## Asset set (fixed)

| Asset | Size |
|---|---|
| Tileset | 1 × 1024 bytes (ANTIC Mode 4 charset) |
| Map chunks | 4 × 960 bytes (40×24 tile codes each), chunk grid 2×2 |

## Conventions

- All multi-byte integers are **little-endian**.
- No C++ structs are serialized; every field is read/written byte-by-byte (struct
  padding is irrelevant).
- Every message fits in the **128-byte** session-lane payload (largest is 89).
- The transport (Stage 5B session lane) preserves **message boundaries, ordering,
  and reliability**; the loader still validates every field.

## Common prefix (3 bytes)

| Offset | Field | Value |
|---|---|---|
| 0 | protocol version | `1` |
| 1 | message kind | see below |
| 2 | transfer id | set by the manifest; later messages must match |

The transfer id lets the loader reject stale messages from an earlier load.

## Message kinds

| Value | Name | Direction |
|---|---|---|
| 1 | `MANIFEST` | server → client |
| 2 | `TILESET_BLOCK` | server → client |
| 3 | `CHUNK_ROWS` | server → client |
| 4 | `COMPLETE` | server → client |
| 5 | `ERROR` | server → client |

## MANIFEST (10 bytes)

| Offset | Field | Value |
|---|---|---|
| 0–2 | prefix | kind = 1 |
| 3–4 | tileset byte count (u16 LE) | `1024` |
| 5 | chunk columns | `2` |
| 6 | chunk grid rows | `2` |
| 7 | chunk width | `40` |
| 8 | chunk height | `24` |
| 9 | chunk count | `4` |

The loader rejects (`ManifestMismatch`) any value that differs from the compiled
demo geometry. The manifest must arrive first; it sets the transfer id.

## TILESET_BLOCK (71 bytes)

| Offset | Field |
|---|---|
| 0–2 | prefix (kind = 2) |
| 3–4 | destination offset (u16 LE) |
| 5–6 | payload length (u16 LE) |
| 7… | payload bytes |

Blocks are **fixed 64-byte, 64-aligned** (16 blocks cover the 1024-byte tileset),
which lets the loader track coverage with a single 16-bit mask. Validation:
`length == 64`, `offset % 64 == 0`, `offset + 64 <= 1024` (else `TilesetRange`).
Duplicate blocks are idempotent; blocks may arrive out of order, only after a
valid manifest.

## CHUNK_ROWS (max 89 bytes)

| Offset | Field |
|---|---|
| 0–2 | prefix (kind = 3) |
| 3 | chunk x (`< 2`) |
| 4 | chunk y (`< 2`) |
| 5 | start row (`< 24`) |
| 6 | row count (`1` or `2`) |
| 7–8 | payload length (u16 LE) = `row_count × 40` |
| 9… | row payload (40 bytes per row) |

At most **2 rows/message** (2×40 + 9 header = 89 ≤ 128; 3 rows would be 129).
Validation: `chunk_x<2`, `chunk_y<2`, `start_row<24`, `1≤row_count≤2`,
`start_row+row_count≤24`, `payload_length==row_count×40`.

**Map-row destination** (direct write into the 88×48 physical map):
```
logical_y     = chunk_y * 24 + (start_row + r)        # r = 0..row_count-1
physical_col  = 4 + chunk_x * 40                       # 4 = left padding
byte_offset   = logical_y * 88 + physical_col          # stride 88
```
First-cell offsets: chunk(0,0)=4, (1,0)=44, (0,1)=2116, (1,1)=2156. Rows are
written directly into the final physical map; padding columns are never touched.

## COMPLETE (3 bytes)

Prefix only (kind = 4). Accepted only when coverage is complete:
- all 16 tileset blocks received, **and**
- all 24 rows of all 4 chunks received.

Otherwise the loader fails with `PrematureComplete`. Byte-count alone is not
trusted — overlapping duplicates cannot falsely satisfy completion (coverage is
tracked by per-block / per-row masks).

## ERROR (4 bytes)

Prefix (kind = 5) + a 1-byte server code. The loader enters `Failed`.

## Transfer sequence

```
MANIFEST → {TILESET_BLOCK × 16, CHUNK_ROWS × 48} (any order) → COMPLETE
```

## Validation / error codes

`BadVersion, BadKind, BadLength, BadState, ManifestMismatch, WrongTransfer,
TilesetRange, ChunkCoord, ChunkRowRange, ChunkPayloadSize, PrematureComplete,
ServerError`. On any error the loader enters `Failed`, stops accepting asset data
(until `reset()`), retains the code, and does **not** install the tileset or start
gameplay. Partially written map RAM is harmless because gameplay has not begun.

## Why no CRC

The Stage 5B transport is the **reliable, framed session lane** (TCP-backed),
which already guarantees integrity, ordering, and message boundaries. Adding a CRC
would duplicate that guarantee for no benefit on a 6502, so none is included. (If a
future unreliable transport is used, a CRC/sequence layer would be reconsidered.)

## Session transport layer (Stage 5B)

Stage 5B carries the asset payloads above over EDGE's reliable **session lane**
(`Game::net.session`). Two layers stay distinct: the session frame (below) and the
asset payload (above). One asset payload = one session message payload, passed
intact to `AssetLoader::consume()`.

### Session wire frame (engine/net_api.h)

```
[ kind (1) ][ size_lo (1) ][ size_hi (1) ][ payload (size) ]    # size little-endian
```

This is the exact EDGE session wire format (NOT a bare 2-byte length prefix).
`recv()` returns the `kind` and the payload.

### Session message kinds (Stage 5B, demo-local)

| Value | Name | Direction | Payload |
|---|---|---|---|
| 1 | asset payload | server → client | a Stage 5A asset message (above) |
| 2 | transfer request | client → server | `[version][transfer_id][asset_set][credit]` |
| 3 | credit grant | client → server | `[transfer_id][credit]` |

### Flow + credit pacing

The client connects, sends a **request** (its new transfer id + an initial credit
of 2), and the server replies with asset messages — at most `credit` of them —
then waits. As the client drains its 256-byte RX ring it sends **credit** grants
to top the window back up to 2. The window of 2 bounds in-flight bytes to ≤ 2×92 =
184 B (< the 256-B RX ring), so the ring never overflows (the engine's RX drain
drops bytes on a full ring — there is no safe natural backpressure). The server
stamps every asset payload with the request's transfer id; the loader's existing
transfer-id check rejects stale payloads. The transfer ends with the asset-layer
`COMPLETE` (sent as one more credited asset message).

Largest session frame = 3 + 89 = **92 bytes**. Control messages are 3+4 (request)
and 3+2 (credit).
