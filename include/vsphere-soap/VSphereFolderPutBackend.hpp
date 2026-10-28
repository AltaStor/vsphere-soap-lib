// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere folder-put flat VMDK streaming upload target

#pragma once

// Part of vsphere-soap-lib's public API: the folder-put import fallback's byte-streaming half;
// a write-only disk that PUTs a monolithicFlat VMDK's raw extent bytes directly to a datastore path
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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "vsphere-soap/Result.hpp"

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

/// Write-only disk that streams a monolithicFlat VMDK's raw extent with one HTTPS PUT of a precomputed
/// `Content-Length`, since the datastore folder endpoint has no ranged PUT.
///
/// Writes must come in non-decreasing, non-overlapping offset order.
/// Gaps are sent as zeros in bounded batches, and `flush()` zero-fills the tail, so the body always ends
/// at exactly `virtualSizeBytes`.
///
/// It has the usual disk methods but no disk base class: a tool wraps it into its own disk interface.
/// Not thread-safe from the caller's side.
///
class VSphereFolderPutBackend final {
public:
    /// Opens a dedicated HTTPS connection to the PUT URL and starts the background upload thread.
    /// Fails with `ErrorCode::InvalidArgument` on an unparseable URL or a zero size.
    static VSphereSoap::Result<std::unique_ptr<VSphereFolderPutBackend>>
    open(VSphereFolderPutBackendParams params);

    /// Aborts the upload (best-effort) if `flush()` was never called or failed, then joins
    /// the background thread.
    ~VSphereFolderPutBackend();

    VSphereFolderPutBackend(const VSphereFolderPutBackend&) = delete;
    VSphereFolderPutBackend& operator=(const VSphereFolderPutBackend&) = delete;
    VSphereFolderPutBackend(VSphereFolderPutBackend&&) = delete;
    VSphereFolderPutBackend& operator=(VSphereFolderPutBackend&&) = delete;

    /// Always fails with `ErrorCode::NotImplemented`: this backend is write-only.
    VSphereSoap::Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out);

    /// Sends `in` at `offset`, zero-filling any gap after the previous write.
    /// Fails with `ErrorCode::InvalidArgument` on an out-of-order or out-of-range write.
    VSphereSoap::Result<std::uint64_t> writeAt(std::uint64_t offset, std::span<const std::byte> in);

    std::uint64_t virtualSize() const noexcept;
    std::uint64_t sectorSize() const noexcept;

    /// Zero-fills any remaining tail, signals end-of-body, joins the upload thread, and fails if
    /// the datastore's HTTP response wasn't 2xx.
    /// Idempotent.
    VSphereSoap::Error flush();

private:
    class Impl;
    explicit VSphereFolderPutBackend(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VSphereSoap
