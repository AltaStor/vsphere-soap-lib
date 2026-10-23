// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Random-access virtual disk interface

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Result.hpp"

namespace VSphereSoap {

/// Random-access block device abstraction over a virtual machine disk image, regardless of on-disk
/// container format (raw, VMDK, VHDX, QCOW2, ...).
///
/// An `IDisk` owns whatever OS resource backs it (typically a file descriptor), so it is neither
/// copyable nor movable: copying would require duplicating that resource with format-specific semantics,
/// and moving would leave call sites holding a reference/pointer to a logically relocated object.
/// Callers that need to hand off ownership should do so through the owning `std::unique_ptr<IDisk>` (see
/// `IDiskFactory`), not by moving or cloning the `IDisk` itself.
///
/// All offsets and sizes are absolute byte positions into the *virtual* (logical) disk, not into
/// whatever container format stores it on disk.
///
class IDisk {
public:
    IDisk() = default;
    IDisk(const IDisk&) = delete;
    IDisk& operator=(const IDisk&) = delete;
    IDisk(IDisk&&) = delete;
    IDisk& operator=(IDisk&&) = delete;
    virtual ~IDisk() = default;

    /// Reads bytes starting at `offset` into `out`, reading up to `out.size()` bytes.
    /// Returns the number of bytes actually read, which is less than `out.size()` only when the read
    /// reaches the end of the virtual disk (analogous to a short `pread`).
    /// Reading a region that is unallocated in the underlying format (a sparse hole) succeeds and yields
    /// zero bytes, not an error.
    ///
    /// Precondition: `offset <= virtualSize()`.
    /// Does not throw; I/O failures are reported as `Result` holding an `Error` with
    /// `ErrorCode::IoError`.
    ///
    virtual Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out) = 0;

    /// Writes `in.size()` bytes from `in` starting at `offset`.
    /// Returns the number of bytes actually written; implementations should only return less than
    /// `in.size()` when a hard I/O limit was hit, not as a normal outcome, since callers generally
    /// cannot make progress on a short write the way they can on a short read.
    ///
    /// Precondition: `offset + in.size() <= virtualSize()`.
    /// Does not throw; I/O failures are reported as `Result` holding an `Error` with
    /// `ErrorCode::IoError`.
    ///
    virtual Result<std::uint64_t> writeAt(std::uint64_t offset, std::span<const std::byte> in) = 0;

    /// The logical size of the disk in bytes, i.e. the size a guest OS would see.
    /// For sparse formats this is independent of how much space the image currently occupies on the host
    /// filesystem.
    [[nodiscard]] virtual std::uint64_t virtualSize() const noexcept = 0;

    /// The disk's sector size in bytes (typically 512 or 4096).
    /// Offsets passed to `readAt`/`writeAt` are byte offsets, not sector indices; this value is exposed
    /// for callers that need to align I/O or reason about the guest's block size.
    [[nodiscard]] virtual std::uint64_t sectorSize() const noexcept = 0;

    /// Flushes any buffered writes so they are durable (e.g. `fsync`).
    /// Does not throw; failures are reported as `Result` holding an `Error` with `ErrorCode::IoError`.
    virtual Result<void> flush() = 0;

    /// Reports whether the byte range `[offset, offset + length)` reads as all zero, without requiring
    /// the caller to allocate a buffer for the whole range.
    /// Lets a copy loop skip writing sparse regions to the destination instead of materializing zero
    /// runs.
    ///
    /// The base implementation reads the range in chunks via `readAt` and compares against zero; it is
    /// always correct but does a full read.
    /// Formats with allocation metadata (QCOW2 L1/L2 tables, VHDX BAT) should override this to answer
    /// from metadata alone, without touching the data region, whenever the range is provably
    /// unallocated.
    ///
    /// Precondition: `offset + length <= virtualSize()`.
    /// Does not throw; I/O failures are reported as `Result` holding an `Error` with
    /// `ErrorCode::IoError`.
    ///
    virtual Result<bool> isZero(std::uint64_t offset, std::uint64_t length);
};

} // namespace VSphereSoap
