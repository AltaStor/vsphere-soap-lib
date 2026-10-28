// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Diagnostics routing for the library's own log messages

#pragma once

// The library reports what it is doing on the network (which SOAP call, which ranged GET, lease and
// upload progress) through one process-wide sink rather than through a logging framework of its own.
// Each consuming tool routes these lines into whatever it already uses: spdlog, std::cerr behind its own
// prefixes, a file, or nothing at all.
//
// Without a sink of the caller's, messages go to stderr as "Warning: [soap] ..." style lines.
// The level defaults to Warn, so an unconfigured tool only sees problems, not progress chatter.
// VSPHERE_SOAP_LOG_LEVEL (trace|debug|info|warn|error|off) overrides that default at first use.

#include <functional>
#include <string_view>

namespace VSphereSoap::Log {

enum class Level {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Off,
};

/// Receives every message at or above the current level.
/// `channel` names the subsystem: "soap" for the vSphere API calls, "http" for the HTTPS transport.
/// May be called from the library's background upload threads, so it has to be thread-safe.
using Sink = std::function<void(Level level, std::string_view channel, std::string_view message)>;

/// Replaces the sink; an empty function restores the default stderr sink.
void setSink(Sink sink);

/// Messages below `level` are dropped before they are formatted.
void setLevel(Level level);

Level level() noexcept;

/// "trace", "debug", "info", "warn", "error" or "off".
std::string_view toString(Level level) noexcept;

} // namespace VSphereSoap::Log
