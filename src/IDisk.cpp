// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Default IDisk helpers (zero-range detection)

#include "IDisk.hpp"

#include <algorithm>
#include <vector>

namespace VSphereSoap {

namespace {
constexpr std::uint64_t kIsZeroChunkSize = 1ull * 1024 * 1024;
}

Result<bool> IDisk::isZero(std::uint64_t offset, std::uint64_t length)
{
    if (length == 0) {
        return true;
    }

    std::vector<std::byte> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(length, kIsZeroChunkSize)));
    std::uint64_t pos = offset;
    std::uint64_t remaining = length;

    while (remaining > 0) {
        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        Result<std::uint64_t> read = readAt(pos, std::span<std::byte>(buffer.data(), chunk));
        if (!read) {
            return std::move(read).error();
        }
        if (read.value() != chunk) {
            return Error(ErrorCode::IoError,
                         "isZero: short read before covering the requested range");
        }
        const bool allZero = std::all_of(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(chunk),
                                          [](std::byte b) { return b == std::byte{0}; });
        if (!allZero) {
            return false;
        }

        pos += chunk;
        remaining -= chunk;
    }

    return true;
}

} // namespace VSphereSoap
