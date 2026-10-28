// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Diagnostics routing for the library's own log messages

#include "vsphere-soap/Log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "SoapLog.hpp"

namespace VSphereSoap::Log {

namespace {

Level levelFromEnv()
{
    const char* raw = std::getenv("VSPHERE_SOAP_LOG_LEVEL");
    if (raw == nullptr) {
        return Level::Warn;
    }
    const std::string_view value(raw);
    for (Level candidate : {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Off}) {
        if (value == toString(candidate)) {
            return candidate;
        }
    }
    return Level::Warn;
}

std::atomic<Level>& currentLevel()
{
    static std::atomic<Level> instance{levelFromEnv()};
    return instance;
}

// Guards the sink itself, so a tool may swap it while an upload thread is logging.
std::mutex& sinkMutex()
{
    static std::mutex instance;
    return instance;
}

Sink& currentSink()
{
    static Sink instance;
    return instance;
}

void writeToStderr(Level level, std::string_view channel, std::string_view message)
{
    const char* prefix = level >= Level::Error ? "Error: " : level == Level::Warn ? "Warning: " : "";
    std::fprintf(stderr, "%s[%.*s] %.*s\n", prefix, static_cast<int>(channel.size()), channel.data(),
                 static_cast<int>(message.size()), message.data());
}

} // namespace

void setSink(Sink sink)
{
    std::lock_guard<std::mutex> lock(sinkMutex());
    currentSink() = std::move(sink);
}

void setLevel(Level level)
{
    currentLevel().store(level, std::memory_order_relaxed);
}

Level level() noexcept
{
    return currentLevel().load(std::memory_order_relaxed);
}

std::string_view toString(Level level) noexcept
{
    switch (level) {
    case Level::Trace:
        return "trace";
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warn:
        return "warn";
    case Level::Error:
        return "error";
    case Level::Off:
        return "off";
    }
    return "off";
}

} // namespace VSphereSoap::Log

namespace VSphereSoap::Detail {

bool Logger::enabled(Log::Level level) const noexcept
{
    return level != Log::Level::Off && level >= Log::level();
}

void Logger::emit(Log::Level level, std::string_view message) const
{
    std::lock_guard<std::mutex> lock(Log::sinkMutex());
    if (Log::currentSink()) {
        Log::currentSink()(level, m_channel, message);
    } else {
        Log::writeToStderr(level, m_channel, message);
    }
}

} // namespace VSphereSoap::Detail
