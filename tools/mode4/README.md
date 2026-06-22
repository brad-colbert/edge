# Mode4 tooling — inspect & edit the tank demo's ANTIC-4 art

The tank demo renders in **ANTIC mode 4** (4-colour character mode: each byte is
4 pixels × 2 bits, the bit-pair selecting a colour register). The bare `.fnt`
charset and `.scr` map chunks the build consumes have no way to be *viewed* in
their real Mode 4 colours. **Mode4.exe** ("Atari Mode4 Screendesigner", at
`/home/brad/bin/Mode4.exe`) is a GUI editor that does exactly that — it just uses
its own container formats. `mode4_convert.py` bridges the two, losslessly.

## File formats (see `mode4_convert.py` for details)

| Mode4 file | = payload + 144-byte colour/DLI table |
|---|---|
| `.chs` (charset, 1168 B) | `tank_tileset.fnt` (1024 B) + table |
| `.m4c` (screen,  1104 B) | a `chunk_*.scr` (960 B, 40×24) + table |

The 144-byte table only drives the editor's colour *preview*; it does not exist
in the game build (the live palette is `demo/tank/tank_palette.h`). Wrapping only
appends it and unwrapping only strips it, so glyph/tile bytes round-trip
byte-for-byte.

## Inspect / edit workflow

```bash
# 1. Wrap the raw assets into Mode4 containers (--table tank tints the preview
#    toward tank_palette.h; --table mario is the known-good fallback).
python3 tools/mode4/mode4_convert.py wrap-chs \
    demo/tank/assets/tank_tileset.fnt /tmp/tank_mode4/tank.chs --table tank
for c in demo/tank/assets/chunk_*.scr; do
  b=$(basename "$c" .scr)
  python3 tools/mode4/mode4_convert.py wrap-m4c "$c" "/tmp/tank_mode4/$b.m4c" --table tank
done

# 2. Launch the editor (see the wine note below) and load tank.chs (+ a chunk
#    .m4c) from the "Save/load/prefs" tab. The "Characterset" tab shows/edits
#    glyphs in Mode 4 colour; repaint walls to use COLPF1/COLPF2 (bit-pairs
#    10/11) instead of only COLPF0.
setsid wine /home/brad/bin/Mode4.exe </dev/null >/dev/null 2>/dev/null &

# 3. Save from Mode4, then unwrap back into the build's raw assets.
python3 tools/mode4/mode4_convert.py unwrap-chs /tmp/tank_mode4/tank.chs \
    demo/tank/assets/tank_tileset.fnt
# (unwrap-m4c each edited chunk only if you changed the maps)

# 4. Rebuild build-atari-emb — generate_charset_header.cmake / generate_bytes_
#    header.cmake re-embed the binaries automatically; no source edits needed.
```

### wine launch gotcha

Mode4.exe is a PyInstaller one-file app. Under wine it aborts with
`Fatal Python error: init_sys_streams ... [WinError 6] Invalid handle` if its
stdio is redirected to a plain file with no console. Launch it with **valid
stdio handles** via `</dev/null >/dev/null 2>/dev/null` (and `setsid` for its own
session). First start is slow (~5–10 s) while it extracts to
`~/.wine/drive_c/users/$USER/Temp/_MEI*`; the window titled "Atari Mode4
Screendesigner 1.0" (WM_CLASS `mode4.exe.mode4.exe`) then appears on `DISPLAY=:0`.
Tear down with `wineserver -k` (never `pkill -f`).

## Round-trip self-check

```bash
python3 tools/mode4/mode4_convert.py wrap-chs   demo/tank/assets/tank_tileset.fnt /tmp/a.chs
python3 tools/mode4/mode4_convert.py unwrap-chs /tmp/a.chs /tmp/a.fnt
cmp demo/tank/assets/tank_tileset.fnt /tmp/a.fnt   # must be silent (identical)
```
