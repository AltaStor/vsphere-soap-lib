// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for the VMDK grain assembler

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "GrainAssembler.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Detail {
namespace {

using VSphereSoap::ErrorCode;

// 128 sectors * 512, matches the project's default grain size
constexpr std::uint64_t kGrainBytes = 65536;

std::vector<std::byte> pattern(std::size_t n, std::byte seed)
{
    std::vector<std::byte> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::byte>((static_cast<unsigned>(seed) + i) & 0xFF);
    }
    return out;
}

struct RecordedGrain {
    std::uint64_t offset;
    std::vector<std::byte> data;
};

class RecordingAssembler {
public:
    RecordingAssembler()
        : m_assembler(kGrainBytes, [this](std::uint64_t offset, std::span<const std::byte> data) -> Error {
              m_grains.push_back({offset, std::vector<std::byte>(data.begin(), data.end())});
              return {};
          }) {}

    std::vector<RecordedGrain> m_grains;
    GrainAssembler m_assembler;
};

TEST(GrainAssemblerTest, SingleFullGrainWriteEmitsOneGrain)
{
    RecordingAssembler r;
    const auto data = pattern(kGrainBytes, std::byte{1});
    ASSERT_TRUE(r.m_assembler.consume(0, data).ok());
    ASSERT_TRUE(r.m_assembler.finish().ok());

    ASSERT_EQ(r.m_grains.size(), 1u);
    EXPECT_EQ(r.m_grains[0].offset, 0u);
    EXPECT_EQ(r.m_grains[0].data, data);
}

TEST(GrainAssemblerTest, MultiGrainWriteInOneCallSplitsIntoSeparateGrains)
{
    RecordingAssembler r;
    const auto data = pattern(3 * kGrainBytes, std::byte{7});
    ASSERT_TRUE(r.m_assembler.consume(0, data).ok());
    ASSERT_TRUE(r.m_assembler.finish().ok());

    ASSERT_EQ(r.m_grains.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(r.m_grains[i].offset, i * kGrainBytes);
        EXPECT_EQ(r.m_grains[i].data,
                  std::vector<std::byte>(data.begin() + i * kGrainBytes, data.begin() + (i + 1) * kGrainBytes));
    }
}

TEST(GrainAssemblerTest, SmallWritesAccumulateAcrossCallsIntoOneGrain)
{
    RecordingAssembler r;
    const auto part1 = pattern(kGrainBytes / 2, std::byte{1});
    const auto part2 = pattern(kGrainBytes / 2, std::byte{2});
    ASSERT_TRUE(r.m_assembler.consume(0, part1).ok());
    EXPECT_TRUE(r.m_grains.empty()) << "a partial grain must not be emitted until it's complete";
    ASSERT_TRUE(r.m_assembler.consume(kGrainBytes / 2, part2).ok());

    ASSERT_EQ(r.m_grains.size(), 1u);
    EXPECT_EQ(r.m_grains[0].offset, 0u);
    std::vector<std::byte> expected = part1;
    expected.insert(expected.end(), part2.begin(), part2.end());
    EXPECT_EQ(r.m_grains[0].data, expected);
}

TEST(GrainAssemblerTest, GapAtGrainBoundaryIsSkippedNotEmitted)
{
    RecordingAssembler r;
    const auto grain0 = pattern(kGrainBytes, std::byte{1});
    const auto grain2 = pattern(kGrainBytes, std::byte{3});
    ASSERT_TRUE(r.m_assembler.consume(0, grain0).ok());
    // grain 1 (offset kGrainBytes) is skipped entirely: matches a copy loop never calling writeAt() for
    // a zero chunk.
    ASSERT_TRUE(r.m_assembler.consume(2 * kGrainBytes, grain2).ok());
    ASSERT_TRUE(r.m_assembler.finish().ok());

    ASSERT_EQ(r.m_grains.size(), 2u);
    EXPECT_EQ(r.m_grains[0].offset, 0u);
    EXPECT_EQ(r.m_grains[1].offset, 2 * kGrainBytes);
}

TEST(GrainAssemblerTest, GapAfterPartialGrainZeroPadsAndEmitsIt)
{
    RecordingAssembler r;
    const auto part1 = pattern(kGrainBytes / 4, std::byte{9}); // fills a quarter of grain 0
    ASSERT_TRUE(r.m_assembler.consume(0, part1).ok());
    // The rest of grain 0, plus all of grain 1, is skipped as zero: next real data starts at grain 2's
    // boundary.
    const auto grain2 = pattern(kGrainBytes, std::byte{5});
    ASSERT_TRUE(r.m_assembler.consume(2 * kGrainBytes, grain2).ok());
    ASSERT_TRUE(r.m_assembler.finish().ok());

    ASSERT_EQ(r.m_grains.size(), 2u);
    EXPECT_EQ(r.m_grains[0].offset, 0u);
    std::vector<std::byte> expectedGrain0 = part1;
    expectedGrain0.resize(kGrainBytes, std::byte{0});
    EXPECT_EQ(r.m_grains[0].data, expectedGrain0) << "grain 0 must be real data followed by zero padding, not skipped "
                                                    "entirely -- it did contain some real bytes";
    EXPECT_EQ(r.m_grains[1].offset, 2 * kGrainBytes);
}

