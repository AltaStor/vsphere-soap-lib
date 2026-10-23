// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere folder-put flat VMDK streaming upload target

#pragma once

// Part of vsphere-soap-lib's public API: the folder-put import fallback's byte-streaming half;
// a write-only `IDisk` that PUTs a monolithicFlat VMDK's raw extent bytes directly to a datastore path
// with no local staging file, the same "no intermediate files" way `VSphereDatastoreBackend` streams
// a streamOptimized VMDK to an `HttpNfcLease` device URL.
// Deliberately separate from that class rather than a mode flag on it: there is no VMDK sparse/grain
// encoding here at all (a flat extent's byte offset *is* its position in the guest disk), no
// compression, and the datastore `/folder/` PUT endpoint this targets is a different, older, plainer
// HTTP surface than an `HttpNfcLease` device URL; conflating the two would make both harder to read for
// no shared benefit.
// This path exists because some deployments may not accept the chunked `HttpNfcLease` upload the primary
// path relies on.
//
// Pairs with the free functions in VSphereFolderPutImport.hpp: those resolve the target VM/paths and PUT
// the small text descriptor; this class streams the (potentially huge) flat extent itself.

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "IDisk.hpp"
#include "Result.hpp"

namespace VSphereSoap {

struct VSphereFolderPutBackendParams {
    /// Absolute "https://host[:port]/folder/path?dcPath=...&dsName=..."
    /// URL for the flat extent file, as returned by `VSphereFolderPutPrepResult::flatPutUrl`.
    std::string putUrl;

    bool verifyTlsCertificate = true;
    std::string pinnedThumbprint;

    /// The vSphere Web Services session cookie (`SoapClient::sessionCookie()`) to authenticate the PUT
    /// with: the datastore `/folder/` HTTP endpoint accepts the same vim25 session cookie as the SOAP
    /// control-plane calls, the same way `VmwareSoapDisk` already reuses it for its own ranged GET
    /// calls.
    /// As returned by `VSphereFolderPutPrepResult::sessionCookie`.
    ///
    std::string sessionCookie;

    /// The *logical* (guest-visible) disk size; both what `virtualSize()` reports and the exact
    /// `Content-Length` this class commits to on the wire (unlike `VSphereDatastoreBackend`, there is no
    /// grain-size alignment requirement here: a flat extent's size is just its byte count).
    /// Must be nonzero.
    ///
    std::uint64_t virtualSizeBytes = 0;

    /// Bounds each individual network operation, matching
    /// `VSphereDatastoreBackendParams::perOperationTimeout`'s own doc comment (not the overall transfer,
    /// which has no timeout of its own).
    std::chrono::seconds perOperationTimeout{300};
};

/// `IDisk` over a monolithicFlat VMDK's raw extent, written via a single long-lived HTTPS PUT with
/// a precomputed `Content-Length`; required because, as live testing found, a real vCenter's datastore
/// `/folder/` endpoint does not support a ranged/partial PUT at all (`Content-Range` PUT: HTTP 501;
/// a plain PUT without a full, correct `Content-Length`: silently truncates the file to whatever was
/// actually sent).
/// Every `writeAt()` call must therefore arrive in non-decreasing, non-overlapping offset order (a gap
/// is zero-filled on the wire, in bounded batches, never materialized as one huge buffer) and
/// the running byte count sent must land on exactly `virtualSizeBytes` by the time `flush()` is called,
/// or the PUT will desync (a short body hangs; a long one corrupts whatever request reuses the same
/// connection next): `flush()` itself zero-fills any remaining tail so a caller that stops writing early
/// (e.g. the source's last bytes were all zero and the copy loop skipped them) still produces
/// a correctly-sized file.
///
/// Write-only (`readAt` fails with `ErrorCode::NotImplemented`, matching `VSphereDatastoreBackend`).
/// Not thread-safe from the caller's side, same as every other `IDisk` in this codebase, even though it
/// owns a background upload thread internally.
///
class VSphereFolderPutBackend final : public VSphereSoap::IDisk {
public:
    /// Parses `params.putUrl`'s host/port, opens a dedicated HTTPS connection (not shared with any other
    /// transport), and starts the background upload thread immediately (it begins waiting for the first
    /// bytes right away, matching `VSphereDatastoreBackend::open`'s own reasoning for doing so).
    [[nodiscard]] static VSphereSoap::Result<std::unique_ptr<VSphereFolderPutBackend>>
    open(VSphereFolderPutBackendParams params);

    /// Aborts the upload (best-effort) if `flush()` was never called or failed, then joins
    /// the background thread: same discipline as `VSphereDatastoreBackend`'s destructor.
    ~VSphereFolderPutBackend() override;

    VSphereFolderPutBackend(const VSphereFolderPutBackend&) = delete;
    VSphereFolderPutBackend& operator=(const VSphereFolderPutBackend&) = delete;
    VSphereFolderPutBackend(VSphereFolderPutBackend&&) = delete;
    VSphereFolderPutBackend& operator=(VSphereFolderPutBackend&&) = delete;

    /// Always fails with `ErrorCode::NotImplemented`: this backend is write-only.
    VSphereSoap::Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out) override;

    VSphereSoap::Result<std::uint64_t> writeAt(std::uint64_t offset, std::span<const std::byte> in) override;

    [[nodiscard]] std::uint64_t virtualSize() const noexcept override;
    [[nodiscard]] std::uint64_t sectorSize() const noexcept override;

    /// Zero-fills any remaining tail, signals end-of-body, joins the upload thread, and fails if
    /// the datastore's HTTP response wasn't 2xx.
    /// Idempotent.
    VSphereSoap::Result<void> flush() override;

private:
    class Impl;
    explicit VSphereFolderPutBackend(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VSphereSoap
