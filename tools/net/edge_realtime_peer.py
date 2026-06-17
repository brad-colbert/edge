#!/usr/bin/env python3
"""Generic UDP peer for the EDGE realtime networking diagnostic demo (Stage 9S.3).

Pairs with demo/edge_net_realtime_meter.cpp. The TRANSPORT here is deliberately
generic: it moves fixed-size UDP datagrams and knows nothing about Atari, SIO,
POKEY, FujiNet, or Netstream. The only EDGE-specific knowledge — the MeterPacket16
field layout — is isolated in the `meterpacket` helpers below and is used solely to
fill in the echo/ticker timing fields and (optionally) to pretty-print packets.

Modes:
  echo (default)  Reply once per received 16-byte packet: copy the client-owned
                  fields through unchanged, stamp the peer receive/transmit virtual
                  jiffies (T2/T3) and an advancing peer sequence, and send exactly
                  16 bytes back to the sender.
  ticker          Stream generated 16-byte packets for RX-only client testing. A
                  UDP sender needs a destination: either pass --target-host/--target-port,
                  or (default) wait for the first valid inbound packet and learn the
                  sender's address.

Time model: a virtual jiffy clock in Atari-compatible u16 units (wraps mod 65536),
advanced by host monotonic time. It is SEEDED from the first valid client packet's
T1 (coarse only — one-way latency is included). The default jiffy rate is derived
from --tv (see TV_JIFFY_HZ) and verified against the standard ANTIC frame geometry:
cpu_frequency / (scanlines * 114) -> NTSC 1789773/29868 = 59.9227 Hz,
PAL 1773447/35568 = 49.8607 Hz. --hz overrides.

Stdlib only.
"""

import argparse
import socket
import struct
import sys
import time

# Verified jiffy rates (Hz). Derived, not the rounded 60/50 — see module docstring.
TV_JIFFY_HZ = {"ntsc": 59.9227, "pal": 49.8607}

DEFAULT_SIZE = 16

# ── MeterPacket16 layout (DEMO DIAGNOSTIC PAYLOAD ONLY — not an EDGE wire format) ──
#   0  magic (0xE7)      1  version (0x01)   2  type (0=req,1=reply,2=ticker)
#   3  client_status     4..5 client_seq LE  6..7 peer_seq LE
#   8..9 T1 client send jiffy LE             10..11 T2 peer recv vjiffy LE
#   12..13 T3 peer xmit vjiffy LE            14 peer_status   15 pattern (0x5A)
_LAYOUT = "<BBBBHHHHHBB"   # magic,ver,type,cstat, cseq,pseq,t1,t2,t3, pstat,pattern
MAGIC = 0xE7
VERSION = 0x01
PATTERN = 0x5A
TYPE_REQ = 0
TYPE_REPLY = 1
TYPE_TICK = 2


def meterpacket_decode(data):
    """Decode 16 bytes into a dict, or None if it isn't a valid MeterPacket16."""
    if len(data) != 16:
        return None
    (magic, version, ptype, cstat, cseq, pseq,
     t1, t2, t3, pstat, pattern) = struct.unpack(_LAYOUT, data)
    if magic != MAGIC:
        return None
    return {
        "magic": magic, "version": version, "type": ptype, "client_status": cstat,
        "client_seq": cseq, "peer_seq": pseq, "t1": t1, "t2": t2, "t3": t3,
        "peer_status": pstat, "pattern": pattern,
    }


def meterpacket_encode(f):
    """Encode a MeterPacket16 dict back into exactly 16 bytes."""
    return struct.pack(
        _LAYOUT, f["magic"], f["version"], f["type"], f["client_status"],
        f["client_seq"] & 0xFFFF, f["peer_seq"] & 0xFFFF,
        f["t1"] & 0xFFFF, f["t2"] & 0xFFFF, f["t3"] & 0xFFFF,
        f["peer_status"] & 0xFF, f["pattern"] & 0xFF)


def seq_delta(a, b):
    """Signed shortest-path delta on a u16 sequence ring (matches the client's sub16)."""
    return ((a - b + 0x8000) & 0xFFFF) - 0x8000


