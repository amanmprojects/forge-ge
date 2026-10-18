// 01_hello — minimal Forge program: window, camera, shapes, text, input.
#include <forge/forge.hpp>
#include <cstdlib>

int main() {
    forge::EngineConfig cfg;
    cfg.window.title = "Forge 01_hello";
    cfg.window.width = 1280;
    cfg.window.height = 720;
    forge::Engine engine(cfg);
    if (!engine.valid()) return 1;

    forge::Camera2D cam;
    cam.zoom = 1.0f;

    forge::Font font;
    const char* paths[] = {
        "/usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (auto p : paths) {
        if (font.loadFile(p, 28.0f)) break;
    }

    forge::Texture2D tiles;
    tiles.createChecker(8, forge::Color::fromHex(0x2D2A32FF), forge::Color::fromHex(0x3A3642FF));

    forge::Vec2 player{0, 0};
    float t = 0;
    int frames = 0;

    engine.runVariable([&](float dt) {
        t += dt;
        auto& in = engine.input();
        if (in.isDown(forge::Key::Escape)) engine.quit();

        // WASD move
        forge::Vec2 mv = in.moveAxis();
        player += mv * 400.0f * dt;
        cam.follow(player, 6.0f, dt);
        cam.update(dt);
        if (in.pressed(forge::Key::Space)) cam.addTrauma(0.5f);

        forge::IVec2 vp = engine.viewport();
        forge::gl::glViewport(0, 0, vp.x, vp.y);
        forge::gl::glClearColor(0.09f, 0.09f, 0.12f, 1.0f);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);

        auto& batch = engine.batch();
        batch.begin(cam.viewProj(vp.x, vp.y));

        // tiled background quad
        for (int y = -6; y <= 6; ++y)
            for (int x = -10; x <= 10; ++x)
                batch.drawTexture(tiles, {(float)x * 64, (float)y * 64}, {64, 64});

        // player (rotating, player-controlled)
        forge::SpriteDraw d;
        d.pos = player;
        d.size = {48, 48};
        d.rotation = t * 0.8f;
        d.color = forge::Color::fromHex(0xE9C46AFF);
        batch.draw(d);

        // orbiting satellites
        for (int i = 0; i < 8; ++i) {
            float a = t * 1.5f + i * forge::TAU / 8;
            forge::Vec2 p = player + forge::Vec2{std::cos(a), std::sin(a)} * 110.0f;
            batch.drawCircle(p, 12, forge::Color::fromHex(0x2A9D8FFF), 20);
        }

        batch.drawLine({-400, 0}, {400, 0}, forge::Color::fromHex(0xE76F51FF), 2.0f);
        batch.drawRectOutline({player.x - 32, player.y - 32, 64, 64},
                              forge::Color::white(), 2.0f);

        // HUD in screen space
        batch.end();
        batch.begin(engine.defaultProj());
        if (font.valid()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "FPS: %.0f  draw calls: %d  WASD to move, SPACE to shake",
                          engine.fps(), engine.drawCalls());
            font.draw(batch, buf, {-(float)vp.x / 2 + 16, (float)vp.y / 2 - 16}, 0.75f,
                      forge::Color::white());
            // mouse position in world
            forge::Vec2 mw = cam.screenToWorld(in.mousePos(), vp.x, vp.y);
            char buf2[64];
            std::snprintf(buf2, sizeof(buf2), "mouse world: %.0f %.0f", mw.x, mw.y);
            font.draw(batch, buf2, {-(float)vp.x / 2 + 16, (float)vp.y / 2 - 48}, 0.75f,
                      forge::Color::fromHex(0x90BE6DFF));
        }
        batch.end();
        if (++frames >= 600) { // auto-exit for CI; remove for play
            if (std::getenv("CI")) engine.quit();
            frames = 0;
        }
    });
    return 0;
}
