#pragma once
// Profiler: scoped CPU zones per frame + frame-time graph data.
// Zero overhead when FORGE_PROFILING=0 (all inline no-ops).

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

namespace forge {

struct ZoneRecord {
    std::string name;
    double ms = 0;
};

class Profiler {
public:
    static Profiler& instance();

    void beginFrame();
    void endFrame();
    void beginZone(const char* name);
    void endZone();

    // Last frame stats
    double frameMs() const { return lastFrameMs_; }
    const std::vector<ZoneRecord>& zones() const { return lastZones_; }
    double zoneMs(const char* name) const;

    // Rolling history for graphs (120 frames)
    const std::vector<float>& history() const { return history_; }

private:
    struct Open { const char* name; std::chrono::steady_clock::time_point t0; };
    std::vector<Open> stack_;
    std::vector<ZoneRecord> cur_, lastZones_;
    std::vector<float> history_;
    std::chrono::steady_clock::time_point frameT0_;
    double lastFrameMs_ = 0;
};

class ScopedZone {
public:
    explicit ScopedZone(const char* name) {
#if FORGE_PROFILING
        Profiler::instance().beginZone(name);
#else
        (void)name;
#endif
    }
    ~ScopedZone() {
#if FORGE_PROFILING
        Profiler::instance().endZone();
#endif
    }
};

} // namespace forge

#if FORGE_PROFILING
#define FORGE_ZONE(name) ::forge::ScopedZone FORGE_ZONE_##__LINE__(name)
#define FORGE_ZONE_FN() ::forge::ScopedZone FORGE_ZONE_FN_##__LINE__(__func__)
#else
#define FORGE_ZONE(name) ((void)0)
#define FORGE_ZONE_FN() ((void)0)
#endif
