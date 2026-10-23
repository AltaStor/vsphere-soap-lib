// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Internal named loggers behind the public Log sink

#pragma once

// Keeps the spdlog-shaped call sites the code was written with (logger()->warn("{} failed: {}", ...)),
// but formats through std::format and hands the line to whatever sink the consuming tool installed.
// A message below the current level is dropped before any formatting work is done.

#include <format>
#include <string_view>
#include <utility>

#include "Log.hpp"

namespace VSphereSoap::Detail {

class Logger {
public:
    explicit constexpr Logger(std::string_view channel) noexcept : m_channel(channel) {}

    template <typename... Args>
    void trace(std::format_string<Args...> format, Args&&... args) const
    {
        write(Log::Level::Trace, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(std::format_string<Args...> format, Args&&... args) const
    {
        write(Log::Level::Debug, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(std::format_string<Args...> format, Args&&... args) const
    {
        write(Log::Level::Info, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(std::format_string<Args...> format, Args&&... args) const
    {
        write(Log::Level::Warn, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(std::format_string<Args...> format, Args&&... args) const
    {
        write(Log::Level::Error, format, std::forward<Args>(args)...);
    }

    [[nodiscard]] bool enabled(Log::Level level) const noexcept;

private:
    template <typename... Args>
    void write(Log::Level level, std::format_string<Args...> format, Args&&... args) const
    {
        if (enabled(level)) {
            emit(level, std::format(format, std::forward<Args>(args)...));
        }
    }

    void emit(Log::Level level, std::string_view message) const;

    std::string_view m_channel;
};

/// The vSphere API calls: sessions, inventory, snapshots, leases, uploads.
inline const Logger* logger()
{
    static constexpr Logger instance("soap");
    return &instance;
}

/// The HTTPS transport: connects, TLS handshakes, request timings.
inline const Logger* httpLogger()
{
    static constexpr Logger instance("http");
    return &instance;
}

} // namespace VSphereSoap::Detail
