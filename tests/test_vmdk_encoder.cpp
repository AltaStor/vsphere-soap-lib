// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for the streamOptimized VMDK encoder

#include <cstring>
#include <vector>

#include <gtest/gtest.h>
#include <zlib.h>

#include "StreamOptimizedVmdkEncoder.hpp"
#include "Error.hpp"

namespace VSphereSoap::Detail {
namespace {

using VSphereSoap::ErrorCode;

constexpr std::uint64_t kSector = StreamOptimizedVmdkEncoder::kSectorSizeBytes;
constexpr std::uint64_t kGrainSectors = StreamOptimizedVmdkEncoder::kDefaultGrainSizeSectors;
constexpr std::uint64_t kGrainBytes = kGrainSectors * kSector;

std::vector<std::byte> pattern(std::size_t n, std::byte seed)
{
    std::vector<std::byte> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::byte>((static_cast<unsigned>(seed) + i) & 0xFF);
    }
    return out;
}

// Collects everything the encoder writes into one contiguous buffer: the sink itself never seeks, but
// nothing stops a test from concatenating what it received to inspect the whole stream afterward.
class CollectingSink {
public:
    VmdkByteSink asSink()
    {
        return [this](std::span<const std::byte> bytes) -> Error {
            m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
            return {};
        };
    }
    std::vector<std::byte> m_buffer;
};

#pragma pack(push, 1)
struct HeaderFields {
    std::uint32_t magicNumber;
    std::uint32_t version;
    std::uint32_t flags;
    std::uint64_t capacity;
    std::uint64_t grainSize;
    std::uint64_t descriptorOffset;
    std::uint64_t descriptorSize;
    std::uint32_t numGTEsPerGT;
    std::uint64_t rgdOffset;
    std::uint64_t gdOffset;
    std::uint64_t overHead;
    std::uint8_t uncleanShutdown;
    char singleEndLineChar;
    char nonEndLineChar;
    char doubleEndLineChar1;
    char doubleEndLineChar2;
    std::uint16_t compressAlgorithm;
};
#pragma pack(pop)
// 79 bytes: covers the header's leading fields through compressAlgorithm (everything this test actually
// checks); the real on-disk header continues with 433 bytes of padding after this, which this test
// struct doesn't need to represent.
static_assert(sizeof(HeaderFields) == 79, "unexpected padding in this test's partial header view");

HeaderFields readHeaderAt(const std::vector<std::byte>& buf, std::uint64_t sector)
{
    HeaderFields h{};
    std::memcpy(&h, buf.data() + sector * kSector, sizeof(h));
    return h;
}

struct Marker {
    std::uint64_t val;
    std::uint32_t size; // 0 for special markers
    std::uint32_t type;  // only meaningful when size == 0
    std::uint64_t startSector;
    std::uint64_t nextSector; // where the next marker begins (this marker's total padded length)
};

// Minimal reader: parses one marker (grain or special) starting at `sector`, returning it plus a pointer
// at whatever compressed grain payload followed (grain markers only).
Marker readMarkerAt(const std::vector<std::byte>& buf, std::uint64_t sector, std::vector<std::byte>* grainPayloadOut)
{
    Marker m{};
    m.startSector = sector;
    const std::byte* p = buf.data() + sector * kSector;
    std::uint64_t val = 0;
    std::uint32_t size = 0;
    std::memcpy(&val, p, sizeof(val));
    std::memcpy(&size, p + sizeof(val), sizeof(size));
    m.val = val;
    m.size = size;

    if (size == 0) {
        std::uint32_t type = 0;
        std::memcpy(&type, p + sizeof(val) + sizeof(size), sizeof(type));
        m.type = type;
        const std::uint64_t payloadSectors = (type == 0 /*EOS*/) ? 0 : val;
        m.nextSector = sector + 1 + payloadSectors;
    } else {
        if (grainPayloadOut != nullptr) {
            grainPayloadOut->assign(p + sizeof(val) + sizeof(size), p + sizeof(val) + sizeof(size) + size);
        }
        const std::uint64_t totalBytes = sizeof(val) + sizeof(size) + size;
        const std::uint64_t totalSectors = (totalBytes + kSector - 1) / kSector;
        m.nextSector = sector + totalSectors;
    }
    return m;
}

std::vector<std::byte> zlibInflate(const std::vector<std::byte>& compressed, std::size_t expectedSize)
{
    std::vector<std::byte> out(expectedSize);
    uLongf destLen = static_cast<uLongf>(expectedSize);
    const int rc = uncompress(reinterpret_cast<Bytef*>(out.data()), &destLen,
                                reinterpret_cast<const Bytef*>(compressed.data()),
                                static_cast<uLong>(compressed.size()));
    EXPECT_EQ(rc, Z_OK);
    EXPECT_EQ(destLen, expectedSize);
    return out;
}

TEST(StreamOptimizedVmdkEncoderTest, InitialHeaderFieldsMatchSpecAndCallerInputs)
{
    CollectingSink sink;
    const std::uint64_t capacity = 2 * kGrainBytes;
    const std::string descriptor = "# Disk DescriptorFile\nversion=1\n";
    auto encoderResult =
        StreamOptimizedVmdkEncoder::create(sink.asSink(), capacity, descriptor, kGrainSectors, /*compressGrains=*/true);
    ASSERT_TRUE(encoderResult.hasValue()) << encoderResult.error().toString();

    ASSERT_GE(sink.m_buffer.size(), kSector);
    const HeaderFields h = readHeaderAt(sink.m_buffer, 0);
    EXPECT_EQ(h.magicNumber, 0x564d444bu);
    EXPECT_EQ(h.version, 3u);
    EXPECT_EQ(h.flags & 0x1u, 0x1u) << "valid-newline-detection flag must be set";
    EXPECT_EQ(h.flags & 0x10000u, 0x10000u) << "compressed-grains flag must be set";
    EXPECT_EQ(h.flags & 0x20000u, 0x20000u) << "has-markers flag must be set";
    EXPECT_EQ(h.compressAlgorithm, 1u) << "compressAlgorithm must be DEFLATE when compressGrains=true";
    EXPECT_EQ(h.capacity, capacity / kSector);
    EXPECT_EQ(h.grainSize, kGrainSectors);
    EXPECT_EQ(h.descriptorOffset, 1u);
    EXPECT_EQ(h.descriptorSize, (descriptor.size() + kSector - 1) / kSector);
    EXPECT_EQ(h.numGTEsPerGT, 512u);
    EXPECT_EQ(h.rgdOffset, 0u);
    EXPECT_EQ(h.gdOffset, 0xFFFFFFFFFFFFFFFFull) << "gdOffset must be the SPARSE_GD_AT_END sentinel until finish()";
    EXPECT_EQ(h.overHead, 1 + h.descriptorSize);

    // Descriptor text must appear verbatim right after the header.
    std::string embedded(reinterpret_cast<const char*>(sink.m_buffer.data() + kSector), descriptor.size());
    EXPECT_EQ(embedded, descriptor);
}

TEST(StreamOptimizedVmdkEncoderTest, UncompressedIsTheDefaultAndFlagsReflectIt)
{
    CollectingSink sink;
    const std::uint64_t capacity = 2 * kGrainBytes;
    // no compressGrains arg
    auto encoderResult = StreamOptimizedVmdkEncoder::create(sink.asSink(), capacity, "d");
    ASSERT_TRUE(encoderResult.hasValue()) << encoderResult.error().toString();

    const HeaderFields h = readHeaderAt(sink.m_buffer, 0);
    EXPECT_EQ(h.flags & 0x10000u, 0u) << "compressed-grains flag must be clear by default";
    EXPECT_EQ(h.compressAlgorithm, 0u) << "compressAlgorithm must be NONE by default";
}

TEST(StreamOptimizedVmdkEncoderTest, RejectsNonGrainAlignedCapacity)
{
    CollectingSink sink;
    auto result = StreamOptimizedVmdkEncoder::create(sink.asSink(), kGrainBytes + 1, "desc");
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(StreamOptimizedVmdkEncoderTest, RejectsOutOfOrderGrains)
{
    CollectingSink sink;
    auto encoderResult = StreamOptimizedVmdkEncoder::create(sink.asSink(), 4 * kGrainBytes, "desc");
    ASSERT_TRUE(encoderResult.hasValue());
    auto encoder = std::move(encoderResult).value();

    const auto data = pattern(kGrainBytes, std::byte{1});
    ASSERT_TRUE(encoder.writeGrain({2 * kGrainBytes, data}).ok());
    auto result = encoder.writeGrain({kGrainBytes, data}); // goes backwards
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
}

TEST(StreamOptimizedVmdkEncoderTest, RejectsMisalignedGrainOffset)
{
    CollectingSink sink;
    auto encoderResult = StreamOptimizedVmdkEncoder::create(sink.asSink(), 4 * kGrainBytes, "desc");
    ASSERT_TRUE(encoderResult.hasValue());
    auto encoder = std::move(encoderResult).value();
    const auto data = pattern(kGrainBytes, std::byte{1});
    auto result = encoder.writeGrain({kGrainBytes / 2, data});
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
}

TEST(StreamOptimizedVmdkEncoderTest, RejectsWrongSizedGrainData)
{
    CollectingSink sink;
    auto encoderResult = StreamOptimizedVmdkEncoder::create(sink.asSink(), 4 * kGrainBytes, "desc");
    ASSERT_TRUE(encoderResult.hasValue());
    auto encoder = std::move(encoderResult).value();
    const auto tooShort = pattern(kGrainBytes - 1, std::byte{1});
    auto result = encoder.writeGrain({0, tooShort});
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
}

// End-to-end structural round trip: write grain 0 with real data, skip grain 1 entirely (all-zero),
// finish(), then walk the marker chain the same way a real reader (e.g. ESXi's own importer) would;
// grain marker, grain table marker (entry 0 pointing at the grain, entry 1 empty), grain directory
// marker, footer marker + corrected header, EOS; checking each structural fact a spec-compliant reader
// depends on, rather than a single hard-coded byte dump that would be unreadable and brittle to
// maintain.
//
TEST(StreamOptimizedVmdkEncoderTest, TwoGrainDiskWithOneSkippedGrainRoundTripsStructurally)
{
    CollectingSink sink;
    const std::uint64_t capacity = 2 * kGrainBytes;
    const std::string descriptor = "# Disk DescriptorFile\nversion=1\n";
    auto encoderResult =
        StreamOptimizedVmdkEncoder::create(sink.asSink(), capacity, descriptor, kGrainSectors, /*compressGrains=*/true);
    ASSERT_TRUE(encoderResult.hasValue());
    auto encoder = std::move(encoderResult).value();

    const auto grain0Data = pattern(kGrainBytes, std::byte{0x42});
    ASSERT_TRUE(encoder.writeGrain({0, grain0Data}).ok());
    // grain 1 (offset kGrainBytes) deliberately never written: must decode as empty/zero.
    ASSERT_TRUE(encoder.finish().ok());

    const std::uint64_t descriptorSectors = (descriptor.size() + kSector - 1) / kSector;
    const std::uint64_t grainMarkerSector = 1 + descriptorSectors; // right after header + descriptor

    std::vector<std::byte> grainPayload;
    const Marker grainMarker = readMarkerAt(sink.m_buffer, grainMarkerSector, &grainPayload);
    EXPECT_EQ(grainMarker.val, 0u) << "grain marker's LBA must be 0 for the disk's first grain";
    EXPECT_GT(grainMarker.size, 0u);
    const auto decompressed = zlibInflate(grainPayload, kGrainBytes);
    EXPECT_EQ(decompressed, grain0Data) << "grain payload must round-trip through deflate/inflate exactly";

    const Marker gtMarker = readMarkerAt(sink.m_buffer, grainMarker.nextSector, nullptr);
    EXPECT_EQ(gtMarker.size, 0u);
    EXPECT_EQ(gtMarker.type, 1u) << "expected a GRAIN_TABLE marker right after the disk's only grain";
    // Grain table payload (the raw entries) starts at its own sector, right after the marker header's
    // sector, not packed inline with it.
    std::uint32_t gte0 = 0, gte1 = 0;
    const std::uint64_t gtPayloadOffset = (gtMarker.startSector + 1) * kSector;
    std::memcpy(&gte0, sink.m_buffer.data() + gtPayloadOffset, sizeof(gte0));
    std::memcpy(&gte1, sink.m_buffer.data() + gtPayloadOffset + sizeof(gte0), sizeof(gte1));
    EXPECT_EQ(gte0, grainMarkerSector) << "grain table entry 0 must point at the grain marker's sector";
    EXPECT_EQ(gte1, 0u) << "grain table entry 1 (the skipped grain) must be the SPARSE_GTE_EMPTY sentinel";

    const Marker gdMarker = readMarkerAt(sink.m_buffer, gtMarker.nextSector, nullptr);
    EXPECT_EQ(gdMarker.size, 0u);
    EXPECT_EQ(gdMarker.type, 2u) << "expected a GRAIN_DIRECTORY marker right after the grain table";
    std::uint32_t gdEntry0 = 0;
    std::memcpy(&gdEntry0, sink.m_buffer.data() + (gdMarker.startSector + 1) * kSector, sizeof(gdEntry0));
    EXPECT_EQ(gdEntry0, gtMarker.startSector + 1)
        << "grain directory's one entry must point at the grain table's entries, one sector past its marker";

    const Marker footerMarker = readMarkerAt(sink.m_buffer, gdMarker.nextSector, nullptr);
    EXPECT_EQ(footerMarker.size, 0u);
    EXPECT_EQ(footerMarker.type, 3u) << "expected a FOOTER marker right after the grain directory";
    EXPECT_EQ(footerMarker.val, 1u) << "footer payload is always exactly one header-sized sector";
    const HeaderFields footerHeader = readHeaderAt(sink.m_buffer, footerMarker.startSector + 1);
    EXPECT_EQ(footerHeader.magicNumber, 0x564d444bu);
    EXPECT_EQ(footerHeader.capacity, capacity / kSector);
    EXPECT_EQ(footerHeader.gdOffset, gdMarker.startSector + 1)
        << "footer's header copy must carry the real grain directory location (its entries, one sector past its "
           "marker), unlike the placeholder at offset 0";

    const Marker eosMarker = readMarkerAt(sink.m_buffer, footerMarker.nextSector, nullptr);
    EXPECT_EQ(eosMarker.size, 0u);
    EXPECT_EQ(eosMarker.type, 0u) << "stream must end with an EOS marker";
    EXPECT_EQ(eosMarker.val, 0u);

    // Nothing beyond the EOS marker's own sector: the stream is exactly as long as the last thing
    // written, since finish() never pads further.
    EXPECT_EQ(sink.m_buffer.size(), (eosMarker.startSector + 1) * kSector);
}

// Same structural shape as the compressed round trip above, but exercising the default (uncompressed)
// path: the grain payload must be `grain0Data` verbatim (no deflate/inflate involved) and exactly
// `kGrainBytes` long, since an uncompressed grain marker's `size` is always the full grain size.
TEST(StreamOptimizedVmdkEncoderTest, UncompressedGrainRoundTripsVerbatim)
{
    CollectingSink sink;
    const std::uint64_t capacity = kGrainBytes;
    // default: compressGrains=false
    auto encoderResult = StreamOptimizedVmdkEncoder::create(sink.asSink(), capacity, "d");
    ASSERT_TRUE(encoderResult.hasValue());
    auto encoder = std::move(encoderResult).value();

    const auto grain0Data = pattern(kGrainBytes, std::byte{0x7A});
    ASSERT_TRUE(encoder.writeGrain({0, grain0Data}).ok());
    ASSERT_TRUE(encoder.finish().ok());

    const std::uint64_t descriptorSectors = 1; // "d" fits in one sector
    const std::uint64_t grainMarkerSector = 1 + descriptorSectors;
    std::vector<std::byte> grainPayload;
    const Marker grainMarker = readMarkerAt(sink.m_buffer, grainMarkerSector, &grainPayload);
    EXPECT_EQ(grainMarker.val, 0u);
    EXPECT_EQ(grainMarker.size, kGrainBytes) << "uncompressed grain marker size must equal the full grain size";
    EXPECT_EQ(grainPayload, grain0Data) << "uncompressed grain payload must be the raw grain data verbatim";
}

TEST(StreamOptimizedVmdkEncoderTest, ComputeDenseStreamSizeMatchesARealDenseUncompressedEncode)
{
    const std::uint64_t capacity = 5 * kGrainBytes;
    const std::string descriptor = "# Disk DescriptorFile\nversion=1\ncreateType=\"streamOptimized\"\n";

    auto predictedResult = StreamOptimizedVmdkEncoder::computeDenseStreamSizeBytes(capacity, descriptor);
    ASSERT_TRUE(predictedResult.hasValue()) << predictedResult.error().toString();

    CollectingSink sink;
    auto encoderResult = StreamOptimizedVmdkEncoder::create(sink.asSink(), capacity, descriptor);
    ASSERT_TRUE(encoderResult.hasValue());
    auto encoder = std::move(encoderResult).value();
    // Every grain written, none skipped, with arbitrary (non-zero) real data:
    // computeDenseStreamSizeBytes() must predict the same total regardless of grain *content*, since
    // compression is off.
    for (std::uint64_t offset = 0; offset < capacity; offset += kGrainBytes) {
        ASSERT_TRUE(encoder.writeGrain({offset, pattern(kGrainBytes, static_cast<std::byte>(offset))}).ok());
    }
    ASSERT_TRUE(encoder.finish().ok());

    EXPECT_EQ(predictedResult.value(), sink.m_buffer.size());
}

TEST(StreamOptimizedVmdkEncoderTest, WriteGrainAfterFinishFails)
{
    CollectingSink sink;
    auto encoderResult = StreamOptimizedVmdkEncoder::create(sink.asSink(), kGrainBytes, "d");
    ASSERT_TRUE(encoderResult.hasValue());
    auto encoder = std::move(encoderResult).value();
    ASSERT_TRUE(encoder.finish().ok());
    const auto data = pattern(kGrainBytes, std::byte{1});
    auto result = encoder.writeGrain({0, data});
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
}

TEST(StreamOptimizedVmdkEncoderTest, EverythingSectorAligned)
{
    CollectingSink sink;
    auto encoderResult = StreamOptimizedVmdkEncoder::create(sink.asSink(), 3 * kGrainBytes, "odd length desc");
    ASSERT_TRUE(encoderResult.hasValue());
    auto encoder = std::move(encoderResult).value();
    ASSERT_TRUE(encoder.writeGrain({0, pattern(kGrainBytes, std::byte{9})}).ok());
    ASSERT_TRUE(encoder.writeGrain({2 * kGrainBytes, pattern(kGrainBytes, std::byte{3})}).ok());
    ASSERT_TRUE(encoder.finish().ok());
    EXPECT_EQ(sink.m_buffer.size() % kSector, 0u) << "every write to the sink must land on a sector boundary";
}

} // namespace
} // namespace VSphereSoap::Detail
