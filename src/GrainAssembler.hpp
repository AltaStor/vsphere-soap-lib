// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Assembler of writes into stream-optimized VMDK grains

#pragma once

// Turns a sequence of arbitrary-sized, arbitrary-offset writeAt()-shaped byte ranges into a sequence of
// exactly-grain-sized, grain-aligned `StreamOptimizedVmdkEncoder::writeGrain()` calls, matching how
// a disk copy loop drives an IDisk: sequential, non-overlapping offsets, with gaps where it skipped
// chunks its source reported as all zero.
// Split out of VSphereDatastoreBackend.cpp specifically so this arithmetic (the trickiest part of that
// class) is unit-testable without any HTTP/thread involvement, mirroring DatastorePath.hpp's own reason
// for existing.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "Error.hpp"

namespace VSphereSoap::Detail {

/// Called once per completed, exactly-grain-sized, grain-aligned grain.
/// `grainData` is only valid for the duration of the call (GrainAssembler reuses its internal buffer for
/// the next grain immediately after).
using GrainSink = std::function<Error(std::uint64_t offset, std::span<const std::byte> grainData)>;

/// See this header's own top comment.
/// Not thread-safe; not copyable/movable (owns an in-progress partial-grain buffer with no well-defined
/// mid-stream copy, matching StreamOptimizedVmdkEncoder's own stance).
class GrainAssembler {
public:
    /// `denseCapacityBytes`, when set, switches this instance from the default sparse mode (a gap
    /// between two `consume()` calls is simply never emitted; the caller's `sink` never even sees those
    /// grain offsets) to dense mode: every grain up to `*denseCapacityBytes` is guaranteed to reach
    /// `sink` exactly once, gaps included, filled with zeros.
    /// This exists for exactly one reason; see
    /// `StreamOptimizedVmdkEncoder::computeDenseStreamSizeBytes()`'s doc comment: an upload whose
    /// `Content-Length` must be known up front can only be sized correctly if the actual stream it later
    /// writes is unconditionally dense (skipping a real gap would make the precomputed length wrong).
    /// `*denseCapacityBytes` must be a multiple of `grainSizeBytes` (matching
    /// `StreamOptimizedVmdkEncoder::create`'s own precondition on `capacityBytes`) and must be >= every
    /// offset `consume()`/`finish()` will ever see, not checked here, since a violation surfaces
    /// naturally as `sink` being called past what its own caller (e.g. the encoder) considers valid.
    ///
    GrainAssembler(std::uint64_t grainSizeBytes, GrainSink sink,
                    std::optional<std::uint64_t> denseCapacityBytes = std::nullopt);
    GrainAssembler(const GrainAssembler&) = delete;
    GrainAssembler& operator=(const GrainAssembler&) = delete;
    GrainAssembler(GrainAssembler&&) = delete;
    GrainAssembler& operator=(GrainAssembler&&) = delete;

    /// Feeds one writeAt()-shaped byte range.
    /// Calls `sink` once per grain completed as a result (zero or more times: a small `in` that doesn't
    /// complete the grain it starts calls `sink` zero times).
    ///
    /// Fails with `ErrorCode::InvalidArgument` if `offset` goes backward or overlaps a previous call
    /// (`offset` must be >= every byte offset already consumed), or if `offset` is neither grain-aligned
    /// nor an exact contiguous continuation of an in-progress partial grain (see this header's top
    /// comment): callers whose chunk size isn't a grain-size multiple must still never skip a *partial*
    /// grain, only whole ones.
    ///
    Error consume(std::uint64_t offset, std::span<const std::byte> in);

    /// Zero-pads and emits any in-progress partial grain via `sink` (a no-op if `consume()` left nothing
    /// in progress); in dense mode, also zero-fills every remaining grain up to `*denseCapacityBytes`.
    /// Callers should only call this once, after all `consume()` calls are done: matches
    /// `StreamOptimizedVmdkEncoder::finish()`'s own one-shot contract, which this is meant to be paired
    /// with.
    ///
    Error finish();

    /// The lowest offset the next `consume()` may start at: the end of the last one (0 before any).
    std::uint64_t nextExpectedOffset() const noexcept { return m_nextExpectedOffset; }

private:
    /// Dense mode only: calls `sink` with a zero-filled grain for every grain boundary in
    /// `[m_nextGrainOffset, targetOffset)`, advancing `m_nextGrainOffset` past each one.
    /// A no-op in sparse mode.
    Error zeroFillGrainsUpTo(std::uint64_t targetOffset);

    std::uint64_t m_grainSizeBytes;
    GrainSink m_sink;
    std::optional<std::uint64_t> m_denseCapacityBytes;
    std::vector<std::byte> m_pendingGrain;
    std::size_t m_pendingGrainFilled = 0;
    std::uint64_t m_pendingGrainOffset = 0;
    std::uint64_t m_nextExpectedOffset = 0;
    /// Dense mode only: the offset of the next grain boundary not yet passed to `sink` (real or
    /// zero-filled).
    std::uint64_t m_nextGrainOffset = 0;
};

} // namespace VSphereSoap::Detail
