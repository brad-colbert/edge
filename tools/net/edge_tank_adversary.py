#!/usr/bin/env python3
"""Server-driven adversary for the EDGE networked two-tank demo (demo/tank_net).

Pairs with demo/tank_net/atari_tank_net_demo.cpp. Streams an authoritative
adversary-tank STATE snapshot (world position, heading, speed) to the Atari at
~10 Hz over the realtime lane (ADR-015: the host sends state, not input), and —
the link is bidirectional — consumes the Atari's own player snapshots so the
adversary AI can react (chase / avoid).

The TRANSPORT is generic UDP and knows nothing about Atari/SIO/POKEY/FujiNet; the
only demo-specific knowledge is the 16-byte TankPacket16 layout below, kept in one
place. Netstream carries an UNFRAMED byte stream, so inbound 16-byte units are
reassembled across datagram boundaries (resync on the magic+version marker) exactly
like tools/net/edge_realtime_peer.py.

KEY DESIGN — invisible snaps. The Atari dead-reckons the adversary forward between
the 10 Hz snapshots using the SAME Q12.4 motion table as the player tank. This
server replicates that table and advances the adversary IDENTICALLY each tick
(speed * frames_per_tick applications of the per-frame motion vector + the world
clamp). So the position it sends already equals what the client dead-reckoned to,
and each snap only re-affirms it — no visible teleport. Keep --speed-scale in sync
with the demo's EDGE_TANK_SPEED_SCALE (default 3).

Stdlib only.
"""

import argparse
import math
import socket
import struct
import sys
import time

# Verified frame rates (Hz), derived from ANTIC geometry (see edge_realtime_peer.py).
TV_FPS = {"ntsc": 59.9227, "pal": 49.8607}

DEFAULT_SIZE = 16

# ── TankPacket16 layout (must match demo/tank_net/adversary_net.h) ────────────
#   0 magic(0xAD) 1 version(0x01) 2 type(0=adversary S->C, 1=player C->S) 3 status
#   4..5 seq LE   6..7 x_nom LE   8..9 y_nom LE   10 heading(0..15) 11 speed
#   12..14 reserved   15 pattern(0x5A)
_LAYOUT = "<BBBBHHHBB3sB"   # magic,ver,type,status, seq,x,y, heading,speed, resv,pattern
MAGIC = 0xAD
VERSION = 0x01
PATTERN = 0x5A
TYPE_ADVERSARY = 0
TYPE_PLAYER = 1
_MARKER = bytes((MAGIC, VERSION))

# World clamp (nominal px) — the centre box from tank_motion.h clamp_world.
WORLD_X_MIN, WORLD_X_MAX = 8, 632
WORLD_Y_MIN, WORLD_Y_MAX = 8, 376


def decode(data):
    """Decode 16 bytes into a dict, or None if not a valid TankPacket16."""
    if len(data) != 16:
        return None
    magic, ver, ptype, status, seq, x, y, heading, speed, _resv, pattern = \
        struct.unpack(_LAYOUT, data)
    if magic != MAGIC:
        return None
    return {"magic": magic, "version": ver, "type": ptype, "status": status,
            "seq": seq, "x": x, "y": y, "heading": heading, "speed": speed,
            "pattern": pattern}


def encode(f):
    return struct.pack(_LAYOUT, MAGIC, VERSION, f["type"], f.get("status", 0),
                       f["seq"] & 0xFFFF, f["x"] & 0xFFFF, f["y"] & 0xFFFF,
                       f["heading"] & 0x0F, f["speed"] & 0xFF, b"\x00\x00\x00",
                       PATTERN)


def seq_delta(a, b):
    """Signed shortest-path delta on a u16 sequence ring (matches the client)."""
    return ((a - b + 0x8000) & 0xFFFF) - 0x8000


