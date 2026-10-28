// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Library error codes and Error value type

#include "vsphere-soap/Error.hpp"

#include <system_error>

namespace VSphereSoap {

Error::Error(ErrorCode code, const std::string& message)
    : m_code(code), m_message{message} {}

Error::Error(ErrorCode code, const std::string& message, int errnoValue, const std::string& path)
    : m_code(code), m_message{message}, m_errno(errnoValue), m_path{path} {}

bool Error::ok() const noexcept
{
    return m_code == ErrorCode::NoError;
}

ErrorCode Error::code() const noexcept
{
    return m_code;
}

const std::string Error::message() const noexcept
{
    return m_message;
}

std::optional<int> Error::errnoValue() const noexcept
{
    return m_errno;
}

std::optional<std::string> Error::path() const noexcept
{
    return m_path;
}

std::string Error::toString() const
{
    std::string result = m_message;
    if (m_path) {
        result += " (";
        result += *m_path;
        // 0 isn't an error: network errors pass it with just a host as `path`, and its system message
        // ("The operation completed successfully" on Windows) would only confuse.
        if (m_errno && *m_errno != 0) {
            result += ": ";
            // generic_category, not system_category: m_errno is an errno value, and on Windows
            // system_category reads it as a Win32 error code (EOVERFLOW 132 would print as
            // ERROR_SEEK_ON_DEVICE).
            result += std::generic_category().message(*m_errno);
        }
        result += ")";
    }
    return result;
}

Error makeIoError(const std::string& path, int errnoValue,const std::string& message)
{
    return Error(ErrorCode::IoError, message, errnoValue, path);
}

} // namespace VSphereSoap
