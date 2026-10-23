// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere datastore HTTP path helpers

#include "DatastorePath.hpp"

#include <cctype>

namespace VSphereSoap::Detail {

std::string urlEncode(std::string_view in)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (const unsigned char c : in) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == '/') {
            out.push_back('/'); // path separator: preserved, not encoded.
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string deriveFlatName(std::string_view relativePath)
{
    constexpr std::string_view kVmdkSuffix = ".vmdk";
    if (relativePath.size() > kVmdkSuffix.size() && relativePath.ends_with(kVmdkSuffix) &&
        !relativePath.ends_with("-flat.vmdk") && !relativePath.ends_with("-delta.vmdk")) {
        return std::string(relativePath.substr(0, relativePath.size() - kVmdkSuffix.size())) + "-flat.vmdk";
    }
    return std::string(relativePath);
}

std::string siblingPath(std::string_view originalRelativePath, std::string_view newFileName)
{
    const auto slash = originalRelativePath.rfind('/');
    if (slash == std::string_view::npos) {
        return std::string(newFileName);
    }
    return std::string(originalRelativePath.substr(0, slash + 1)) + std::string(newFileName);
}

std::optional<std::string> parseFlatExtentFileName(std::string_view descriptor)
{
    std::size_t pos = 0;
    while (pos <= descriptor.size()) {
        const auto eol = descriptor.find('\n', pos);
        const std::string_view rawLine =
            eol == std::string_view::npos ? descriptor.substr(pos) : descriptor.substr(pos, eol - pos);
        pos = eol == std::string_view::npos ? descriptor.size() + 1 : eol + 1;

        std::string_view line = rawLine;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
            line.remove_prefix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (!(line.starts_with("RW") || line.starts_with("RDONLY") || line.starts_with("NOACCESS"))) {
            continue;
        }

        const auto firstQuote = line.find('"');
        if (firstQuote == std::string_view::npos) {
            continue;
        }
        const auto secondQuote = line.find('"', firstQuote + 1);
        if (secondQuote == std::string_view::npos) {
            continue;
        }
        return std::string(line.substr(firstQuote + 1, secondQuote - firstQuote - 1));
    }
    return std::nullopt;
}

} // namespace VSphereSoap::Detail
