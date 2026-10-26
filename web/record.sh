#!/usr/bin/env bash
# Record the gallery clips: each game's autopilot, rendered offscreen one
# fixed step per frame and piped to ffmpeg (see examples/common/clip.hpp).
# Needs a desktop build, ffmpeg and a display: the games still open a window
# for their GL context.
#
#   ./web/record.sh                       # -> web/dist/clips/<game>.{mp4,jpg}
#   BUILD_DIR=out OUT=/tmp/clips ./web/record.sh
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
OUT="${OUT:-web/dist/clips}"

command -v ffmpeg >/dev/null 2>&1 || { echo "error: ffmpeg not found on PATH" >&2; exit 1; }

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)" \
    --target 02_platformer 07_breakout 08_snake 09_chess 10_lantern
mkdir -p "$OUT"

# record <name> <binary> <frames> <WxH> <poster second> [extra args...]
# The games run from the repo root: chess and lantern load their assets
# relative to it.
record() {
    local name=$1 bin=$2 frames=$3 size=$4 poster=$5
    shift 5
    "$BUILD_DIR/$bin" --record "$OUT/$name.mp4" --frames "$frames" --size "$size" "$@"
    # Poster: one frame of the clip, shown until the video loads.
    ffmpeg -y -hide_banner -loglevel error -ss "$poster" -i "$OUT/$name.mp4" \
        -frames:v 1 -q:v 3 "$OUT/$name.jpg"
}

record platformer 02_platformer  900 1280x720 4
record breakout   07_breakout   1200 1024x768 6
record snake      08_snake      1200 1024x768 12
record chess      09_chess      1500 1280x800 15 --level 4
record lantern    10_lantern     900 1280x768 5

ls -lh "$OUT"
