// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Bodies of the ConnectParams-based operations, run on a ClientConnection

#pragma once

// Each public free function (one login per call) and the matching `VSphereClient` member (one login for
// the client's lifetime) call the body declared here, so the two paths share one implementation.
// A body checks its arguments before calling `connection.get()`, so an invalid argument is reported
// without connecting.

#include <cstdint>
#include <string>
#include <vector>

#include "VimConnection.hpp"
#include "vsphere-soap/Result.hpp"
#include "vsphere-soap/VSphereDiskImport.hpp"
#include "vsphere-soap/VSphereFolderPutImport.hpp"
#include "vsphere-soap/VSphereImportSession.hpp"
#include "vsphere-soap/VSpherePlacement.hpp"
#include "vsphere-soap/VSphereVmNetworkInfo.hpp"

namespace VSphereSoap::Detail {

// Detail also sees Vim::VmLocator through using-directives in other internal headers.
using PublicVmLocator = ::VSphereSoap::VmLocator;

// VSphereVmNetworkInfo.cpp
Result<std::vector<VSphereNetworkAdapterInfo>> listVmNetworkAdapters(ClientConnection& connection,
                                                                     const PublicVmLocator& vm);
Result<std::string> getVmNotes(ClientConnection& connection, const PublicVmLocator& vm);
Result<std::vector<VSphereDiskSummary>> listVmDisks(ClientConnection& connection, const PublicVmLocator& vm);
Result<std::string> getVmPowerState(ClientConnection& connection, const PublicVmLocator& vm);
Error powerOnVm(ClientConnection& connection, const PublicVmLocator& vm);
Error powerOffVm(ClientConnection& connection, const PublicVmLocator& vm);
Error destroyVm(ClientConnection& connection, const PublicVmLocator& vm);
Error checkDatastoreWriteAccess(ClientConnection& connection, const std::string& datastoreName,
                                const std::string& resourcePoolMoref);

// VSpherePlacement.cpp
Result<VSpherePlacement> resolvePlacement(ClientConnection& connection, const VSpherePlacementSpec& spec);

// VSphereDiskImport.cpp
Result<VSphereAttachResult> attachImportedDisk(ClientConnection& connection, const VSphereAttachSpec& spec);
Result<std::string> releaseImportedDisk(ClientConnection& connection, const std::string& placeholderVmId,
                                        const std::string& datastorePath, const std::string& diskProvisioning);
Result<bool> datastoreDiskExists(ClientConnection& connection, const std::string& datastorePath);
Error eagerZeroAllDisks(ClientConnection& connection, const PublicVmLocator& vm);
Error checkAttachPoint(ClientConnection& connection, const PublicVmLocator& targetVm,
                       const VSphereAttachControllerSpec& controller);

// VSphereFolderPutImport.cpp
Result<VSphereFolderPutPrepResult> prepareFolderPutImport(ClientConnection& connection,
                                                          const VSphereFolderPutPrepSpec& spec);
Result<VSphereFolderPutPrepResult> prepareFolderPutOverwrite(ClientConnection& connection,
                                                             const PublicVmLocator& vm, const DiskSelector& disk);
Error finishFolderPutOverwrite(ClientConnection& connection, const PublicVmLocator& vm);
Result<VSphereAttachResult> finishFolderPutImport(ClientConnection& connection, const PublicVmLocator& targetVm,
                                                  const std::string& descriptorDatastorePath,
                                                  std::uint64_t virtualSizeBytes,
                                                  const VSphereAttachControllerSpec& controller);
Error cleanupFolderPutImport(ClientConnection& connection, const PublicVmLocator& targetVm,
                             const std::string& descriptorDatastorePath);

// VSphereImportSession.cpp
Error validateImport(ClientConnection& connection, const VSphereImportVmSpec& spec);

} // namespace VSphereSoap::Detail
