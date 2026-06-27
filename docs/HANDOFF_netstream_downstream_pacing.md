# Handoff: Netstream downstream pacing / growing delay

**Date:** 2026-06-27 · **Status:** EDGE-side mitigation landed (packet packing); the
**root fix is expected in fujinet-firmware** and is the subject of this handoff.

**Repro pinned at:** EDGE repo `/home/brad/Dropbox/Projects/Atari/edge`, branch
`uber_tank_net_demo` @ `b0805a0` (the packing + seq-echo instrument) — rebuild the demo
and run the server from there to reproduce the `lag=` measurement below.

## TL;DR

The tank_net demo streams small UDP "netstream" packets bidirectionally through
FujiNet over NetSIO. **Downstream (server→Atari) packets accumulate unbounded delay**
when the rate is high; the link's raw baud is not the limit. Evidence points at the
firmware's `pace_to_atari()` byte pacing and the blocking SIO service loop in
`fujinet-firmware`. The EDGE demo now packs all adversaries into one packet/tick to
stay under the ceiling, but a fresh effort should fix the pacing in the firmware so the
lane sustains higher downstream rates.

## The problem & how it was measured

- **Demo:** `demo/tank_net` (EDGE repo). 1 local tank + 3 server-driven "adversary"
  tanks. A Python server (`tools/net/edge_tank_adversary.py`) streams authoritative
  adversary STATE at ~10 Hz; the Atari streams its player state back.
