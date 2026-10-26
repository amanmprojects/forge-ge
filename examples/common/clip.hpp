#pragma once
// clip.hpp - deterministic video capture for the example games (desktop only).
//
//   ./07_breakout --record breakout.mp4 --frames 1200 [--size 1024x768]
//
// recordClip() runs `frames` fixed steps; after each one the game renders into
// an offscreen target of the requested size, and the RGBA pixels are piped to
// ffmpeg (which must be on PATH) as a 60 fps H.264 MP4. Every frame is one
// simulation step, so a clip is exactly as long as it says and repeats bit for
// bit, however slow the machine is. Audio is not captured.
#include <forge/forge.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if !FORGE_PLATFORM_WEB

#if defined(_WIN32)
#define FORGE_CLIP_POPEN _popen
#define FORGE_CLIP_PCLOSE _pclose
#define FORGE_CLIP_WRITE_MODE "wb"
#else
#define FORGE_CLIP_POPEN popen
#define FORGE_CLIP_PCLOSE pclose
#define FORGE_CLIP_WRITE_MODE "w"
#endif

namespace clip {

constexpr int kFps = 60;

// step() advances the game by one 1/60 s tick; render(fb) draws the current
// state into `fb` (the --shot path every example already has). Returns true
// when ffmpeg accepted every frame and exited cleanly.
template <class Step, class Render>
bool recordClip(const std::string& path, int width, int height, int frames, Step step,
                Render render, const forge::Engine& engine) {
    forge::Framebuffer fbo;
    if (!fbo.create(width, height)) {
        std::printf("record: could not create a %dx%d target\n", width, height);
        return false;
    }
    // GL rows are bottom-up; ffmpeg flips them instead of a per-frame memcpy.
    // yuv420p + faststart: plays in every browser and starts before it loads.
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd),
                  "ffmpeg -y -hide_banner -loglevel error -f rawvideo -pixel_format rgba "
                  "-video_size %dx%d -framerate %d -i - -vf vflip -c:v libx264 -preset slow "
                  "-crf 20 -pix_fmt yuv420p -movflags +faststart \"%s\"",
                  width, height, kFps, path.c_str());
    FILE* pipe = FORGE_CLIP_POPEN(cmd, FORGE_CLIP_WRITE_MODE);
    if (!pipe) {
        std::printf("record: could not start ffmpeg\n");
        return false;
    }
    std::vector<uint8_t> px;
    bool ok = true;
    for (int i = 0; i < frames && ok; ++i) {
        step();
        render(&fbo);
        ok = fbo.readPixels(px) && std::fwrite(px.data(), 1, px.size(), pipe) == px.size();
    }
    forge::Framebuffer::unbind(engine.viewport().x, engine.viewport().y);
    const int status = FORGE_CLIP_PCLOSE(pipe);
    std::printf("record: %s (%dx%d, %d frames, %.1f s) %s\n", path.c_str(), width, height,
                frames, (double)frames / kFps, ok && status == 0 ? "OK" : "FAILED");
    return ok && status == 0;
}

} // namespace clip

#endif // !FORGE_PLATFORM_WEB
