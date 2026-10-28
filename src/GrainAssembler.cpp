// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Assembler of writes into stream-optimized VMDK grains

#include "GrainAssembler.hpp"

#include <cstring>
#include <utility>

#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

GrainAssembler::GrainAssembler(std::uint64_t grainSizeBytes, GrainSink sink,
                                  std::optional<std::uint64_t> denseCapacityBytes)
    : m_grainSizeBytes(grainSizeBytes), m_sink(std::move(sink)), m_denseCapacityBytes(denseCapacityBytes),
      m_pendingGrain(grainSizeBytes) {}

Error GrainAssembler::zeroFillGrainsUpTo(std::uint64_t targetOffset)
{
    if (!m_denseCapacityBytes) {
        return {};
    }
    // Zero-initialized once and reused for every gap grain: content is identical for all of them by
    // definition (they're all-zero), so there's nothing to gain from a fresh buffer per grain.
    const std::vector<std::byte> zeroGrain(m_grainSizeBytes, std::byte{0});
    while (m_nextGrainOffset < targetOffset) {
        if (auto result = m_sink(m_nextGrainOffset, std::span<const std::byte>(zeroGrain)); !result.ok()) {
            return result;
        }
        m_nextGrainOffset += m_grainSizeBytes;
    }
    return {};
}

Error GrainAssembler::consume(std::uint64_t offset, std::span<const std::byte> in)
{
    if (in.empty()) {
        return {};
    }
    if (offset < m_nextExpectedOffset) {
        return Error(ErrorCode::InvalidArgument,
                     "GrainAssembler::consume: offset " + std::to_string(offset) +
                         " goes backward or overlaps a previous call (expected >= " +
                         std::to_string(m_nextExpectedOffset) + ")");
    }

    std::uint64_t pos = offset;
    std::span<const std::byte> remaining = in;

    if (m_pendingGrainFilled > 0) {
        const std::uint64_t continuation = m_pendingGrainOffset + m_pendingGrainFilled;
        if (pos == continuation) {
            const std::uint64_t space = m_grainSizeBytes - m_pendingGrainFilled;
            const std::uint64_t take = std::min<std::uint64_t>(space, remaining.size());
            std::memcpy(m_pendingGrain.data() + m_pendingGrainFilled, remaining.data(), take);
            m_pendingGrainFilled += take;
            pos += take;
            remaining = remaining.subspan(take);
            if (m_pendingGrainFilled == m_grainSizeBytes) {
                const std::uint64_t grainOffset = m_pendingGrainOffset;
                m_pendingGrainFilled = 0;
                if (auto result = m_sink(grainOffset, std::span<const std::byte>(m_pendingGrain.data(), m_grainSizeBytes));
                    !result.ok()) {
                    return result;
                }
                m_nextGrainOffset = grainOffset + m_grainSizeBytes;
            }
        } else {
            if (pos % m_grainSizeBytes != 0) {
                return Error(ErrorCode::InvalidArgument,
                             "GrainAssembler::consume: offset " + std::to_string(pos) +
                                 " is neither a contiguous continuation of the in-progress grain (expected " +
                                 std::to_string(continuation) + ") nor grain-aligned");
            }
            std::memset(m_pendingGrain.data() + m_pendingGrainFilled, 0, m_grainSizeBytes - m_pendingGrainFilled);
            const std::uint64_t grainOffset = m_pendingGrainOffset;
            m_pendingGrainFilled = 0;
            if (auto result = m_sink(grainOffset, std::span<const std::byte>(m_pendingGrain.data(), m_grainSizeBytes));
                !result.ok()) {
                return result;
            }
            m_nextGrainOffset = grainOffset + m_grainSizeBytes;
            // The old pending grain is accounted for; anything further still unwritten between it and
            // `pos` is a real gap.
            if (auto result = zeroFillGrainsUpTo(pos); !result.ok()) {
                return result;
            }
        }
    }

    if (m_pendingGrainFilled == 0 && !remaining.empty()) {
        if (pos % m_grainSizeBytes != 0) {
            return Error(ErrorCode::InvalidArgument,
                         "GrainAssembler::consume: offset " + std::to_string(pos) + " is not grain-aligned");
        }
        if (auto result = zeroFillGrainsUpTo(pos); !result.ok()) {
            return result;
        }
        while (remaining.size() >= m_grainSizeBytes) {
            if (auto result = m_sink(pos, remaining.first(m_grainSizeBytes)); !result.ok()) {
                return result;
            }
            m_nextGrainOffset = pos + m_grainSizeBytes;
            pos += m_grainSizeBytes;
            remaining = remaining.subspan(m_grainSizeBytes);
        }
        if (!remaining.empty()) {
            m_pendingGrainOffset = pos;
            std::memcpy(m_pendingGrain.data(), remaining.data(), remaining.size());
            m_pendingGrainFilled = remaining.size();
        }
    }

    m_nextExpectedOffset = offset + in.size();
    return {};
}

Error GrainAssembler::finish()
{
    if (m_pendingGrainFilled > 0) {
        std::memset(m_pendingGrain.data() + m_pendingGrainFilled, 0, m_grainSizeBytes - m_pendingGrainFilled);
        const std::uint64_t grainOffset = m_pendingGrainOffset;
        m_pendingGrainFilled = 0;
        if (auto result = m_sink(grainOffset, std::span<const std::byte>(m_pendingGrain.data(), m_grainSizeBytes));
            !result.ok()) {
            return result;
        }
        m_nextGrainOffset = grainOffset + m_grainSizeBytes;
    }
    if (m_denseCapacityBytes) {
        return zeroFillGrainsUpTo(*m_denseCapacityBytes);
    }
    return {};
}

} // namespace VSphereSoap::Detail
