# Web build (WebAssembly / WebGL2)

Five games build for the browser: `02_platformer`, `07_breakout`, `08_snake`,
`09_chess` and `10_lantern`. Emscripten compiles the engine and each game to
WASM, SDL3 comes from Emscripten's port, and the OpenGL 3.3 core path maps onto
WebGL2 (OpenGL ES 3.0). There is no plugin, no canvas shim and no JavaScript
glue beyond the generated loader. The published site is
<https://amanmprojects.github.io/forge-ge/>.

## Build

```bash
git clone https://github.com/emscripten-core/emsdk ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest   # once

./web/build.sh                 # Release -> web/dist/
./web/build.sh Debug           # keep assertions (bigger)
```

`web/build.sh` sources `$EMSDK/emsdk_env.sh` (or `~/emsdk/emsdk_env.sh`) when
`emcc` is not already on `PATH`, configures a CMake build with Emscripten's
toolchain in `build-web/` and builds the examples. The output is a static site:

```
web/dist/index.html                 gallery (copied from web/index.html)
web/dist/<game>/index.{html,js,wasm[,data]}
web/dist/clips/<game>.{mp4,jpg}     from web/record.sh (not part of the build)
```

with `<game>` one of `platformer`, `breakout`, `snake`, `chess`, `lantern`.
Every page is [`shell.html.in`](shell.html.in) with the game's title, controls
and aspect ratio filled in by `forge_web_page()` in `CMakeLists.txt`; add a game
by giving it a heap-owned main loop (below) and one `forge_web_page()` call. The
other examples' wasm binaries stay in `build-web/`. Tests are off - they are
headless desktop binaries.

## Clips

```bash
./web/record.sh                # desktop Release build in build/ -> web/dist/clips/
```

Each game's `--record FILE --frames N --size WxH` runs its autopilot one fixed
1/60 s step per frame, renders every frame into an offscreen `Framebuffer` and
pipes the pixels to `ffmpeg` (H.264, yuv420p, faststart), so the clips are
smooth and identical run to run whatever the machine's speed. The helper is
[`examples/common/clip.hpp`](../examples/common/clip.hpp). The games still open
a window for the GL context, so this needs a display (or `xvfb-run`). Clips
carry no audio.

## Run and deploy

```bash
python3 -m http.server 8000 -d web/dist    # then open http://localhost:8000/
./web/deploy.sh                            # web/dist -> gh-pages on origin
```

`web/deploy.sh` copies `web/dist` into a fresh single-commit `gh-pages` branch
(plus `.nojekyll`) and force-pushes it, so old wasm and clips never accumulate
in the repository's history; GitHub Pages serves that branch as-is, `.wasm` as
`application/wasm`. Any static host works: no cross-origin isolation headers
are needed (see *Threads* below), and nothing is fetched from another origin.
Opening the files from `file://` fails only on the fetch of `index.wasm`,
which browsers allow over HTTP alone.

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
| C++ exceptions | native | `-fwasm-exceptions` (`forge::script` unwinds with them) |

Because `emscripten_set_main_loop(..., simulate_infinite_loop = 1)` unwinds
`main()`'s stack and keeps running from the browser callback, the game state
must outlive `main()`. Every web game makes the engine and the game function-
local statics for exactly that reason; a desktop-style `Engine engine;` local
would be dead memory by the first frame. Heap owners are no better: with
`-fwasm-exceptions` the unwind runs the destructors of `main()`'s locals, so a
`std::unique_ptr` frees the game under the running loop. Statics are never
destroyed (the runtime never exits). The loop lambdas capture nothing for the
same reason: a frame counter or the parsed arguments captured by reference
would be read from a dead stack frame.

`Engine::quit()` is a no-op on the web: there is nothing to quit to, and a
cancelled loop is a frozen canvas, so the games' Q/ESC-to-quit keys do nothing
there.

Threads are deliberately unused: pthreads in the browser need `SharedArrayBuffer`
plus `Cross-Origin-Opener-Policy`/`Cross-Origin-Embedder-Policy` headers, which
would make the page un-embeddable and fail on hosts that cannot set headers. The
inline bot search is bounded by nodes (the wall-clock budget is switched off),
so a move never blocks the page for more than a few dozen milliseconds.

## Assets

Browsers have no filesystem fonts or working directory, so each page preloads
what its game probes into Emscripten's virtual filesystem (`index.data`), at the
path the desktop build would find it:

| Game | Preloaded |
|---|---|
| chess | `examples/chess/assets/` at `examples/chess/assets/` (sprites, SVGs, font) |
| breakout, snake | DejaVu Sans Mono Bold at `/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf` |
| lantern | the same font at `/usr/share/fonts/TTF/DejaVuSansMono.ttf`, and `assets/lantern.fgs` |
| platformer | nothing (it draws no text) |

So the load paths in the game code work unchanged. Lantern's F5/F9 saves write
to the in-memory filesystem and are gone on reload.