- **Transport chain:** Altirra (Atari emu) ⇄ `netsio.atdevice` ⇄ **netsiohub**
  (`python -m netsiohub`, UDP 9997) ⇄ **fujinet-pc firmware** ⇄ real UDP socket ⇄
  Python server (run as a Docker peer at `172.30.0.2:9000`, separate IP so it doesn't
  collide with the firmware's own `:9000` bind).
- **Instrument:** the C→S player packet echoes back the last adversary sequence the
  Atari has applied + an RX-overflow counter; the server computes
  `lag = seq_delta(sent_seq, echoed_seq)` (in packets) and prints it. See
  `tools/net/edge_tank_adversary.py` (`--decode --stats-interval`) and the echo fields
  in `demo/tank_net/adversary_net.h`.

### Findings (Altirra + NetSIO + fujinet-pc, emulator)

| Downstream rate | Lag over a ~50 s run |
|---|---|
| 30 pkt/s (3 adv × 10 Hz, one packet each) | grows without bound → **953** packets |
| 6 pkt/s (3 adv × 2 Hz) | **flat at 0** (max 1) |

- **`ovf=0` throughout** ⇒ the EDGE engine RX ring (8×packet, drop-oldest) never
  overflowed, so the backlog accumulates **below the engine** — in the FujiNet/NetSIO
  inbound→SIO path.
- **Baud is not the limit.** The lane runs `kNetstreamNominalBaud=31250` → AUDF3=21 →
  **~31960 bps** (`1789790/(2*(AUDF3+7))`, confirmed by Altirra `NetSIO to Atari @
  31960`). That carries ~3100 B/s; the 30-pkt/s load is ~480 B/s, ~5× headroom. A host
  test now guards 31250→AUDF3=21 (`tests/backends/atari/test_netstream_init_prepare.cpp`).
- Conclusion: the limit is **per-packet / pacing**, not bandwidth. Likely substantially
  an emulator NetSIO external-clock characteristic, but the *unbounded growth* is a real
  producer/consumer mismatch worth fixing in the firmware.

## Firmware suspects (fujinet-firmware: `/home/brad/Projects.local/fujinet-firmware`)

Primary file: **`lib/device/sio/netstream.cpp`** (class `sioNetStream`; header
`netstream.h`). Concurrent/streaming SIO ("Mode B"), enabled by Fuji command `0xF0`
(`FUJICMD_ENABLE_UDPSTREAM`, `include/fujiCommandID.h`).

1. **`pace_to_atari(min_gap_us)`** (≈ netstream.cpp:112-128) feeds the Atari **one byte
   at a time with a fixed inter-byte gap** and caps at **16 bytes per call**
   (`send_count < 16`). Gaps: `NETSTREAM_MIN_GAP_US_SIO=520µs` (~19.2 k),
   `NETSTREAM_MIN_GAP_US_MIDI=320µs` (~31.25 k). **Check the gap actually matches the
   negotiated 31250 baud** — a 520µs gap throttles to ~1923 B/s regardless of the real
   line rate, and the 16-byte cap bounds burst drain.
2. **`sio_handle_netstream()`** (≈ netstream.cpp:217-410) is entered from the SIO
   service loop (`lib/bus/sio/sio.cpp` `systemBus::service()`, which does `return;`
   after dispatching to it) and **blocks the whole loop** while active. Its inbound
   branch (`pace_to_atari`) competes with the outbound Atari→Net branch
   (`SYSTEM_BUS.available()` handling); under bidirectional traffic the outbound branch
   can **starve** `pace_to_atari`, so the 2048-byte `rx_ring` (drop-oldest) backs up.
3. **`rx_ring[NETSTREAM_RX_RING_SIZE=2048]`** is the inbound queue; on overflow it drops
   the oldest byte. Watch `rx_drop_count`.
4. **NetSIO transport:** `lib/bus/sio/NetSIO.cpp` (UDP to netsiohub :9997); baud changes
   via `NETSIO_SPEED_CHANGE`. Credit/flow-control messages exist
   (`NETSIO_CREDIT_*`) — worth checking whether they pace downstream delivery.

### Candidate fixes to evaluate (firmware)
- Make the `pace_to_atari` inter-byte gap track the **negotiated baud** (31250), not a
  fixed 19.2 k assumption.
- **Interleave** `pace_to_atari` fairly with the outbound branch so upstream traffic
  can't starve inbound draining (or drain inbound to empty before/while batching out).
- Raise or remove the **16-byte-per-call** cap so a backlog can catch up.
- Sanity-check that nothing re-paces or re-frames each UDP datagram (per-datagram
  overhead would explain why *packet count*, not bytes, drove the lag).

## Reproduce & rebuild

**Stack (leave the user's running stack alone where possible):**
- netsiohub: `python -m netsiohub` (UDP 9997) — usually already running.
- fujinet-pc: `cd build/dist && ./run-fujinet` (TCP 8000; reconnects to netsiohub).
  ⚠️ **Never `fuser -k 9000/udp`** — the firmware binds `:9000` for the netstream peer
  and it shares the port; killing it takes the firmware down.
- Server peer (Docker, separate IP):
  ```
  docker network create --subnet 172.30.0.0/24 --gateway 172.30.0.1 nspeer   # once
  docker run -d --name adv-srv --network nspeer --ip 172.30.0.2 \
    -v <edge>/tools/net:/app:ro python:3-slim \
    python3 -u /app/edge_tank_adversary.py --host 0.0.0.0 --port 9000 \
    --hz 10 --stats-interval 0.5 --decode
  docker logs -f adv-srv          # watch the lag= trend
  ```
- Demo (EDGE repo): build with the realtime adapter ON, peer = the Docker IP:
  ```
  cmake -S . -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake \
    -DEDGE_BUILD_DEMO=ON -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON \
    -DEDGE_NET_PEER_HOST=172.30.0.2 -DEDGE_NET_PEER_PORT=9000
  cmake --build build-atari --target atari_tank_net_demo
  ```
  Launch headless in Altirra (Mode B keeps the netsio device):
  ```
  cd <Altirra dir> && wine Altirra64.exe /ntsc /nobasic /tempprofile \
    /debug /debugbrkrun /debugcmd:g /run Z:\<path>\atari_tank_net_demo.xex
  ```
  Tear down with `wineserver -k` + `docker rm -f adv-srv` (don't touch fujinet).

**Firmware rebuild:** `cd /home/brad/Projects.local/fujinet-firmware && ./build.sh -p`
(PC build) → `build/dist/fujinet` + `run-fujinet`. (See `fujinet_pc.cmake`,
`build.sh`.) ESP32 differs; the PC path uses `lib/hardware/TTYChannel` / NetSIO.

**The measurement tool is the seq-echo server** above — a fix should turn the
`lag=` trend from "rises without bound at high `--hz`" into "stays flat." Sweep
`--hz 10/20/30` to find the new ceiling after a firmware change.

## EDGE-side mitigation already landed (context, not the fix)

To keep the demo usable now, EDGE packs all adversaries into **one 32-byte packet per
tick** (10 pkt/s instead of 30) — see `demo/tank_net/adversary_net.h` (`TankPacket32`),
the per-demo `GameConfig::realtime_packet_bytes`, and the relaxed Atari HAL size guard
(`engine/platform/atari/fujinet_netstream_realtime.h`). This reduces downstream packet
count and total bytes but does **not** raise the firmware's per-byte pacing ceiling —
which is what this handoff is about.

## Throughput results AFTER the firmware fix (2026-06-27)

Re-ran the seq-echo sweep against the updated firmware (combined 32-byte packet, so
downstream pkt/s = `--hz`). All runs held `tx` at the full rate, `ovf=0`, no
`bad`/`stale`:

| `--hz` | downstream | steady-state lag | behavior |
|---|---|---|---|
| 10 | 10 pkt/s, ~320 B/s | **~1 pkt (~100 ms)**, flat | buffer stays shallow ✅ |
| 30 | 30 pkt/s, ~960 B/s | ~75 pkt (~2.5 s), **bounded** | inbound buffers full |
| 60 | 60 pkt/s, ~1920 B/s | ~90 pkt (~1.5 s), **bounded** | inbound buffers full |

**Win — runaway eliminated.** The 30 pkt/s case that previously grew *unbounded to
953* now **plateaus and stays bounded**, even pushed to 60 pkt/s / ~1920 B/s. The lane
no longer falls behind without limit.

**Remaining issue — bufferbloat at high rates.** The plateau is a roughly *fixed packet
depth* (~75–90 packets ≈ the firmware `rx_ring` + engine/NS buffers running FULL), not
a rate-dependent latency: note the time-latency is *lower* at hz=60 (1.5 s) than hz=30
(2.5 s) for the same ~80-packet depth. So at high rates the inbound buffers sit full,
adding constant latency. The demo's real operating rate (10 Hz) is unaffected — flat
~100 ms.

**Next firmware lead (optional, gated on whether low-latency-at-high-rate matters):**
keep the inbound buffer **shallow** instead of letting it fill — drain `rx_ring` to the
freshest bytes each service pass, shrink `NETSTREAM_RX_RING_SIZE`, or drop-oldest
sooner. The goal is to turn the hz=30/60 "bounded but ~2 s" into "bounded and small."

## Receive path / backpressure (the Atari is NOT the limiter)

A natural question is whether the Atari reads too slowly and causes the backpressure.
It does not. The EDGE receive path has two stages, neither gated at the 10 Hz
send rate:

1. **Wire → NS input ring: IRQ-driven, per byte.** `SerialInputIrqHandler` (VSERIN
   `$020A`, in `fujinet_netstream_handler.S`) pulls every byte off POKEY SERIN the
   instant it arrives, asynchronously — already as-fast-as-possible, independent of the
   main loop.
2. **NS ring → app: once per frame (~60 Hz NTSC), full drain.** `frame_step` calls
   `poll()` then `while (recv(pkt))`; both drain to `WouldBlock`, so every frame empties
   the 128-byte NS input ring into the engine ring and then the engine ring into the app.

So the Atari accepts bytes at **full wire speed and applies no backpressure on the
wire** — it pulls everything the firmware sends, when it sends it. The firmware
`rx_ring` fills purely because UDP-in > `pace_to_atari`-out, and that pace is a *fixed
timer* (520 µs/byte) **upstream of the SIO wire**. The Atari is downstream of the pace,
emits no credit upstream, and so cannot drain that buffer faster. "Read faster to
relieve backpressure" is a credit/flow-control idea; this link is timer-paced, not
demand-paced — hence the fix is firmware-side.

**The one read-side risk — only under a future fast firmware.** The main-loop drain
just has to keep the **128-byte NS input ring** (4 × 32-byte frames) from overflowing
between 60 Hz polls; if it overflows, the RX IRQ drops bytes → **byte misalignment** →
the fixed-32-byte deframer desyncs (see "Drop alignment" — the EDGE Atari receiver has
no resync marker). At the current pace (~32 B ≈ 1 frame per 16.6 ms) there is ~4×
margin, so no risk today. **But if a firmware fix makes downstream much faster or
burstier, ensure a burst cannot exceed ~128 bytes between the Atari's 60 Hz drains** —
otherwise pace the burst, enlarge the Atari NS ring, or poll more than once per frame on
the EDGE side.

## Bufferbloat fix VERIFIED (frame-aligned drop-oldest + `netstream_rx_depth`, 2026-06-27)

The firmware was changed to drop **whole 32-byte frames** (not bytes) when the inbound
ring is full, with a configurable depth `netstream_rx_depth` (`[Network]` in
`fnconfig.ini`; `0`/absent = firmware default `NETSTREAM_RX_MAX_FRAMES=4`, min 2). The
seq-echo sweep confirms it — every cell `ovf=0 bad=0 stale=0`, `resync` flat at 8 B (the
one-time startup sync), i.e. the deframer never desynced even while dropping frames:

| `--hz` | pre-fix (bufferbloat) | depth=4 | depth=2 |
|---|---|---|---|
| 10 | ~1 / ~100 ms | ~1 / ~100 ms | ~1 / ~100 ms |
| 20 | — | ~10 / ~500 ms | ~9 / ~450 ms |
| 30 | ~75 / **~2.5 s** | ~15 / ~500 ms | ~13 / ~430 ms |
| 60 | ~90 / **~1.5 s** | ~26 / ~430 ms | ~26 / ~430 ms |

- **Runaway → bounded → shallow.** The deep ~2.5 s buffer at 30 Hz collapsed to ~0.5 s;
  60 Hz from ~1.5 s to ~0.43 s. The demo's real 10 Hz operating point is ~100 ms.
- **Frame-aligned drops verified at the shallowest depth (2).** No corruption signal at
  any rate — confirms the whole-datagram drop policy (the EDGE Atari receiver has no
  resync marker, so byte-level drops would have been fatal; see "Drop alignment").
- **`depth=2` is marginally tighter than 4** (peak lag −2-3 pkt at hz=20/30, ~flat at
  60). Diminishing returns below ~4 because the residual high-rate lag is dominated by
  the **measurement round-trip** (echo returns on the Atari's 10 Hz player TX + wire
  RTT, ≈ ~17-26 "packets" at 60 pkt/s), not `rx_ring` depth. The round-trip is the
  floor; buffer depth can't go below it.

**Status: resolved.** `netstream_rx_depth=2` is a safe tightest-latency default; `4` is
equally fine for the demo (10 Hz is ~100 ms either way).
