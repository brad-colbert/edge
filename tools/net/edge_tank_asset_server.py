#!/usr/bin/env python3
"""edge_tank_asset_server.py — Stage 5A tank-demo asset server (stdlib only).

Emits the demo-local network-asset protocol (see demo/tank/ASSET_PROTOCOL.md and
demo/tank/asset_protocol.h) for the fixed tank asset set: one 1024-byte tileset
and four 960-byte map chunks. All multi-byte integers are little-endian.

This is a DEMO-LOCAL protocol, not a generic EDGE asset protocol. Stage 5A does
not require the real transport; this tool can:
  * --capture FILE : write length-prefixed framed messages (u16 LE length + payload)
  * --hex          : print each message as hex
  * --tcp          : a simple TCP listener that sends one length-prefixed framed
                     message per protocol message on connect (placeholder shape
                     for the Stage 5B session connection)

No external dependencies.
"""

import argparse
import os
import socket
import struct
import sys

# ── Protocol constants (mirror demo/tank/asset_protocol.h) ──────────────────
VERSION = 1
K_MANIFEST, K_TILESET_BLOCK, K_CHUNK_ROWS, K_COMPLETE, K_ERROR = 1, 2, 3, 4, 5

TILESET_BYTES = 1024
TILESET_BLOCK = 64                 # 16 blocks
CHUNK_COLS, CHUNK_GRID_ROWS = 2, 2
CHUNK_W, CHUNK_H = 40, 24
CHUNK_COUNT = 4
ROW_BYTES = 40
MAX_ROWS_PER_MSG = 2
SESSION_MAX = 128

ASSET_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "demo", "tank", "assets")
TILESET_FILE = "tank_tileset.fnt"
CHUNK_FILES = {  # (chunk_x, chunk_y) -> file
    (0, 0): "chunk_0_0.scr", (1, 0): "chunk_1_0.scr",
    (0, 1): "chunk_0_1.scr", (1, 1): "chunk_1_1.scr",
}


def _u16(v):
    return struct.pack("<H", v)


def _prefix(kind, xfer):
    return bytes([VERSION, kind, xfer])


def msg_manifest(xfer):
    return _prefix(K_MANIFEST, xfer) + _u16(TILESET_BYTES) + bytes(
        [CHUNK_COLS, CHUNK_GRID_ROWS, CHUNK_W, CHUNK_H, CHUNK_COUNT])


def msg_tileset_block(xfer, offset, payload):
    return _prefix(K_TILESET_BLOCK, xfer) + _u16(offset) + _u16(len(payload)) + payload


def msg_chunk_rows(xfer, cx, cy, start_row, rows):
    rc = len(rows) // ROW_BYTES
    return (_prefix(K_CHUNK_ROWS, xfer) + bytes([cx, cy, start_row, rc])
            + _u16(len(rows)) + rows)


def msg_complete(xfer):
    return _prefix(K_COMPLETE, xfer)


def load_assets():
    tpath = os.path.join(ASSET_DIR, TILESET_FILE)
    with open(tpath, "rb") as f:
        tileset = f.read()
    if len(tileset) != TILESET_BYTES:
        sys.exit("error: %s is %d bytes, expected %d" % (tpath, len(tileset), TILESET_BYTES))
    chunks = {}
    for key, name in CHUNK_FILES.items():
        with open(os.path.join(ASSET_DIR, name), "rb") as f:
            data = f.read()
        if len(data) != CHUNK_W * CHUNK_H:
            sys.exit("error: %s is %d bytes, expected %d" % (name, len(data), CHUNK_W * CHUNK_H))
        chunks[key] = data
    return tileset, chunks


def build_messages(xfer, shuffle=False, duplicate=False):
    tileset, chunks = load_assets()
    head = [msg_manifest(xfer)]
    data = []
    for blk in range(TILESET_BYTES // TILESET_BLOCK):
        off = blk * TILESET_BLOCK
        data.append(msg_tileset_block(xfer, off, tileset[off:off + TILESET_BLOCK]))
    for cy in range(CHUNK_GRID_ROWS):
        for cx in range(CHUNK_COLS):
            payload = chunks[(cx, cy)]
            for start in range(0, CHUNK_H, MAX_ROWS_PER_MSG):
                rows = payload[start * ROW_BYTES:(start + MAX_ROWS_PER_MSG) * ROW_BYTES]
                data.append(msg_chunk_rows(xfer, cx, cy, start, rows))
    if duplicate and data:
        data.insert(len(data) // 2, data[0])          # a harmless duplicate
    if shuffle:
        import random
        random.Random(1234).shuffle(data)             # manifest-first / complete-last preserved
    msgs = head + data + [msg_complete(xfer)]
    for m in msgs:
        if len(m) > SESSION_MAX:
            sys.exit("error: message of %d bytes exceeds session max %d" % (len(m), SESSION_MAX))
    return msgs


def main():
    ap = argparse.ArgumentParser(description="Stage 5A tank asset server")
    ap.add_argument("--xfer", type=int, default=0x42, help="transfer id (0-255)")
    ap.add_argument("--shuffle", action="store_true", help="reorder data messages (test)")
    ap.add_argument("--duplicate", action="store_true", help="inject a duplicate block (test)")
    ap.add_argument("--capture", metavar="FILE", help="write length-prefixed framed messages")
    ap.add_argument("--hex", action="store_true", help="print each message as hex")
    ap.add_argument("--tcp", action="store_true", help="serve over TCP (length-prefixed frames)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9000)
    args = ap.parse_args()

    msgs = build_messages(args.xfer & 0xFF, args.shuffle, args.duplicate)
    total = sum(len(m) for m in msgs)
    print("built %d messages, %d payload bytes, max msg %d"
          % (len(msgs), total, max(len(m) for m in msgs)))

    if args.hex:
        for i, m in enumerate(msgs):
            print("%3d kind=%d len=%3d  %s" % (i, m[1], len(m), m.hex()))

    if args.capture:
        with open(args.capture, "wb") as f:
            for m in msgs:
                f.write(_u16(len(m)) + m)              # u16 LE length prefix + payload
        print("wrote %s (length-prefixed frames)" % args.capture)

    if args.tcp:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((args.host, args.port))
        srv.listen(1)
        print("listening on %s:%d ..." % (args.host, args.port))
        while True:
            conn, addr = srv.accept()
            print("connection from %s" % (addr,))
            try:
                for i, m in enumerate(msgs):
                    conn.sendall(_u16(len(m)) + m)
                    print("  sent %3d/%d kind=%d len=%d" % (i + 1, len(msgs), m[1], len(m)))
                print("  transfer complete")
            except OSError as e:
                print("  connection error: %s" % e)
            finally:
                conn.close()

    if not (args.hex or args.capture or args.tcp):
        print("(no output mode; use --hex, --capture FILE, or --tcp)")


if __name__ == "__main__":
    main()
