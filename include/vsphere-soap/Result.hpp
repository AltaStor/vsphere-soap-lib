// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Result<T> value-or-error type

#pragma once

#include <cassert>
#include <utility>
#include <variant>

#include "vsphere-soap/Error.hpp"

namespace VSphereSoap {

/// Hand-rolled `std::expected<T, Error>` substitute (this project targets C++20, where `std::expected`
/// is not available).
/// Every fallible operation in this library returns a `Result<T>` (or a bare `Error` when it has no payload)
/// rather than throwing; this is the one error-reporting mechanism used throughout the library.
/// Exceptions are not part of the public contract of any function that returns `Result` or `Error`.
///
/// Usage:
/// ```
/// Result<std::uint64_t> r = disk.readAt(offset, buf);
/// if (!r) {
///     return r.error(); // propagate, implicitly converts to Result<Other>
/// }
/// use(r.value());
/// ```
///
template <typename T>
class [[nodiscard]] Result {
public:
    /// Constructs a successful result holding `value`.
    /// Implicit so that `return someT;` works directly inside a function returning `Result<T>`.
    Result(T value) : m_storage(std::move(value)) {}

    /// Constructs a failed result holding `error`.
    /// Implicit so that `return someError;` works directly inside a function returning `Result<T>`.
    /// Precondition: `!error.ok()`, a success `Error` carries no value to return.
    Result(Error error) : m_storage(std::move(error)) { assert(!std::get<Error>(m_storage).ok()); }

    /// True if this result holds a value rather than an error.
    bool hasValue() const noexcept { return std::holds_alternative<T>(m_storage); }

    /// Equivalent to `hasValue()`.
    explicit operator bool() const noexcept { return hasValue(); }

    /// Accesses the held value.
    /// Precondition: `hasValue()`.
    /// Does not throw; violating the precondition trips an assertion in debug builds and is undefined
    /// behavior in release builds: callers must check `hasValue()` (or `operator bool`) first.
    ///
    T& value() &
    {
        assert(hasValue());
        return std::get<T>(m_storage);
    }

    /// `const` overload of `value()`.
    /// Same precondition.
    const T& value() const &
    {
        assert(hasValue());
        return std::get<T>(m_storage);
    }

    /// Rvalue overload of `value()`, for moving the held value out of a temporary `Result`.
    /// Same precondition.
    T&& value() &&
    {
        assert(hasValue());
        return std::get<T>(std::move(m_storage));
    }

    /// Accesses the held error.
    /// Precondition: `!hasValue()`.
    /// Same no-throw/assert contract as `value()`.
    const Error& error() const &
    {
        assert(!hasValue());
        return std::get<Error>(m_storage);
    }

    /// Rvalue overload of `error()`, for moving the error out of a temporary `Result`.
    /// Same precondition.
    Error&& error() &&
    {
        assert(!hasValue());
        return std::get<Error>(std::move(m_storage));
    }

private:
    std::variant<T, Error> m_storage;
};

} // namespace VSphereSoap
