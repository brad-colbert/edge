#!/usr/bin/env python3
"""edge_tank_dual_server.py — combined server for the dual-lane tank demo (stdlib only).

Pairs with demo/tank_dual_net/atari_tank_dual_net_demo.cpp, which uses BOTH EDGE
networking lanes SEQUENTIALLY:

  PHASE 1 — the Atari downloads the playfield map + tileset over the reliable
            TCP/session lane (Stage-5A asset protocol).
  HANDOFF — the Atari closes TCP and opens the UDP/realtime lane.
  PHASE 2 — three adversary tanks are streamed to the Atari over UDP at ~10 Hz.

This one process mirrors that sequence: it serves exactly one asset transfer over
TCP, then drops into the adversary UDP loop — one command, the same mental model as
the binary. All protocol logic is imported from the two standalone servers
(edge_tank_asset_server.py, edge_tank_adversary.py); only the sequencing is new.

TCP and UDP can share one port number (different protocols, different sockets), so a
single --port (default 9000) usually suffices; --asset-port / --adv-port split them.

The UDP socket is bound BEFORE the TCP asset transfer so that the first adversary-
bound packets the Atari sends right after the handoff are never lost during the TCP
linger window.

If you prefer the two-process path, run instead:
    edge_tank_asset_server.py --oneshot           # then, once it completes:
    edge_tank_adversary.py --port 9000

Stdlib only.
"""

import argparse
import socket
import sys

import edge_tank_adversary as adv
import edge_tank_asset_server as assets


def main():
    ap = argparse.ArgumentParser(
        description="Combined TCP-assets-then-UDP-adversary server for the dual-lane tank demo.")
    # Shared / transport.
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=9000,
                    help="port shared by the TCP asset lane and UDP adversary lane (default 9000)")
    ap.add_argument("--asset-port", type=int, default=None,
                    help="TCP asset-lane port (default: --port)")
    ap.add_argument("--adv-port", type=int, default=None,
                    help="UDP adversary-lane port (default: --port)")
    ap.add_argument("--linger", type=float, default=10.0,
                    help="seconds to keep the TCP socket open after COMPLETE so the "
                         "FujiNet client can drain the final messages before FIN")
    # Phase-2 adversary options (mirror edge_tank_adversary.py).
    ap.add_argument("--count", type=int, default=3,
                    help="number of adversaries (default 3, the demo's kAdvCount)")
    ap.add_argument("--mode", choices=["chase", "avoid", "patrol"], default="chase",
                    help="fallback AI for adversaries not named in --modes (default chase)")
    ap.add_argument("--modes", default="chase,avoid,patrol",
                    help="per-adversary AI, comma list (default 'chase,avoid,patrol')")
    ap.add_argument("--hz", type=float, default=10.0, help="snapshot rate (default 10)")
    ap.add_argument("--tv", choices=["ntsc", "pal"], default="ntsc",
                    help="client TV standard, for frames-per-tick (default ntsc)")
    ap.add_argument("--speed", type=int, default=1,
                    help="adversary dead-reckon speed in steps/frame (default 1)")
    ap.add_argument("--speed-scale", type=int, default=3,
                    help="must match the demo's EDGE_TANK_SPEED_SCALE (default 3)")
    ap.add_argument("--start", default="160,96",
                    help="fallback start x,y nominal for adversaries not in --starts")
    ap.add_argument("--starts", default="40,344;624,376;624,16",
                    help="per-adversary start x,y, semicolon list (default three corner areas)")
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

    asset_port = args.asset_port if args.asset_port is not None else args.port
    adv_port = args.adv_port if args.adv_port is not None else args.port

    # Bind the UDP adversary socket ONCE, up front, and keep it across rounds so the
    # Atari's first post-handoff packets are buffered (not dropped) during the TCP
    # linger, and so each new session reuses the same lane.
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind((args.host, adv_port))
    udp.setblocking(False)
    # run_adversary_server() reads args.host/args.port; point them at the adv lane.
    args.port = adv_port

    print("dual server ready: TCP assets %s:%d  +  UDP adversaries %s:%d"
          % (args.host, asset_port, args.host, adv_port))
    try:
        while True:
            # PHASE 1 — serve exactly one asset transfer over TCP, then close listener.
            tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            tcp.bind((args.host, asset_port))
            tcp.listen(1)
            print("PHASE 1 (TCP assets): waiting for a request on %s:%d ..."
                  % (args.host, asset_port))
            conn, addr = tcp.accept()
            try:
                assets.serve_one(conn, addr, oneshot=True, linger=args.linger)
            except OSError as e:
                print("  asset transfer error: %s — back to waiting" % e)
                tcp.close()
                continue
            tcp.close()

            # PHASE 2 — stream adversaries until the client sends BYE (then loop back
            # to Phase 1) or Ctrl-C (exit). Reuses the persistent UDP socket.
            print("PHASE 2 (UDP adversaries): streaming on %s:%d ..." % (args.host, adv_port))
            if adv.run_adversary_server(args, sock=udp) == "interrupt":
                break
            print("client left — returning to wait for a new TCP asset request.")
    except KeyboardInterrupt:
        pass
    finally:
        udp.close()


if __name__ == "__main__":
    main()