# Each MeterPacket16 starts with magic(0xE7) + version(0x01) — used as the stream
# resync marker below.
_MARKER = bytes((MAGIC, VERSION))


class Reassembler:
    """Reassemble fixed-size packets from an UNFRAMED byte stream.

    Netstream (UDP-seq) carries a raw byte stream with no wire framing: the FujiNet
    firmware forwards serial bytes to UDP in whatever chunks they arrive, so a 16-byte
    unit may split across several datagrams (or several units may share one). The Atari
    EDGE adapter already reassembles 16-byte units from its serial byte ring; this peer
    must do the same instead of assuming one datagram == one packet.

    Datagrams are assumed in order (true on a local NetSIO link). If the stream desyncs
    (a lost/reordered datagram drops bytes), we resync on the magic+version marker and
    count the discarded bytes — there is no framing/checksum to recover otherwise."""

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
                if idx < 0:                       # no marker yet; keep a tail byte
                    self.resync_bytes += len(self.buf) - 1
                    del self.buf[:-1]
                    break
                self.resync_bytes += idx
                del self.buf[:idx]
        return out


class LinkStats:
    """Windowed + cumulative forward-path reliability and throughput for a fixed-size
    packet stream. The peer sees every client_seq, so it measures the FORWARD path
    (sender -> peer) authoritatively: throughput in packets/s and bytes/s, loss from
    sequence-gap analysis, plus duplicate and reorder counts. 'bad' = malformed /
    wrong-size datagrams that never decoded."""

    def __init__(self, interval, pkt_size=DEFAULT_SIZE, label="echo"):
        self.interval = interval
        self.pkt_size = pkt_size
        self.label = label
        self.t0 = time.monotonic()
        # Cumulative (lifetime) totals.
        self.tot_rx = self.tot_tx = self.tot_bad = 0
        self.tot_dup = self.tot_reorder = self.tot_resync = 0
        self.tot_uniq = self.tot_expected = 0
        self._reset_window()

    def _reset_window(self):
        self.win_start = time.monotonic()
        self.rx = self.tx = self.bad = self.dup = self.reorder = self.resync = 0
        self.seen = set()
        self.cmin = self.cmax = self.prev = None

    def on_rx(self, cseq):
        self.rx += 1
        self.tot_rx += 1
        if cseq in self.seen:
            self.dup += 1
            self.tot_dup += 1
        else:
            self.seen.add(cseq)
        if self.prev is not None and seq_delta(cseq, self.prev) < 0:
            self.reorder += 1
            self.tot_reorder += 1
        self.prev = cseq
        if self.cmin is None:
            self.cmin = self.cmax = cseq
        else:
            if seq_delta(cseq, self.cmin) < 0:
                self.cmin = cseq
            if seq_delta(cseq, self.cmax) > 0:
                self.cmax = cseq

    def on_tx(self):
        self.tx += 1
        self.tot_tx += 1

    def on_bad(self):
        self.bad += 1
        self.tot_bad += 1

    def on_resync(self, nbytes):
        self.resync += nbytes
        self.tot_resync += nbytes

    def _expected(self):
        # Packets the sender *should* have sent across the seen span (gap analysis).
        return 0 if self.cmin is None else seq_delta(self.cmax, self.cmin) + 1

    def due(self):
        return (time.monotonic() - self.win_start) >= self.interval

    def report(self):
        """Format the per-window line and roll the window."""
        elapsed = time.monotonic() - self.win_start
        exp = self._expected()
        uniq = len(self.seen)
        fwd_loss = (1.0 - uniq / exp) if exp > 0 else 0.0
        rx_pps, tx_pps = self.rx / elapsed, self.tx / elapsed
        self.tot_uniq += uniq
        self.tot_expected += exp
        line = ("%s %.1fs: rx %.1fpps %.0fB/s  fwd_loss %.1f%% (%d/%d)  "
                "dup %d reorder %d resync %dB bad %d | tx %.1fpps %.0fB/s"
                % (self.label, elapsed, rx_pps, rx_pps * self.pkt_size, fwd_loss * 100.0,
                   uniq, exp, self.dup, self.reorder, self.resync, self.bad,
                   tx_pps, tx_pps * self.pkt_size))
        self._reset_window()
        return line

    def summary(self):
        dur = max(time.monotonic() - self.t0, 1e-9)
        loss = (1.0 - self.tot_uniq / self.tot_expected) if self.tot_expected > 0 else 0.0
        return ("summary: %.1fs  rx %d (%.1fpps, %.0fB/s)  tx %d  bad %d  "
                "dup %d reorder %d resync %dB  forward_loss %.1f%% (%d/%d)"
                % (dur, self.tot_rx, self.tot_rx / dur, self.tot_rx * self.pkt_size / dur,
                   self.tot_tx, self.tot_bad, self.tot_dup, self.tot_reorder,
                   self.tot_resync, loss * 100.0, self.tot_uniq, self.tot_expected))


