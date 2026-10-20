# Chess piece sprites

The 18 piece sprites the game loads:

- `wK.png` … `bP.png` — the board palettes (White, Black);
- `tP.png` … `tK.png` — the flat `tray` palette for the captured-piece rows.

All are 256×256 RGBA with a transparent background, and each one is **one whole
board square**: the game draws a sprite as a square box centred on the square,
exactly like the web board this set comes from. `09_chess` loads them through
`forge::AssetManager::loadTexture` (with hot reload), probing
`examples/chess/assets/`, `../examples/chess/assets/` and
`../../examples/chess/assets/` so the binary works from the repo root and from
the build directory.

## Where the art comes from

`svg/*.svg` is the piece set of
[amanmprojects/chess](https://github.com/amanmprojects/chess) (`src/pieces.js`):
the classic Cburnett design, drawn once on the traditional 45×45 chess-SVG
canvas and recoloured per palette, with no external assets.
[`make_pieces.sh`](make_pieces.sh) rasterizes those SVGs with librsvg and
ImageMagick:

```bash
./make_pieces.sh     # needs rsvg-convert or ImageMagick with the RSVG delegate
```

Piece art: Cburnett — CC BY-SA 3.0 / GFDL. The game code is MIT; the sprites and
SVGs keep their own licence, so keep this notice with them.

## Replacing them with your own art

Any 256×256 PNG works. Conventions the game relies on:

- **file names**: `wK wQ wR wB wN wP` for White, `bK bQ bR bB bN bP` for Black
  (K = king, Q = queen, R = rook, B = bishop, N = knight, P = pawn), and the
  same six letters with a `t` prefix for the captured rows (a single flat tone,
  since a row only ever holds one side's losses);
- **one square per file**: the canvas maps onto one board square, so leave a
  margin under the base — the source set stands its pieces on a shared ground
  line about 13 % above the bottom edge, art centred horizontally;
- keep the art inside the canvas (the game draws the full box, with no clipping
  cushion) and let relative heights read like a real set — the pawn is shorter
  than the king, not scaled to fill the square.

If the folder is missing, the game draws procedural silhouettes instead (same
layout, no assets needed) — so `--shot`/`CI` runs stay valid either way.
