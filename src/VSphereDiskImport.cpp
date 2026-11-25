// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Public wrapper for attaching uploaded disks to vSphere VMs

#include "vsphere-soap/VSphereDiskImport.hpp"

#include <algorithm>

#include "ClientOperations.hpp"
#include "DatastorePath.hpp"
#include "DiskAttach.hpp"
#include "VmLookup.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "SoapLog.hpp"
#include "vsphere-soap/vim/VimInventory.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;
using VSphereSoap::Result;

using Detail::logger;

Result<VSphereAttachResult> Detail::attachImportedDisk(Detail::ClientConnection& connection,
                                                       const VSphereAttachSpec& spec)
{
    if (spec.placeholderVmId.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereAttachSpec::placeholderVmId must not be empty");
    }

    if (Error invalid = Detail::checkVmLocator(spec.targetVm, "VSphereAttachSpec::targetVm"); !invalid.ok()) {
        return invalid;
    }

    logger()->info("attaching placeholder VM {}'s disk to target VM on {}:{}", spec.placeholderVmId,
                   connection.params().host, connection.params().port);

    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::VimSession* session = open.value()->session.get();

    Vim::VimInventory inventory(*session);
    auto targetMorefResult = Detail::findVm(inventory, spec.targetVm, "VSphereAttachSpec::targetVm");
    if (!targetMorefResult) {
        return std::move(targetMorefResult).error();
    }

    Detail::DiskAttachSpec attachSpec;
    attachSpec.placeholderVm = Vim::MoRef{"VirtualMachine", spec.placeholderVmId};
    attachSpec.targetVm = targetMorefResult.value();
    attachSpec.controller.controllerType = spec.controller.controllerType;
    attachSpec.controller.busNumber = spec.controller.busNumber;
    attachSpec.controller.createIfMissing = spec.controller.createIfMissing;
    attachSpec.desiredFileName = spec.desiredFileName;
    attachSpec.diskProvisioning = spec.diskProvisioning;
    attachSpec.desiredDatastoreDir = spec.desiredDatastoreDir;

    auto result = Detail::attachUploadedDisk(*session, attachSpec);
    if (!result) {
        return std::move(result).error();
    }

    return VSphereAttachResult{result.value().datastorePath, result.value().controllerKey,
                                 result.value().unitNumber};
}

Result<std::string> Detail::releaseImportedDisk(Detail::ClientConnection& connection,
                                                const std::string& placeholderVmId, const std::string& datastorePath,
                                                const std::string& diskProvisioning)
{
    if (placeholderVmId.empty()) {
        return Error(ErrorCode::InvalidArgument, "releaseImportedDisk: the placeholder VM id must not be empty");
    }
    const auto parsed = Vim::parseDatastorePath(datastorePath);
    if (!parsed || parsed->second.empty() || parsed->second.back() == '/') {
        return Error(ErrorCode::InvalidArgument,
                     "releaseImportedDisk: '" + datastorePath + "' is not a '[datastore] dir/name.vmdk' path");
    }
    const auto slash = parsed->second.find_last_of('/');

    logger()->info("moving placeholder VM {}'s disk to {} on {}:{}", placeholderVmId, datastorePath,
                   connection.params().host, connection.params().port);

    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::VimSession* session = open.value()->session.get();

    Detail::DiskAttachSpec spec;
    spec.placeholderVm = Vim::MoRef{"VirtualMachine", placeholderVmId};
    spec.desiredFileName = slash == std::string::npos ? parsed->second : parsed->second.substr(slash + 1);
    const std::string dir = slash == std::string::npos ? std::string() : " " + parsed->second.substr(0, slash);
    spec.desiredDatastoreDir = "[" + parsed->first + "]" + dir;
    spec.diskProvisioning = diskProvisioning;

    auto result = Detail::attachUploadedDisk(*session, spec);
    if (!result) {
        return std::move(result).error();
    }
    return result.value().datastorePath;
}

Result<bool> Detail::datastoreDiskExists(Detail::ClientConnection& connection, const std::string& datastorePath)
{
    const auto parsed = Vim::parseDatastorePath(datastorePath);
    if (!parsed || parsed->second.empty() || parsed->second.back() == '/') {
        return Error(ErrorCode::InvalidArgument, "'" + datastorePath + "' is not a '[datastore] dir/name.vmdk' path");
    }

    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::IHttpTransport& transport = *open.value()->transport;
    const Vim::SoapClient& soap = *open.value()->soap;
    Vim::VimSession* session = open.value()->session.get();
    Vim::VimInventory inventory(*session);

    // The datastore HTTP path names the datacenter, so find the one that lists the datastore.
    auto datastores = inventory.listObjects("Datastore", {"name"});
    if (!datastores) {
        return std::move(datastores).error();
    }
    const auto datastore = std::find_if(datastores.value().begin(), datastores.value().end(),
                                        [&](const auto& ds) { return ds.get("name") == parsed->first; });
    if (datastore == datastores.value().end()) {
        return Error(ErrorCode::InvalidArgument, "datastore '" + parsed->first + "' not found");
    }
    auto datacenters = inventory.listObjects("Datacenter", {"name", "datastore"});
    if (!datacenters) {
        return std::move(datacenters).error();
    }
    const auto datacenter = std::find_if(datacenters.value().begin(), datacenters.value().end(), [&](const auto& dc) {
        const std::string listed = "," + dc.get("datastore") + ",";
        return listed.find("," + datastore->moref.value + ",") != std::string::npos;
    });
    if (datacenter == datacenters.value().end()) {
        return Error(ErrorCode::InvalidFormat, "no datacenter lists datastore '" + parsed->first + "'");
    }

    // A disk is its descriptor and its extent, and either one taken makes the name unusable.
    const std::string query =
        "dcPath=" + Detail::urlEncode(datacenter->get("name")) + "&dsName=" + Detail::urlEncode(parsed->first);
    for (const std::string& relative : {parsed->second, Detail::deriveFlatName(parsed->second)}) {
        auto exists = Detail::datastoreFileExists(transport, "/folder/" + Detail::urlEncode(relative) + "?" + query,
                                                  soap.sessionCookie());
        if (!exists || exists.value()) {
            return exists;
        }
    }
    return false;
}