class VirtualJiffyClock:
    """Atari-compatible u16 jiffy clock advanced by host monotonic time."""

    def __init__(self, hz):
        self.hz = hz
        self._seeded = False
        self._seed_jiffy = 0
        self._seed_mono = 0.0

    def seeded(self):
        return self._seeded

    def seed(self, jiffy_t1):
        # Coarse: T1 includes one-way latency. Seed once, on the first valid packet.
        self._seed_jiffy = jiffy_t1 & 0xFFFF
        self._seed_mono = time.monotonic()
        self._seeded = True

    def now(self):
        if not self._seeded:
            return 0
        elapsed = time.monotonic() - self._seed_mono
        return int(self._seed_jiffy + elapsed * self.hz) & 0xFFFF


def _maybe_seed(clock, fields):
    if not clock.seeded():
        clock.seed(fields["t1"])


def run_echo(sock, clock, stats, decode, quiet, dump_bad=False):
    peer_seq = 0
    reasm = Reassembler(DEFAULT_SIZE)
    prev_resync = 0
    print("echo: waiting for client packets...")
    while True:
        data, addr = sock.recvfrom(2048)
        # Netstream is an unframed byte stream — reassemble 16-byte units across
        # datagram boundaries rather than assuming one datagram == one packet.
        for unit in reasm.feed(data):
            t2 = clock.now()                     # stamp receive ASAP
            fields = meterpacket_decode(unit)
            if fields is None:                   # marker matched but body invalid
                stats.on_bad()
                if dump_bad:
                    print("BAD unit %s" % unit.hex(" "))
                continue
            _maybe_seed(clock, fields)
            t2 = clock.now()                     # re-read after a possible seed
            stats.on_rx(fields["client_seq"])

            # Preserve client-owned fields; stamp peer-owned ones.
            peer_seq = (peer_seq + 1) & 0xFFFF
            reply = dict(fields)
            reply["type"] = TYPE_REPLY
            reply["peer_seq"] = peer_seq
            reply["t2"] = t2
            reply["peer_status"] = 0x01          # bit0: peer active/echo
            reply["t3"] = clock.now()            # stamp transmit just before send
            sock.sendto(meterpacket_encode(reply), addr)
            stats.on_tx()
            if decode and not quiet:
                print("rx %s:%d %s" % (addr[0], addr[1], _fmt(fields)))

        if reasm.resync_bytes != prev_resync:
            stats.on_resync(reasm.resync_bytes - prev_resync)
            prev_resync = reasm.resync_bytes

        if stats.due():
            print(stats.report())


