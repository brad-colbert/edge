# fujinet_session_validate

Minimal Stage 8H runtime validation demo for the optional fujinet-lib session lane.

This is a validation tool, not a gameplay sample.

## What It Exercises

- `Game::net.session.connect_tcp(host, port)`
- one small `send_bytes("ping")`
- bounded `poll()` + `recv()` loop
- on-screen status codes:
  - connect result
  - send result
  - read result
  - `last_error.status`
  - `last_error.detail`

Realtime lane is intentionally not used.

## Build Gating

This demo opens a TCP connection over the `N:` device, so it must link
`libfujinet.a` built from the `llvm_changes` branch of
<https://github.com/brad-colbert/fujinet-lib-llvm>. Upstream fujinet-lib is CC65
and will not link under llvm-mos.

It is built only when all of the following are true:

- `EDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON`
- valid `EDGE_FUJINETLIB_INCLUDE_DIR` and `EDGE_FUJINETLIB_LIBRARY` are set
  (both are derived for you from `EDGE_FUJINETLIB_ROOT`)
- Atari/llvm-mos build path is active (same gating style as existing FujiNet demo rules)

Default OFF builds remain unchanged.

## Host/Port Configuration

Set via compile definitions. CMake always supplies both, defaulting to the cache
variables `EDGE_FUJINET_VALIDATE_HOST` (`192.168.1.205`) and
`EDGE_FUJINET_VALIDATE_PORT` (`9000`); the source falls back to `127.0.0.1:9000`
only when the demo is compiled outside CMake.

In CMake, pass:

```sh
-DEDGE_FUJINET_VALIDATE_HOST=192.168.1.50 -DEDGE_FUJINET_VALIDATE_PORT=9000
```

The build injects these as preprocessor definitions. The running demo prints the
host and port it actually compiled in, so a mismatch is visible on screen.

## Expected Server

Any simple TCP echo-style server is enough. For example:

- listen on configured host/port
- accept one client
- read `ping`
- optionally reply with a small payload (a few bytes)

## Runtime Observations To Record

- connect status and detail
- send status and detail
- read status over bounded polling window
- last error status/detail evolution
- whether send appears to stall frame progression (blocking risk)
- behavior when server is absent, refuses connect, or closes after accept

## Notes

`session.send_bytes()` currently uses fujinet-lib `network_write` on this path.
`network_write` may block for a FujiNet/CIO transaction, so keep writes small and
avoid using this path for timing-critical realtime traffic.
