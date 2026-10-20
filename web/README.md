# Web build (WebAssembly / WebGL2)

`09_chess` also builds for the browser: Emscripten compiles the engine and the
game to WASM, SDL3 comes from Emscripten's port, and the OpenGL 3.3 core path
maps onto WebGL2 (OpenGL ES 3.0). There is no plugin, no canvas shim and no
JavaScript glue beyond the generated loader.

## Build

```bash
git clone https://github.com/emscripten-core/emsdk ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest   # once

./web/build.sh                 # Release -> web/dist/{index.html,index.js,index.wasm,index.data}
./web/build.sh Debug           # keep assertions (bigger)
BUILD_DIR=out ./web/build.sh   # build elsewhere than build-web/
```

`web/build.sh` sources `$EMSDK/emsdk_env.sh` (or `~/emsdk/emsdk_env.sh`) when
`emcc` is not already on `PATH`, configures a CMake build with Emscripten's
toolchain and builds the examples. Only `09_chess` has a page: it lands in
`web/dist/` as `index.html` + friends, while the other examples' wasm binaries
stay in `build-web/`. Tests are off - they are headless desktop binaries.

## Run and deploy

```bash
python3 -m http.server 8000 -d web/dist    # then open http://localhost:8000/

cd web/dist                                # any static host works; Vercel below
vercel link --project forge-chess          # once - writes web/dist/.vercel/
vercel deploy --prod                       # -> https://forge-chess-opal.vercel.app
```

`web/dist/` is a build output and is gitignored, so the linked `.vercel/` folder
stays local. The deployment is static (no build step, no environment): Vercel
uploads the four files and serves `index.wasm` as `application/wasm`. No
cross-origin isolation headers are needed (see *Threads* below), and the page
embeds everything it loads — sprites, font, shaders — so it also works from
`file://` except for the fetch of `index.wasm`, which browsers only allow over
HTTP.

## What the port changes

Everything web-specific is behind `FORGE_PLATFORM_WEB`
(`include/forge/core/platform.hpp`); the desktop build compiles the same
sources unchanged.

| Area | Desktop | Web |
|---|---|---|
| GL loader | `SDL_GL_GetProcAddress` | GL is linked into the module, symbols bound directly |
| Shaders | `#version 330 core` | `#version 300 es` + ES precision (same bodies) |
| Context | GL 3.3 core | GL ES 3.0 (WebGL2) |
| Frame loop | `while (!quitting)` | `emscripten_set_main_loop_arg` driven by rAF |
| Threads | bot on a `std::thread` | none - the bot searches inline, paced by the game |
| Sound | miniaudio (ALSA/PulseAudio) | miniaudio's WebAudio backend |

Because `emscripten_set_main_loop(..., simulate_infinite_loop = 1)` unwinds
`main()`'s stack and keeps running from the browser callback, the game state
must outlive `main()`. `examples/chess/main.cpp` allocates the engine and the
game on the heap for the web build for exactly that reason; a desktop-style
`Engine engine;` local would be dead memory by the first frame. If you port
another example, put its state on the heap (or in globals) the same way.

Threads are deliberately unused: pthreads in the browser need `SharedArrayBuffer`
plus `Cross-Origin-Opener-Policy`/`Cross-Origin-Embedder-Policy` headers, which
would make the page un-embeddable and fail on hosts that cannot set headers. The
inline bot search is bounded by nodes (the wall-clock budget is switched off),
so a move never blocks the page for more than a few dozen milliseconds.

## Assets

`--preload-file examples/chess/assets@examples/chess/assets` packs the whole
example asset folder into `index.data`, so the load paths the game already
probes (`examples/chess/assets/...`) work unchanged in the browser's virtual
filesystem. That includes `assets/fonts/DejaVuSansMono-Bold.ttf` - browsers
have no system fonts, so the HUD would otherwise fall back to shape-only text.
