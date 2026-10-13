// 06_verify — renders a known scene, reads back pixels, checks correctness.
// Exit 0 = rendering pipeline verified. For CI with a display.
#include <forge/forge.hpp>
#include <cstdio>
#include <vector>

static forge::Color readPixel(int x, int y) {
    uint8_t px[4] = {};
    forge::gl::glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return forge::Color::fromBytes(px[0], px[1], px[2], px[3]);
}

int main() {
    forge::EngineConfig cfg;
    cfg.window.title = "Forge verify";
    cfg.window.width = 400;
    cfg.window.height = 300;
    cfg.window.vsync = false;
    forge::Engine engine(cfg);
    if (!engine.valid()) return 1;

    int fails = 0;
    auto expect = [&](const char* name, forge::Color got, forge::Color want, float tol = 0.05f) {
        bool ok = std::fabs(got.r - want.r) < tol && std::fabs(got.g - want.g) < tol &&
                  std::fabs(got.b - want.b) < tol;
        std::printf("%s: got (%.2f %.2f %.2f) want (%.2f %.2f %.2f) %s\n", name,
                    got.r, got.g, got.b, want.r, want.g, want.b, ok ? "OK" : "FAIL");
        if (!ok) ++fails;
    };

    engine.runVariable([&](float dt) {
        (void)dt;
        forge::IVec2 vp = engine.viewport();
        forge::gl::glViewport(0, 0, vp.x, vp.y);
        forge::gl::glClearColor(0, 0, 0, 1);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);

        auto& batch = engine.batch();
        batch.begin(engine.defaultProj());
        float cx = 0, cy = 0;
        // solid red square at center (200x200 world px)
        batch.drawRect({cx - 100, cy - 100, 200, 200}, forge::Color::red());
        // green square offset right, overlapping edge
        batch.drawRect({cx + 60, cy - 25, 50, 50}, forge::Color::green());
        batch.end();

        // readback: viewport center in pixels
        int px = vp.x / 2, py = vp.y / 2;
        expect("center-red", readPixel(px, py), forge::Color::red());
        expect("green-offset", readPixel(px + 70, py), forge::Color::green());
        expect("corner-black", readPixel(5, 5), forge::Color::black());
        expect("calls==1", forge::Color{(float)engine.drawCalls(), 0, 0},
               forge::Color{1, 0, 0}); // 1 draw call
        engine.quit();
    });

    std::printf(fails == 0 ? "RENDER VERIFIED\n" : "RENDER BROKEN (%d)\n", fails);
    return fails == 0 ? 0 : 1;
}
