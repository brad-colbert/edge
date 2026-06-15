#!/usr/bin/env python3
# tests/backends/atari/netstream_peer_echo.py
#
# UDP echo peer for Netstream Mode B (emulator) validation -- the network endpoint the Atari
# talks to through FujiNet. Used by netstream_datapath_altirra_probe / netstream_txirq_diag_probe
# (see documents/PLATFORM_ATARI.md "Netstream Mode B emulator validation").
#
# Behaviour: records every inbound datagram (recv.bin + recv.log), and once the expected
# outbound pattern A0..AF has arrived (possibly after a leading FujiNet REGISTER packet, and
# possibly split across datagrams), replies EXACTLY ONCE with 50..5F. Replying once -- only
# after the full pattern -- avoids injecting duplicate replies / stray bytes into the Atari
# RX ring.
#
# Why a distinct IP is required: the FujiNet firmware binds 0.0.0.0:<port> AND sends to
# host:<port>, so a 127.0.0.1 peer self-loops. Run this in a Docker container (or a second
# host) with its own IP -- see scripts/netstream_modeb_peer.sh.
#
# Env: NSPEER_OUT (output dir, default /out -- the Docker mount), NSPEER_PORT (default 9000).

import os
import socket
import time

OUT  = os.environ.get("NSPEER_OUT", "/out")
PORT = int(os.environ.get("NSPEER_PORT", "9000"))
EXPECTED = bytes(range(0xA0, 0xB0))   # A0..AF (probe's outbound packet)
REPLY    = bytes(range(0x50, 0x60))   # 50..5F (probe's expected inbound packet)

os.makedirs(OUT, exist_ok=True)
recv_bin = os.path.join(OUT, "recv.bin")
recv_log = os.path.join(OUT, "recv.log")
open(recv_bin, "wb").close()
open(recv_log, "w").close()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", PORT))
print(f"netstream echo peer listening on 0.0.0.0:{PORT}, out={OUT}", flush=True)

buf = bytearray()
sent_reply = False
while True:
    data, addr = s.recvfrom(2048)
    with open(recv_log, "a") as log:
        log.write(f"{time.time():.6f} from={addr} len={len(data)} hex={data.hex()}\n")
    with open(recv_bin, "ab") as f:
        f.write(data)
        f.flush()
    buf.extend(data)
    if not sent_reply and EXPECTED in buf:    # full pattern present -> reply once
        s.sendto(REPLY, addr)
        sent_reply = True
        with open(recv_log, "a") as log:
            log.write(f"{time.time():.6f} reply len={len(REPLY)} hex={REPLY.hex()} to={addr}\n")
