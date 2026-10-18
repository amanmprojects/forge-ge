// 04_audio — synthesize SFX in memory (no asset files), play + visualize.
#include <forge/forge.hpp>
#include <SDL3/SDL.h>
#include <cstdio>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>

// Write a 16-bit mono WAV into memory.
static std::vector<uint8_t> makeWav(float freq, float dur, float vol, bool noise) {
    const int rate = 22050;
    int frames = (int)(dur * rate);
    std::vector<int16_t> pcm(frames);
    forge::Random rng{(uint64_t)(freq * 100)};
    for (int i = 0; i < frames; ++i) {
        float t = (float)i / rate;
        float env = 1.0f - (float)i / frames; // decay
        float s = noise ? (rng.nextFloat() * 2 - 1) * env
                        : std::sin(forge::TAU * freq * t) * env;
        // simple pitch sweep up for "blip" feel
        pcm[i] = (int16_t)(s * vol * 32767);
    }
    std::vector<uint8_t> wav(44 + frames * 2);
    auto w32 = [&](int o, uint32_t v) { std::memcpy(&wav[o], &v, 4); };
    auto w16 = [&](int o, uint16_t v) { std::memcpy(&wav[o], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    w32(4, 36 + frames * 2);
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    w32(16, 16); w16(20, 1); w16(22, 1);
    w32(24, rate); w32(28, rate * 2); w16(32, 2); w16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    w32(40, frames * 2);
    std::memcpy(&wav[44], pcm.data(), frames * 2);
    return wav;
}

int main() {
    forge::EngineConfig cfg;
    cfg.window.title = "Forge 04_audio — 1/2/3 to play, M music";
    forge::Engine engine(cfg);
    if (!engine.valid()) return 1;

    // decode synthesized WAVs through the real asset path
    auto w1 = makeWav(440, 0.25f, 0.6f, false);
    auto w2 = makeWav(660, 0.25f, 0.6f, false);
    auto w3 = makeWav(0, 0.4f, 0.5f, true);
    forge::SoundHandle blip1 = engine.audio().loadMemory(w1.data(), w1.size());
    forge::SoundHandle blip2 = engine.audio().loadMemory(w2.data(), w2.size());
    forge::SoundHandle hiss = engine.audio().loadMemory(w3.data(), w3.size());
    std::printf("sounds: %d %d %d (audio %s)\n", blip1.valid(), blip2.valid(), hiss.valid(),
                engine.audio().valid() ? "live" : "muted");

    forge::Font font;
    font.loadFile("/usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf", 24.0f);

    float pulse[3] = {};
    int frames = 0;
    engine.runVariable([&](float dt) {
        auto& in = engine.input();
        if (in.isDown(forge::Key::Escape)) engine.quit();
        if (in.pressed(forge::Key::N1)) { engine.audio().play(blip1); pulse[0] = 1; }
        if (in.pressed(forge::Key::N2)) { engine.audio().play(blip2, 1.0f, -0.6f); pulse[1] = 1; }
        if (in.pressed(forge::Key::N3)) { engine.audio().play(hiss, 0.8f, 0.6f); pulse[2] = 1; }
        if (in.pressed(forge::Key::M)) engine.audio().playMusic(blip2, 0.5f);
        for (float& p : pulse) p = std::max(0.0f, p - dt * 2);

        forge::IVec2 vp = engine.viewport();
        forge::gl::glViewport(0, 0, vp.x, vp.y);
        forge::gl::glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        auto& batch = engine.batch();
        batch.begin(engine.defaultProj());

        const char* labels[] = {"1: blip 440Hz", "2: blip 660Hz (left)", "3: noise (right)"};
        for (int i = 0; i < 3; ++i) {
            float y = 100 - i * 70;
            float w = 300 * (0.2f + 0.8f * pulse[i]);
            batch.drawRect({-350, y, w, 40},
                           forge::Color::fromHex(pulse[i] > 0.05f ? 0x2A9D8FFF : 0x444444FF));
            if (font.valid()) font.draw(batch, labels[i], {-340, y + 40}, 0.7f, forge::Color::white());
        }
        if (font.valid()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "voices: %d  fps: %.0f",
                          engine.audio().voiceCount(), engine.fps());
            font.draw(batch, buf, {-350, -160}, 0.8f, forge::Color::white());
        }
        batch.end();
        if (++frames >= 600) { // muted audio or CI: auto-exit; remove for play
            if (!blip1.valid() || std::getenv("CI")) engine.quit();
            frames = 0;
        }
    });
    return 0;
}
