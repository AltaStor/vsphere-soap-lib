// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Public wrapper for attaching uploaded disks to vSphere VMs

#include "vsphere-soap/VSphereDiskImport.hpp"

#include <algorithm>

#include "DatastorePath.hpp"
#include "DiskAttach.hpp"
#include "VmLookup.hpp"
#include "vsphere-soap/vim/HttpTransportAdapter.hpp"
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

Result<VSphereAttachResult> attachImportedDisk(const ConnectParams& params, const VSphereAttachSpec& spec)
{
    if (spec.placeholderVmId.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereAttachSpec::placeholderVmId must not be empty");
    }

    if (Error invalid = Detail::checkVmLocator(spec.targetVm, "VSphereAttachSpec::targetVm"); !invalid.ok()) {
        return invalid;
    }

    logger()->info("attaching placeholder VM {}'s disk to target VM on {}:{}", spec.placeholderVmId, params.host,
                    params.port);

    Vim::HttpTransportAdapter transport(params.host, params.port, params.verifyTlsCertificate,
                                             params.pinnedThumbprint);
    Vim::SoapClient soap(transport, params.host);

    auto sessionResult = Vim::VimSession::open(soap, params.username, params.password);
    if (!sessionResult) {
        logger()->warn("VIM session open against {}:{} failed: {}", params.host, params.port,
                        sessionResult.error().toString());
        return std::move(sessionResult).error();
    }
    std::unique_ptr<Vim::VimSession> session = std::move(sessionResult).value();

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

Result<std::string> releaseImportedDisk(const ConnectParams& params, const std::string& placeholderVmId,
                                        const std::string& datastorePath, const std::string& diskProvisioning)
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

    logger()->info("moving placeholder VM {}'s disk to {} on {}:{}", placeholderVmId, datastorePath, params.host,
                   params.port);

    Vim::HttpTransportAdapter transport(params.host, params.port, params.verifyTlsCertificate, params.pinnedThumbprint);
    Vim::SoapClient soap(transport, params.host);
    auto sessionResult = Vim::VimSession::open(soap, params.username, params.password);
    if (!sessionResult) {
        return std::move(sessionResult).error();
    }

    Detail::DiskAttachSpec spec;
    spec.placeholderVm = Vim::MoRef{"VirtualMachine", placeholderVmId};
    spec.desiredFileName = slash == std::string::npos ? parsed->second : parsed->second.substr(slash + 1);
    const std::string dir = slash == std::string::npos ? std::string() : " " + parsed->second.substr(0, slash);
    spec.desiredDatastoreDir = "[" + parsed->first + "]" + dir;
    spec.diskProvisioning = diskProvisioning;

    auto result = Detail::attachUploadedDisk(*sessionResult.value(), spec);
    if (!result) {
        return std::move(result).error();
    }
    return result.value().datastorePath;
}

Result<bool> datastoreDiskExists(const ConnectParams& params, const std::string& datastorePath)
{
    const auto parsed = Vim::parseDatastorePath(datastorePath);
    if (!parsed || parsed->second.empty() || parsed->second.back() == '/') {
        return Error(ErrorCode::InvalidArgument, "'" + datastorePath + "' is not a '[datastore] dir/name.vmdk' path");
    }

    Vim::HttpTransportAdapter transport(params.host, params.port, params.verifyTlsCertificate, params.pinnedThumbprint);
    Vim::SoapClient soap(transport, params.host);
    auto sessionResult = Vim::VimSession::open(soap, params.username, params.password);
    if (!sessionResult) {
        return std::move(sessionResult).error();
    }
    Vim::VimInventory inventory(*sessionResult.value());

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

Error eagerZeroAllDisks(const ConnectParams& params, const VmLocator& vm)
{
    if (Error invalid = Detail::checkVmLocator(vm, "VmLocator"); !invalid.ok()) {
        return invalid;
    }

    Vim::HttpTransportAdapter transport(params.host, params.port, params.verifyTlsCertificate,
                                             params.pinnedThumbprint);
    Vim::SoapClient soap(transport, params.host);

    auto sessionResult = Vim::VimSession::open(soap, params.username, params.password);
    if (!sessionResult) {
        logger()->warn("VIM session open against {}:{} failed: {}", params.host, params.port,
                        sessionResult.error().toString());
        return std::move(sessionResult).error();
    }
    auto session = std::move(sessionResult).value();

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

} // namespace VSphereSoap
