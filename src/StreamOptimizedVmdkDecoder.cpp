// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// streamOptimized VMDK stream decoder

#include "StreamOptimizedVmdkDecoder.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include <zlib.h>

#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

namespace {

constexpr std::uint32_t kMagic = 0x564d444b; // "KDMV"
constexpr std::uint32_t kFlagCompressedGrains = 0x10000;
constexpr std::uint32_t kFlagHasMarkers = 0x20000;
constexpr std::uint32_t kMarkerEos = 0;
constexpr std::size_t kMarkerBytes = 16; // val, size, and the type of a metadata marker
constexpr std::size_t kGrainMarkerBytes = 12;

// Grain size bounds: the format allows any power of two of at least 8 sectors; 2048 sectors (1 MiB) is far above
// anything VMware writes and keeps a corrupt header from asking for a huge buffer.
constexpr std::uint64_t kMinGrainSectors = 8;
constexpr std::uint64_t kMaxGrainSectors = 2048;

template <typename T>
T readLe(const std::vector<std::byte>& buffer, std::size_t at)
{
    T value{};
    std::memcpy(&value, buffer.data() + at, sizeof(T)); // the library only targets little-endian hosts
    return value;
}

std::uint64_t roundUpToSector(std::uint64_t bytes)
{
    return (bytes + StreamOptimizedVmdkDecoder::kSectorSize - 1) / StreamOptimizedVmdkDecoder::kSectorSize *
           StreamOptimizedVmdkDecoder::kSectorSize;
}

} // namespace

StreamOptimizedVmdkDecoder::StreamOptimizedVmdkDecoder(VmdkGrainSink sink) : m_sink(std::move(sink)) {}

Error StreamOptimizedVmdkDecoder::feed(std::span<const std::byte> bytes)
{
    while (!bytes.empty() && m_state != State::Done) {
        if (m_state == State::Skip) {
            const std::uint64_t take = std::min<std::uint64_t>(m_skip, bytes.size());
            m_skip -= take;
            m_position += take;
            bytes = bytes.subspan(take);
            if (m_skip == 0) {
                m_state = State::Marker;
                m_need = kMarkerBytes;
            }
            continue;
        }

        const std::uint64_t take = std::min<std::uint64_t>(m_need - m_buffer.size(), bytes.size());
        m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(take));
        m_position += take;
        bytes = bytes.subspan(take);
        if (m_buffer.size() < m_need) {
            break;
        }

        Error step;
        switch (m_state) {
        case State::Header:
            step = parseHeader();
            break;
        case State::Marker:
            step = parseMarker();
            break;
        case State::Grain:
            step = emitGrain();
            break;
        case State::Skip:
        case State::Done:
            break;
        }
        if (!step.ok()) {
            return step;
        }
    }
    return {};
}

Error StreamOptimizedVmdkDecoder::parseHeader()
{
    const auto magic = readLe<std::uint32_t>(m_buffer, 0);
    const auto flags = readLe<std::uint32_t>(m_buffer, 8);
    m_capacitySectors = readLe<std::uint64_t>(m_buffer, 12);
    m_grainSectors = readLe<std::uint64_t>(m_buffer, 20);
    const auto overHead = readLe<std::uint64_t>(m_buffer, 64);
    m_buffer.clear();

    if (magic != kMagic) {
        return Error(ErrorCode::InvalidFormat, "export stream is not a sparse VMDK (no KDMV magic)");
    }
    if ((flags & kFlagHasMarkers) == 0) {
        return Error(ErrorCode::InvalidFormat, "export stream is a sparse VMDK without markers, not streamOptimized");
    }
    if (m_grainSectors < kMinGrainSectors || m_grainSectors > kMaxGrainSectors ||
        (m_grainSectors & (m_grainSectors - 1)) != 0) {
        return Error(ErrorCode::InvalidFormat,
                     "export stream has an unsupported grain size of " + std::to_string(m_grainSectors) + " sectors");
    }
    if (overHead == 0) {
        return Error(ErrorCode::InvalidFormat, "export stream header has no overhead before its grains");
    }
    m_compressed = (flags & kFlagCompressedGrains) != 0;
    m_inflated.assign(grainSizeBytes(), std::byte{0});

    m_state = State::Skip;
    m_skip = overHead * kSectorSize - kSectorSize;
    if (m_skip == 0) {
        m_state = State::Marker;
        m_need = kMarkerBytes;
    }
    return {};
}

Error StreamOptimizedVmdkDecoder::parseMarker()
{
    const auto val = readLe<std::uint64_t>(m_buffer, 0);
    const auto size = readLe<std::uint32_t>(m_buffer, 8);
    const auto type = readLe<std::uint32_t>(m_buffer, 12);

    if (size != 0) {
        // A grain: its compressed bytes start right after the 12-byte marker, so the last 4 bytes read are already
        // part of them.
        if (size < kMarkerBytes - kGrainMarkerBytes || size > grainSizeBytes() + kSectorSize * 2) {
            return Error(ErrorCode::InvalidFormat, "export stream has a grain marker of implausible size " +
                                                       std::to_string(size) + " at stream offset " +
                                                       std::to_string(m_position - kMarkerBytes));
        }
        m_grainLba = val;
        m_grainSize = size;
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + kGrainMarkerBytes);
        m_state = State::Grain;
        m_need = size;
        if (m_buffer.size() >= m_need) {
            return emitGrain();
        }
        return {};
    }

    m_buffer.clear();
    if (type == kMarkerEos) {
        m_state = State::Done;
        return {};
    }
    // Grain table, grain directory or footer: the rest of the marker's sector, then `val` sectors of metadata.
    m_state = State::Skip;
    m_skip = kSectorSize - kMarkerBytes + val * kSectorSize;
    return {};
}

Error StreamOptimizedVmdkDecoder::emitGrain()
{
    const std::uint64_t offset = m_grainLba * kSectorSize;
    if (offset < m_nextOffset) {
        return Error(ErrorCode::InvalidFormat, "export stream grains go backwards: grain at " + std::to_string(offset) +
                                                   " after one ending at " + std::to_string(m_nextOffset));
    }

    std::fill(m_inflated.begin(), m_inflated.end(), std::byte{0});
    if (m_compressed) {
        uLongf inflatedSize = static_cast<uLongf>(m_inflated.size());
        const int rc = uncompress(reinterpret_cast<Bytef*>(m_inflated.data()), &inflatedSize,
                                  reinterpret_cast<const Bytef*>(m_buffer.data()), static_cast<uLong>(m_grainSize));
        if (rc != Z_OK) {
            return Error(ErrorCode::InvalidFormat, "export stream grain at " + std::to_string(offset) +
                                                       " does not inflate (zlib code " + std::to_string(rc) + ")");
        }
    } else {
        std::memcpy(m_inflated.data(), m_buffer.data(), std::min<std::size_t>(m_grainSize, m_inflated.size()));
    }
    m_buffer.clear();

    // The marker and its compressed bytes are padded to a whole sector.
    const std::uint64_t used = kGrainMarkerBytes + m_grainSize;
    m_skip = roundUpToSector(used) - used;
    m_state = m_skip == 0 ? State::Marker : State::Skip;
    m_need = kMarkerBytes;
    m_nextOffset = offset + grainSizeBytes();

    return m_sink(offset, m_inflated);
}

} // namespace VSphereSoap::Detail
