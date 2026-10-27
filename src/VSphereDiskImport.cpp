// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Public wrapper for attaching uploaded disks to vSphere VMs

#include "VSphereDiskImport.hpp"

#include "DiskAttach.hpp"
#include "HttpTransportAdapter.hpp"
#include "SoapClient.hpp"
#include "SoapLog.hpp"
#include "VimInventory.hpp"
#include "VimSession.hpp"
#include "Error.hpp"

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

    Detail::VmLocator targetLocator;
    if (spec.targetVm.moref) {
        targetLocator = Detail::VmLocator::byMoref(*spec.targetVm.moref);
    } else if (spec.targetVm.uuid) {
        targetLocator = Detail::VmLocator::byUuid(*spec.targetVm.uuid);
    } else if (spec.targetVm.inventoryPath) {
        targetLocator = Detail::VmLocator::byInventoryPath(*spec.targetVm.inventoryPath);
    } else {
        return Error(ErrorCode::InvalidArgument, "VSphereAttachSpec::targetVm has none of moref/uuid/inventoryPath set");
    }

    logger()->info("attaching placeholder VM {}'s disk to target VM on {}:{}", spec.placeholderVmId, params.host,
                    params.port);

    Detail::HttpTransportAdapter transport(params.host, params.port, params.verifyTlsCertificate,
                                             params.pinnedThumbprint);
    Detail::SoapClient soap(transport, params.host);

    auto sessionResult = Detail::VimSession::open(soap, params.username, params.password);
    if (!sessionResult) {
        logger()->warn("VIM session open against {}:{} failed: {}", params.host, params.port,
                        sessionResult.error().toString());
        return std::move(sessionResult).error();
    }
    std::unique_ptr<Detail::VimSession> session = std::move(sessionResult).value();

    Detail::VimInventory inventory(*session);
    auto targetMorefResult = inventory.findVm(targetLocator);
    if (!targetMorefResult) {
        return std::move(targetMorefResult).error();
    }

    Detail::DiskAttachSpec attachSpec;
    attachSpec.placeholderVm = Detail::MoRef{"VirtualMachine", spec.placeholderVmId};
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

Error eagerZeroAllDisks(const ConnectParams& params, const VmLocator& vm)
{
    Detail::VmLocator locator;
    if (vm.moref) {
        locator = Detail::VmLocator::byMoref(*vm.moref);
    } else if (vm.uuid) {
        locator = Detail::VmLocator::byUuid(*vm.uuid);
    } else if (vm.inventoryPath) {
        locator = Detail::VmLocator::byInventoryPath(*vm.inventoryPath);
    } else {
        return Error(ErrorCode::InvalidArgument, "VmLocator has none of moref/uuid/inventoryPath set");
    }

    Detail::HttpTransportAdapter transport(params.host, params.port, params.verifyTlsCertificate,
                                             params.pinnedThumbprint);
    Detail::SoapClient soap(transport, params.host);

    auto sessionResult = Detail::VimSession::open(soap, params.username, params.password);
    if (!sessionResult) {
        logger()->warn("VIM session open against {}:{} failed: {}", params.host, params.port,
                        sessionResult.error().toString());
        return std::move(sessionResult).error();
    }
    auto session = std::move(sessionResult).value();

    Detail::VimInventory inventory(*session);
    auto vmMorefResult = inventory.findVm(locator);
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
