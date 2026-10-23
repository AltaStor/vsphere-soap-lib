// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Stream-optimized VMDK streaming encoder

#include "StreamOptimizedVmdkEncoder.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <zlib.h>

#include "Error.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

namespace {

// --- On-disk structures, "Virtual Disk Format 5.0" ------------------------
// Every multi-byte field is little-endian; this library only targets little-endian build hosts, so plain
// struct member assignment is used rather than an explicit endian-safe pack.

#pragma pack(push, 1)
struct SparseExtentHeaderOnDisk {
    std::uint32_t magicNumber = 0x564d444b; // "KDMV" read as bytes on disk
    std::uint32_t version = 3;              // 3 = streamOptimized
    std::uint32_t flags = 0;
    std::uint64_t capacity = 0;         // sectors
    std::uint64_t grainSize = 0;        // sectors
    std::uint64_t descriptorOffset = 0; // sectors
    std::uint64_t descriptorSize = 0;   // sectors
    std::uint32_t numGTEsPerGT = 512;
    std::uint64_t rgdOffset = 0; // no redundant grain dir for streamOptimized
    std::uint64_t gdOffset = 0;  // SPARSE_GD_AT_END until finish()
    std::uint64_t overHead = 0;  // sectors before grain data starts
    std::uint8_t uncleanShutdown = 0;
    char singleEndLineChar = '\n';
    char nonEndLineChar = ' ';
    char doubleEndLineChar1 = '\r';
    char doubleEndLineChar2 = '\n';
    std::uint16_t compressAlgorithm = 1; // 1 = deflate
    std::uint8_t pad[433] = {};
};
#pragma pack(pop)
static_assert(sizeof(SparseExtentHeaderOnDisk) == 512, "SparseExtentHeader must be exactly one sector");

// Header flag bits actually used here.
constexpr std::uint32_t kFlagValidNewLineTest = 0x1;
constexpr std::uint32_t kFlagCompressedGrains = 0x10000;
constexpr std::uint32_t kFlagHasMarkers = 0x20000;

constexpr std::uint64_t kSparseGdAtEnd = 0xFFFFFFFFFFFFFFFFull;

// A "special" marker (grain table / grain directory / footer / EOS): 8-byte val + 4-byte size(==0) +
// 4-byte type, occupying the first 16 bytes of a sector, zero-padded the rest of the way.
// A GRAIN marker shares the same first 12 bytes (val=LBA, size=compressed byte length) but size is
// always nonzero there, which is exactly how a reader tells the two apart.
//
#pragma pack(push, 1)
struct MarkerHeaderOnDisk {
    std::uint64_t val = 0;
    std::uint32_t size = 0;
};
#pragma pack(pop)
static_assert(sizeof(MarkerHeaderOnDisk) == 12, "marker header must be 12 bytes");

enum class SpecialMarkerType : std::uint32_t {
    Eos = 0,
    GrainTable = 1,
    GrainDirectory = 2,
    Footer = 3,
};

std::uint64_t ceilDiv(std::uint64_t a, std::uint64_t b) { return (a + b - 1) / b; }

/// zlib-wrapped (not raw) deflate of `data`: best-known match for what VMware's own tools produce/expect
/// for a streamOptimized grain; see StreamOptimizedVmdkEncoder.hpp's caller-facing note.
/// Confirmed by real ESXi uploads accepting the result.
Result<std::vector<std::byte>> deflateGrain(std::span<const std::byte> data)
{
    uLongf boundLen = compressBound(static_cast<uLong>(data.size()));
    std::vector<std::byte> out(boundLen);
    uLongf destLen = boundLen;
    const int rc = compress2(reinterpret_cast<Bytef*>(out.data()), &destLen,
                              reinterpret_cast<const Bytef*>(data.data()), static_cast<uLong>(data.size()),
                              Z_DEFAULT_COMPRESSION);
    if (rc != Z_OK) {
        return Error(ErrorCode::IoError, "zlib compress2 failed with code " + std::to_string(rc));
    }
    out.resize(destLen);
    return out;
}

} // namespace

