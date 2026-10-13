#pragma once
// High-resolution time: delta clamping, FPS counter, fixed-timestep accumulator.

#include <cstdint>

namespace forge {

class Time {
public:
    Time();

    // Call once per frame from Engine. Returns clamped delta seconds.
    float tick();
    float delta() const { return delta_; }       // clamped frame delta
    float rawDelta() const { return rawDelta_; } // unclamped
    double elapsed() const { return elapsed_; }  // seconds since construction
    uint64_t frame() const { return frame_; }

    float fps() const { return fps_; }           // smoothed
    void setMaxDelta(float d) { maxDelta_ = d; }

private:
    uint64_t freq_ = 0;
    uint64_t last_ = 0;
    float delta_ = 0.0f;
    float rawDelta_ = 0.0f;
    double elapsed_ = 0.0;
    uint64_t frame_ = 0;
    float fps_ = 0.0f;
    float maxDelta_ = 0.1f; // clamp physics explosions on hitches
    float fpsAccum_ = 0.0f;
    int fpsFrames_ = 0;
};

// Fixed-timestep helper: accumulate variable dt, step at fixed hz.
class FixedStepper {
public:
    explicit FixedStepper(float step = 1.0f / 60.0f, int maxSteps = 5)
        : step_(step), maxSteps_(maxSteps) {}

    template <typename Fn>
    void update(float dt, Fn&& fn) {
        accumulator_ += dt;
        int steps = 0;
        while (accumulator_ >= step_ && steps < maxSteps_) {
            fn(step_);
            accumulator_ -= step_;
            ++steps;
        }
        if (steps == maxSteps_) accumulator_ = 0.0f; // drop backlog after hitch
    }
    float alpha() const { return accumulator_ / step_; }

private:
    float step_;
    int maxSteps_;
    float accumulator_ = 0.0f;
};

} // namespace forge
