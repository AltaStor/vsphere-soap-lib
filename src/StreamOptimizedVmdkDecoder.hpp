// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// streamOptimized VMDK stream decoder

#pragma once

// The reverse of StreamOptimizedVmdkEncoder: turns the byte stream of an export lease's disk back into grains of
// disk content, as the bytes arrive.
// Only the forward-only parts of the format are read: the header, the grain markers and the end-of-stream marker.
// The grain tables, the grain directory and the footer are skipped, since a stream carries every grain inline.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "vsphere-soap/Result.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Result;

/// Receives one decoded grain: its logical byte offset and its content, a whole grain long.
using VmdkGrainSink = std::function<Error(std::uint64_t offset, std::span<const std::byte> data)>;

class StreamOptimizedVmdkDecoder {
public:
    explicit StreamOptimizedVmdkDecoder(VmdkGrainSink sink);

    /// Feeds the next bytes of the stream.
    /// Fails with `ErrorCode::InvalidFormat` on a stream that is not a streamOptimized VMDK, a grain that does not
    /// inflate, or grains that go backwards; with whatever the sink returns when it fails.
    Error feed(std::span<const std::byte> bytes);

    /// True once the end-of-stream marker was read.
    bool finished() const noexcept { return m_state == State::Done; }

    /// Logical disk size from the header; 0 until the header was read.
    std::uint64_t capacityBytes() const noexcept { return m_capacitySectors * kSectorSize; }
    std::uint64_t grainSizeBytes() const noexcept { return m_grainSectors * kSectorSize; }

    static constexpr std::uint64_t kSectorSize = 512;

private:
    enum class State {
        Header,   // the first sector
        Skip,     // bytes to pass over: the rest of the overhead, metadata, padding
        Marker,   // the 16 bytes at the start of a sector
        Grain,    // a grain's compressed bytes
        Done,
    };

    Error parseHeader();
    Error parseMarker();
    Error emitGrain();

    VmdkGrainSink m_sink;
    State m_state = State::Header;
    std::vector<std::byte> m_buffer; // what the current state still needs, collected across feeds
    std::uint64_t m_need = kSectorSize;
    std::uint64_t m_skip = 0;
    std::uint64_t m_position = 0; // stream bytes consumed so far
    std::uint64_t m_capacitySectors = 0;
    std::uint64_t m_grainSectors = 0;
    bool m_compressed = true;
    std::uint64_t m_grainLba = 0;
    std::uint32_t m_grainSize = 0;
    std::uint64_t m_nextOffset = 0; // the lowest offset the next grain may have
    std::vector<std::byte> m_inflated;
};

} // namespace VSphereSoap::Detail
