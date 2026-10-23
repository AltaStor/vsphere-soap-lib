// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Result<T> value-or-error type

#pragma once

#include <cassert>
#include <optional>
#include <utility>
#include <variant>

#include "Error.hpp"

namespace VSphereSoap {

/// Hand-rolled `std::expected<T, Error>` substitute (this project targets C++20, where `std::expected`
/// is not available).
/// Every fallible operation in this library returns a `Result<T>` (or `Result<void>`) rather than
/// throwing; this is the one error-reporting mechanism used throughout the library.
/// Exceptions are not part of the public contract of any function that returns `Result`.
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
    Result(Error error) : m_storage(std::move(error)) {}

    /// True if this result holds a value rather than an error.
    [[nodiscard]] bool hasValue() const noexcept { return std::holds_alternative<T>(m_storage); }

    /// Equivalent to `hasValue()`.
    explicit operator bool() const noexcept { return hasValue(); }

    /// Accesses the held value.
    /// Precondition: `hasValue()`.
    /// Does not throw; violating the precondition trips an assertion in debug builds and is undefined
    /// behavior in release builds: callers must check `hasValue()` (or `operator bool`) first.
    ///
    [[nodiscard]] T& value() &
    {
        assert(hasValue());
        return std::get<T>(m_storage);
    }

    /// `const` overload of `value()`.
    /// Same precondition.
    [[nodiscard]] const T& value() const &
    {
        assert(hasValue());
        return std::get<T>(m_storage);
    }

    /// Rvalue overload of `value()`, for moving the held value out of a temporary `Result`.
    /// Same precondition.
    [[nodiscard]] T&& value() &&
    {
        assert(hasValue());
        return std::get<T>(std::move(m_storage));
    }

    /// Accesses the held error.
    /// Precondition: `!hasValue()`.
    /// Same no-throw/assert contract as `value()`.
    [[nodiscard]] const Error& error() const &
    {
        assert(!hasValue());
        return std::get<Error>(m_storage);
    }

    /// Rvalue overload of `error()`, for moving the error out of a temporary `Result`.
    /// Same precondition.
    [[nodiscard]] Error&& error() &&
    {
        assert(!hasValue());
        return std::get<Error>(std::move(m_storage));
    }

private:
    std::variant<T, Error> m_storage;
};

/// Specialization for operations that succeed with no payload (e.g. `IDisk::flush`).
/// Behaves like `Result<T>` with the value case carrying no data.
template <>
class [[nodiscard]] Result<void> {
public:
    /// Constructs a successful result.
    Result() noexcept = default;

    /// Constructs a failed result holding `error`.
    /// Implicit so that `return someError;` works directly inside a function returning `Result<void>`.
    Result(Error error) noexcept : m_error(std::move(error)) {}

    /// True if this result represents success.
    [[nodiscard]] bool hasValue() const noexcept { return !m_error.has_value(); }

    /// Equivalent to `hasValue()`.
    explicit operator bool() const noexcept { return hasValue(); }

    /// Accesses the held error.
    /// Precondition: `!hasValue()`.
    /// Does not throw; violating the precondition trips an assertion in debug builds and is undefined
    /// behavior in release builds.
    ///
    [[nodiscard]] const Error& error() const &
    {
        assert(!hasValue());
        return *m_error;
    }

    /// Rvalue overload of `error()`.
    /// Same precondition.
    [[nodiscard]] Error&& error() &&
    {
        assert(!hasValue());
        return std::move(*m_error);
    }

private:
    std::optional<Error> m_error;
};

} // namespace VSphereSoap
