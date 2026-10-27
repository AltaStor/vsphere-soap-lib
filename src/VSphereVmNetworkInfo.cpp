// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere VM network adapter query

#include "VSphereVmNetworkInfo.hpp"

#include <algorithm>
#include <chrono>

#include "DiskAttach.hpp"
#include "HttpTransportAdapter.hpp"
#include "SoapClient.hpp"
#include "SoapLog.hpp"
#include "TaskWait.hpp"
#include "VimInventory.hpp"
#include "VimSession.hpp"
#include "VimTypes.hpp"
#include "Error.hpp"

namespace VSphereSoap {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;
using VSphereSoap::Result;

namespace {
using Detail::logger;

Result<Detail::VmLocator> toInternalLocator(const VmLocator& vm)
{
    if (vm.moref) return Detail::VmLocator::byMoref(*vm.moref);
    if (vm.uuid) return Detail::VmLocator::byUuid(*vm.uuid);
    if (vm.inventoryPath) return Detail::VmLocator::byInventoryPath(*vm.inventoryPath);
    return Error(ErrorCode::InvalidArgument, "VmLocator has none of moref/uuid/inventoryPath set");
}

const char* powerStateName(Detail::VimPowerState state)
{
    switch (state) {
        case Detail::VimPowerState::PoweredOn: return "poweredOn";
        case Detail::VimPowerState::PoweredOff: return "poweredOff";
        case Detail::VimPowerState::Suspended: return "suspended";
        case Detail::VimPowerState::Unknown: return "unknown";
    }
    return "unknown";
}
} // namespace

Result<std::vector<VSphereNetworkAdapterInfo>> listVmNetworkAdapters(const ConnectParams& params,
                                                                         const VmLocator& vm)
{
    Detail::VmLocator internalLocator;
    if (vm.moref) {
        internalLocator = Detail::VmLocator::byMoref(*vm.moref);
    } else if (vm.uuid) {
        internalLocator = Detail::VmLocator::byUuid(*vm.uuid);
    } else if (vm.inventoryPath) {
        internalLocator = Detail::VmLocator::byInventoryPath(*vm.inventoryPath);
    } else {
        return Error(ErrorCode::InvalidArgument, "VmLocator has none of moref/uuid/inventoryPath set");
    }

    logger()->info("listing network adapters on {}:{}", params.host, params.port);
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
    auto vmMorefResult = inventory.findVm(internalLocator);
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }

    auto vmInfoResult = inventory.describeVm(vmMorefResult.value());
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }
    const Detail::VimVmInfo& vmInfo = vmInfoResult.value();

    std::vector<Detail::VimNetworkAdapterInfo> sorted = vmInfo.networkAdapters;
    std::sort(sorted.begin(), sorted.end(),
              [](const Detail::VimNetworkAdapterInfo& a, const Detail::VimNetworkAdapterInfo& b) {
                  return a.key < b.key;
              });

    std::vector<VSphereNetworkAdapterInfo> out;
    out.reserve(sorted.size());
    for (const Detail::VimNetworkAdapterInfo& adapter : sorted) {
        VSphereNetworkAdapterInfo info;
        info.sourceName = adapter.networkName;
        info.networkName = adapter.networkName;
        info.macAddress = adapter.macAddress;
        info.vlanId = adapter.vlanId;
        info.connected = adapter.connected;
        info.startConnected = adapter.startConnected;
        out.push_back(std::move(info));
    }
    logger()->info("VM '{}' has {} network adapter(s)", vmInfo.name, out.size());
    return out;
}

Result<std::string> getVmNotes(const ConnectParams& params, const VmLocator& vm)
{
    Detail::VmLocator internalLocator;
    if (vm.moref) {
        internalLocator = Detail::VmLocator::byMoref(*vm.moref);
    } else if (vm.uuid) {
        internalLocator = Detail::VmLocator::byUuid(*vm.uuid);
    } else if (vm.inventoryPath) {
        internalLocator = Detail::VmLocator::byInventoryPath(*vm.inventoryPath);
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
    auto vmMorefResult = inventory.findVm(internalLocator);
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }

    auto vmInfoResult = inventory.describeVm(vmMorefResult.value());
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }
    return vmInfoResult.value().annotation;
}

Result<std::vector<VSphereDiskSummary>> listVmDisks(const ConnectParams& params, const VmLocator& vm)
{
    auto internalLocator = toInternalLocator(vm);
    if (!internalLocator) {
        return std::move(internalLocator).error();
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
    auto vmMorefResult = inventory.findVm(internalLocator.value());
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }
    auto vmInfoResult = inventory.describeVm(vmMorefResult.value());
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }

    std::vector<VSphereDiskSummary> out;
    out.reserve(vmInfoResult.value().disks.size());
    for (const Detail::VimDiskInfo& disk : vmInfoResult.value().disks) {
        out.push_back(VSphereDiskSummary{disk.key, disk.capacityBytes, disk.thinProvisioned, disk.eagerlyScrub});
    }
    logger()->info("VM '{}' has {} disk(s)", vmInfoResult.value().name, out.size());
    return out;
}

