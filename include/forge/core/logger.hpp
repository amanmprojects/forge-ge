#pragma once
// Minimal structured logger: levels, colored stderr, optional file sink.
// Hot path: logEnabled() check before formatting.

#include <string>
#include <string_view>
#include <mutex>
#include <cstdio>

namespace forge {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel l) { level_ = l; }
    LogLevel level() const { return level_; }
    bool enabled(LogLevel l) const { return (int)l >= (int)level_; }

    void setFile(const std::string& path); // empty = disable file sink

    void log(LogLevel l, const char* file, int line, const std::string& msg);

    template <typename... Args>
    void logf(LogLevel l, const char* file, int line, const char* fmt, Args... args) {
        if (!enabled(l)) return;
        char buf[2048];
        // Every call site passes a literal; clang's -Wformat-security cannot
        // see through the variadic parameter here.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-security"
#endif
        std::snprintf(buf, sizeof(buf), fmt, args...);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
        log(l, file, line, std::string(buf));
    }

private:
    Logger() = default;
    const char* levelName(LogLevel l) const;
    const char* levelColor(LogLevel l) const;

    LogLevel level_ = LogLevel::Info;
    std::mutex mutex_;
    std::FILE* file_ = nullptr;
    std::string filePath_;
};

} // namespace forge

#define FORGE_LOG_TRACE(...) do { auto& L = ::forge::Logger::instance(); if (L.enabled(::forge::LogLevel::Trace)) L.logf(::forge::LogLevel::Trace, __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define FORGE_LOG_DEBUG(...) do { auto& L = ::forge::Logger::instance(); if (L.enabled(::forge::LogLevel::Debug)) L.logf(::forge::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define FORGE_LOG_INFO(...)  do { auto& L = ::forge::Logger::instance(); if (L.enabled(::forge::LogLevel::Info))  L.logf(::forge::LogLevel::Info,  __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define FORGE_LOG_WARN(...)  do { auto& L = ::forge::Logger::instance(); if (L.enabled(::forge::LogLevel::Warn))  L.logf(::forge::LogLevel::Warn,  __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define FORGE_LOG_ERROR(...) do { auto& L = ::forge::Logger::instance(); if (L.enabled(::forge::LogLevel::Error)) L.logf(::forge::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define FORGE_LOG_FATAL(...) do { auto& L = ::forge::Logger::instance(); L.logf(::forge::LogLevel::Fatal, __FILE__, __LINE__, __VA_ARGS__); } while (0)
