// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere folder-put import control plane

#pragma once

// Part of vsphere-soap-lib's public API: the folder-put import fallback's control-plane half.
// Pairs with VSphereFolderPutBackend.hpp (the data-plane half, which streams the flat extent itself).
//
// Unlike the primary (`VSphereImportSession`/`VSphereDatastoreBackend`/`attachImportedDisk`) path, this
// one has no placeholder VM: the file is written directly into the target VM's own folder, so there is
// nothing to detach/move/unregister afterward; `finishFolderPutImport` is a single attach reconfig.

#include <cstdint>
#include <memory>
#include <string>

#include "Result.hpp"
#include "VSphereDiskImport.hpp" // reuses VSphereAttachControllerSpec/VSphereAttachResult
#include "VmwareSoapDisk.hpp"     // reuses ConnectParams/VmLocator

namespace VSphereSoap {

/// Opaque RAII handle keeping `VSphereFolderPutPrepResult`'s vim25 session (and therefore its
/// `sessionCookie`) alive and logged in.
/// The datastore `/folder/` PUT this cookie authenticates is issued *after* `prepareFolderPutImport`
/// returns (by a separate `VSphereFolderPutBackend` call, on a separate HTTPS connection): confirmed
/// live that without this, the session backing the cookie gets best-effort logged out the moment its
/// owning `VimSession` is destroyed (every `VimSession` in this codebase does this in its destructor),
/// invalidating the cookie before the flat extent PUT ever uses it, which surfaces as a bare HTTP 401
/// with no other explanation.
/// A caller just needs to keep `VSphereFolderPutPrepResult` itself alive (this member included) until
/// after that PUT completes; nothing needs to be called on this type directly.
///
class VSphereFolderPutSessionHandle;

struct VSphereFolderPutPrepSpec {
    /// Identifies the already-existing, powered-off target VM: same precondition as the primary path's
    /// `attachImportedDisk`.
    VmLocator targetVm;
    /// Filename (no directory, no "[datastore]" prefix) for the new disk, e.g. "mydisk" or
    /// "mydisk.vmdk": a ".vmdk" extension is appended if missing.
    /// Required (unlike the primary path's `VSphereAttachSpec::desiredFileName`, there is no
    /// placeholder-assigned name to fall back to here).
    ///
    std::string fileName;
    /// The *logical* (guest-visible) disk size this path will write: becomes both the flat extent's
    /// exact `Content-Length` and the descriptor's declared sector count.
    /// Must be nonzero.
    std::uint64_t virtualSizeBytes = 0;
    /// Embedded in the descriptor's `ddb.adapterType` line, purely cosmetic (see `VSphereAttachSpec`'s
    /// own equivalent field's doc comment on why this doesn't drive the real attach).
    /// Empty defaults to "lsilogic".
    std::string adapterType;
};

struct VSphereFolderPutPrepResult {
    // Declared (not defaulted) and defined out-of-line in VSphereFolderPutImport.cpp, where
    // VSphereFolderPutSessionHandle is a complete type; required because this struct holds
    // a unique_ptr<VSphereFolderPutSessionHandle> to that forward-declared type: an implicitly-defaulted
    // destructor/move (the usual choice for a plain struct like this) would need the complete type at
    // every call site that destroys or moves a VSphereFolderPutPrepResult, including ones (like
    // Result<VSphereFolderPutPrepResult>'s own destructor, instantiated wherever a caller receives one)
    // that never otherwise need to know VSphereFolderPutSessionHandle exists.
    //
    VSphereFolderPutPrepResult();
    ~VSphereFolderPutPrepResult();
    VSphereFolderPutPrepResult(VSphereFolderPutPrepResult&&) noexcept;
    VSphereFolderPutPrepResult& operator=(VSphereFolderPutPrepResult&&) noexcept;
    VSphereFolderPutPrepResult(const VSphereFolderPutPrepResult&) = delete;
    VSphereFolderPutPrepResult& operator=(const VSphereFolderPutPrepResult&) = delete;

    /// Ready to hand to `VSphereFolderPutBackendParams::putUrl` to stream the flat extent itself.
    std::string flatPutUrl;
    /// Ready to hand to `VSphereFolderPutBackendParams::sessionCookie`.
    std::string sessionCookie;
    /// The descriptor's own "[datastore] relative/path.vmdk"; this is the disk to attach, and what
    /// `finishFolderPutImport` needs.
    std::string descriptorDatastorePath;
    /// See `VSphereFolderPutSessionHandle`'s own doc comment: keep this alive (i.e. keep the whole
    /// `VSphereFolderPutPrepResult` in scope) until after streaming the flat extent completes.
    std::unique_ptr<VSphereFolderPutSessionHandle> keepAlive;
};

/// Connects, resolves the target VM (fails if not powered off), builds the destination paths inside its
/// own folder, and PUTs the small monolithicFlat text descriptor (not the flat extent itself: that could
/// be many GB and has no business being buffered into a `std::string` the way this small PUT is).
/// The returned `flatPutUrl`/`sessionCookie` are then handed to a fresh
/// `VSphereFolderPutBackend::open()` call (a separate HTTPS connection, matching how the primary path's
/// lease device URL and SOAP control plane are always separate connections too) to stream the real data.
///
/// Fails with `ErrorCode::InvalidArgument` if the target VM is powered on or
/// `spec.fileName`/`spec.virtualSizeBytes` is empty/zero; `ErrorCode::IoError` on
/// connection/auth/transport failure.
///
VSphereSoap::Result<VSphereFolderPutPrepResult> prepareFolderPutImport(
    const ConnectParams& params, const VSphereFolderPutPrepSpec& spec);

/// Attaches the already-fully-written file at `descriptorDatastorePath` (from `prepareFolderPutImport`'s
/// result, after the caller's own `VSphereFolderPutBackend` stream succeeded) to the target VM: a single
/// `ReconfigVM_Task`, no placeholder/detach/move/unregister, since the file is already at its final
/// location.
/// Connects independently (does not reuse `prepareFolderPutImport`'s session).
///
/// Reports the attached disk as `eagerZeroedThick` (`thinProvisioned` false, `eagerlyScrub` true) on
/// the resulting backing; an accurate label, not an assumption: this transport always PUTs the disk's
/// full logical size with every byte (zeros included) actually sent, so the file is genuinely,
/// physically fully allocated by the time this is called, the functional definition of
/// eager-zeroed-thick regardless of which VIM API path created it.
///
VSphereSoap::Result<VSphereAttachResult> finishFolderPutImport(
    const ConnectParams& params, const VmLocator& targetVm, const std::string& descriptorDatastorePath,
    std::uint64_t virtualSizeBytes, const VSphereAttachControllerSpec& controller);

/// Best-effort cleanup for a failed/cancelled `folder-put` upload: deletes both the descriptor
/// (`descriptorDatastorePath`) and its derived flat extent file from the datastore
/// (`DeleteDatastoreFile_Task`), unlike the primary lease transport where a single `Destroy_Task` on
/// the placeholder VM removes everything at once; folder-put has no placeholder, so each file needs its
/// own delete.
/// Connects independently.
/// Logs (does not fail loudly on) a file that turns out not to exist, e.g. because the flat extent's PUT
/// never got far enough to create it; only returns an error for a connection/auth failure that meant no
/// cleanup could even be attempted.
///
VSphereSoap::Error cleanupFolderPutImport(const ConnectParams& params,
                                                                     const VmLocator& targetVm,
                                                                     const std::string& descriptorDatastorePath);

} // namespace VSphereSoap
