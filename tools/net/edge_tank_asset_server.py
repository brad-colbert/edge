#!/usr/bin/env python3
"""edge_tank_asset_server.py — Stage 5B tank-demo asset server (stdlib only).

Serves the demo's asset set (one 1024-byte tileset + four 960-byte map chunks)
over the EDGE reliable session lane. Two layers:

  * Session transport (EDGE session wire): each message is framed as
        [kind(1)] [size_lo(1)] [size_hi(1)] [payload(size)]      (little-endian)
    matching engine/net_api.h SessionLane. Session kinds:
        1 = asset payload (server -> client)
        2 = transfer request (client -> server)
        3 = credit grant   (client -> server)
  * Tank asset protocol (the payload bytes): see demo/tank/ASSET_PROTOCOL.md and
    demo/tank/asset_protocol.h (MANIFEST / TILESET_BLOCK / CHUNK_ROWS / COMPLETE).

The client connects, sends a request (its transfer id + initial credit), then
grants more credit as it drains; the server sends at most the granted number of
asset messages so the client's 256-byte RX ring never overflows. Demo-local
protocol, not a generic EDGE streaming system. No external dependencies.
"""

import argparse
import os
import socket
import struct
import sys

# ── Tank asset protocol constants (mirror demo/tank/asset_protocol.h) ───────
VERSION = 1
K_MANIFEST, K_TILESET_BLOCK, K_CHUNK_ROWS, K_COMPLETE = 1, 2, 3, 4
TILESET_BYTES, TILESET_BLOCK = 1024, 64
CHUNK_COLS, CHUNK_GRID_ROWS, CHUNK_W, CHUNK_H, CHUNK_COUNT = 2, 2, 40, 24, 4
ROW_BYTES, MAX_ROWS_PER_MSG, SESSION_MAX = 40, 2, 128

# ── Session transport constants (mirror demo/tank/net_session_loader.h) ─────
SESS_ASSET, SESS_REQUEST, SESS_CREDIT = 1, 2, 3

ASSET_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "demo", "tank", "assets")
CHUNK_FILES = {(0, 0): "chunk_0_0.scr", (1, 0): "chunk_1_0.scr",
               (0, 1): "chunk_0_1.scr", (1, 1): "chunk_1_1.scr"}


def _u16(v):
    return struct.pack("<H", v)


def session_frame(kind, payload):
    """Wrap a payload in the EDGE session wire frame."""
    return bytes([kind, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF]) + payload


# ── Asset-protocol payload builders ─────────────────────────────────────────
def _prefix(kind, xfer):
    return bytes([VERSION, kind, xfer])


def p_manifest(xfer):
    return _prefix(K_MANIFEST, xfer) + _u16(TILESET_BYTES) + bytes(
        [CHUNK_COLS, CHUNK_GRID_ROWS, CHUNK_W, CHUNK_H, CHUNK_COUNT])


def p_tileset_block(xfer, off, payload):
    return _prefix(K_TILESET_BLOCK, xfer) + _u16(off) + _u16(len(payload)) + payload


def p_chunk_rows(xfer, cx, cy, start, rows):
    rc = len(rows) // ROW_BYTES
    return _prefix(K_CHUNK_ROWS, xfer) + bytes([cx, cy, start, rc]) + _u16(len(rows)) + rows


def p_complete(xfer):
    return _prefix(K_COMPLETE, xfer)


def load_assets():
    with open(os.path.join(ASSET_DIR, "tank_tileset.fnt"), "rb") as f:
        tileset = f.read()
    if len(tileset) != TILESET_BYTES:
        sys.exit("tileset size %d != %d" % (len(tileset), TILESET_BYTES))
    chunks = {}
    for k, name in CHUNK_FILES.items():
        with open(os.path.join(ASSET_DIR, name), "rb") as f:
            d = f.read()
        if len(d) != CHUNK_W * CHUNK_H:
            sys.exit("%s size %d != %d" % (name, len(d), CHUNK_W * CHUNK_H))
        chunks[k] = d
    return tileset, chunks


