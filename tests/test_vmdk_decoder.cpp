// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for the streamOptimized VMDK decoder

#include <cstring>
#include <map>
#include <vector>

#include <gtest/gtest.h>

#include "StreamOptimizedVmdkDecoder.hpp"
#include "StreamOptimizedVmdkEncoder.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Detail {
namespace {

using VSphereSoap::ErrorCode;

constexpr std::uint64_t kGrainBytes = StreamOptimizedVmdkEncoder::kDefaultGrainSizeSectors * 512;
constexpr std::uint64_t kCapacity = 16 * kGrainBytes;

std::vector<std::byte> pattern(std::size_t n, unsigned seed)
{
    std::vector<std::byte> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::byte>((seed * 31 + i * 7) & 0xFF);
    }
    return out;
}

// A whole stream from the encoder, with data in the grains given.
std::vector<std::byte> encode(const std::map<std::uint64_t, std::vector<std::byte>>& grains, bool compress)
{
    std::vector<std::byte> stream;
    auto encoder = StreamOptimizedVmdkEncoder::create(
        [&](std::span<const std::byte> bytes) -> Error {
            stream.insert(stream.end(), bytes.begin(), bytes.end());
            return {};
        },
        kCapacity, "# Disk DescriptorFile\n", StreamOptimizedVmdkEncoder::kDefaultGrainSizeSectors, compress);
    EXPECT_TRUE(encoder);
    for (const auto& [offset, data] : grains) {
        EXPECT_TRUE(encoder.value().writeGrain(VmdkGrain{offset, data}).ok());
    }
    EXPECT_TRUE(encoder.value().finish().ok());
    return stream;
}

// Feeds `stream` in pieces of `step` bytes and collects what comes out.
std::map<std::uint64_t, std::vector<std::byte>> decode(const std::vector<std::byte>& stream, std::size_t step,
                                                       bool& finished)
{
    std::map<std::uint64_t, std::vector<std::byte>> grains;
    StreamOptimizedVmdkDecoder decoder([&](std::uint64_t offset, std::span<const std::byte> data) -> Error {
        grains[offset].assign(data.begin(), data.end());
        return {};
    });
    for (std::size_t at = 0; at < stream.size(); at += step) {
        const std::size_t count = std::min(step, stream.size() - at);
        EXPECT_TRUE(decoder.feed(std::span(stream).subspan(at, count)).ok());
    }
    finished = decoder.finished();
    EXPECT_EQ(decoder.capacityBytes(), kCapacity);
    return grains;
}

TEST(StreamOptimizedVmdkDecoderTest, RoundTripsTheEncoderInAnyChunking)
{
    const std::map<std::uint64_t, std::vector<std::byte>> grains{
        {0, pattern(kGrainBytes, 1)},
        {3 * kGrainBytes, pattern(kGrainBytes, 2)},
        {4 * kGrainBytes, pattern(kGrainBytes, 3)},
        {15 * kGrainBytes, pattern(kGrainBytes, 4)},
    };
    const auto stream = encode(grains, /*compress=*/true);

    for (std::size_t step : {std::size_t{1}, std::size_t{7}, std::size_t{512}, std::size_t{4096}, stream.size()}) {
        bool finished = false;
        EXPECT_EQ(decode(stream, step, finished), grains) << "step " << step;
        EXPECT_TRUE(finished) << "step " << step;
    }
}

TEST(StreamOptimizedVmdkDecoderTest, ReadsUncompressedGrainsToo)
{
    const std::map<std::uint64_t, std::vector<std::byte>> grains{{2 * kGrainBytes, pattern(kGrainBytes, 9)}};
    bool finished = false;
    EXPECT_EQ(decode(encode(grains, /*compress=*/false), 1000, finished), grains);
    EXPECT_TRUE(finished);
}

TEST(StreamOptimizedVmdkDecoderTest, StopsAtEndOfStream)
{
    auto stream = encode({{0, pattern(kGrainBytes, 5)}}, true);
    stream.resize(stream.size() + 4096, std::byte{0x5a}); // trailing junk after the EOS marker is ignored
    bool finished = false;
    EXPECT_EQ(decode(stream, 333, finished).size(), 1u);
    EXPECT_TRUE(finished);
}

TEST(StreamOptimizedVmdkDecoderTest, RefusesWhatIsNotAStreamOptimizedVmdk)
{
    StreamOptimizedVmdkDecoder decoder([](std::uint64_t, std::span<const std::byte>) -> Error { return {}; });
    const std::vector<std::byte> flat(1024, std::byte{0});
    const Error error = decoder.feed(flat);
    EXPECT_FALSE(error.ok());
    EXPECT_EQ(error.code(), ErrorCode::InvalidFormat);
}

TEST(StreamOptimizedVmdkDecoderTest, PassesTheSinkErrorThrough)
{
    const auto stream = encode({{0, pattern(kGrainBytes, 6)}}, true);
    StreamOptimizedVmdkDecoder decoder([](std::uint64_t, std::span<const std::byte>) -> Error {
        return Error(ErrorCode::Cancelled, "reader went away");
    });
    const Error error = decoder.feed(stream);
    EXPECT_EQ(error.code(), ErrorCode::Cancelled);
}

TEST(StreamOptimizedVmdkDecoderTest, RefusesACorruptGrain)
{
    auto stream = encode({{0, pattern(kGrainBytes, 7)}}, true);
    // The grain's compressed bytes start 12 bytes into the first sector after the header and descriptor
    // (overHead, read from the header at byte 64).
    std::uint64_t overHead = 0;
    std::memcpy(&overHead, stream.data() + 64, sizeof(overHead));
    for (std::size_t i = 0; i < 64; ++i) {
        stream[overHead * 512 + 12 + i] = std::byte{0xff};
    }
    StreamOptimizedVmdkDecoder decoder([](std::uint64_t, std::span<const std::byte>) -> Error { return {}; });
    EXPECT_EQ(decoder.feed(stream).code(), ErrorCode::InvalidFormat);
}

} // namespace
} // namespace VSphereSoap::Detail
