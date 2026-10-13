#include "forge/core/time.hpp"
#include <SDL3/SDL.h>

namespace forge {

Time::Time() {
    freq_ = SDL_GetPerformanceFrequency();
    last_ = SDL_GetPerformanceCounter();
}

float Time::tick() {
    uint64_t now = SDL_GetPerformanceCounter();
    double dt = freq_ > 0 ? (double)(now - last_) / (double)freq_ : 0.016;
    last_ = now;
    if (dt < 0) dt = 0;
    rawDelta_ = (float)dt;
    delta_ = rawDelta_ > maxDelta_ ? maxDelta_ : rawDelta_;
    elapsed_ += rawDelta_;
    ++frame_;

    // smoothed fps over 0.25s windows
    fpsAccum_ += rawDelta_;
    ++fpsFrames_;
    if (fpsAccum_ >= 0.25f && fpsFrames_ > 0) {
        fps_ = fpsFrames_ / fpsAccum_;
        fpsAccum_ = 0.0f;
        fpsFrames_ = 0;
    }
    return delta_;
}

} // namespace forge
