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

## Real FujiNet backend (Stage 5C)

Stages 5A/5B run the loader and the session lane over a *fake*/stub transport.
Stage 5C links the tank demo's `LiveSession` build against the real
**fujinet-lib** session backend (`Game::net.session` → `FujinetNetwork` HAL →
`FujinetLibSessionAdapter` → `N:TCP://host:port/`) and validates an end-to-end
transfer over real FujiNet SIO.

### Dependency

- fujinet-lib root (validated local checkout):
  `/home/brad/Dropbox/Projects/Atari/fujinetlib-llvm`
  - source commit `7803c86` (tag `wave3b-done`)
  - public headers: `<root>/src/include` (`fujinet-network.h`, `fujinet-network-atari.h`)
  - static library: `<root>/build/libfujinet.a`
  - built with the `/usr/local/bin` llvm-mos toolchain
    (`mos-atari8-dos-clang`, `llvm-ar`) — verified, no rebuild required.

The CMake variables are configurable; the path above is the validated default,
not a hard-coded requirement. A focused CMake guard rejects the prohibited legacy
toolchain mount (`/mnt/old_ubuntu_22`) in any fujinet-lib path variable.

### Stub vs. real backend (don't confuse them)

`EDGE_TANK_ASSET_SOURCE=LiveSession` alone builds a **stub** session HAL whose
`connect_tcp`/`poll` return `Ok` *without ever connecting* — useful only for
failure-path testing. The real backend requires
`EDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON` (which adds the include dir + links
`libfujinet.a`). The distinction is made impossible to miss:

- configure prints `Live transport backend: RealFujinetLib` (or warns loudly for
  the stub), and
- the `.xex` embeds a marker string — `EDGE-LIVE-BACKEND:RealFujinetLib` or
  `EDGE-LIVE-BACKEND:Stub` — readable via `strings` / the linker map, and mirrored
  into the demo's `NetDebug.backend_real` byte.

### Build (real backend)

```sh
cmake -B build-atari-live \
  -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake \
  -DEDGE_BUILD_DEMO=ON \
  -DEDGE_TANK_ASSET_SOURCE=LiveSession \
  -DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON \
  -DEDGE_FUJINETLIB_ROOT=/home/brad/Dropbox/Projects/Atari/fujinetlib-llvm \
  -DEDGE_TANK_NET_HOST=192.168.1.205 \
  -DEDGE_TANK_NET_PORT=9000
cmake --build build-atari-live --target atari_tank_demo
```

`EDGE_TANK_NET_HOST` must be an address the FujiNet can reach (the dev host's LAN
address — not `127.0.0.1`, which is the FujiNet's own loopback).

### Server

```sh
python3 tools/net/edge_tank_asset_server.py --host 0.0.0.0 --port 9000 --linger 10
# --selftest verifies framing/payloads (66 messages, max 89 B) and exits.
# --linger N keeps the socket open N s after COMPLETE so the FujiNet client can
#   drain the final messages before the FIN (see defect 3 below); default 10 s.
```

### Validated run (reaches gameplay)

Run path: Altirra (NetSIO `custom` device) → fujinet-emulator-bridge
(`netsiohub`, UDP 9997) → fujinet-pc firmware (real TCP) → asset server. The
firmware connects from the LAN address and performs the real `N:TCP` transfer.

Result: TCP connect succeeds; the client sends `request(version=1,
transfer_id=0x42, asset_set=1, credit=2)`; the server streams all **66** asset
messages (5,619 framed wire bytes) while the client tops the credit window back to
2 as it drains (never more than 2 in flight, RX ring never overflows); the loader
reaches 16/16 tileset blocks and 96/96 chunk rows, accepts `COMPLETE`, binds the
network tileset + physical map, installs the palette, and enters gameplay. The
live playfield is pixel-identical to the Embedded and SimulatedNetwork builds, and
tank movement/camera are unchanged. Verified end-to-end on the
Altirra+netsiohub+fujinet-pc path; final client state dump (`-DEDGE_TANK_NET_DIAG`,
H: self-dump): `state=Ready net_error=0 loader_error=0 messages=66 tiles=16
rows=96 overflow=0`.

### Defects found on the real backend (and fixed)

Three real-backend defects were demonstrated and fixed; **no** asset-protocol,
credit-window, or RX-ring change was needed (those are correct as designed).

1. **Loading timeouts too short.** Real `N:TCP` read latency is far higher than the
   simulated path; the original 3 s inactivity window tripped a false
   `InactivityTimeout` mid-transfer. Widened (demo) to **connect 30 s / inactivity
   60 s**. The timers reset on each accepted message / successful connect, so they
   bound a genuine stall, not the whole transfer.

2. **Per-byte SIO reads → frame desync + extreme slowness.** The engine session
   lane drains the RX one byte at a time; each `network_read_nb` is a full
   `network_status` + `sio_read` SIO transaction, so one transfer was ~5,600 SIO
   round-trips over emulated NetSIO. That was pathologically slow **and** an
   occasional dropped/garbled byte desynchronised the session framing (observed as
   a false `UnexpectedKind` ~⅓ of the way in). Fixed in the **Atari FujiNet
   adapter** (`fujinet_session_fujinetlib.h`) by staging one bulk `network_read_nb`
   (up to 256 B) and serving the byte-at-a-time drain from it — ~30–90× fewer SIO
   transactions. No change to the generic recv seam. Transfer time dropped from
   minutes to ~12 s and the desync disappeared (all frames `kind=1`).

3. **Server closed too early → tail truncation.** After the bulk fix the transfer
   reliably reached 64/66, then stalled: the server `close()`d immediately after
   the last write and the FujiNet firmware discarded the final ~150 bytes still
   buffered unread when the FIN arrived (client stuck missing the last chunk row +
   `COMPLETE`). Fixed in the server with a post-`COMPLETE` **linger** (`--linger`,
   default 10 s): it keeps the socket open, absorbing late credit grants until the
   client has drained everything. This is correct shutdown ordering, **not**
   application retransmission.
