// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Library error codes and Error value type

#pragma once

#include <cstdint>
#include <optional>
#include <string>


namespace VSphereSoap {

// ABI discipline: this library guarantees ABI compatibility only for consumers built with the same
// compiler and standard library ABI as this library itself (i.e. the library and its consumers are
// shipped and rebuilt together as one bundle, not consumed as a distro package built once and used by
// arbitrary third-party binaries).
// On that basis, standard library types (std::string, std::function, std::optional, ...) are used
// directly in this public interface rather than hidden behind an ABI-stable wrapper.
//
// Given that, concrete exported classes that carry meaningful internal state still use the pImpl idiom
// (see VmwareSoapDisk.hpp) so that adding a field doesn't force every consumer to rebuild; that's
// a compile-firewall argument, not an ABI one.
// Error is a deliberate exception to that rule: it is a small, frequently-constructed value type (built
// on essentially every I/O failure) whose four fields form its entire public contract via the accessors
// below, so pImpl here would only add a heap allocation to a hot path for no corresponding benefit in
// a same-toolchain-bundle build.

/// Category of failure.
/// Kept small and coarse; callers branch on this, not on message text.
enum class ErrorCode {
    NoError = 0,     ///< Success; no error.
    IoError,         ///< A syscall (open/pread/pwrite/fsync/...) failed. Carries errno + path.
    InvalidFormat,   ///< On-disk header/magic/version did not parse as the expected format.
    InvalidArgument, ///< Caller passed a value that violates a documented precondition.
    NotImplemented,  ///< Feature is intentionally unimplemented in this build (e.g. payload
                     /// < read/write for formats that only have header parsing so far).
    Cancelled,       ///< Operation observed a stop request via std::stop_token and gave up.
};

/// Carries enough context to explain a failure without throwing.
/// Every fallible operation in this library reports errors through `Result<T>` (see Result.hpp) or, when it
/// has no payload, by returning `Error` directly, never via a raw `std::runtime_error` escaping to a caller.
/// A default-constructed `Error` has code `ErrorCode::NoError` and stands for success, so `return {};` reports
/// success from a function returning `Error`.
///
/// I/O failures always carry both `errnoValue()` and `path()` so a tool can print a message like
/// `open(/mnt/disk.raw): No such file or directory` without the caller having to reconstruct it.
///
/// Usage:
/// ```
/// Error err = disk.flush();
/// if (!err.ok()) {
///     return err; // propagate, implicitly converts to Result<Other>
/// }
/// ```
///
class [[nodiscard]] Error {
public:
    /// Constructs a success value with code `ErrorCode::NoError`.
    Error() noexcept = default;

    /// Constructs an error with no errno/path context, e.g. InvalidFormat or InvalidArgument.
    Error(ErrorCode code, const std::string& message);

    /// Constructs an I/O error.
    /// `errnoValue` should be the `errno` captured immediately after the failing syscall; `path` is
    /// the file the syscall was operating on.
    Error(ErrorCode code, const std::string& message, int errnoValue, const std::string& path);

    /// True if this value stands for success, i.e. its code is `ErrorCode::NoError`.
    bool ok() const noexcept;

    /// The category of failure.
    ErrorCode code() const noexcept;

    /// Human-readable description.
    /// Does not include errno/path text; use `toString()` for a fully formatted message.
    const std::string message() const noexcept;

    /// `errno` captured at the point of failure, if this is an I/O error.
    std::optional<int> errnoValue() const noexcept;

    /// Path the failing operation targeted, if this is an I/O error.
    std::optional<std::string> path() const noexcept;

    /// Formats code, message, and (if present) path + strerror(errno) into a single human-readable line,
    /// suitable for printing by a tool.
    std::string toString() const;

private:
    ErrorCode m_code = ErrorCode::NoError;
    std::string m_message;
    std::optional<int> m_errno;
    std::optional<std::string> m_path;
};

/// Convenience factory for I/O errors: captures `errno` at the call site and pairs it with `path`.
/// Prefer this over constructing `Error` directly at syscall call sites.
Error makeIoError(const std::string& path, int errnoValue, const std::string& message);

} // namespace VSphereSoap
