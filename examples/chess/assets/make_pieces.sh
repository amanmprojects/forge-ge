#!/usr/bin/env bash
# Regenerates the chess piece sprites (wK.png ... bP.png, tP.png ... tK.png)
# from the SVG sources in svg/.
#
#   ./make_pieces.sh
#
# The SVGs are the piece set of amanmprojects/chess (src/pieces.js): the
# classic Cburnett design on the traditional 45x45 canvas, one file per piece
# and palette — w*/b* are the board palettes, t* the flat `tray` palette the
# captured pieces use. The game draws each sprite as one whole board square,
# so the framing here is exactly the web board's.
#
# Rasterizing needs librsvg (`rsvg-convert`) or ImageMagick built with the
# RSVG delegate; both give 256x256 RGBA PNGs on a transparent background.
#
# Piece art: Cburnett (en.wikipedia.org/wiki/User:Cburnett), CC BY-SA 3.0 /
# GFDL — keep this notice (and assets/README.md) with the files.
set -euo pipefail
cd "$(dirname "$0")"

size=256

render() { # <in.svg> <out.png>
    if command -v rsvg-convert >/dev/null 2>&1; then
        rsvg-convert -w "$size" -h "$size" "$1" -o "$2"
    else
        magick -background none "$1" -resize "${size}x${size}" "$2"
    fi
}

for f in svg/*.svg; do
    render "$f" "$(basename "${f%.svg}").png"
done
echo "wrote $(ls w?.png b?.png t?.png | wc -l) sprites (${size}x${size}) in $(pwd)"
