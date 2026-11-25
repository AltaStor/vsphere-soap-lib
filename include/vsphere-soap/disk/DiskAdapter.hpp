// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// IDisk adapter over the core disk classes

#pragma once

// The core disk classes share one method set (readAt, writeAt, virtualSize, sectorSize, flush, and isZero on
// some of them) but no base class.
// This adapter turns any of them into an IDisk.
// A tool with its own disk interface writes the same few lines against that interface, converting the
// library's Error into its own error type where they differ.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "vsphere-soap/disk/IDisk.hpp"
#include "vsphere-soap/Result.hpp"

namespace VSphereSoap::Disk {

/// The method set a class needs to be adapted.
template <class T>
concept DiskLike = requires(T& disk, const T& constDisk, std::uint64_t offset, std::span<std::byte> out,
                            std::span<const std::byte> in) {
    { disk.readAt(offset, out) } -> std::same_as<Result<std::uint64_t>>;
    { disk.writeAt(offset, in) } -> std::same_as<Result<std::uint64_t>>;
    { constDisk.virtualSize() } -> std::same_as<std::uint64_t>;
    { constDisk.sectorSize() } -> std::same_as<std::uint64_t>;
    { disk.flush() } -> std::same_as<Error>;
};

/// Owns a disk object and exposes it as an IDisk.
/// Its own `isZero` is used when it has one, otherwise the default read-and-compare.
template <DiskLike T>
class DiskAdapter final : public IDisk {
public:
    explicit DiskAdapter(std::unique_ptr<T> disk) noexcept : m_disk(std::move(disk)) {}

    Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out) override
    {
        return m_disk->readAt(offset, out);
    }

    Result<std::uint64_t> writeAt(std::uint64_t offset, std::span<const std::byte> in) override
    {
        return m_disk->writeAt(offset, in);
    }

    std::uint64_t virtualSize() const noexcept override { return m_disk->virtualSize(); }
    std::uint64_t sectorSize() const noexcept override { return m_disk->sectorSize(); }

    Error flush() override { return m_disk->flush(); }

    Result<bool> isZero(std::uint64_t offset, std::uint64_t length) override
    {
        if constexpr (requires { { m_disk->isZero(offset, length) } -> std::same_as<Result<bool>>; }) {
            return m_disk->isZero(offset, length);
        } else {
            return IDisk::isZero(offset, length);
        }
    }

    /// The wrapped object, for its own methods such as the upload keep-alive or the CBT state.
    T& disk() noexcept { return *m_disk; }
    const T& disk() const noexcept { return *m_disk; }

private:
    std::unique_ptr<T> m_disk;
};

/// Wraps the result of a disk class's `open()`, passing its error through unchanged.
template <DiskLike T>
Result<std::unique_ptr<DiskAdapter<T>>> adaptDisk(Result<std::unique_ptr<T>> opened)
{
    if (!opened) {
        return std::move(opened).error();
    }
    return std::make_unique<DiskAdapter<T>>(std::move(opened).value());
}

} // namespace VSphereSoap::Disk
