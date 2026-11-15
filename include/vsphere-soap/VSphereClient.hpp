// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// One reusable vSphere SOAP session for several operations

#pragma once

// Part of vsphere-soap-lib's public API. Each ConnectParams-based free function in this library opens a
// TLS connection, logs in, runs one call and logs out. A caller making several calls against the same
// host with the same credentials should open one VSphereClient instead: one handshake and one
// Login/Logout for all of them, rather than a new connection per call (bursts of which have tripped
// connection-flood protection in the field).
//
// The members mirror those free functions, minus the ConnectParams argument, with the same arguments,
// results and failure modes; the free functions themselves now run the same code through a temporary
// connection. Long-lived transfer objects (VmwareSoapDisk, VSphereImportSession) keep their own session.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vsphere-soap/Result.hpp"
#include "vsphere-soap/VSphereDiskImport.hpp"
#include "vsphere-soap/VSphereFolderPutImport.hpp"
#include "vsphere-soap/VSphereImportSession.hpp"
#include "vsphere-soap/VSpherePlacement.hpp"
#include "vsphere-soap/VSphereVmNetworkInfo.hpp"
#include "vsphere-soap/VmwareSoapDisk.hpp" // ConnectParams/VmLocator/DiskSelector

namespace VSphereSoap {

/// A logged-in vSphere SOAP session that any number of operations run on.
///
/// Not thread-safe: use one client per thread. Calls on one client run one after another over one
/// connection.
/// An expired session is handled underneath: a call that faults with `NotAuthenticated` logs in again
/// and is retried once.
///
class VSphereClient final {
public:
    /// Connects and logs in.
    /// Fails as the free functions do when they cannot log in: `ErrorCode::IoError` on a connection, TLS
    /// or authentication failure, `ErrorCode::InvalidFormat` on an unexpected `RetrieveServiceContent`
    /// response.
    static Result<std::unique_ptr<VSphereClient>> open(const ConnectParams& params);

    /// Logs out, best-effort: a failed Logout is not reported and the session just expires on the server.
    /// When a `VSphereFolderPutPrepResult` from `prepareFolderPutImport()`/`prepareFolderPutOverwrite()`
    /// is still alive, the Logout waits until that result is destroyed too (its `keepAlive` shares this
    /// session, so the cookie it carries keeps working).
    ~VSphereClient();

    VSphereClient(const VSphereClient&) = delete;
    VSphereClient& operator=(const VSphereClient&) = delete;
    VSphereClient(VSphereClient&&) = delete;
    VSphereClient& operator=(VSphereClient&&) = delete;

    /// See the free function of the same name in VSphereVmNetworkInfo.hpp.
    Result<std::vector<VSphereNetworkAdapterInfo>> listVmNetworkAdapters(const VmLocator& vm);
    /// See the free function of the same name in VSphereVmNetworkInfo.hpp.
    Result<std::string> getVmNotes(const VmLocator& vm);
    /// See the free function of the same name in VSphereVmNetworkInfo.hpp.
    Result<std::vector<VSphereDiskSummary>> listVmDisks(const VmLocator& vm);
    /// See the free function of the same name in VSphereVmNetworkInfo.hpp.
    Result<std::string> getVmPowerState(const VmLocator& vm);
    /// See the free function of the same name in VSphereVmNetworkInfo.hpp.
    Error powerOnVm(const VmLocator& vm);
    /// See the free function of the same name in VSphereVmNetworkInfo.hpp.
    Error powerOffVm(const VmLocator& vm);
    /// See the free function of the same name in VSphereVmNetworkInfo.hpp.
    Error destroyVm(const VmLocator& vm);
    /// See the free function of the same name in VSphereVmNetworkInfo.hpp.
    Error checkDatastoreWriteAccess(const std::string& datastoreName, const std::string& resourcePoolMoref);

    /// See the free function of the same name in VSpherePlacement.hpp.
    Result<VSpherePlacement> resolvePlacement(const VSpherePlacementSpec& spec);

    /// See the free function of the same name in VSphereDiskImport.hpp.
    Result<VSphereAttachResult> attachImportedDisk(const VSphereAttachSpec& spec);
    /// See the free function of the same name in VSphereDiskImport.hpp.
    Result<std::string> releaseImportedDisk(const std::string& placeholderVmId, const std::string& datastorePath,
                                            const std::string& diskProvisioning);
    /// See the free function of the same name in VSphereDiskImport.hpp.
    Result<bool> datastoreDiskExists(const std::string& datastorePath);
    /// See the free function of the same name in VSphereDiskImport.hpp.
    Error eagerZeroAllDisks(const VmLocator& vm);
    /// See the free function of the same name in VSphereDiskImport.hpp.
    Error checkAttachPoint(const VmLocator& targetVm, const VSphereAttachControllerSpec& controller);

    /// See the free function of the same name in VSphereFolderPutImport.hpp.
    /// The result's `sessionCookie` is this client's session, and its `keepAlive` shares it: the cookie
    /// stays valid until both this client and the result are gone.
    Result<VSphereFolderPutPrepResult> prepareFolderPutImport(const VSphereFolderPutPrepSpec& spec);
    /// See the free function of the same name in VSphereFolderPutImport.hpp; same session sharing as
    /// `prepareFolderPutImport()`.
    Result<VSphereFolderPutPrepResult> prepareFolderPutOverwrite(const VmLocator& vm, const DiskSelector& disk);
    /// See the free function of the same name in VSphereFolderPutImport.hpp.
    Error finishFolderPutOverwrite(const VmLocator& vm);
    /// See the free function of the same name in VSphereFolderPutImport.hpp.
    Result<VSphereAttachResult> finishFolderPutImport(const VmLocator& targetVm,
                                                      const std::string& descriptorDatastorePath,
                                                      std::uint64_t virtualSizeBytes,
                                                      const VSphereAttachControllerSpec& controller);
    /// See the free function of the same name in VSphereFolderPutImport.hpp.
    Error cleanupFolderPutImport(const VmLocator& targetVm, const std::string& descriptorDatastorePath);

    /// See `VSphereImportSession::validate()`.
    Error validateImport(const VSphereImportVmSpec& spec);

private:
    class Impl;
    explicit VSphereClient(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace VSphereSoap
