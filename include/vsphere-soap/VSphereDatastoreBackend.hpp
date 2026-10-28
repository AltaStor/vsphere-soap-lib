// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere datastore streaming upload target via HttpNfcLease

#pragma once

// The import side of vsphere-soap-lib, data plane: a VMware destination backend that uploads a whole
// virtual disk to a datastore via an HttpNfcLease device URL (see HttpNfcLeaseSession.hpp) as
// a stream-optimized VMDK, one HTTP PUT chunk at a time; no local staging file.
// This is the "chunk by chunk, no temp files" write path: writeAt() calls from the disk-copy loop feed
// a StreamOptimizedVmdkEncoder directly, whose output feeds a single long-lived HTTPS PUT running on
// a background thread for the whole transfer's duration.
// Two modes exist (see `compressGrains` below), sorted out only by live testing against a real ESXi
// HttpNfcLease endpoint: the real one is compressed grains + sparse skipping + chunked transfer encoding
// + the `Overwrite: t` header (that last one is not optional; the target file already exists as
// a placeholder by the time the lease is ready, and its absence looks exactly like a broken upload:
// the server resets the connection mid-transfer rather than returning a clean 403).
// The other (dense, every grain present, uncompressed, precomputed Content-Length) was this class's
// first design, built on the wrong assumption that chunked encoding itself was the problem; kept working
// and tested since it's a real HTTP capability, but a real vCenter/ESXi rejects its uncompressed output
// outright once fully received.
//
// Deliberately separate from HttpNfcLeaseSession: that class owns the lease's lifecycle (creating
// the VM, obtaining device URLs, keepalive, complete/abort) for a whole (possibly multi-disk) VM import;
// this class is purely the disk-shaped byte pump for *one* disk's device URL, handed to it by whatever
// orchestrates the two together (not this class's job; see HttpNfcLeaseSession.hpp's own doc comment on
// why VM creation and datastore writing are split like this).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "vsphere-soap/Result.hpp"

namespace VSphereSoap {

struct VSphereDatastoreBackendParams {
    /// One `HttpNfcLeaseInfo.deviceUrl[].url` entry (see HttpNfcLeaseSession.hpp's `LeaseDeviceUrl`),
    /// already `*`-host-substituted: an absolute "https://host[:port]/path?query" URL.
    /// This URL is treated as self-authenticating (the lease ticket is embedded in its path/query by
    /// vCenter): no separate session cookie or credentials are sent alongside it.
    /// Live uploads have not needed one so far; if a device URL ever turns out to need the SOAP
    /// session's cookie too, this needs a cookie parameter added.
    ///
    std::string deviceUrl;

    bool verifyTlsCertificate = true;
    /// SHA-1 fingerprint of the peer's leaf certificate, colon-hex: see
    /// `VSphereSoap::ConnectParams::pinnedThumbprint` (same meaning, just not reusing that struct since
    /// this class needs no vCenter username/password, only TLS trust settings).
    std::string pinnedThumbprint;

    /// The *logical* (guest-visible) disk size this backend reports via `virtualSize()` and encodes into
    /// the VMDK's `SparseExtentHeader`.
    /// Must be a nonzero multiple of `grainSizeSectors * 512` bytes: callers that don't already have
    /// a grain-aligned size should round up themselves (see `StreamOptimizedVmdkEncoder::create`'s own
    /// precondition, which this forwards to).
    ///
    std::uint64_t virtualSizeBytes = 0;

    /// The embedded VMDK descriptor text (the "# Disk DescriptorFile" section); this class has no
    /// opinion on adapter type/geometry wording, it just streams whatever descriptor the caller built
    /// alongside the OVF descriptor used to create the VM.
    std::string vmdkDescriptor;

    std::uint64_t grainSizeSectors = 128; // matches StreamOptimizedVmdkEncoder::kDefaultGrainSizeSectors
    /// Should be `true` for any real upload: confirmed live against a real ESXi HttpNfcLease endpoint
    /// that an uncompressed upload is rejected outright once fully received (HTTP 500 "Not a supported
    /// disk format (uncompressed disk)").
    /// `true` switches this backend to chunked transfer encoding with sparse grain skipping (compressed
    /// output size is data-dependent, so there's no `Content-Length` to precompute, and nothing stops
    /// sparse skipping anymore either; this is the smaller, and now the only server-accepted, mode).
    /// `false` keeps the alternative dense/precomputed-`Content-Length` mode this class was originally
    /// (and wrongly) built around as the only path: still implemented and tested, just not usable
    /// against a real vCenter/ESXi today; see VSphereDatastoreBackend.cpp's `open()` for the full story.
    /// The VMDK that mode streams is also non-standard (uncompressed grains behind GRAIN markers) and
    /// misread by other tools such as qemu-img; see StreamOptimizedVmdkEncoder::create's doc comment.
    ///
    bool compressGrains = false;