class Reassembler:
    """Reassemble fixed-size packets from an unframed byte stream (see module doc)."""

    def __init__(self, size=DEFAULT_SIZE):
        self.size = size
        self.buf = bytearray()
        self.resync_bytes = 0

    def feed(self, data):
        self.buf += data
        out = []
        while len(self.buf) >= self.size:
            if self.buf[0] == MAGIC and self.buf[1] == VERSION:
                out.append(bytes(self.buf[:self.size]))
                del self.buf[:self.size]
            else:
                idx = self.buf.find(_MARKER, 1)
                if idx < 0:
                    self.resync_bytes += len(self.buf) - 1
                    del self.buf[:-1]
                    break
                self.resync_bytes += idx
                del self.buf[:idx]
        return out


# ── Motion model — mirrors demo/tank/tank_motion.h (Q12.4 nominal px/frame) ────
def motion_table(scale):
    """The 16-entry Q12.4 per-frame motion vectors, scaled like the client's table.
    dy negative = north. Index 0 = N, clockwise."""
    base = [(0, -8), (3, -7), (6, -6), (7, -3), (8, 0), (7, 3), (6, 6), (3, 7),
            (0, 8), (-3, 7), (-6, 6), (-7, 3), (-8, 0), (-7, -3), (-6, -6), (-3, -7)]
    return [(dx * scale, dy * scale) for dx, dy in base]


class TankSim:
    """Adversary kinematics in Q12.4, advanced exactly like the client's move_tank()."""

    def __init__(self, x_nom, y_nom, heading, table):
        self.x_q4 = x_nom << 4
        self.y_q4 = y_nom << 4
        self.heading = heading
        self.table = table

    def step(self):
        dx, dy = self.table[self.heading & 15]
        self.x_q4 += dx
        self.y_q4 += dy
        self.x_q4 = _clamp(self.x_q4, WORLD_X_MIN << 4, WORLD_X_MAX << 4)
        self.y_q4 = _clamp(self.y_q4, WORLD_Y_MIN << 4, WORLD_Y_MAX << 4)

    def advance(self, steps):
        for _ in range(steps):
            self.step()

    @property
    def x_nom(self):
        return self.x_q4 >> 4

    @property
    def y_nom(self):
        return self.y_q4 >> 4


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def heading_to(dx, dy):
    """Nearest of 16 headings pointing along (dx,dy). N=-y, clockwise, E=+x."""
    if dx == 0 and dy == 0:
        return 0
    ang = math.atan2(dx, -dy)              # 0 at north, +pi/2 at east (clockwise)
    return int(round(ang / (2.0 * math.pi / 16.0))) & 15


def choose_heading(mode, adv, player, t, patrol_period):
    """AI: pick the adversary's heading for this tick."""
    if mode == "patrol" or player is None:
        # Roam: sweep heading over time; the world clamp keeps it in bounds.
        return int(t / patrol_period) & 15
    dx = player[0] - adv.x_nom
    dy = player[1] - adv.y_nom
    if mode == "avoid":
        dx, dy = -dx, -dy
    return heading_to(dx, dy)