def run_ticker(sock, clock, stats, args, decode, quiet):
    target = None
    if args.target_host is not None:             # explicit target (validated in main)
        target = (args.target_host, args.target_port)
        print("ticker: streaming to %s:%d at %g Hz" % (target[0], target[1], args.ticker_hz))
    else:
        print("ticker: waiting for first inbound packet to learn target...")

    period = 1.0 / args.ticker_hz
    sock.setblocking(False)
    peer_seq = 0
    last_t1 = 0
    reasm = Reassembler(DEFAULT_SIZE)
    next_send = time.monotonic()

    while True:
        # Drain any inbound (learn target / seed clock / capture last client T1),
        # reassembling 16-byte units from the unframed byte stream.
        try:
            while True:
                data, addr = sock.recvfrom(2048)
                for unit in reasm.feed(data):
                    fields = meterpacket_decode(unit)
                    if fields is None:
                        stats.on_bad()
                        continue
                    stats.on_rx(fields["client_seq"])
                    _maybe_seed(clock, fields)
                    last_t1 = fields["t1"]
                    if target is None:
                        target = addr
                        print("ticker: learned target %s:%d" % (target[0], target[1]))
        except BlockingIOError:
            pass

        now = time.monotonic()
        if target is not None and now >= next_send:
            if not clock.seeded():
                clock.seed(0)                    # no client T1 yet: coarse start at 0
            peer_seq = (peer_seq + 1) & 0xFFFF
            t2 = clock.now()
            pkt = {
                "magic": MAGIC, "version": VERSION, "type": TYPE_TICK,
                "client_status": 0, "client_seq": 0, "peer_seq": peer_seq,
                "t1": last_t1, "t2": t2, "t3": clock.now(),
                "peer_status": 0x02, "pattern": PATTERN,   # bit1: peer ticker
            }
            sock.sendto(meterpacket_encode(pkt), target)
            stats.on_tx()
            next_send += period
            if next_send < now:                  # fell behind; resync schedule
                next_send = now + period

        if stats.due():
            print(stats.report())

        time.sleep(0.001)


def _fmt(f):
    return ("type=%d cseq=%d pseq=%d t1=%d t2=%d t3=%d cstat=0x%02X"
            % (f["type"], f["client_seq"], f["peer_seq"], f["t1"], f["t2"],
               f["t3"], f["client_status"]))


def main():
    parser = argparse.ArgumentParser(
        description="Generic UDP peer for the EDGE realtime networking demo.")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=9000, help="UDP port (default 9000)")
    parser.add_argument("--mode", choices=["echo", "ticker"], default="echo",
                        help="echo (default): reply per packet; ticker: stream packets")
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE,
                        help="expected packet size in bytes (default 16)")
    parser.add_argument("--tv", choices=["ntsc", "pal"], default="ntsc",
                        help="TV standard for the virtual jiffy rate (default ntsc)")
    parser.add_argument("--hz", type=float, default=None,
                        help="override virtual jiffy rate (else derived from --tv)")
    parser.add_argument("--ticker-hz", type=float, default=30.0,
                        help="ticker send rate in packets/sec (default 30)")
    parser.add_argument("--target-host", default=None,
                        help="ticker: explicit destination host (else learn from inbound)")
    parser.add_argument("--target-port", type=int, default=None,
                        help="ticker: explicit destination port")
    parser.add_argument("--stats-interval", type=float, default=1.0,
                        help="seconds between reliability/throughput report lines (default 1.0)")
    parser.add_argument("--dump-bad", action="store_true",
                        help="hexdump undecodable datagrams (diagnose 'bad' counts)")
    parser.add_argument("--decode", action="store_true",
                        help="pretty-print decoded MeterPacket16 fields")
    parser.add_argument("--quiet", action="store_true",
                        help="suppress per-packet logs (keep periodic summary)")
    args = parser.parse_args()

    if (args.target_host is None) != (args.target_port is None):
        parser.error("--target-host and --target-port must be given together")
    if args.size != DEFAULT_SIZE:
        print("warning: --size %d != 16; non-16-byte datagrams are ignored" % args.size,
              file=sys.stderr)

    hz = args.hz if args.hz is not None else TV_JIFFY_HZ[args.tv]
    clock = VirtualJiffyClock(hz)
    stats = LinkStats(args.stats_interval, pkt_size=DEFAULT_SIZE, label=args.mode)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    print("edge_realtime_peer: bound %s:%d mode=%s tv=%s jiffy=%.4fHz"
          % (args.host, args.port, args.mode, args.tv, hz))

    try:
        if args.mode == "echo":
            run_echo(sock, clock, stats, args.decode, args.quiet, args.dump_bad)
        else:
            run_ticker(sock, clock, stats, args, args.decode, args.quiet)
    except KeyboardInterrupt:
        print("\n" + stats.summary())
        print("shutting down.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