class StreamOptimizedVmdkEncoder::Impl {
public:
    VmdkByteSink m_sink;
    std::uint64_t m_capacityBytes = 0;
    bool m_compressGrains = false;
    std::uint64_t m_grainSizeSectors = 0;
    std::uint64_t m_grainSizeBytes = 0;
    std::uint64_t m_totalGrains = 0;
    std::uint32_t m_numGTEsPerGT = 512;
    std::string m_descriptor;
    std::uint64_t m_descriptorSectors = 0;
    std::uint64_t m_overHeadSectors = 0;

    // Streaming position, in sectors from the start of the file, of the next byte `sink` will receive;
    // this is exactly the file offset a grain-table/grain-directory entry needs to record for whatever
    // gets written next, so every write*() helper advances it by exactly what it wrote.
    std::uint64_t m_cursorSectors = 0;

    bool m_finished = false;
    // logical byte offset of the last grain written, for ordering checks
    std::optional<std::uint64_t> m_lastGrainOffset;

    // One entry per grain actually written (offset omitted => empty/zero), built up incrementally;
    // flushed into on-disk grain tables at finish().
    // Keyed by grain INDEX (offset / grainSizeBytes), pointing at the sector where that grain's marker
    // begins.
    //
    std::vector<std::pair<std::uint64_t, std::uint64_t>> m_grainIndexToMarkerSector;

    Result<void> writeRaw(std::span<const std::byte> bytes)
    {
        if (auto result = m_sink(bytes); !result) {
            return result;
        }
        return {};
    }

    // Pads `bytes` (already written to a temp buffer by the caller) up to the next sector boundary and
    // writes the whole padded buffer, keeping cursorSectors in sync.
    // `bytes.size()` need not itself be sector-aligned; only the total written-to-sink length is.
    Result<void> writePadded(std::vector<std::byte> bytes)
    {
        const std::uint64_t rem = bytes.size() % kSectorSizeBytes;
        if (rem != 0) {
            bytes.resize(bytes.size() + (kSectorSizeBytes - rem), std::byte{0});
        }
        if (auto result = writeRaw(bytes); !result) {
            return result;
        }
        m_cursorSectors += bytes.size() / kSectorSizeBytes;
        return {};
    }

    // Writes a "special" marker (grain table / grain directory / footer /EOS): the 16-byte
    // val+size(0)+type header, padded out to its own full sector, followed by `payload` padded out to
    // its own separate run of sectors; per spec, `val` for these marker types means "sectors of payload
    // following in their own right", not "this marker's own total padded length" the way a GRAIN
    // marker's `size` does.
    // Returns the sector this marker itself started at; the payload starts one sector later, and that
    // payload sector (not the marker's) is what a grain directory entry or the footer's gdOffset must
    // record (VMware's spec, and what qemu-img requires to read the file at all).
    //
    Result<std::uint64_t> writeSpecialMarker(std::uint64_t val, SpecialMarkerType type,
                                               std::span<const std::byte> payload)
    {
        const std::uint64_t markerSector = m_cursorSectors;
        MarkerHeaderOnDisk marker;
        marker.val = val;
        marker.size = 0;
        std::vector<std::byte> headerBuf(sizeof(marker) + sizeof(SpecialMarkerType));
        std::memcpy(headerBuf.data(), &marker, sizeof(marker));
        std::memcpy(headerBuf.data() + sizeof(marker), &type, sizeof(type));
        if (auto result = writePadded(std::move(headerBuf)); !result) {
            return std::move(result).error();
        }
        if (!payload.empty()) {
            std::vector<std::byte> payloadBuf(payload.begin(), payload.end());
            if (auto result = writePadded(std::move(payloadBuf)); !result) {
                return std::move(result).error();
            }
        }
        return markerSector;
    }