Result<std::string> getVmPowerState(const ConnectParams& params, const VmLocator& vm)
{
    auto internalLocator = toInternalLocator(vm);
    if (!internalLocator) {
        return std::move(internalLocator).error();
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
    auto vmMorefResult = inventory.findVm(internalLocator.value());
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }
    auto vmInfoResult = inventory.describeVm(vmMorefResult.value());
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }
    return std::string(powerStateName(vmInfoResult.value().powerState));
}

Error powerOnVm(const ConnectParams& params, const VmLocator& vm)
{
    auto internalLocator = toInternalLocator(vm);
    if (!internalLocator) {
        return std::move(internalLocator).error();
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
    auto vmMorefResult = inventory.findVm(internalLocator.value());
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }

    logger()->info("powering on VM {}", vmMorefResult.value().value);
    pugi::xml_document request = Detail::makeVimRequest("PowerOnVM_Task", vmMorefResult.value());
    auto callResult = session->call("urn:vim25/PowerOnVM_Task", request.document_element());
    if (!callResult) {
        return std::move(callResult).error();
    }
    const Detail::MoRef task = Detail::parseMoRef(Detail::soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "PowerOnVM_Task response missing task MoRef");
    }
    auto waited = Detail::waitForVimTask(*session, task);
    if (!waited) {
        return std::move(waited).error();
    }
    return {};
}

Error powerOffVm(const ConnectParams& params, const VmLocator& vm)
{
    auto internalLocator = toInternalLocator(vm);
    if (!internalLocator) {
        return std::move(internalLocator).error();
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
    auto vmMorefResult = inventory.findVm(internalLocator.value());
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }

    logger()->info("powering off VM {}", vmMorefResult.value().value);
    pugi::xml_document request = Detail::makeVimRequest("PowerOffVM_Task", vmMorefResult.value());
    auto callResult = session->call("urn:vim25/PowerOffVM_Task", request.document_element());
    if (!callResult) {
        return std::move(callResult).error();
    }
    const Detail::MoRef task = Detail::parseMoRef(Detail::soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "PowerOffVM_Task response missing task MoRef");
    }
    auto waited = Detail::waitForVimTask(*session, task);
    if (!waited) {
        return std::move(waited).error();
    }
    return {};
}

Error destroyVm(const ConnectParams& params, const VmLocator& vm)
{
    auto internalLocator = toInternalLocator(vm);
    if (!internalLocator) {
        return std::move(internalLocator).error();
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
    auto vmMorefResult = inventory.findVm(internalLocator.value());
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }

    logger()->info("destroying VM {}", vmMorefResult.value().value);
    pugi::xml_document request = Detail::makeVimRequest("Destroy_Task", vmMorefResult.value());
    auto callResult = session->call("urn:vim25/Destroy_Task", request.document_element());
    if (!callResult) {
        return std::move(callResult).error();
    }
    const Detail::MoRef task = Detail::parseMoRef(Detail::soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "Destroy_Task response missing task MoRef");
    }
    auto waited = Detail::waitForVimTask(*session, task);
    if (!waited) {
        return std::move(waited).error();
    }
    return {};
}

Error checkDatastoreWriteAccess(const ConnectParams& params, const std::string& datastoreName,
                                         const std::string& resourcePoolMoref)
{
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
    auto datacenterResult = inventory.resolveDatacenterMoRef(Detail::MoRef{"ResourcePool", resourcePoolMoref});
    if (!datacenterResult) {
        return Error(ErrorCode::InvalidArgument,
                     "could not resolve datacenter for resource pool '" + resourcePoolMoref +
                         "' to check datastore write access: " + datacenterResult.error().message());
    }

    // Uniqueness is a courtesy (a stale leftover from an interrupted prior run would just make this
    // create fail with "already exists", not corrupt anything), not a correctness requirement:
    // millisecond epoch time is more than sufficient.
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string marker = "[" + datastoreName + "] .preflight-write-test-" + std::to_string(nowMs);

    if (auto made = Detail::makeDatastoreDirectory(*session, marker, datacenterResult.value()); !made.ok()) {
        return Error(ErrorCode::IoError,
                     "could not create a test directory on datastore '" + datastoreName +
                         "' -- this session likely lacks Datastore.FileManagement there: " + made.message());
    }
    if (auto deleted = Detail::deleteDatastoreFile(*session, marker, datacenterResult.value()); !deleted.ok()) {
        logger()->warn("write-access probe left behind '{}' on datastore '{}' (cleanup failed, but the create "
                        "already proved write access): {}",
                        marker, datastoreName, deleted.toString());
    }
    return {};
}

} // namespace VSphereSoap