TEST(GrainAssemblerTest, FinishZeroPadsTrailingPartialGrain)
{
    RecordingAssembler r;
    const auto part = pattern(kGrainBytes / 3, std::byte{4});
    ASSERT_TRUE(r.m_assembler.consume(0, part).ok());
    ASSERT_TRUE(r.m_assembler.finish().ok());

    ASSERT_EQ(r.m_grains.size(), 1u);
    EXPECT_EQ(r.m_grains[0].offset, 0u);
    std::vector<std::byte> expected = part;
    expected.resize(kGrainBytes, std::byte{0});
    EXPECT_EQ(r.m_grains[0].data, expected);
}

TEST(GrainAssemblerTest, FinishWithNothingPendingEmitsNothing)
{
    RecordingAssembler r;
    const auto data = pattern(kGrainBytes, std::byte{1});
    ASSERT_TRUE(r.m_assembler.consume(0, data).ok());
    ASSERT_EQ(r.m_grains.size(), 1u);
    ASSERT_TRUE(r.m_assembler.finish().ok());
    EXPECT_EQ(r.m_grains.size(), 1u) << "finish() must not re-emit or duplicate an already-completed grain";
}

TEST(GrainAssemblerTest, RejectsBackwardOffset)
{
    RecordingAssembler r;
    const auto data = pattern(kGrainBytes, std::byte{1});
    ASSERT_TRUE(r.m_assembler.consume(kGrainBytes, data).ok());
    auto result = r.m_assembler.consume(0, data);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
}

TEST(GrainAssemblerTest, RejectsOverlappingOffset)
{
    RecordingAssembler r;
    const auto data = pattern(kGrainBytes, std::byte{1});
    ASSERT_TRUE(r.m_assembler.consume(0, data).ok());
    // overlaps the tail of what was just consumed
    auto result = r.m_assembler.consume(kGrainBytes / 2, data);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
}

TEST(GrainAssemblerTest, RejectsNonGrainAlignedGapAfterPartialGrain)
{
    RecordingAssembler r;
    const auto part1 = pattern(kGrainBytes / 4, std::byte{1});
    ASSERT_TRUE(r.m_assembler.consume(0, part1).ok());
    // Jump to a misaligned offset that's neither a contiguous continuation nor a grain boundary.
    const auto data = pattern(kGrainBytes, std::byte{2});
    auto result = r.m_assembler.consume(kGrainBytes + 1, data);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
}

TEST(GrainAssemblerTest, RejectsNonGrainAlignedFreshOffset)
{
    RecordingAssembler r;
    const auto data = pattern(kGrainBytes, std::byte{1});
    auto result = r.m_assembler.consume(1, data);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
}

TEST(GrainAssemblerTest, EmptyInputIsANoOp)
{
    RecordingAssembler r;
    EXPECT_TRUE(r.m_assembler.consume(0, {}).ok());
    EXPECT_TRUE(r.m_grains.empty());
}

// --- dense mode (denseCapacityBytes set): every grain must reach the sink, gaps zero-filled rather than
// omitted; see GrainAssembler.hpp's own doc comment on why this exists (a precomputed Content-Length
// upload can't tolerate a sparse-skipped gap changing the actual byte count).

class DenseRecordingAssembler {
public:
    explicit DenseRecordingAssembler(std::uint64_t capacityBytes)
        : m_assembler(kGrainBytes,
                     [this](std::uint64_t offset, std::span<const std::byte> data) -> Error {
                         m_grains.push_back({offset, std::vector<std::byte>(data.begin(), data.end())});
                         return {};
                     },
                     capacityBytes) {}

    std::vector<RecordedGrain> m_grains;
    GrainAssembler m_assembler;
};

