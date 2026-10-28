// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Name-based placement lookup for imported VMs

#include "vsphere-soap/VSpherePlacement.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

#include "vsphere-soap/vim/HttpTransportAdapter.hpp"
#include "Placement.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "SoapLog.hpp"
#include "vsphere-soap/vim/VimInventory.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Detail {

namespace {

// Array-valued properties come back from listObjects() as their elements joined with ','.
bool listContains(std::string_view joined, std::string_view wanted)
{
    while (!joined.empty()) {
        const std::size_t comma = joined.find(',');
        if (joined.substr(0, comma) == wanted) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        joined.remove_prefix(comma + 1);
    }
    return false;
}

std::string describeCandidates(const std::vector<const VimObjectProperties*>& candidates)
{
    std::string out;
    for (const VimObjectProperties* candidate : candidates) {
        if (!out.empty()) {
            out += ", ";
        }
        out += "'" + candidate->get("name") + "' (" + candidate->moref.value + ")";
    }
    return out;
}

// Exactly one match, or an InvalidArgument error saying what was looked for and what was found instead.
Result<const VimObjectProperties*> pickOne(const std::vector<const VimObjectProperties*>& matches,
                                           const std::string& what, const std::string& name)
{
    if (matches.empty()) {
        return Error(ErrorCode::InvalidArgument, "no " + what + " named '" + name + "'");
    }
    if (matches.size() > 1) {
        return Error(ErrorCode::InvalidArgument,
                     "more than one " + what + " named '" + name + "': " + describeCandidates(matches));
    }
    return matches.front();
}

std::vector<const VimObjectProperties*> named(const std::vector<VimObjectProperties>& objects, const std::string& name)
{
    std::vector<const VimObjectProperties*> matches;
    for (const auto& object : objects) {
        if (object.get("name") == name) {
            matches.push_back(&object);
        }
    }
    return matches;
}

} // namespace

Result<VSpherePlacement> resolvePlacement(VimSession& session, const VSpherePlacementSpec& spec)
{
    if (spec.datastoreName.empty()) {
        return Error(ErrorCode::InvalidArgument, "placement needs a datastore name");
    }
    VimInventory inventory(session);
    VSpherePlacement placement;

    auto datastores = inventory.listObjects("Datastore", {"name"});
    if (!datastores) {
        return std::move(datastores).error();
    }
    auto datastore = pickOne(named(datastores.value(), spec.datastoreName), "datastore", spec.datastoreName);
    if (!datastore) {
        return std::move(datastore).error();
    }
    placement.datastoreId = datastore.value()->moref.value;

    // The compute resource has to see the datastore; a cluster and a standalone host are both
    // ComputeResources.
    auto computes = inventory.listObjects("ComputeResource", {"name", "datastore", "resourcePool"});
    if (!computes) {
        return std::move(computes).error();
    }
    std::vector<const VimObjectProperties*> computeMatches;
    for (const auto& compute : computes.value()) {
        const bool nameMatches = spec.computeName.empty() || compute.get("name") == spec.computeName;
        if (nameMatches && listContains(compute.get("datastore"), placement.datastoreId)) {
            computeMatches.push_back(&compute);
        }
    }
    if (computeMatches.empty()) {
        return Error(ErrorCode::InvalidArgument,
                     spec.computeName.empty()
                         ? "no cluster or host has datastore '" + spec.datastoreName + "' mounted"
                         : "no cluster or host named '" + spec.computeName + "' with datastore '" +
                               spec.datastoreName + "' mounted");
    }
    if (computeMatches.size() > 1) {
        return Error(ErrorCode::InvalidArgument, "datastore '" + spec.datastoreName +
                                                     "' is mounted on more than one cluster or host, name one of: " +
                                                     describeCandidates(computeMatches));
    }
    const VimObjectProperties& compute = *computeMatches.front();

    if (spec.resourcePoolName.empty()) {
        placement.resourcePoolId = compute.get("resourcePool");
        if (placement.resourcePoolId.empty()) {
            return Error(ErrorCode::InvalidFormat, "compute resource '" + compute.get("name") + "' has no resource pool");
        }
    } else {
        auto pools = inventory.listObjects("ResourcePool", {"name", "owner"});
        if (!pools) {
            return std::move(pools).error();
        }
        std::vector<const VimObjectProperties*> poolMatches;
        for (const VimObjectProperties* pool : named(pools.value(), spec.resourcePoolName)) {
            if (pool->get("owner") == compute.moref.value) {
                poolMatches.push_back(pool);
            }
        }
        auto pool = pickOne(poolMatches, "resource pool under '" + compute.get("name") + "'", spec.resourcePoolName);
        if (!pool) {
            return std::move(pool).error();
        }
        placement.resourcePoolId = pool.value()->moref.value;
    }

    if (spec.folderName.empty()) {
        auto datacenters = inventory.listObjects("Datacenter", {"name", "datastore", "vmFolder"});
        if (!datacenters) {
            return std::move(datacenters).error();
        }
        const auto datacenter = std::find_if(datacenters.value().begin(), datacenters.value().end(), [&](const auto& dc) {
            return listContains(dc.get("datastore"), placement.datastoreId);
        });
        if (datacenter == datacenters.value().end()) {
            return Error(ErrorCode::InvalidFormat, "no datacenter lists datastore '" + spec.datastoreName + "'");
        }
        placement.folderId = datacenter->get("vmFolder");
    } else {
        auto folders = inventory.listObjects("Folder", {"name", "childType"});
        if (!folders) {
            return std::move(folders).error();
        }
        std::vector<const VimObjectProperties*> folderMatches;
        for (const VimObjectProperties* folder : named(folders.value(), spec.folderName)) {
            if (listContains(folder->get("childType"), "VirtualMachine")) {
                folderMatches.push_back(folder);
            }
        }
        auto folder = pickOne(folderMatches, "VM folder", spec.folderName);
        if (!folder) {
            return std::move(folder).error();
        }
        placement.folderId = folder.value()->moref.value;
    }

    logger()->debug("placement for datastore '{}': datastore {}, resource pool {}, folder {}", spec.datastoreName,
                    placement.datastoreId, placement.resourcePoolId, placement.folderId);
    return placement;
}

} // namespace VSphereSoap::Detail

namespace VSphereSoap {

Result<VSpherePlacement> resolvePlacement(const ConnectParams& params, const VSpherePlacementSpec& spec)
{
    Vim::HttpTransportAdapter transport(params.host, params.port, params.verifyTlsCertificate,
                                           params.pinnedThumbprint);
    Vim::SoapClient soap(transport, params.host);

    auto session = Vim::VimSession::open(soap, params.username, params.password);
    if (!session) {
        return std::move(session).error();
    }
    return Detail::resolvePlacement(*session.value(), spec);
}

} // namespace VSphereSoap