Error Detail::eagerZeroAllDisks(Detail::ClientConnection& connection, const Detail::PublicVmLocator& vm)
{
    if (Error invalid = Detail::checkVmLocator(vm, "VmLocator"); !invalid.ok()) {
        return invalid;
    }

    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::VimSession* session = open.value()->session.get();

    Vim::VimInventory inventory(*session);
    auto vmMorefResult = Detail::findVm(inventory, vm);
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }
    auto vmInfoResult = inventory.describeVm(vmMorefResult.value());
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }
    auto datacenterResult = inventory.resolveDatacenterMoRef(vmMorefResult.value());
    if (!datacenterResult) {
        return std::move(datacenterResult).error();
    }

    for (const auto& disk : vmInfoResult.value().disks) {
        if (auto zeroed = Detail::eagerZeroVirtualDisk(*session, disk.rawFileName, datacenterResult.value());
            !zeroed.ok()) {
            return Error(ErrorCode::IoError,
                         "failed to eager-zero disk '" + disk.rawFileName + "': " + zeroed.message());
        }
    }
    return {};
}

Error Detail::checkAttachPoint(Detail::ClientConnection& connection, const Detail::PublicVmLocator& targetVm,
                              const VSphereAttachControllerSpec& controller)
{
    auto resolved = Detail::resolveAttachPoint(connection, targetVm, controller);
    if (!resolved) {
        return std::move(resolved).error();
    }
    return {};
}

Result<std::string> Detail::resolveAttachPoint(Detail::ClientConnection& connection,
                                               const Detail::PublicVmLocator& targetVm,
                                               const VSphereAttachControllerSpec& controller)
{
    if (Error invalid = Detail::checkVmLocator(targetVm, "VmLocator"); !invalid.ok()) {
        return invalid;
    }

    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::VimInventory inventory(*open.value()->session);
    auto vmMoref = Detail::findVm(inventory, targetVm);
    if (!vmMoref) {
        return std::move(vmMoref).error();
    }
    auto vmInfo = inventory.describeVm(vmMoref.value());
    if (!vmInfo) {
        return std::move(vmInfo).error();
    }

    Detail::ControllerSelector selector;
    selector.controllerType = controller.controllerType;
    selector.busNumber = controller.busNumber;
    selector.createIfMissing = controller.createIfMissing;
    auto choice = Detail::chooseControllerAndUnit(vmInfo.value(), selector);
    if (!choice) {
        return std::move(choice).error();
    }
    return Detail::describeControllerChoice(vmInfo.value(), choice.value());
}

// One-shot wrappers: one login per call. See VSphereClient for running several operations on one login.

Result<VSphereAttachResult> attachImportedDisk(const ConnectParams& params, const VSphereAttachSpec& spec)
{
    Detail::ClientConnection connection(params);
    return Detail::attachImportedDisk(connection, spec);
}

Result<std::string> releaseImportedDisk(const ConnectParams& params, const std::string& placeholderVmId,
                                        const std::string& datastorePath, const std::string& diskProvisioning)
{
    Detail::ClientConnection connection(params);
    return Detail::releaseImportedDisk(connection, placeholderVmId, datastorePath, diskProvisioning);
}

Result<bool> datastoreDiskExists(const ConnectParams& params, const std::string& datastorePath)
{
    Detail::ClientConnection connection(params);
    return Detail::datastoreDiskExists(connection, datastorePath);
}

Error checkAttachPoint(const ConnectParams& params, const VmLocator& targetVm,
                       const VSphereAttachControllerSpec& controller)
{
    Detail::ClientConnection connection(params);
    return Detail::checkAttachPoint(connection, targetVm, controller);
}

Result<std::string> resolveAttachPoint(const ConnectParams& params, const VmLocator& targetVm,
                                       const VSphereAttachControllerSpec& controller)
{
    Detail::ClientConnection connection(params);
    return Detail::resolveAttachPoint(connection, targetVm, controller);
}

Error eagerZeroAllDisks(const ConnectParams& params, const VmLocator& vm)
{
    Detail::ClientConnection connection(params);
    return Detail::eagerZeroAllDisks(connection, vm);
}

} // namespace VSphereSoap
