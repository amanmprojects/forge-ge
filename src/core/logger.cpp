#include "forge/core/logger.hpp"
#include <chrono>
#include <ctime>

namespace forge {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

const char* Logger::levelName(LogLevel l) const {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "?";
    }
}

const char* Logger::levelColor(LogLevel l) const {
    switch (l) {
        case LogLevel::Trace: return "\x1b[37m";
        case LogLevel::Debug: return "\x1b[36m";
        case LogLevel::Info:  return "\x1b[32m";
        case LogLevel::Warn:  return "\x1b[33m";
        case LogLevel::Error: return "\x1b[31m";
        case LogLevel::Fatal: return "\x1b[35m";
        default: return "\x1b[0m";
    }
}

void Logger::setFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) { std::fclose(file_); file_ = nullptr; }
    filePath_ = path;
    if (!path.empty()) {
        file_ = std::fopen(path.c_str(), "w");
    }
}

void Logger::log(LogLevel l, const char* file, int line, const std::string& msg) {
    if (!enabled(l)) return;
    std::lock_guard<std::mutex> lock(mutex_);

    // timestamp HH:MM:SS
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_{};
    localtime_r(&t, &tm_);
    char ts[16];
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d", tm_.tm_hour, tm_.tm_min, tm_.tm_sec);

    // short filename
    const char* shortFile = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') shortFile = p + 1;
    }

    std::fprintf(stderr, "%s[%s] %-5s %s:%d: %s\x1b[0m\n",
                 levelColor(l), ts, levelName(l), shortFile, line, msg.c_str());

    if (file_) {
        std::fprintf(file_, "[%s] %-5s %s:%d: %s\n", ts, levelName(l), shortFile, line, msg.c_str());
        std::fflush(file_);
    }

    if (l == LogLevel::Fatal) std::fflush(stderr);
}

} // namespace forge
