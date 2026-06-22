# Tank demo asset provenance

These binary assets are derived from the **ATank** project
(`/home/brad/Projects/Atari/atank`), authored by Brad Colbert.

| EDGE file | ATank source | Notes |
|---|---|---|
| `tank_tileset.fnt` | `data/a4color1.fnt` | 1024-byte ANTIC Mode 4 charset (128 glyphs × 8) |
| `chunk_0_0.scr` | `data/maps/tile11.scr` | center 2×2 region, top-left (ATank row 1, col 1) |
| `chunk_1_0.scr` | `data/maps/tile12.scr` | top-right (ATank row 1, col 2) |
| `chunk_0_1.scr` | `data/maps/tile21.scr` | bottom-left (ATank row 2, col 1) |
| `chunk_1_1.scr` | `data/maps/tile22.scr` | bottom-right (ATank row 2, col 2) |

ATank filenames encode `tile<row><col>`. The EDGE chunk grid uses
`chunk_<col>_<row>` (chunk_x, chunk_y). The four files above are the genuinely
adjacent **center** 2×2 quadrant of ATank's 4×4 map, chosen for its central
landmark (a zigzag chevron straddling the four-chunk intersection) and seam
features.

Each `.scr` is 960 raw Mode 4 tile codes (40×24, row-major). Tile codes range
0..90 (all < 128, so COLPF3 is unused by the map). Intended palette (from the
`.m4c` designer files, pixel-value order): COLBK=4, COLPF0=0, COLPF1=7,
COLPF2=27, COLPF3=54.

**Licensing note (requires owner confirmation):** ATank is distributed under
**GPLv3**; EDGE is **MIT**. Both are authored by Brad Colbert, who as the
copyright holder may include these original assets in EDGE under MIT. This
embedding was done at the project owner's explicit direction. If EDGE is ever
redistributed, confirm these assets are intended to be MIT-licensed here.