    Result<void> writeHeader(std::uint64_t gdOffsetSectors)
    {
        SparseExtentHeaderOnDisk header;
        header.flags = kFlagValidNewLineTest | kFlagHasMarkers | (m_compressGrains ? kFlagCompressedGrains : 0);
        header.capacity = m_capacityBytes / kSectorSizeBytes;
        header.grainSize = m_grainSizeSectors;
        header.descriptorOffset = 1; // right after this header sector
        header.descriptorSize = m_descriptorSectors;
        header.numGTEsPerGT = m_numGTEsPerGT;
        header.rgdOffset = 0;
        header.gdOffset = gdOffsetSectors;
        header.overHead = m_overHeadSectors;
        // 0 = COMPRESSION_NONE, 1 = COMPRESSION_DEFLATE
        header.compressAlgorithm = m_compressGrains ? 1 : 0;

        std::vector<std::byte> buf(sizeof(header));
        std::memcpy(buf.data(), &header, sizeof(header));
        return writePadded(std::move(buf));
    }
};

Result<StreamOptimizedVmdkEncoder> StreamOptimizedVmdkEncoder::create(VmdkByteSink sink, std::uint64_t capacityBytes,
                                                                          std::string descriptor,
                                                                          std::uint64_t grainSizeSectors,
                                                                          bool compressGrains)
{
    if (!sink) {
        return Error(ErrorCode::InvalidArgument, "StreamOptimizedVmdkEncoder::create: sink is required");
    }
    if (grainSizeSectors == 0 || (grainSizeSectors & (grainSizeSectors - 1)) != 0) {
        return Error(ErrorCode::InvalidArgument, "StreamOptimizedVmdkEncoder::create: grainSizeSectors must be a "
                                                     "nonzero power of two");
    }
    const std::uint64_t grainSizeBytes = grainSizeSectors * StreamOptimizedVmdkEncoder::kSectorSizeBytes;
    if (capacityBytes == 0 || capacityBytes % grainSizeBytes != 0) {
        return Error(ErrorCode::InvalidArgument,
                     "StreamOptimizedVmdkEncoder::create: capacityBytes must be a nonzero multiple of the grain "
                     "size (" +
                         std::to_string(grainSizeBytes) + " bytes)");
    }

    auto impl = std::make_unique<Impl>();
    impl->m_sink = std::move(sink);
    impl->m_capacityBytes = capacityBytes;
    impl->m_compressGrains = compressGrains;
    impl->m_grainSizeSectors = grainSizeSectors;
    impl->m_grainSizeBytes = grainSizeBytes;
    impl->m_totalGrains = capacityBytes / grainSizeBytes;
    impl->m_descriptor = std::move(descriptor);
    impl->m_descriptorSectors = ceilDiv(impl->m_descriptor.size(), StreamOptimizedVmdkEncoder::kSectorSizeBytes);
    // header (1 sector) + descriptor: grain data starts right after, and unlike gdOffset this is known
    // up front, so the placeholder header already carries the real value (only gdOffset needs the "ask
    // the footer instead" sentinel).
    impl->m_overHeadSectors = 1 + impl->m_descriptorSectors;

    if (auto result = impl->writeHeader(kSparseGdAtEnd); !result) {
        return std::move(result).error();
    }
    // Descriptor bytes right after the header, padded to a full sector.
    std::vector<std::byte> descBuf(impl->m_descriptor.size());
    std::memcpy(descBuf.data(), impl->m_descriptor.data(), impl->m_descriptor.size());
    if (auto result = impl->writePadded(std::move(descBuf)); !result) {
        return std::move(result).error();
    }

    return StreamOptimizedVmdkEncoder(std::move(impl));
}

