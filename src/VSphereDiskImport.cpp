// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Public wrapper for attaching uploaded disks to vSphere VMs

#include "vsphere-soap/VSphereDiskImport.hpp"

#include "DiskAttach.hpp"
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

    Vim::VmLocator targetLocator;
    if (spec.targetVm.moref) {
        targetLocator = Vim::VmLocator::byMoref(*spec.targetVm.moref);
    } else if (spec.targetVm.uuid) {
        targetLocator = Vim::VmLocator::byUuid(*spec.targetVm.uuid);
    } else if (spec.targetVm.inventoryPath) {
        targetLocator = Vim::VmLocator::byInventoryPath(*spec.targetVm.inventoryPath);
    } else {
        return Error(ErrorCode::InvalidArgument, "VSphereAttachSpec::targetVm has none of moref/uuid/inventoryPath set");
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
    auto targetMorefResult = inventory.findVm(targetLocator);
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

Error eagerZeroAllDisks(const ConnectParams& params, const VmLocator& vm)
{
    Vim::VmLocator locator;
    if (vm.moref) {
        locator = Vim::VmLocator::byMoref(*vm.moref);
    } else if (vm.uuid) {
        locator = Vim::VmLocator::byUuid(*vm.uuid);
    } else if (vm.inventoryPath) {
        locator = Vim::VmLocator::byInventoryPath(*vm.inventoryPath);
    } else {
        return Error(ErrorCode::InvalidArgument, "VmLocator has none of moref/uuid/inventoryPath set");
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
