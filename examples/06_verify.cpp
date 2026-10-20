// 06_verify — renders a known scene, reads back pixels, checks correctness.
// Exit 0 = rendering pipeline verified. For CI with a display.
//
// Covers: basic batching + render-target tracking, scissor clipping, MSAA
// resolve, the post-process fullscreen pass, the 2D lighting composite, and
// UTF-8 text (when a system font is available).
#include <forge/forge.hpp>
#include <cstdio>
#include <cstring>
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

    // Un-ended batch must flush into the framebuffer it was drawn for, not the
    // one bound at the next begin() (quads used to leak onto the screen).
    forge::Framebuffer fbo;
    if (!fbo.create(400, 300)) { std::printf("FBO create failed\n"); return 1; }

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

        // --- render-target tracking: begin() without end() ---
        fbo.bind();
        forge::gl::glViewport(0, 0, fbo.width(), fbo.height());
        forge::gl::glClearColor(0, 0, 0, 1);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        batch.begin(engine.defaultProj());
        batch.drawRect({-100, -100, 200, 200}, forge::Color::red()); // leaked quad
        // no end() on purpose: unbind and start a new batch for the screen
        forge::Framebuffer::unbind(vp.x, vp.y);
        forge::gl::glClearColor(0, 0, 0, 1);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        batch.begin(engine.defaultProj());
        batch.drawRect({-100, -100, 200, 200}, forge::Color::green());
        batch.end();

        expect("screen-green", readPixel(px, py), forge::Color::green());
        fbo.bind(); // FBO read buffer defaults to GL_COLOR_ATTACHMENT0
        int fpx = fbo.width() / 2, fpy = fbo.height() / 2;
        expect("fbo-red", readPixel(fpx, fpy), forge::Color::red());
        forge::Framebuffer::unbind(vp.x, vp.y);

        // --- scissor clipping ---
        // The scissor rect is in framebuffer pixels, bottom-left origin.
        forge::gl::glClearColor(0, 0, 0, 1);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        batch.begin(engine.defaultProj());
        batch.setScissor({100, 100, 100, 100});
        batch.drawRect({-400, -400, 800, 800}, forge::Color::red());
        batch.end();
        expect("scissor-inside", readPixel(150, 150), forge::Color::red());
        expect("scissor-outside", readPixel(50, 50), forge::Color::black());
        // end() must have disabled the scissor again.
        forge::gl::glClearColor(0, 0, 0, 1);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        batch.begin(engine.defaultProj());
        batch.drawRect({-400, -400, 800, 800}, forge::Color::green());
        batch.end();
        expect("scissor-reset", readPixel(150, 150), forge::Color::green());

        // --- blend modes ---
        // Additive: red + green over black must reach yellow (alpha blending
        // would leave green on top).
        forge::gl::glClearColor(0, 0, 0, 1);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        batch.begin(engine.defaultProj());
        batch.setBlendMode(forge::SpriteBatch::BlendMode::Additive);
        batch.drawRect({-100, -100, 200, 200}, forge::Color::red());
        batch.drawRect({-100, -100, 200, 200}, forge::Color::green());
        batch.end();
        expect("blend-additive", readPixel(px, py), forge::Color{1, 1, 0, 1}, 0.08f);
        // end() restores alpha blending; painting over red must show green.
        forge::gl::glClearColor(0, 0, 0, 1);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        batch.begin(engine.defaultProj());
        batch.drawRect({-100, -100, 200, 200}, forge::Color::red());
        batch.drawRect({-100, -100, 200, 200}, forge::Color::green());
        batch.end();
        expect("blend-reset", readPixel(px, py), forge::Color::green());
        if (batch.blendMode() != forge::SpriteBatch::BlendMode::Alpha)
            std::printf("blend-mode-not-reset: FAIL\n");

        // --- MSAA resolve ---
        forge::Framebuffer msaa;
        if (msaa.create(64, 64, 4) && msaa.multisampled()) {
            msaa.bind();
            forge::gl::glClearColor(0, 0, 0, 1);
            forge::gl::glClear(GL_COLOR_BUFFER_BIT);
            batch.begin(forge::Mat4::ortho(-32, 32, -32, 32));
            batch.drawRect({-16, -16, 32, 32}, forge::Color::blue());
            batch.end();
            std::vector<uint8_t> px;
            bool ok = msaa.readPixels(px);
            forge::Color center = ok ? forge::Color::fromBytes(
                px[((size_t)32 * 64 + 32) * 4 + 0], px[((size_t)32 * 64 + 32) * 4 + 1],
                px[((size_t)32 * 64 + 32) * 4 + 2], px[((size_t)32 * 64 + 32) * 4 + 3])
                : forge::Color::black();
            expect("msaa-resolve", center, forge::Color::blue());
            // resize() keeps the target valid.
            ok = msaa.resize(80, 48) && msaa.valid() && msaa.width() == 80 && msaa.height() == 48;
            expect("msaa-resize", ok ? forge::Color::white() : forge::Color::black(),
                   forge::Color::white());
        }
        forge::Framebuffer::unbind(vp.x, vp.y);

        // --- post-process pass ---
        forge::Framebuffer postFbo;
        if (postFbo.create(vp.x, vp.y)) {
            postFbo.bind();
            forge::gl::glClearColor(0, 0, 0, 1);
            forge::gl::glClear(GL_COLOR_BUFFER_BIT);
            batch.begin(engine.defaultProj());
            batch.drawRect({-200, -150, 400, 300}, forge::Color::white());
            batch.end();
            postFbo.resolve();
            forge::Framebuffer::unbind(vp.x, vp.y);
            forge::Texture2D view;
            view.adoptRaw(postFbo.texture(), postFbo.width(), postFbo.height(), false);
            forge::PostProcess post;
            if (post.init(forge::PostProcess::Effect::Vignette)) {
                post.setVignette(0.9f, 0.45f);
                post.setScanlines(0.0f); // keep the center pure for the check
                post.apply(view, vp);
                forge::Color mid = readPixel(vp.x / 2, vp.y / 2);
                forge::Color corner = readPixel(4, 4);
                expect("post-center-bright", mid, forge::Color::white(), 0.25f);
                bool darker = corner.r < mid.r - 0.1f;
                expect("post-vignette", darker ? forge::Color::white() : forge::Color::black(),
                       forge::Color::white());

                // Orientation: a bottom-half-only source must stay at the
                // bottom (catches a mirrored fullscreen pass).
                postFbo.bind();
                forge::gl::glClearColor(0, 0, 0, 1);
                forge::gl::glClear(GL_COLOR_BUFFER_BIT);
                batch.begin(engine.defaultProj());
                batch.drawRect({-200, -150, 400, 150}, forge::Color::white());
                batch.end();
                postFbo.resolve();
                forge::Framebuffer::unbind(vp.x, vp.y);
                post.apply(view, vp);
                forge::Color low = readPixel(vp.x / 2, vp.y / 2 - 60);
                forge::Color high = readPixel(vp.x / 2, vp.y / 2 + 60);
                expect("post-orientation",
                       (low.r > 0.5f && high.r < 0.3f) ? forge::Color::white()
                                                       : forge::Color::black(),
                       forge::Color::white());
            }
        }
        forge::Framebuffer::unbind(vp.x, vp.y);

        // --- 2D lighting composite ---
        forge::Framebuffer lit;
        if (lit.create(vp.x, vp.y)) {
            lit.bind();
            forge::gl::glClearColor(0, 0, 0, 1);
            forge::gl::glClear(GL_COLOR_BUFFER_BIT);
            batch.begin(engine.defaultProj());
            batch.drawRect({-200, -150, 400, 300}, forge::Color::white());
            batch.end();
            forge::LightingSystem lights;
            if (lights.init(vp.x, vp.y)) {
                lights.setAmbient(forge::Color::black());
                forge::Light2D light;
                light.pos = {0, 0};
                light.radius = 90.0f;
                light.color = forge::Color::white();
                light.intensity = 1.0f;
                lights.addLight(light);
                forge::Camera2D cam;
                cam.position = {0, 0};
                lights.render(batch, cam, vp, &lit);
                lit.resolve();
                std::vector<uint8_t> px;
                bool ok = lit.readPixels(px);
                auto at = [&](int x, int y) {
                    size_t i = ((size_t)y * (size_t)vp.x + (size_t)x) * 4;
                    return ok ? forge::Color::fromBytes(px[i], px[i + 1], px[i + 2], px[i + 3])
                              : forge::Color::black();
                };
                forge::Color near = at(vp.x / 2, vp.y / 2);
                forge::Color far = at(4, 4);
                bool bright = near.r > 0.5f && far.r < 0.2f;
                expect("lighting-falloff", bright ? forge::Color::white() : forge::Color::black(),
                       forge::Color::white());

                // Off-center light: verifies the lightmap composite is not
                // mirrored/offset. World (120, 80) -> pixel (320, 230).
                // Repaint the white backdrop: the first composite already
                // multiplied it by the black ambient.
                lit.bind();
                forge::gl::glClearColor(0, 0, 0, 1);
                forge::gl::glClear(GL_COLOR_BUFFER_BIT);
                batch.begin(engine.defaultProj());
                batch.drawRect({-200, -150, 400, 300}, forge::Color::white());
                batch.end();
                lights.clearLights();
                forge::Light2D spot;
                spot.pos = {120, 80};
                spot.radius = 40.0f;
                spot.color = forge::Color::white();
                lights.addLight(spot);
                lights.render(batch, cam, vp, &lit);
                lit.resolve();
                ok = lit.readPixels(px);
                forge::Color hit = at(vp.x / 2 + 120, vp.y / 2 + 80);
                forge::Color missV = at(vp.x / 2 + 120, vp.y / 2 - 80);
                forge::Color missH = at(vp.x / 2 - 120, vp.y / 2 + 80);
                bool placed = hit.r > 0.5f && missV.r < 0.2f && missH.r < 0.2f;
                expect("lighting-placement", placed ? forge::Color::white() : forge::Color::black(),
                       forge::Color::white());
                lights.shutdown();
            }
        }
        forge::Framebuffer::unbind(vp.x, vp.y);

        // --- UTF-8 text ---
        forge::Font font;
        const char* candidates[] = {
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/System/Library/Fonts/Helvetica.ttc",
            "C:/Windows/Fonts/segoeui.ttf",
        };
        bool haveFont = false;
        for (const char* path : candidates) {
            if (font.loadFile(path, 28.0f)) { haveFont = true; break; }
        }
        if (haveFont) {
            forge::gl::glClearColor(0, 0, 0, 1);
            forge::gl::glClear(GL_COLOR_BUFFER_BIT);
            batch.begin(engine.defaultProj());
            font.draw(batch, "h\xC3\xA9llo \xE2\x80\x94 \xE4\xB8\x96\xE7\x95\x8C",
                      {-180, 40}, 1.0f, forge::Color::white());
            // Wrapped text must report the height it actually draws.
            float h = font.heightWrapped("the quick brown fox jumps over the lazy dog", 200, 1.0f);
            bool wraps = h > font.lineHeight() * 1.1f;
            expect("utf8-wrapped-height", wraps ? forge::Color::white() : forge::Color::black(),
                   forge::Color::white());
            batch.end();
            // Any lit pixel in the text band means glyphs were rasterized (a
            // missing-glyph run would be blank here).
            int lit = 0;
            for (int y = vp.y / 2 - 40; y < vp.y / 2 + 80; ++y)
                for (int x = 20; x < vp.x - 20; x += 3)
                    if (readPixel(x, y).r > 0.2f) ++lit;
            std::printf("utf8 lit pixels: %d\n", lit);
            expect("utf8-glyphs", lit > 10 ? forge::Color::white() : forge::Color::black(),
                   forge::Color::white());
        } else {
            std::printf("utf8-glyphs: skipped (no system font)\n");
        }

        engine.quit();
    });

    std::printf(fails == 0 ? "RENDER VERIFIED\n" : "RENDER BROKEN (%d)\n", fails);
    return fails == 0 ? 0 : 1;
}
