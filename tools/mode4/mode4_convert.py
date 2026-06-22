#!/usr/bin/env python3

"""Wrap/unwrap the tank demo's raw Mode 4 assets to and from the Mode4.exe
editor's container formats.

Mode4.exe (a Windows ANTIC-mode-4 charset/screen editor, run under wine) stores:

  .chs (charset) = 1024-byte ANTIC-4 charset      + 144-byte colour/DLI table
  .m4c (screen)  =  960-byte screen (40x24 codes)  + 144-byte colour/DLI table

The EDGE build instead consumes the bare payloads:

  demo/tank/assets/tank_tileset.fnt = 1024 bytes (the charset, no table)
  demo/tank/assets/chunk_*.scr      =  960 bytes (a 40x24 screen, no table)

So conversion is pure append (wrap) / strip (unwrap) of the trailing 144-byte
table -- the glyph and tile-code bytes are never touched, which makes the
round-trip byte-for-byte lossless. The 144-byte table only drives the editor's
colour *preview*; it does not exist in, and has no effect on, the game build
(the live palette comes from demo/tank/tank_palette.h).

Table structure (reverse-engineered from the bundled Mario sample): 24 records
of 5 bytes, one per Mode 4 character row -- `0x3d C1 0x00 C2 C3` where C1..C3 are
colour slots -- followed by 24 trailing per-row flag bytes (24*5 + 24 = 144).

Usage:
  mode4_convert.py wrap-chs   in.fnt out.chs [--table mario|tank|FILE]
  mode4_convert.py unwrap-chs in.chs out.fnt
  mode4_convert.py wrap-m4c   in.scr out.m4c [--table mario|tank|FILE]
  mode4_convert.py unwrap-m4c in.m4c out.scr
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

CHARSET_BYTES = 1024
SCREEN_BYTES = 960
TABLE_BYTES = 144
CHS_BYTES = CHARSET_BYTES + TABLE_BYTES  # 1168
M4C_BYTES = SCREEN_BYTES + TABLE_BYTES  # 1104

# The known-good 144-byte colour/DLI table copied verbatim from the Mario sample
# (Mode4/Mario/mario.dli). Mode4.exe opens cleanly with this; glyph/tile bytes
# are unaffected by it, so it is a safe default for inspecting any charset.
MARIO_TABLE = bytes.fromhex(
    # 24 per-row colour records (5 bytes: 3d C1 00 C2 C3) ...
    "3d0a005b0d3d0700050d3d0700050d3d0700050d3d0700050d"
    "3d0700050d3d0700050d3d0700050d3d0700050d3d0700050d"
    "3d7a005b0d3d7a005b0d3d7a005b0d3d5d005b0d3d5d005b0d"
    "3d5d005b0d3d5d005b0d3d5d005b0d3d5d005b0d3d5d005b0d"
    "3d13005b173d13005b173d13005b173d13005b17"
    # ... then 24 trailing per-row flag bytes.
    "010100000000000000000100000100000000000001000000"
)
assert len(MARIO_TABLE) == TABLE_BYTES, len(MARIO_TABLE)

# Tank palette colour-register values (mirror of demo/tank/tank_palette.h, kept
# in pixel-value order). Used only to tint the editor preview for --table tank.
TANK_COLPF0 = 0x00  # walls
TANK_COLPF1 = 0x07  # light detail
TANK_COLPF2 = 0x1B  # accent (27 decimal)


def build_tank_table() -> bytes:
    """Best-effort table that tints Mode4's preview toward the tank palette.

    Starts from the Mario template (markers + trailing flags preserved) and
    overwrites the two obvious per-row colour slots with the tank detail/accent
    registers, uniformly across all 24 rows. The mapping of C1..C3 to specific
    COLPF registers is not fully pinned down, so treat this as a starting point
    and fine-tune colours inside Mode4 if needed -- glyph editing is unaffected.
    """
    table = bytearray(MARIO_TABLE)
    for row in range(24):
        base = row * 5  # record = 3d C1 00 C2 C3
        table[base + 1] = TANK_COLPF1  # C1
        table[base + 3] = TANK_COLPF2  # C2
    return bytes(table)


def resolve_table(spec: str) -> bytes:
    if spec == "mario":
        return MARIO_TABLE
    if spec == "tank":
        return build_tank_table()
    data = Path(spec).read_bytes()
    if len(data) != TABLE_BYTES:
        raise ValueError(
            f"--table file '{spec}' is {len(data)} bytes; expected {TABLE_BYTES}"
        )
    return data


def read_exact(path: Path, expected: int, what: str) -> bytes:
    data = path.read_bytes()
    if len(data) != expected:
        raise ValueError(
            f"{what}: '{path}' is {len(data)} bytes; expected {expected}"
        )
    return data


def wrap(src: Path, dst: Path, payload_bytes: int, table: bytes, what: str) -> None:
    payload = read_exact(src, payload_bytes, what)
    dst.write_bytes(payload + table)


def unwrap(src: Path, dst: Path, container_bytes: int, payload_bytes: int, what: str) -> None:
    data = read_exact(src, container_bytes, what)
    dst.write_bytes(data[:payload_bytes])


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def add_table_arg(p: argparse.ArgumentParser) -> None:
        p.add_argument(
            "--table",
            default="mario",
            help="colour/DLI table: 'mario' (default, known-good), 'tank' "
            "(best-effort tank tint), or a path to a 144-byte table file",
        )

    p = sub.add_parser("wrap-chs", help="raw .fnt charset -> Mode4 .chs")
    p.add_argument("source")
    p.add_argument("destination")
    add_table_arg(p)

    p = sub.add_parser("unwrap-chs", help="Mode4 .chs -> raw .fnt charset")
    p.add_argument("source")
    p.add_argument("destination")

    p = sub.add_parser("wrap-m4c", help="raw .scr screen -> Mode4 .m4c")
    p.add_argument("source")
    p.add_argument("destination")
    add_table_arg(p)

    p = sub.add_parser("unwrap-m4c", help="Mode4 .m4c -> raw .scr screen")
    p.add_argument("source")
    p.add_argument("destination")

    return parser


def run(args: argparse.Namespace) -> int:
    src = Path(args.source)
    dst = Path(args.destination)

    if args.command == "wrap-chs":
        wrap(src, dst, CHARSET_BYTES, resolve_table(args.table), "charset")
    elif args.command == "unwrap-chs":
        unwrap(src, dst, CHS_BYTES, CHARSET_BYTES, "chs")
    elif args.command == "wrap-m4c":
        wrap(src, dst, SCREEN_BYTES, resolve_table(args.table), "screen")
    elif args.command == "unwrap-m4c":
        unwrap(src, dst, M4C_BYTES, SCREEN_BYTES, "m4c")
    else:  # pragma: no cover - argparse enforces the choices
        raise ValueError(f"unknown command {args.command!r}")

    print(f"Wrote {dst} ({dst.stat().st_size} bytes)")
    return 0


def main() -> int:
    args = build_arg_parser().parse_args()
    try:
        return run(args)
    except (ValueError, FileNotFoundError, OSError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
