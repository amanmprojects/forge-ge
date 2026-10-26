#!/usr/bin/env bash
# WebAssembly build for Forge (WebGL2 / Emscripten).
#
#   ./web/build.sh            # release build -> web/dist/ (gallery + one folder per game)
#   ./web/build.sh Debug      # debug build
#   EMSDK=/path/to/emsdk ./web/build.sh
#
# Requires the Emscripten SDK. If emcc is not on PATH the script tries
# $EMSDK/emsdk_env.sh (the usual install layout) before giving up.
set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v emcc >/dev/null 2>&1; then
    if [[ -n "${EMSDK:-}" && -f "$EMSDK/emsdk_env.sh" ]]; then
        # shellcheck disable=SC1091
        source "$EMSDK/emsdk_env.sh" >/dev/null
    elif [[ -f "$HOME/emsdk/emsdk_env.sh" ]]; then
        # shellcheck disable=SC1091
        source "$HOME/emsdk/emsdk_env.sh" >/dev/null
    else
        echo "error: emcc not found. Install the Emscripten SDK and source emsdk_env.sh," >&2
        echo "       or set EMSDK=/path/to/emsdk." >&2
        exit 1
    fi
fi

BUILD_TYPE="${1:-Release}"
BUILD_DIR="build-web"

emcmake cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "Built: web/dist/index.html (gallery) + web/dist/{platformer,breakout,snake,chess,lantern}/"
echo "Serve it with: python3 -m http.server 8000 -d web/dist"
