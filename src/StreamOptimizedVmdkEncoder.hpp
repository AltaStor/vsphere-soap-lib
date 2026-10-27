// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Stream-optimized VMDK streaming encoder

#pragma once

// Streaming encoder for VMware's "streamOptimized" sparse VMDK format (VMware "Virtual Disk Format 5.0"
// spec): the one format an HttpNfcLease upload endpoint accepts.
// A flat or plain sparse VMDK is rejected there; this is what ovftool/govc build under the hood before
// PUTting to a lease device URL.
//
// On-disk shape (all multi-byte integers little-endian, everything sector (512-byte) aligned):
//
//   [SparseExtentHeader, 1 sector]             placeholder gdOffset/etc., not known until the
//                                              stream ends
//   [grain marker + deflated data]*            one per non-all-zero grain, in ascending LBA order
//   [grain table marker + grain table data]*   one grain table per 512 consecutive grains
//                                              (numGTEsPerGT)
//   [grain directory marker + directory data]  one entry per grain table
//   [footer marker + SparseExtentHeader]       corrected copy of the header with the real
//                                              gdOffset/overHead
//   [EOS marker]
//
// A grain not written at all (never passed to `writeGrain`) is treated as all-zero and simply never gets
// a grain-table entry pointing at real data (VMware's SPARSE_GTE_EMPTY sentinel, 0); this encoder never
// emits a marker for an unwritten or all-zero grain, matching the "all-zero grains are skipped"
// requirement: skip is not an optimization here, it is the only way an unwritten grain is representable
// at all in this format.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Result.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Result;

/// `writeGrain`'s output sink: raw bytes to append to the stream, already laid out (marker + deflated
/// payload + padding, or table/footer sectors); the encoder never seeks backwards, so this can be handed
/// straight to an HTTPS PUT's chunked body, a file, or anything else append-only.
using VmdkByteSink = std::function<Error(std::span<const std::byte>)>;

/// One 64 KiB-by-default grain of *logical* (decompressed, guest-visible) disk content and where it
/// belongs.
struct VmdkGrain {
    /// Grain-aligned logical byte offset (i.e. `lba * kSectorSize`, and `lba` must be a multiple of
    /// `grainSize` in sectors).
    /// Grains must be written in strictly ascending offset order: the encoder has no buffering to
    /// reorder them.
    ///
    std::uint64_t offset = 0;
    /// Exactly the encoder's grain size in bytes (`grainSizeSectors * 512`), including for the disk's
    /// final grain: callers pad the logical content with zeros up to the grain boundary themselves
    /// rather than the encoder guessing at a short final grain's real size, since a short last grain has
    /// no representation in this format regardless.
    ///
    std::span<const std::byte> data;
};

/// Streams one virtual disk's content into a "streamOptimized" sparse VMDK byte stream via `sink`.
/// Construct, call `writeGrain` in ascending-offset order for every grain that isn't entirely zero
/// (an all-zero grain is simply omitted: do not call `writeGrain` for it), then `finish()` exactly once.
/// Neither `writeGrain` nor `finish` may be called again after `finish` returns (successfully or not).
///
/// Not thread-safe; not copyable/movable (holds streaming state that has no well-defined mid-stream
/// copy).
///
class StreamOptimizedVmdkEncoder {
public:
    /// Sector size this format always uses (fixed by the spec, not a tuning knob).
    static constexpr std::uint64_t kSectorSizeBytes = 512;

    /// Default grain size: 128 sectors = 64 KiB, matching the task's own spec and what ESXi's own tools
    /// produce.
    /// Must be a power of two (spec requirement); this encoder does not validate arbitrary
    /// caller-supplied grain sizes beyond that, since every call site in this project uses the default.
    ///
    static constexpr std::uint64_t kDefaultGrainSizeSectors = 128;

