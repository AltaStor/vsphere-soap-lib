// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Random-access virtual disk interface

#pragma once

// Optional part of the library, built as the vsphere-soap-disk target.
// The core disk classes don't derive from it, so a tool with a disk interface of its own links only the core
// and adapts them to that interface instead.

#include <cstddef>
#include <cstdint>
#include <span>

#include "vsphere-soap/Result.hpp"

namespace VSphereSoap::Disk {

/// Random-access virtual disk.
/// Offsets and sizes are byte positions in the logical disk the guest sees, not in its container file.
/// Neither copyable nor movable: ownership passes through `std::unique_ptr<IDisk>`.
///
class IDisk {
public:
    IDisk() = default;
    IDisk(const IDisk&) = delete;
    IDisk& operator=(const IDisk&) = delete;
    IDisk(IDisk&&) = delete;
    IDisk& operator=(IDisk&&) = delete;
    virtual ~IDisk() = default;

    /// Reads up to `out.size()` bytes at `offset` and returns the count read.
    /// The count is short only at the end of the disk; unallocated regions read as zeros.
    /// Fails with `ErrorCode::IoError`.
    virtual Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out) = 0;

    /// Writes `in` at `offset` and returns the count written.
    /// Precondition: `offset + in.size() <= virtualSize()`.
    /// Fails with `ErrorCode::IoError`.
    virtual Result<std::uint64_t> writeAt(std::uint64_t offset, std::span<const std::byte> in) = 0;

    /// Logical size in bytes.
    virtual std::uint64_t virtualSize() const noexcept = 0;

    /// Sector size in bytes, typically 512 or 4096.
    virtual std::uint64_t sectorSize() const noexcept = 0;

    /// Makes buffered writes durable.
    /// Returns a default `Error` on success.
    virtual Error flush() = 0;

    /// Whether `[offset, offset + length)` reads as all zero.
    /// The default reads the range in chunks and compares it; formats with allocation metadata override it.
    /// Precondition: `offset + length <= virtualSize()`.
    virtual Result<bool> isZero(std::uint64_t offset, std::uint64_t length);
};

} // namespace VSphereSoap::Disk