def main():
    ap = argparse.ArgumentParser(description="EDGE networked-tank adversary server.")
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=9000, help="UDP port (default 9000)")
    ap.add_argument("--mode", choices=["chase", "avoid", "patrol"], default="chase",
                    help="adversary AI (default chase)")
    ap.add_argument("--hz", type=float, default=10.0, help="snapshot rate (default 10)")
    ap.add_argument("--tv", choices=["ntsc", "pal"], default="ntsc",
                    help="client TV standard, for frames-per-tick (default ntsc)")
    ap.add_argument("--speed", type=int, default=1,
                    help="adversary dead-reckon speed in steps/frame (default 1)")
    ap.add_argument("--speed-scale", type=int, default=3,
                    help="must match the demo's EDGE_TANK_SPEED_SCALE (default 3)")
    ap.add_argument("--start", default="160,96", help="adversary start x,y nominal (default 160,96)")
    ap.add_argument("--patrol-period", type=float, default=0.6,
                    help="seconds per heading step in patrol/no-player mode (default 0.6)")
    ap.add_argument("--target-host", default=None,
                    help="explicit client host (else learn from first inbound packet)")
    ap.add_argument("--target-port", type=int, default=None, help="explicit client port")
    ap.add_argument("--stats-interval", type=float, default=1.0,
                    help="seconds between report lines (default 1.0)")
    ap.add_argument("--decode", action="store_true", help="log inbound player packets")
    args = ap.parse_args()

    if (args.target_host is None) != (args.target_port is None):
        ap.error("--target-host and --target-port must be given together")
    try:
        sx, sy = (int(v) for v in args.start.split(","))
    except ValueError:
        ap.error("--start must be 'x,y' nominal pixels")

    fps = TV_FPS[args.tv]
    frames_per_tick = max(1, int(round(fps / args.hz)))
    steps_per_tick = args.speed * frames_per_tick
    table = motion_table(args.speed_scale)
    adv = TankSim(sx, sy, 0, table)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    sock.setblocking(False)
    reasm = Reassembler(DEFAULT_SIZE)

    target = (args.target_host, args.target_port) if args.target_host else None
    print("edge_tank_adversary: bound %s:%d mode=%s hz=%g fpt=%d steps/tick=%d scale=%d"
          % (args.host, args.port, args.mode, args.hz, frames_per_tick,
             steps_per_tick, args.speed_scale))
    if target:
        print("streaming to %s:%d" % target)
    else:
        print("waiting for first inbound packet to learn target...")

    player = None             # last-known player (x,y) nominal
    player_last_seq = None
    out_seq = 0
    period = 1.0 / args.hz
    next_send = time.monotonic()
    t0 = time.monotonic()
    win_start = t0
    rx = tx = bad = stale = 0

    try:
        while True:
            # Drain inbound player snapshots (learn target + track position).
            try:
                while True:
                    data, addr = sock.recvfrom(2048)
                    for unit in reasm.feed(data):
                        f = decode(unit)
                        if f is None:
                            bad += 1
                            continue
                        if f["type"] != TYPE_PLAYER:
                            continue
                        if player_last_seq is not None and \
                           seq_delta(f["seq"], player_last_seq) <= 0:
                            stale += 1
                            continue
                        player_last_seq = f["seq"]
                        player = (f["x"], f["y"])
                        rx += 1
                        if target is None:
                            target = addr
                            print("learned target %s:%d" % (target[0], target[1]))
                        if args.decode:
                            print("rx player seq=%d pos=(%d,%d) hdg=%d spd=%d"
                                  % (f["seq"], f["x"], f["y"], f["heading"], f["speed"]))
            except BlockingIOError:
                pass

            now = time.monotonic()
            if now >= next_send:
                # Advance the adversary EXACTLY as the client will dead-reckon it,
                # then snapshot — so the client's snap matches its own DR (no jump).
                adv.heading = choose_heading(args.mode, adv, player,
                                             now - t0, args.patrol_period)
                adv.advance(steps_per_tick)
                if target is not None:
                    sock.sendto(encode({"type": TYPE_ADVERSARY, "status": 0x01,
                                        "seq": out_seq, "x": adv.x_nom, "y": adv.y_nom,
                                        "heading": adv.heading, "speed": args.speed}),
                                target)
                    tx += 1
                    out_seq = (out_seq + 1) & 0xFFFF
                next_send += period
                if next_send < now:
                    next_send = now + period

            if now - win_start >= args.stats_interval:
                el = now - win_start
                print("%.1fs: rx %.1f/s tx %.1f/s  adv=(%d,%d) hdg=%d  bad=%d stale=%d resync=%dB"
                      % (el, rx / el, tx / el, adv.x_nom, adv.y_nom, adv.heading,
                         bad, stale, reasm.resync_bytes))
                win_start = now
                rx = tx = 0

            time.sleep(0.001)
    except KeyboardInterrupt:
        dur = max(time.monotonic() - t0, 1e-9)
        print("\nsummary: %.1fs  bad=%d stale=%d resync=%dB" % (dur, bad, stale,
                                                               reasm.resync_bytes))
        print("shutting down.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
