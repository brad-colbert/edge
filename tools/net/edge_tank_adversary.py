#!/usr/bin/env python3
"""Server-driven adversaries for the EDGE networked multi-tank demo (demo/tank_net).

Pairs with demo/tank_net/atari_tank_net_demo.cpp. Streams authoritative
adversary-tank STATE snapshots (world position, heading, speed) to the Atari at
~10 Hz over the realtime lane (ADR-015: the host sends state, not input), and —
the link is bidirectional — consumes the Atari's own player snapshots so the
adversary AI can react (chase / avoid / patrol).

Drives N adversaries (default 3, the demo's kAdvCount), each an independent AI with
its own start position and mode. ALL adversaries are packed into ONE combined packet
per tick (status bit i marks record i live) — 1 pkt/tick instead of one-per-adversary,
the downstream-overload mitigation that keeps the Atari's receive path from falling
behind.

The TRANSPORT is generic UDP and knows nothing about Atari/SIO/POKEY/FujiNet; the
only demo-specific knowledge is the 32-byte TankPacket32 layout below, kept in one
place. Netstream carries an UNFRAMED byte stream, so inbound 32-byte units are
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

MAX_ADV = 3                  # adversary records per combined packet (tanknet::kMaxAdv)
DEFAULT_SIZE = 32

# ── TankPacket32 layout (must match demo/tank_net/adversary_net.h) ────────────
# ONE combined packet per tick carries ALL adversaries (downstream-overload fix):
#   0 magic(0xAD) 1 version(0x02) 2 type(0=adversary S->C, 1=player C->S)
#   3 status (S->C: bit i = rec[i] live; C->S: bit0 have_state)   4..5 seq LE
#   6..23  rec[0..2], each = x_nom LE(2) y_nom LE(2) heading(1) speed(1)
#          (C->S: rec[0] = player state)
#   24..25 echo_seq LE (C->S: adv[0] last-applied seq)   26 echo_flags (C->S health)
#   27..29 hit_count[0..2] (C->S: monotonic per-adversary missile-hit counter)
#   30 reserved   31 pattern(0x5A)
_LAYOUT = "<BBBBH" + "HHBB" * MAX_ADV + "HB" + "B" * MAX_ADV + "BB"
MAGIC = 0xAD
VERSION = 0x02
PATTERN = 0x5A
TYPE_ADVERSARY = 0
TYPE_PLAYER = 1
TYPE_BYE = 2
_MARKER = bytes((MAGIC, VERSION))

# World clamp (nominal px) — the centre box from tank_motion.h clamp_world.
WORLD_X_MIN, WORLD_X_MAX = 8, 632
WORLD_Y_MIN, WORLD_Y_MAX = 8, 376


def decode(data):
    """Decode 32 bytes into a dict, or None if not a valid TankPacket32."""
    if len(data) != DEFAULT_SIZE:
        return None
    f = struct.unpack(_LAYOUT, data)
    magic, ver, ptype, status, seq = f[0:5]
    if magic != MAGIC:
        return None
    recs = []
    for i in range(MAX_ADV):
        x, y, heading, speed = f[5 + i * 4: 9 + i * 4]
        recs.append({"x": x, "y": y, "heading": heading, "speed": speed})
    echo_seq, echo_flags = f[5 + MAX_ADV * 4], f[6 + MAX_ADV * 4]
    hit_base = 7 + MAX_ADV * 4
    hit_count = list(f[hit_base: hit_base + MAX_ADV])
    d = {"magic": magic, "version": ver, "type": ptype, "status": status,
         "seq": seq, "recs": recs, "echo_seq": echo_seq, "echo_flags": echo_flags,
         "hit_count": hit_count, "pattern": f[-1]}
    # Player (C->S) state lives in rec[0]; flatten for convenience.
    d.update(x=recs[0]["x"], y=recs[0]["y"],
             heading=recs[0]["heading"], speed=recs[0]["speed"])
    return d


def encode_adversaries(seq, recs):
    """Build a combined S->C adversary packet: one record per adversary (max MAX_ADV),
    status bit i set for each present record."""
    status = 0
    fields = []
    for i in range(MAX_ADV):
        if i < len(recs):
            r = recs[i]
            status |= (1 << i)
            fields += [r["x"] & 0xFFFF, r["y"] & 0xFFFF,
                       r["heading"] & 0x0F, r["speed"] & 0xFF]
        else:
            fields += [0, 0, 0, 0]
    # Tail (S->C): echo_seq=0, echo_flags=0, hit_count[0..2]=0, reserved=0, pattern.
    return struct.pack(_LAYOUT, MAGIC, VERSION, TYPE_ADVERSARY, status,
                       seq & 0xFFFF, *fields, 0, 0, *([0] * MAX_ADV), 0, PATTERN)


def seq_delta(a, b):
    """Signed shortest-path delta on a u16 sequence ring (matches the client)."""
    return ((a - b + 0x8000) & 0xFFFF) - 0x8000


def hit_delta(a, b):
    """Signed shortest-path delta on a u8 hit-counter ring: > 0 means a is newer than
    b (handles the 255->0 wrap correctly; a reordered older value reads <= 0)."""
    return ((a - b + 0x80) & 0xFF) - 0x80


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
    if mode == "chase":
        return heading_to(dx, dy)

    # avoid: flee away from the player, but WALL-AWARE so it never pins in a corner.
    # Fleeing straight away from an upper-right player drives it into the lower-left
    # wall, where the hard world clamp would jam it. Instead: if the flee vector pushes
    # into a wall, zero that component to slide ALONG the wall; if cornered (pushing into
    # both), circle the perimeter clockwise so it keeps moving and looks deliberate.
    fx, fy = -dx, -dy
    m = 24  # px proximity that counts as "at" a wall
    blk_x = (adv.x_nom <= WORLD_X_MIN + m and fx < 0) or \
            (adv.x_nom >= WORLD_X_MAX - m and fx > 0)
    blk_y = (adv.y_nom <= WORLD_Y_MIN + m and fy < 0) or \
            (adv.y_nom >= WORLD_Y_MAX - m and fy > 0)
    if blk_x and blk_y:
        # Cornered: slide along a wall, clockwise around the field, by corner.
        if adv.x_nom <= WORLD_X_MIN + m and adv.y_nom >= WORLD_Y_MAX - m:
            fx, fy = 0, -1          # lower-left  -> north (up the left wall)
        elif adv.x_nom <= WORLD_X_MIN + m:
            fx, fy = 1, 0           # upper-left  -> east  (along the top wall)
        elif adv.y_nom <= WORLD_Y_MIN + m:
            fx, fy = 0, 1           # upper-right -> south (down the right wall)
        else:
            fx, fy = -1, 0          # lower-right -> west  (along the bottom wall)
    elif blk_x:
        fx = 0                       # slide vertically along the side wall
    elif blk_y:
        fy = 0                       # slide horizontally along the top/bottom wall
    return heading_to(fx, fy)


def main():
    ap = argparse.ArgumentParser(description="EDGE networked-tank adversary server.")
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=9000, help="UDP port (default 9000)")
    ap.add_argument("--count", type=int, default=3,
                    help="number of adversaries (default 3, the demo's kAdvCount)")
    ap.add_argument("--mode", choices=["chase", "avoid", "patrol"], default="chase",
                    help="fallback AI for adversaries not named in --modes (default chase)")
    ap.add_argument("--modes", default="chase,avoid,patrol",
                    help="per-adversary AI, comma list (default 'chase,avoid,patrol'); "
                         "entries beyond the list fall back to --mode")
    ap.add_argument("--hz", type=float, default=10.0, help="snapshot rate (default 10)")
    ap.add_argument("--tv", choices=["ntsc", "pal"], default="ntsc",
                    help="client TV standard, for frames-per-tick (default ntsc)")
    ap.add_argument("--speed", type=int, default=1,
                    help="adversary dead-reckon speed in steps/frame (default 1)")
    ap.add_argument("--speed-scale", type=int, default=3,
                    help="must match the demo's EDGE_TANK_SPEED_SCALE (default 3)")
    ap.add_argument("--start", default="160,96",
                    help="fallback start x,y nominal for adversaries not in --starts (default 160,96)")
    ap.add_argument("--starts", default="40,344;624,376;624,16",
                    help="per-adversary start x,y, semicolon list "
                         "(default '40,344;624,376;624,16' — three corner areas; the "
                         "lower-left is inset ~32px off the walls so a chase tank does "
                         "not spawn pinned in the world-clamp corner); "
                         "entries beyond the list fall back to --start")
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
    if args.count < 1:
        ap.error("--count must be >= 1")

    # Loop so a client BYE returns us to waiting for the next client (re-binds its own
    # socket each round); Ctrl-C (interrupt) exits. Old clients that never send BYE
    # simply stream until Ctrl-C, as before.
    while run_adversary_server(args) == "bye":
        print("client left — waiting for the next one...")


def run_adversary_server(args, sock=None):
    """Bind (or adopt) a UDP socket and stream adversary snapshots until interrupted.

    Shared by main() and the combined dual-lane server (edge_tank_dual_server.py).
    When `sock` is given it is used as-is (already bound, non-blocking) — the dual
    server binds UDP BEFORE the TCP asset transfer so no early adversary packet is
    lost during the TCP linger. Otherwise a fresh socket is bound to args.host:port.
    Validation that main() performs with ap.error is repeated here via sys.exit so
    the function is self-contained for either caller.
    """
    def parse_xy(s):
        x, y = (int(v) for v in s.split(","))
        return x, y

    try:
        fallback_start = parse_xy(args.start)
        starts = [parse_xy(s) for s in args.starts.split(";") if s.strip()]
    except ValueError:
        sys.exit("--start/--starts must be 'x,y' nominal pixels (semicolon-separated for --starts)")
    modes = [m for m in args.modes.split(",") if m.strip()]
    for m in modes:
        if m not in ("chase", "avoid", "patrol"):
            sys.exit("--modes entries must be chase|avoid|patrol")

    fps = TV_FPS[args.tv]
    frames_per_tick = max(1, int(round(fps / args.hz)))
    steps_per_tick = args.speed * frames_per_tick
    table = motion_table(args.speed_scale)

    # One independent adversary per index: own sim + AI mode. ALL advs now share ONE
    # combined packet per tick (out_seq below), so there is no per-adversary sequence.
    advs = []
    for i in range(args.count):
        sx, sy = starts[i] if i < len(starts) else fallback_start
        mode = modes[i] if i < len(modes) else args.mode
        # Keep the start corner so a missile-hit respawn returns the adversary there.
        advs.append({"sim": TankSim(sx, sy, 0, table), "mode": mode, "start": (sx, sy)})

    owns_sock = sock is None
    if owns_sock:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((args.host, args.port))
        sock.setblocking(False)
    reasm = Reassembler(DEFAULT_SIZE)

    # Discard any datagrams buffered from a PREVIOUS client before streaming. A client
    # quits by sending BYE several times (UDP is lossy); on a reused socket the extra
    # BYEs — and stale player packets — would otherwise be read here and instantly end
    # (or mislead) this fresh session. The new client streams continuously, so dropping
    # whatever is already buffered at start is harmless.
    try:
        while True:
            sock.recvfrom(2048)
    except BlockingIOError:
        pass

    target = (args.target_host, args.target_port) if args.target_host else None
    print("edge_tank_adversary: bound %s:%d advs=%d hz=%g fpt=%d steps/tick=%d scale=%d"
          % (args.host, args.port, len(advs), args.hz, frames_per_tick,
             steps_per_tick, args.speed_scale))
    for i, a in enumerate(advs):
        print("  adv %d: mode=%s start=(%d,%d)"
              % (i, a["mode"], a["sim"].x_nom, a["sim"].y_nom))
    if target:
        print("streaming to %s:%d" % target)
    else:
        print("waiting for first inbound packet to learn target...")

    player = None             # last-known player (x,y) nominal
    player_last_seq = None
    # Per-adversary missile-hit counter last seen from the client. None until the first
    # player packet establishes a baseline (so we never respawn on the initial value).
    last_hit_count = [None] * len(advs)
    period = 1.0 / args.hz
    next_send = time.monotonic()
    t0 = time.monotonic()
    win_start = t0
    rx = tx = bad = stale = 0
    # Timing-echo diagnostics: how many combined packets the client is behind (lag),
    # tracked as the latest value plus the run-wide min/max so a growing trend is
    # obvious. pkt_sent_seq is the most recent combined-packet seq we transmitted.
    out_seq = 0                # combined-packet sequence (one per tick, all advs)
    pkt_sent_seq = None
    lag_pkts = None
    lag_min = lag_max = None
    cli_ovf = 0          # latest client-reported RX-overflow-event count (echo_flags)
    quitting = False     # set when the client sends a BYE (type 2): stop streaming

    try:
        while not quitting:
            # Drain inbound player snapshots (learn target + track position).
            try:
                while True:
                    data, addr = sock.recvfrom(2048)
                    for unit in reasm.feed(data):
                        f = decode(unit)
                        if f is None:
                            bad += 1
                            continue
                        if f["type"] == TYPE_BYE:
                            print("client BYE (seq=%d) — ending stream" % f["seq"])
                            quitting = True
                            break
                        if f["type"] != TYPE_PLAYER:
                            continue
                        if player_last_seq is not None and \
                           seq_delta(f["seq"], player_last_seq) <= 0:
                            stale += 1
                            continue
                        player_last_seq = f["seq"]
                        player = (f["x"], f["y"])
                        rx += 1
                        cli_ovf = f["echo_flags"]
                        # Missile-hit respawn: the client streams a monotonic per-adversary
                        # hit counter. Edge-detect each change (seq_delta > 0 handles u8 wrap
                        # and reorder) and respawn that adversary at its start corner. The
                        # first packet only records the baseline (no spurious respawn).
                        hc = f["hit_count"]
                        for i, a in enumerate(advs):
                            if i >= len(hc):
                                break
                            if last_hit_count[i] is not None and \
                               hit_delta(hc[i], last_hit_count[i]) > 0:
                                sx, sy = a["start"]
                                a["sim"] = TankSim(sx, sy, 0, table)
                                print("adv %d HIT — respawning to (%d,%d)" % (i, sx, sy))
                            last_hit_count[i] = hc[i]
                        # End-to-end lag: how far adv[0]'s last-applied snapshot trails
                        # the seq we most recently sent (>=0; clamp tiny negatives from
                        # in-flight reorder to 0). Resolution ~1 packet = one send tick.
                        if pkt_sent_seq is not None:
                            d = seq_delta(pkt_sent_seq, f["echo_seq"])
                            lag_pkts = d if d > 0 else 0
                            lag_min = lag_pkts if lag_min is None else min(lag_min, lag_pkts)
                            lag_max = lag_pkts if lag_max is None else max(lag_max, lag_pkts)
                        if target is None:
                            target = addr
                            print("learned target %s:%d" % (target[0], target[1]))
                        if args.decode:
                            lag_s = ("lag=%d (~%dms)" % (lag_pkts, round(lag_pkts * period * 1000))
                                     if lag_pkts is not None else "lag=?")
                            print("rx player seq=%d pos=(%d,%d) hdg=%d spd=%d  %s ovf=%d"
                                  % (f["seq"], f["x"], f["y"], f["heading"], f["speed"],
                                     lag_s, cli_ovf))
                    if quitting:
                        break
            except BlockingIOError:
                pass
            if quitting:
                break

            now = time.monotonic()
            if now >= next_send:
                # Advance each adversary EXACTLY as the client will dead-reckon it, then
                # snapshot ALL of them into ONE combined packet (downstream-overload fix:
                # 1 pkt/tick instead of one-per-adversary). Client snap matches its own DR.
                recs = []
                for a in advs:
                    sim = a["sim"]
                    sim.heading = choose_heading(a["mode"], sim, player,
                                                 now - t0, args.patrol_period)
                    sim.advance(steps_per_tick)
                    recs.append({"x": sim.x_nom, "y": sim.y_nom,
                                 "heading": sim.heading, "speed": args.speed})
                if target is not None:
                    sock.sendto(encode_adversaries(out_seq, recs), target)
                    tx += 1
                    pkt_sent_seq = out_seq            # combined-packet seq just sent
                    out_seq = (out_seq + 1) & 0xFFFF
                next_send += period
                if next_send < now:
                    next_send = now + period

            if now - win_start >= args.stats_interval:
                el = now - win_start
                pos = " ".join("(%d,%d)" % (a["sim"].x_nom, a["sim"].y_nom) for a in advs)
                if lag_pkts is None:
                    lag_s = "lag=? "
                else:
                    lag_s = ("lag=%d (~%dms, min%d/max%d) ovf=%d "
                             % (lag_pkts, round(lag_pkts * period * 1000),
                                lag_min, lag_max, cli_ovf))
                print("%.1fs: rx %.1f/s tx %.1f/s  advs=%s  %sbad=%d stale=%d resync=%dB"
                      % (el, rx / el, tx / el, pos, lag_s, bad, stale, reasm.resync_bytes))
                win_start = now
                rx = tx = 0

            time.sleep(0.001)
    except KeyboardInterrupt:
        dur = max(time.monotonic() - t0, 1e-9)
        print("\nsummary: %.1fs  bad=%d stale=%d resync=%dB" % (dur, bad, stale,
                                                               reasm.resync_bytes))
        print("shutting down.")
        if owns_sock:
            sock.close()
        return "interrupt"
    # Reached only when the client sent BYE. Leave a caller-owned socket open so the
    # combined server can reuse it for the next round; close one we created ourselves.
    if owns_sock:
        sock.close()
    return "bye"


if __name__ == "__main__":
    main()