TEST(GrainAssemblerTest, DenseModeFillsLeadingGapWithZeros)
{
    DenseRecordingAssembler r(3 * kGrainBytes);
    const auto grain2 = pattern(kGrainBytes, std::byte{9});
    // Grains 0 and 1 are skipped by the caller (e.g. a zero-region source chunk): dense mode must still
    // emit them, zero-filled.
    ASSERT_TRUE(r.m_assembler.consume(2 * kGrainBytes, grain2).ok());
    ASSERT_TRUE(r.m_assembler.finish().ok());

    ASSERT_EQ(r.m_grains.size(), 3u);
    EXPECT_EQ(r.m_grains[0].offset, 0u);
    EXPECT_EQ(r.m_grains[0].data, std::vector<std::byte>(kGrainBytes, std::byte{0}));
    EXPECT_EQ(r.m_grains[1].offset, kGrainBytes);
    EXPECT_EQ(r.m_grains[1].data, std::vector<std::byte>(kGrainBytes, std::byte{0}));
    EXPECT_EQ(r.m_grains[2].offset, 2 * kGrainBytes);
    EXPECT_EQ(r.m_grains[2].data, grain2);
}

TEST(GrainAssemblerTest, DenseModeFillsGapBetweenPartialGrainAndNextWrite)
{
    DenseRecordingAssembler r(4 * kGrainBytes);
    const auto part = pattern(kGrainBytes / 4, std::byte{1}); // partially fills grain 0
    ASSERT_TRUE(r.m_assembler.consume(0, part).ok());
    const auto grain3 = pattern(kGrainBytes, std::byte{5});
    // Grains 1 and 2 are skipped entirely; grain 0 is only partially real.
    ASSERT_TRUE(r.m_assembler.consume(3 * kGrainBytes, grain3).ok());
    ASSERT_TRUE(r.m_assembler.finish().ok());

    ASSERT_EQ(r.m_grains.size(), 4u);
    std::vector<std::byte> expectedGrain0 = part;
    expectedGrain0.resize(kGrainBytes, std::byte{0});
    EXPECT_EQ(r.m_grains[0].data, expectedGrain0);
    EXPECT_EQ(r.m_grains[1].data, std::vector<std::byte>(kGrainBytes, std::byte{0}));
    EXPECT_EQ(r.m_grains[2].data, std::vector<std::byte>(kGrainBytes, std::byte{0}));
    EXPECT_EQ(r.m_grains[3].data, grain3);
}

TEST(GrainAssemblerTest, DenseModeFillsTrailingGapOnFinish)
{
    DenseRecordingAssembler r(3 * kGrainBytes);
    const auto grain0 = pattern(kGrainBytes, std::byte{2});
    ASSERT_TRUE(r.m_assembler.consume(0, grain0).ok());
    // Nothing ever written for grains 1/2: finish() must still emit them.
    ASSERT_TRUE(r.m_assembler.finish().ok());

    ASSERT_EQ(r.m_grains.size(), 3u);
    EXPECT_EQ(r.m_grains[0].data, grain0);
    EXPECT_EQ(r.m_grains[1].data, std::vector<std::byte>(kGrainBytes, std::byte{0}));
    EXPECT_EQ(r.m_grains[2].data, std::vector<std::byte>(kGrainBytes, std::byte{0}));
}

TEST(GrainAssemblerTest, DenseModeAllGrainsWrittenMatchesSparseModeExactly)
{
    // When nothing is ever skipped, dense and sparse modes must behave identically: dense mode isn't
    // supposed to change anything about the *written* grains, only about what happens to the gaps.
    DenseRecordingAssembler dense(2 * kGrainBytes);
    RecordingAssembler sparse;
    const auto grain0 = pattern(kGrainBytes, std::byte{1});
    const auto grain1 = pattern(kGrainBytes, std::byte{2});
    ASSERT_TRUE(dense.m_assembler.consume(0, grain0).ok());
    ASSERT_TRUE(dense.m_assembler.consume(kGrainBytes, grain1).ok());
    ASSERT_TRUE(dense.m_assembler.finish().ok());
    ASSERT_TRUE(sparse.m_assembler.consume(0, grain0).ok());
    ASSERT_TRUE(sparse.m_assembler.consume(kGrainBytes, grain1).ok());
    ASSERT_TRUE(sparse.m_assembler.finish().ok());

    ASSERT_EQ(dense.m_grains.size(), sparse.m_grains.size());
    for (std::size_t i = 0; i < dense.m_grains.size(); ++i) {
        EXPECT_EQ(dense.m_grains[i].offset, sparse.m_grains[i].offset);
        EXPECT_EQ(dense.m_grains[i].data, sparse.m_grains[i].data);
    }
}

TEST(GrainAssemblerTest, SinkFailurePropagates)
{
    int calls = 0;
    GrainAssembler assembler(kGrainBytes, [&](std::uint64_t, std::span<const std::byte>) -> Error {
        ++calls;
        return VSphereSoap::Error(ErrorCode::IoError, "simulated sink failure");
    });
    const auto data = pattern(kGrainBytes, std::byte{1});
    auto result = assembler.consume(0, data);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), ErrorCode::IoError);
    EXPECT_EQ(calls, 1);
}

} // namespace
} // namespace VSphereSoap::Detail
