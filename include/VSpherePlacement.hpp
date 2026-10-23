// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Name-based placement lookup for imported VMs

#pragma once

// Turns the names an operator knows (a datastore, a cluster or standalone host, optionally a resource
// pool and a VM folder) into the managed object ids `VSphereImportVmSpec` takes, over the same SOAP
// session as everything else in this library.
// Works on vCenter and on a standalone ESXi host.

#include <string>

#include "Result.hpp"
#include "VmwareSoapDisk.hpp" // reuses ConnectParams

namespace VSphereSoap {

struct VSpherePlacementSpec {
    /// Datastore the imported disks go to.
    /// Required.
    std::string datastoreName;
    /// Cluster or standalone host (a ComputeResource) to run the VM on.
    /// Empty picks the only compute resource that has the datastore mounted, and fails if there is more
    /// than one.
    std::string computeName;
    /// Resource pool under that compute resource.
    /// Empty uses its root pool.
    std::string resourcePoolName;
    /// VM folder to put the VM in, by a name unique among VM folders.
    /// Empty uses the root VM folder of the datastore's datacenter.
    std::string folderName;
};

/// Managed object ids, ready for `VSphereImportVmSpec::datastoreId`, `resourcePoolId` and `folderId`.
struct VSpherePlacement {
    std::string datastoreId;
    std::string resourcePoolId;
    std::string folderId;
};

/// Connects, logs in, and resolves `spec` by name.
/// Fails with `ErrorCode::InvalidArgument` when a name matches nothing or more than one object
/// (the message lists the candidates), `ErrorCode::IoError` on connection/auth/transport failure.
[[nodiscard]] Result<VSpherePlacement> resolvePlacement(const ConnectParams& params, const VSpherePlacementSpec& spec);

} // namespace VSphereSoap
