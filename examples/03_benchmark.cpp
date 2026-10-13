// 03_benchmark — sprite stress test. Renders N rotating sprites, prints stats.
// Exits after ~600 frames so CI can run it. Validates batch throughput.
#include <forge/forge.hpp>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    int N = argc > 1 ? std::atoi(argv[1]) : 10000;

    forge::EngineConfig cfg;
    cfg.window.title = "Forge 03_benchmark";
    cfg.window.width = 1280;
    cfg.window.height = 720;
    cfg.window.vsync = false; // measure raw throughput
    forge::Engine engine(cfg);
    if (!engine.valid()) return 1;

    forge::Texture2D tex;
    tex.createChecker(4, forge::Color::fromHex(0xE9C46AFF), forge::Color::fromHex(0x2A9D8FFF), 16);

    struct Item { forge::Vec2 p; float rot, spin; float scale; forge::Color c; };
    std::vector<Item> items((size_t)N);
    forge::Random rng{7};
    for (auto& it : items) {
        it.p = {rng.range(-900, 900), rng.range(-500, 500)};
        it.rot = rng.range(0, forge::TAU);
        it.spin = rng.range(-2, 2);
        it.scale = rng.range(8, 40);
        it.c = forge::Color{rng.nextFloat(), rng.nextFloat(), rng.nextFloat(), 1.0f};
    }

    forge::Camera2D cam;
    int frames = 0;
    double simMs = 0, drawMs = 0;

    engine.runVariable([&](float dt) {
        uint64_t t0 = SDL_GetPerformanceCounter();
        for (auto& it : items) {
            it.rot += it.spin * dt;
            it.p.y -= 20.0f * dt;
            if (it.p.y < -520) it.p.y = 520;
        }
        uint64_t t1 = SDL_GetPerformanceCounter();

        forge::IVec2 vp = engine.viewport();
        forge::gl::glViewport(0, 0, vp.x, vp.y);
        forge::gl::glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);

        auto& batch = engine.batch();
        batch.begin(cam.viewProj(vp.x, vp.y));
        for (auto& it : items) {
            forge::SpriteDraw d;
            d.tex = &tex;
            d.pos = it.p;
            d.size = {it.scale, it.scale};
            d.rotation = it.rot;
            d.color = it.c;
            batch.draw(d);
        }
        batch.end();
        uint64_t t2 = SDL_GetPerformanceCounter();
        uint64_t freq = SDL_GetPerformanceFrequency();
        simMs += (double)(t1 - t0) * 1000.0 / freq;
        drawMs += (double)(t2 - t1) * 1000.0 / freq;

        if ((++frames % 120) == 0)
            std::printf("frame %d fps=%.0f quads=%d calls=%d\n",
                        frames, engine.fps(), batch.quadsDrawn(), batch.drawCalls());
        if (frames >= 600) engine.quit();
    });

    std::printf("BENCH n=%d frames=%d avgSim=%.3fms avgDraw=%.3fms fps=%.0f\n",
                N, frames, simMs / frames, drawMs / frames, engine.fps());
    return 0;
}