def build_asset_payloads(xfer):
    """The 66 ordered Stage 5A asset payloads for transfer id `xfer`."""
    tileset, chunks = load_assets()
    msgs = [p_manifest(xfer)]
    for blk in range(TILESET_BYTES // TILESET_BLOCK):
        off = blk * TILESET_BLOCK
        msgs.append(p_tileset_block(xfer, off, tileset[off:off + TILESET_BLOCK]))
    for cy in range(CHUNK_GRID_ROWS):
        for cx in range(CHUNK_COLS):
            payload = chunks[(cx, cy)]
            for s in range(0, CHUNK_H, MAX_ROWS_PER_MSG):
                msgs.append(p_chunk_rows(xfer, cx, cy, s,
                                         payload[s * ROW_BYTES:(s + MAX_ROWS_PER_MSG) * ROW_BYTES]))
    msgs.append(p_complete(xfer))
    for m in msgs:
        if len(m) > SESSION_MAX:
            sys.exit("asset payload %d > session max %d" % (len(m), SESSION_MAX))
    return msgs


# ── Session-frame reader ────────────────────────────────────────────────────
def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_session_message(conn):
    hdr = recv_exact(conn, 3)
    if hdr is None:
        return None
    kind = hdr[0]
    size = hdr[1] | (hdr[2] << 8)
    payload = recv_exact(conn, size) if size else b""
    if payload is None:
        return None
    return kind, payload


def serve_one(conn, addr, oneshot, linger=10.0):
    print("connection from %s" % (addr,))
    msg = read_session_message(conn)
    if msg is None or msg[0] != SESS_REQUEST or len(msg[1]) < 4:
        print("  bad/no request; closing")
        conn.close()
        return
    ver, xfer, asset_set, credit = msg[1][0], msg[1][1], msg[1][2], msg[1][3]
    print("  request: version=%d xfer=%d asset_set=%d initial_credit=%d" % (ver, xfer, asset_set, credit))
    payloads = build_asset_payloads(xfer)
    idx = 0
    sent_bytes = 0
    while idx < len(payloads):
        while credit > 0 and idx < len(payloads):
            frame = session_frame(SESS_ASSET, payloads[idx])
            conn.sendall(frame)
            sent_bytes += len(frame)
            credit -= 1
            idx += 1
            print("  sent %2d/%d (asset kind=%d, %d B frame), credit=%d"
                  % (idx, len(payloads), payloads[idx - 1][1], len(frame), credit))
        if idx >= len(payloads):
            break
        m = read_session_message(conn)         # wait for more credit
        if m is None:
            print("  client disconnected mid-transfer")
            conn.close()
            return
        if m[0] == SESS_CREDIT and len(m[1]) >= 2:
            credit += m[1][1]
            print("  credit grant +%d -> %d" % (m[1][1], credit))
    print("  transfer complete: %d messages, %d wire bytes" % (len(payloads), sent_bytes))
    # Linger before closing (Stage 5C). Over real FujiNet the client drains the RX
    # over several frames; if we FIN immediately the firmware discards the last
    # messages still buffered unread (observed: client stalls at 64/66, missing the
    # final chunk row + COMPLETE). Wait for the client to finish — it closes the
    # socket (EOF) once it has COMPLETE, or the linger times out. This is correct
    # shutdown ordering, NOT application retransmission.
    conn.settimeout(linger)
    try:
        while conn.recv(64):           # absorb late credit grants until EOF/timeout
            pass
    except (socket.timeout, OSError):
        pass
    conn.close()


def selftest():
    payloads = build_asset_payloads(0x42)
    assert len(payloads) == 66, len(payloads)
    assert max(len(p) for p in payloads) == 89, max(len(p) for p in payloads)
    # Frame + parse round-trip through the session-frame reader's logic.
    for p in payloads:
        f = session_frame(SESS_ASSET, p)
        kind = f[0]
        size = f[1] | (f[2] << 8)
        assert kind == SESS_ASSET
        assert size == len(p)
        assert f[3:] == p
    # Request parsing.
    req = session_frame(SESS_REQUEST, bytes([VERSION, 0x42, 1, 2]))
    assert req[0] == SESS_REQUEST and req[3] == VERSION and req[4] == 0x42 and req[6] == 2
    # Transfer id propagation: a tileset block payload carries the request's xfer.
    assert payloads[1][2] == 0x42
    print("selftest OK: 66 payloads, max 89 B, session framing + request parse verified")


def main():
    ap = argparse.ArgumentParser(description="Stage 5B tank asset server")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--oneshot", action="store_true", help="serve one client then exit")
    ap.add_argument("--linger", type=float, default=10.0,
                    help="seconds to keep the socket open after COMPLETE so the "
                         "FujiNet client can drain the final messages before FIN")
    ap.add_argument("--selftest", action="store_true", help="verify framing/payloads and exit")
    ap.add_argument("--hex", action="store_true", help="print asset payloads as hex and exit")
    ap.add_argument("--capture-session", metavar="FILE",
                    help="write the session-wire frames (kind+len+payload) for xfer 0x42")
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return
    if args.hex:
        for i, p in enumerate(build_asset_payloads(0x42)):
            print("%2d kind=%d len=%2d %s" % (i, p[1], len(p), p.hex()))
        return
    if args.capture_session:
        with open(args.capture_session, "wb") as f:
            for p in build_asset_payloads(0x42):
                f.write(session_frame(SESS_ASSET, p))
        print("wrote %s (session-wire frames)" % args.capture_session)
        return

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print("listening on %s:%d ..." % (args.host, args.port))
    while True:
        conn, addr = srv.accept()
        try:
            serve_one(conn, addr, args.oneshot, args.linger)
        except OSError as e:
            print("  connection error: %s" % e)
        if args.oneshot:
            break


if __name__ == "__main__":
    main()
