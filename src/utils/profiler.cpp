#include "forge/utils/profiler.hpp"

namespace forge {

Profiler& Profiler::instance() {
    static Profiler p;
    return p;
}

void Profiler::beginFrame() {
#if FORGE_PROFILING
    frameT0_ = std::chrono::steady_clock::now();
    cur_.clear();
    stack_.clear();
#else
    (void)0;
#endif
}

void Profiler::endFrame() {
#if FORGE_PROFILING
    auto t1 = std::chrono::steady_clock::now();
    lastFrameMs_ = std::chrono::duration<double, std::milli>(t1 - frameT0_).count();
    lastZones_ = cur_;
    history_.push_back((float)lastFrameMs_);
    if (history_.size() > 120) history_.erase(history_.begin());
#endif
}

void Profiler::beginZone(const char* name) {
#if FORGE_PROFILING
    stack_.push_back({name, std::chrono::steady_clock::now()});
#else
    (void)name;
#endif
}

void Profiler::endZone() {
#if FORGE_PROFILING
    if (stack_.empty()) return;
    Open o = stack_.back();
    stack_.pop_back();
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - o.t0).count();
    for (auto& z : cur_) {
        if (z.name == o.name) { z.ms += ms; return; }
    }
    cur_.push_back({o.name, ms});
#endif
}

double Profiler::zoneMs(const char* name) const {
    for (auto& z : lastZones_)
        if (z.name == name) return z.ms;
    return 0;
}

} // namespace forge