StreamOptimizedVmdkEncoder::StreamOptimizedVmdkEncoder(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
StreamOptimizedVmdkEncoder::StreamOptimizedVmdkEncoder(StreamOptimizedVmdkEncoder&&) noexcept = default;
StreamOptimizedVmdkEncoder& StreamOptimizedVmdkEncoder::operator=(StreamOptimizedVmdkEncoder&&) noexcept = default;
StreamOptimizedVmdkEncoder::~StreamOptimizedVmdkEncoder() = default;

Result<void> StreamOptimizedVmdkEncoder::writeGrain(const VmdkGrain& grain)
{
    if (m_impl->m_finished) {
        return Error(ErrorCode::InvalidArgument, "StreamOptimizedVmdkEncoder::writeGrain called after finish()");
    }
    if (grain.offset % m_impl->m_grainSizeBytes != 0) {
        return Error(ErrorCode::InvalidArgument, "StreamOptimizedVmdkEncoder::writeGrain: offset " +
                                                       std::to_string(grain.offset) + " is not grain-aligned");
    }
    if (grain.data.size() != m_impl->m_grainSizeBytes) {
        return Error(ErrorCode::InvalidArgument,
                     "StreamOptimizedVmdkEncoder::writeGrain: data size " + std::to_string(grain.data.size()) +
                         " != grain size " + std::to_string(m_impl->m_grainSizeBytes));
    }
    if (grain.offset >= m_impl->m_capacityBytes) {
        return Error(ErrorCode::InvalidArgument, "StreamOptimizedVmdkEncoder::writeGrain: offset " +
                                                       std::to_string(grain.offset) + " is past capacity");
    }
    if (m_impl->m_lastGrainOffset && grain.offset <= *m_impl->m_lastGrainOffset) {
        return Error(ErrorCode::InvalidArgument,
                     "StreamOptimizedVmdkEncoder::writeGrain: offset " + std::to_string(grain.offset) +
                         " is not strictly greater than the previous grain's offset (" +
                         std::to_string(*m_impl->m_lastGrainOffset) + ") -- grains must be written in ascending order");
    }

    // Either path ends up with `emitted` pointing at the bytes that follow the marker header on disk:
    // compressed if this encoder was asked for it, `grain.data` verbatim otherwise (see
    // StreamOptimizedVmdkEncoder ::create's doc comment on why uncompressed is the default right now).
    std::vector<std::byte> compressedStorage;
    std::span<const std::byte> emitted = grain.data;
    if (m_impl->m_compressGrains) {
        auto compressed = deflateGrain(grain.data);
        if (!compressed) {
            return std::move(compressed).error();
        }
        compressedStorage = std::move(compressed).value();
        emitted = compressedStorage;
    }

    const std::uint64_t lba = grain.offset / StreamOptimizedVmdkEncoder::kSectorSizeBytes;
    const std::uint64_t markerSector = m_impl->m_cursorSectors;

    MarkerHeaderOnDisk markerHeader;
    markerHeader.val = lba;
    markerHeader.size = static_cast<std::uint32_t>(emitted.size());

    std::vector<std::byte> buf(sizeof(markerHeader) + emitted.size());
    std::memcpy(buf.data(), &markerHeader, sizeof(markerHeader));
    std::memcpy(buf.data() + sizeof(markerHeader), emitted.data(), emitted.size());
    if (auto result = m_impl->writePadded(std::move(buf)); !result) {
        return result;
    }

    const std::uint64_t grainIndex = grain.offset / m_impl->m_grainSizeBytes;
    m_impl->m_grainIndexToMarkerSector.emplace_back(grainIndex, markerSector);
    m_impl->m_lastGrainOffset = grain.offset;
    return {};
}

Result<void> StreamOptimizedVmdkEncoder::finish()
{
    if (m_impl->m_finished) {
        return Error(ErrorCode::InvalidArgument, "StreamOptimizedVmdkEncoder::finish called twice");
    }
    m_impl->m_finished = true;

    const std::uint64_t numGrainTables = ceilDiv(m_impl->m_totalGrains, m_impl->m_numGTEsPerGT);
    // where each grain table's entries begin (marker sector + 1)
    std::vector<std::uint64_t> grainTableSectors;
    grainTableSectors.reserve(numGrainTables);

    // grainIndexToMarkerSector is already in ascending order (writeGrain enforces ascending offsets), so
    // a single linear scan can bucket entries into their grain table without re-sorting.
    std::size_t nextEntry = 0;
    for (std::uint64_t gt = 0; gt < numGrainTables; ++gt) {
        const std::uint64_t firstGrain = gt * m_impl->m_numGTEsPerGT;
        const std::uint64_t entriesInThisTable = std::min<std::uint64_t>(m_impl->m_numGTEsPerGT, m_impl->m_totalGrains - firstGrain);

        std::vector<std::uint32_t> table(entriesInThisTable, 0); // 0 = SPARSE_GTE_EMPTY
        while (nextEntry < m_impl->m_grainIndexToMarkerSector.size()) {
            const auto& [grainIndex, markerSector] = m_impl->m_grainIndexToMarkerSector[nextEntry];
            if (grainIndex >= firstGrain + entriesInThisTable) {
                break;
            }
            table[grainIndex - firstGrain] = static_cast<std::uint32_t>(markerSector);
            ++nextEntry;
        }

        const std::uint64_t tablePayloadSectors =
            ceilDiv(table.size() * sizeof(std::uint32_t), StreamOptimizedVmdkEncoder::kSectorSizeBytes);
        auto gtMarkerSector = m_impl->writeSpecialMarker(
            tablePayloadSectors, SpecialMarkerType::GrainTable,
            std::as_bytes(std::span(table.data(), table.size())));
        if (!gtMarkerSector) {
            return std::move(gtMarkerSector).error();
        }
        grainTableSectors.push_back(gtMarkerSector.value() + 1);
    }

    std::vector<std::uint32_t> grainTableSectors32;
    grainTableSectors32.reserve(grainTableSectors.size());
    for (const std::uint64_t s : grainTableSectors) {
        grainTableSectors32.push_back(static_cast<std::uint32_t>(s));
    }
    const std::uint64_t gdPayloadSectors =
        ceilDiv(grainTableSectors32.size() * sizeof(std::uint32_t), StreamOptimizedVmdkEncoder::kSectorSizeBytes);
    auto gdMarkerSector = m_impl->writeSpecialMarker(
        gdPayloadSectors, SpecialMarkerType::GrainDirectory,
        std::as_bytes(std::span(grainTableSectors32.data(), grainTableSectors32.size())));
    if (!gdMarkerSector) {
        return std::move(gdMarkerSector).error();
    }

    // Footer: a FOOTER marker (val = 1 sector of payload following) then a corrected header copy, this
    // time with the real gdOffset; the sector the directory's entries start at, one past its marker.
    if (auto result = m_impl->writeSpecialMarker(1, SpecialMarkerType::Footer, {}); !result) {
        return std::move(result).error();
    }
    if (auto result = m_impl->writeHeader(gdMarkerSector.value() + 1); !result) {
        return result;
    }

    if (auto result = m_impl->writeSpecialMarker(0, SpecialMarkerType::Eos, {}); !result) {
        return std::move(result).error();
    }

    return {};
}

Result<std::uint64_t> StreamOptimizedVmdkEncoder::computeDenseStreamSizeBytes(std::uint64_t capacityBytes,
                                                                                  const std::string& descriptor,
                                                                                  std::uint64_t grainSizeSectors)
{
    std::uint64_t total = 0;
    VmdkByteSink countingSink = [&total](std::span<const std::byte> bytes) -> Result<void> {
        total += bytes.size();
        return {};
    };
    auto encoderResult = create(countingSink, capacityBytes, descriptor, grainSizeSectors, /*compressGrains=*/false);
    if (!encoderResult) {
        return std::move(encoderResult).error();
    }
    auto encoder = std::move(encoderResult).value();

    const std::uint64_t grainSizeBytes = grainSizeSectors * kSectorSizeBytes;
    const std::vector<std::byte> zeroGrain(grainSizeBytes, std::byte{0});
    for (std::uint64_t offset = 0; offset < capacityBytes; offset += grainSizeBytes) {
        if (auto result = encoder.writeGrain(VmdkGrain{offset, zeroGrain}); !result) {
            return std::move(result).error();
        }
    }
    if (auto result = encoder.finish(); !result) {
        return std::move(result).error();
    }
    return total;
}

} // namespace VSphereSoap::Detail
