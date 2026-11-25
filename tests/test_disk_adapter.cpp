// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// IDisk adapter over classes with the core disk method set

#include <gtest/gtest.h>

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "vsphere-soap/disk/DiskAdapter.hpp"

namespace {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;
using VSphereSoap::Result;
using VSphereSoap::Disk::DiskAdapter;
using VSphereSoap::Disk::IDisk;
using VSphereSoap::Disk::adaptDisk;

// In-memory disk with the core method set and no isZero of its own.
class MemoryDisk {
public:
    explicit MemoryDisk(std::size_t size) : m_bytes(size) {}

    Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out)
    {
        ++readCalls;
        const std::uint64_t count = std::min<std::uint64_t>(out.size(), m_bytes.size() - offset);
        std::copy_n(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset), count, out.begin());
        return count;
    }

    Result<std::uint64_t> writeAt(std::uint64_t offset, std::span<const std::byte> in)
    {
        std::copy(in.begin(), in.end(), m_bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        return in.size();
    }

    std::uint64_t virtualSize() const noexcept { return m_bytes.size(); }
    std::uint64_t sectorSize() const noexcept { return 512; }

    Error flush() { return flushError; }

    int readCalls = 0;
    Error flushError;

private:
    std::vector<std::byte> m_bytes;
};

// Same, but answers isZero itself without reading.
class MetadataDisk : public MemoryDisk {
public:
    using MemoryDisk::MemoryDisk;

    Result<bool> isZero(std::uint64_t /*offset*/, std::uint64_t /*length*/) { return false; }
};

TEST(DiskAdapterTest, ForwardsReadsWritesAndSizes)
{
    DiskAdapter<MemoryDisk> adapter(std::make_unique<MemoryDisk>(4096));
    IDisk& disk = adapter;

    const std::vector<std::byte> pattern{std::byte{1}, std::byte{2}, std::byte{3}};
    auto written = disk.writeAt(100, pattern);
    ASSERT_TRUE(written);
    EXPECT_EQ(written.value(), 3u);

    std::vector<std::byte> back(3);
    auto read = disk.readAt(100, back);
    ASSERT_TRUE(read);
    EXPECT_EQ(read.value(), 3u);
    EXPECT_EQ(back, pattern);

    EXPECT_EQ(disk.virtualSize(), 4096u);
    EXPECT_EQ(disk.sectorSize(), 512u);
}

TEST(DiskAdapterTest, DefaultIsZeroReadsAndCompares)
{
    DiskAdapter<MemoryDisk> adapter(std::make_unique<MemoryDisk>(3 * 1024 * 1024));
    IDisk& disk = adapter;

    auto empty = disk.isZero(0, disk.virtualSize());
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty.value());
    EXPECT_EQ(adapter.disk().readCalls, 3); // 1 MiB chunks

    const std::byte one{1};
    ASSERT_TRUE(disk.writeAt(2 * 1024 * 1024 + 5, std::span(&one, 1)));
    auto dirty = disk.isZero(0, disk.virtualSize());
    ASSERT_TRUE(dirty);
    EXPECT_FALSE(dirty.value());
}

TEST(DiskAdapterTest, UsesOwnIsZeroWhenPresent)
{
    DiskAdapter<MetadataDisk> adapter(std::make_unique<MetadataDisk>(4096));
    IDisk& disk = adapter;

    auto zero = disk.isZero(0, 4096);
    ASSERT_TRUE(zero);
    EXPECT_FALSE(zero.value());
    EXPECT_EQ(adapter.disk().readCalls, 0);
}

TEST(DiskAdapterTest, PassesFlushErrorThrough)
{
    auto memory = std::make_unique<MemoryDisk>(512);
    memory->flushError = Error(ErrorCode::IoError, "datastore answered 500");
    DiskAdapter<MemoryDisk> adapter(std::move(memory));

    const Error error = adapter.flush();
    EXPECT_FALSE(error.ok());
    EXPECT_EQ(error.code(), ErrorCode::IoError);
    EXPECT_EQ(error.message(), "datastore answered 500");
}

TEST(DiskAdapterTest, AdaptDiskKeepsOpenErrors)
{
    Result<std::unique_ptr<MemoryDisk>> failed = Error(ErrorCode::InvalidArgument, "bad URL");
    auto adapted = adaptDisk(std::move(failed));
    ASSERT_FALSE(adapted);
    EXPECT_EQ(adapted.error().code(), ErrorCode::InvalidArgument);

    auto opened = adaptDisk(Result<std::unique_ptr<MemoryDisk>>(std::make_unique<MemoryDisk>(1024)));
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened.value()->virtualSize(), 1024u);
}

} // namespace
