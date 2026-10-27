// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// streamOptimized VMDK writer for qemu-img interop test

// Helper for vmdk_encoder_qemu_img_interop.cmake: writes a compressed streamOptimized VMDK with
// StreamOptimizedVmdkEncoder, plus the raw image it should decode to, so a third-party reader (qemu-img)
// can be checked against our encoder's output, not just our own reader, which accepts layouts other
// tools reject.
//
// Usage: vmdk_encoder_writer <out.vmdk> <out.raw>
//
// The disk spans more than one grain table (600 grains, 512 per table) and skips most grains, so both
// the grain directory and sparse (absent) grain table entries are exercised.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "StreamOptimizedVmdkEncoder.hpp"
#include "Error.hpp"

using VSphereSoap::Detail::StreamOptimizedVmdkEncoder;
using VSphereSoap::Detail::VmdkGrain;

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <out.vmdk> <out.raw>\n", argv[0]);
        return 2;
    }

    constexpr std::uint64_t kGrainBytes =
        StreamOptimizedVmdkEncoder::kDefaultGrainSizeSectors * StreamOptimizedVmdkEncoder::kSectorSizeBytes;
    constexpr std::uint64_t kGrains = 600;
    constexpr std::uint64_t kCapacity = kGrains * kGrainBytes;
    const std::string descriptor = "# Disk DescriptorFile\n"
                                   "version=1\n"
                                   "CID=fffffffe\n"
                                   "parentCID=ffffffff\n"
                                   "createType=\"streamOptimized\"\n"
                                   "\n"
                                   "RW " +
                                   std::to_string(kCapacity / StreamOptimizedVmdkEncoder::kSectorSizeBytes) +
                                   " SPARSE \"encoder.vmdk\"\n"
                                   "\n"
                                   "ddb.virtualHWVersion = \"4\"\n"
                                   "ddb.adapterType = \"lsilogic\"\n";

    std::ofstream vmdk(argv[1], std::ios::binary | std::ios::trunc);
    std::ofstream raw(argv[2], std::ios::binary | std::ios::trunc);
    if (!vmdk || !raw) {
        std::fprintf(stderr, "vmdk_encoder_writer: cannot open output files\n");
        return 1;
    }

    auto encoderResult = StreamOptimizedVmdkEncoder::create(
        [&vmdk](std::span<const std::byte> bytes) -> VSphereSoap::Error {
            vmdk.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!vmdk) {
                return VSphereSoap::Error(VSphereSoap::ErrorCode::IoError, "vmdk_encoder_writer: write failed");
            }
            return {};
        },
        kCapacity, descriptor, StreamOptimizedVmdkEncoder::kDefaultGrainSizeSectors, /*compressGrains=*/true);
    if (!encoderResult) {
        std::fprintf(stderr, "vmdk_encoder_writer: %s\n", encoderResult.error().toString().c_str());
        return 1;
    }
    auto encoder = std::move(encoderResult).value();

    std::vector<std::byte> grain(kGrainBytes);
    for (std::uint64_t index = 0; index < kGrains; ++index) {
        // Grains 0, 1, 7, 511 (last of the first table), 512 (first of the second) and 599 (the disk's
        // last) carry data; the rest stay zero.
        const bool allocated = index == 0 || index == 1 || index == 7 || index == 511 || index == 512 ||
                               index == kGrains - 1;
        for (std::size_t i = 0; i < grain.size(); ++i) {
            grain[i] = allocated ? static_cast<std::byte>((index * 31 + i / 512) & 0xFF) : std::byte{0};
        }
        raw.write(reinterpret_cast<const char*>(grain.data()), static_cast<std::streamsize>(grain.size()));
        if (allocated) {
            if (auto result = encoder.writeGrain(VmdkGrain{index * kGrainBytes, grain}); !result.ok()) {
                std::fprintf(stderr, "vmdk_encoder_writer: %s\n", result.toString().c_str());
                return 1;
            }
        }
    }
    if (auto result = encoder.finish(); !result.ok()) {
        std::fprintf(stderr, "vmdk_encoder_writer: %s\n", result.toString().c_str());
        return 1;
    }
    vmdk.close();
    raw.close();
    if (!vmdk || !raw) {
        std::fprintf(stderr, "vmdk_encoder_writer: failed to flush output files\n");
        return 1;
    }
    return 0;
}