    /// `capacityBytes` is the *logical* (guest-visible) disk size; must be a multiple of the grain size
    /// in bytes (callers own padding a source disk up to that boundary; this encoder never invents
    /// padding grains of its own beyond what a caller explicitly writes).
    /// `descriptor` is the embedded VMDK descriptor text (the small "# Disk DescriptorFile" section
    /// every VMDK carries, describing geometry/adapter type); callers own building one that's consistent
    /// with `capacityBytes` and whatever adapter type the destination VM's disk device actually is.
    /// Writes the initial placeholder header to `sink` before returning.
    /// `compressGrains` picks the on-disk `compressAlgorithm` (deflate vs none).
    /// Only `true` produces a standard streamOptimized VMDK, and every production caller passes it.
    ///
    /// `false` (still the default, for historical reasons) is NON-STANDARD: VMware's spec defines
    /// streamOptimized grains as compressed only, yet this mode still writes each grain behind a GRAIN
    /// marker (12-byte header + raw data) with the grain table pointing at that marker.
    /// Consequences:
    ///  - ESXi's HttpNfcLease endpoint rejects the upload once fully received (HTTP 500 "Not a supported
    ///    disk format (uncompressed disk)"; confirmed live, see VSphereDatastoreBackend.cpp's open()).
    ///  - qemu-img treats an uncompressed grain-table entry as pointing at raw data, so it reads
    ///    the marker header as disk content: every grain comes back shifted by 12 bytes, with no error
    ///    and a clean `qemu-img check`.
    ///    Silent corruption, not a rejection.
    /// Kept for tests and computeDenseStreamSizeBytes() only; never hand its output to a third-party
    /// tool.
    ///
    static Result<StreamOptimizedVmdkEncoder> create(VmdkByteSink sink, std::uint64_t capacityBytes,
                                                                       std::string descriptor,
                                                                       std::uint64_t grainSizeSectors =
                                                                           kDefaultGrainSizeSectors,
                                                                       bool compressGrains = false);

    StreamOptimizedVmdkEncoder(StreamOptimizedVmdkEncoder&&) noexcept;
    StreamOptimizedVmdkEncoder& operator=(StreamOptimizedVmdkEncoder&&) noexcept;
    StreamOptimizedVmdkEncoder(const StreamOptimizedVmdkEncoder&) = delete;
    StreamOptimizedVmdkEncoder& operator=(const StreamOptimizedVmdkEncoder&) = delete;
    ~StreamOptimizedVmdkEncoder();

    /// Compresses `grain.data` first if this encoder was created with `compressGrains = true`, otherwise
    /// uses it as-is; either way emits a grain marker (LBA + the emitted size) followed by those bytes,
    /// padded with zeros out to the next sector boundary.
    /// Fails with `ErrorCode::InvalidArgument` if `grain.offset` isn't grain-aligned, isn't strictly
    /// greater than the previous grain's offset, or `grain.data.size()` isn't exactly one grain;
    /// `ErrorCode::IoError` if `sink` fails.
    ///
    Error writeGrain(const VmdkGrain& grain);

    /// Emits the grain table(s), grain directory, footer (a corrected copy of the header with the real
    /// `gdOffset`/`overHead` now known), and the final EOS marker.
    /// After this returns (success or failure), no further calls are valid.
    Error finish();

    /// The exact total byte size of a stream this encoder would produce for
    /// `capacityBytes`/`descriptor`/`grainSizeSectors` with `compressGrains = false` and *every* grain
    /// written (none skipped as all-zero), i.e. a fully dense, uncompressed stream, whose size is
    /// therefore a pure function of these three inputs, not of any actual grain content.
    /// This exists for exactly one reason: an HTTP PUT that needs its `Content-Length` known before
    /// the first byte goes out (confirmed live against a real ESXi HttpNfcLease endpoint to be
    /// necessary; it resets the connection on a chunked-encoded PUT; see
    /// `VSphereSoap::Detail::HttpsRestClient::requestPutStream`'s doc comment).
    /// Computed by actually running a real encoder instance over zero-filled grains into a byte-counting
    /// sink, rather than reimplementing this class's sector arithmetic a second time separately: correct
    /// by construction, at the cost of a cheap dry run (no real data touched, just counting).
    /// Same failure modes as `create()`.
    ///
    static Result<std::uint64_t> computeDenseStreamSizeBytes(std::uint64_t capacityBytes,
                                                                              const std::string& descriptor,
                                                                              std::uint64_t grainSizeSectors =
                                                                                  kDefaultGrainSizeSectors);

private:
    class Impl;
    explicit StreamOptimizedVmdkEncoder(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VSphereSoap::Detail