    /// Bounds each individual network operation (one chunk's write, or the final response read), NOT
    /// the overall transfer, which has no timeout of its own and simply takes as long as `writeAt()`
    /// calls keep arriving (see VSphereDatastoreBackend.cpp's class-level doc comment on why that's
    /// the correct behavior here).
    ///
    std::chrono::seconds perOperationTimeout{300};
};

/// `virtualSizeBytes` rounded up to a whole grain, the size
/// `VSphereDatastoreBackendParams::virtualSizeBytes` and the matching
/// `VSphereImportDiskSpec::capacityBytes` need.
/// The tail past the source's own size reads as zero.
///
std::uint64_t roundUpToGrain(std::uint64_t virtualSizeBytes, std::uint64_t grainSizeSectors = 128);

/// The embedded descriptor text of a streamOptimized VMDK, for
/// `VSphereDatastoreBackendParams::vmdkDescriptor`.
/// `capacityBytes` is the grain-rounded size the disk is uploaded with, and `fileName` the extent name
/// it records ("disk.vmdk"; ESXi renames the file to its own choice on import anyway).
/// `adapterType` ("paravirtual", "lsilogic", "lsilogicsas", "buslogic", "ide") should match
/// the controller the VM gets the disk on; it is the guest's hint, not what decides the controller.
///
std::string makeStreamOptimizedDescriptor(std::uint64_t capacityBytes, const std::string& fileName,
                                                        const std::string& adapterType = "paravirtual");

/// Write-only disk uploaded through an HttpNfcLease device URL with no local staging file.
/// Written bytes go through the streamOptimized VMDK encoder into one HTTPS PUT that a background thread
/// keeps open for the whole transfer.
///
/// Writes must come in non-decreasing, non-overlapping offset order.
/// Each offset is either grain-aligned or continues the previous write exactly, so whole grains may be
/// skipped but never part of one.
/// Skipped grains are left out of the upload in compressed mode and zero-filled in dense mode.
/// Any other order fails with `ErrorCode::InvalidArgument`.
///
/// `flush()` finalizes the upload and the destructor aborts it when `flush()` did not succeed, so a partial
/// upload never looks like a valid disk.
/// It has the usual disk methods but no disk base class: a tool wraps it into its own disk interface.
/// Not thread-safe from the caller's side.
///
class VSphereDatastoreBackend final {
public:
    /// Opens a dedicated HTTPS connection to the device URL and starts the background upload thread.
    /// Nothing reaches the wire before the first write or flush produces data.
    /// Fails with `ErrorCode::InvalidArgument` on an unparseable "https://host[:port]/..." URL or a size that is
    /// not a nonzero multiple of the grain size.
    ///
    static VSphereSoap::Result<std::unique_ptr<VSphereDatastoreBackend>>
    open(VSphereDatastoreBackendParams params);

    /// Aborts the upload (best-effort) if `flush()` was never called or failed, then joins
    /// the background thread.
    ~VSphereDatastoreBackend();

    VSphereDatastoreBackend(const VSphereDatastoreBackend&) = delete;
    VSphereDatastoreBackend& operator=(const VSphereDatastoreBackend&) = delete;
    VSphereDatastoreBackend(VSphereDatastoreBackend&&) = delete;
    VSphereDatastoreBackend& operator=(VSphereDatastoreBackend&&) = delete;

    /// Always fails with `ErrorCode::NotImplemented`: this backend is write-only.
    VSphereSoap::Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out);

    /// Encodes `in` at `offset` and queues it for the upload, in the order the class comment describes.
    VSphereSoap::Result<std::uint64_t> writeAt(std::uint64_t offset, std::span<const std::byte> in);

    std::uint64_t virtualSize() const noexcept;
    std::uint64_t sectorSize() const noexcept;

    /// Emits the last partial grain and the VMDK footer, ends the upload and fails unless the datastore
    /// answered 2xx.
    /// Idempotent.
    VSphereSoap::Error flush();

    /// Keeps the upload connection from going idle long enough for ESXi to drop it (it aborts
    /// an HttpNfcLease upload that has sent nothing for roughly a minute; confirmed live), which a long
    /// all-zero stretch of the source otherwise causes: zero regions produce no bytes on the wire.
    /// Call it regularly from the disk-copy loop (e.g. its progress callback, which also fires for
    /// zero-skipped chunks) passing `knownFinalUpTo`, the offset below which no further `writeAt()` will
    /// come.
    /// After enough silence it sends whatever encoded bytes are still batched, or else one explicit
    /// all-zero grain from the already-final, never-written range below `knownFinalUpTo` (zeros where
    /// the disk is zero anyway, so the content is unchanged).
    /// A no-op otherwise, and in dense (uncompressed) mode, which never goes idle.
    /// Same thread rules as `writeAt()`.
    ///
    VSphereSoap::Error keepAliveIfIdle(std::uint64_t knownFinalUpTo);

private:
    class Impl;
    explicit VSphereDatastoreBackend(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VSphereSoap
