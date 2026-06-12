#!/usr/bin/env python3
"""UDP position-streaming test server for the Atari edge demo.

Waits for a "hello" packet from an Atari client, then streams 4-byte
[x, y, count] position packets back to the sender at a fixed rate, where count
is a little-endian uint16 per-packet counter ("<BBH"). The counter lets the
client detect dropped, duplicated, or reordered packets. The motion follows a
Lissajous curve scaled into the valid Player/Missile coordinate ranges.

Stdlib only.
"""

import argparse
import math
import socket
import struct
import time

# Lissajous parameters.
A = 3
B = 2
DELTA = math.pi / 2
T_STEP = 0.05

# Target output ranges (clamped to valid Atari coordinates).
X_MIN, X_MAX = 24, 204    # valid P/M horizontal range
Y_MIN, Y_MAX = 32, 220    # visible scanline range


def lissajous(t):
    """Return (x, y) uint8 coordinates for time t along the Lissajous curve."""
    sx = math.sin(A * t)              # -1..1
    sy = math.sin(B * t + DELTA)      # -1..1
    x = round(X_MIN + (sx + 1) * 0.5 * (X_MAX - X_MIN))
    y = round(Y_MIN + (sy + 1) * 0.5 * (Y_MAX - Y_MIN))
    # Defensive clamp to uint8 range.
    x = max(0, min(255, x))
    y = max(0, min(255, y))
    return x, y


def main():
    parser = argparse.ArgumentParser(
        description="UDP position-streaming test server (Lissajous motion)."
    )
    parser.add_argument("--port", type=int, default=2222,
                        help="UDP port to bind (default: 2222)")
    parser.add_argument("--rate", type=float, default=20.0,
                        help="Packets per second to stream (default: 20)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))

    print("Listening on 0.0.0.0:%d, waiting for hello..." % args.port)

    try:
        _, addr = sock.recvfrom(1024)
        print("Hello from %s:%d, streaming at %g Hz" % (addr[0], addr[1], args.rate))

        period = 1.0 / args.rate
        t = 0.0
        count = 0
        # Authoritative send-rate report: a per-packet print can't legibly render
        # at high rates (the terminal scroll blurs into a wrong eyeball estimate),
        # so measure and print the ACTUAL sends/sec once per wall-clock second.
        sent_window = 0
        window_start = time.monotonic()
        while True:
            x, y = lissajous(t)
            # <BBH : x (u8), y (u8), count (u16 little-endian).
            sock.sendto(struct.pack("<BBH", x, y, count), addr)
            count = (count + 1) & 0xFFFF
            sent_window += 1

            now = time.monotonic()
            elapsed = now - window_start
            if elapsed >= 1.0:
                print("sent %d packets in %.3fs = %.1f Hz (count=%d, x=%d y=%d)"
                      % (sent_window, elapsed, sent_window / elapsed, count, x, y))
                sent_window = 0
                window_start = now

            t += T_STEP
            time.sleep(period)
    except KeyboardInterrupt:
        print("\nShutting down.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
