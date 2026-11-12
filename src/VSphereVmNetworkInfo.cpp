// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere VM network adapter query

#include "vsphere-soap/VSphereVmNetworkInfo.hpp"

#include <algorithm>
#include <chrono>

#include "ClientOperations.hpp"
#include "DiskAttach.hpp"
#include "VmLookup.hpp"
#include "SoapLog.hpp"
#include "TaskWait.hpp"
#include "vsphere-soap/vim/VimInventory.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/vim/VimTypes.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Detail {

namespace {

const char* powerStateName(Vim::VimPowerState state)
{
    switch (state) {
        case Vim::VimPowerState::PoweredOn: return "poweredOn";
        case Vim::VimPowerState::PoweredOff: return "poweredOff";
        case Vim::VimPowerState::Suspended: return "suspended";
        case Vim::VimPowerState::Unknown: return "unknown";
    }
    return "unknown";
}

/// Connects (if not yet), resolves `vm`, and reads its properties: the start of every VM query below.
/// The locator must already have been checked.
Result<Vim::VimVmInfo> describeVm(ClientConnection& connection, const PublicVmLocator& vm)
{
    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::VimInventory inventory(*open.value()->session);
    auto vmMoref = findVm(inventory, vm);
    if (!vmMoref) {
        return std::move(vmMoref).error();
    }
    return inventory.describeVm(vmMoref.value());
}

/// Resolves `vm`, runs the no-argument VM task `method` on it (e.g. "PowerOnVM_Task") and waits for it.
Error runVmTask(ClientConnection& connection, const PublicVmLocator& vm, const char* method, const char* verb)
{
    if (Error invalid = checkVmLocator(vm, "VmLocator"); !invalid.ok()) {
        return invalid;
    }
    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::VimSession& session = *open.value()->session;
    Vim::VimInventory inventory(session);
    auto vmMorefResult = findVm(inventory, vm);
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }

    logger()->info("{} VM {}", verb, vmMorefResult.value().value);
    pugi::xml_document request = Vim::makeVimRequest(method, vmMorefResult.value());
    auto callResult = session.call(request.document_element());
    if (!callResult) {
        return std::move(callResult).error();
    }
    const Vim::MoRef task = Vim::parseMoRef(Vim::soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, std::string(method) + " response missing task MoRef");
    }
    auto waited = waitForVimTask(session, task);
    if (!waited) {
        return std::move(waited).error();
    }
    return {};
}

} // namespace

Result<std::vector<VSphereNetworkAdapterInfo>> listVmNetworkAdapters(ClientConnection& connection,
                                                                     const PublicVmLocator& vm)
{
    if (Error invalid = checkVmLocator(vm, "VmLocator"); !invalid.ok()) {
        return invalid;
    }

    logger()->info("listing network adapters on {}:{}", connection.params().host, connection.params().port);
    auto vmInfoResult = describeVm(connection, vm);
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }
    const Vim::VimVmInfo& vmInfo = vmInfoResult.value();

    std::vector<Vim::VimNetworkAdapterInfo> sorted = vmInfo.networkAdapters;
    std::sort(sorted.begin(), sorted.end(),
              [](const Vim::VimNetworkAdapterInfo& a, const Vim::VimNetworkAdapterInfo& b) {
                  return a.key < b.key;
              });

    std::vector<VSphereNetworkAdapterInfo> out;
    out.reserve(sorted.size());
    for (const Vim::VimNetworkAdapterInfo& adapter : sorted) {
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

Result<std::string> getVmNotes(ClientConnection& connection, const PublicVmLocator& vm)
{
    if (Error invalid = checkVmLocator(vm, "VmLocator"); !invalid.ok()) {
        return invalid;
    }
    auto vmInfoResult = describeVm(connection, vm);
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }
    return vmInfoResult.value().annotation;
}

Result<std::vector<VSphereDiskSummary>> listVmDisks(ClientConnection& connection, const PublicVmLocator& vm)
{
    if (Error invalid = checkVmLocator(vm, "VmLocator"); !invalid.ok()) {
        return invalid;
    }
    auto vmInfoResult = describeVm(connection, vm);
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }

    std::vector<VSphereDiskSummary> out;
    out.reserve(vmInfoResult.value().disks.size());
    for (const Vim::VimDiskInfo& disk : vmInfoResult.value().disks) {
        out.push_back(VSphereDiskSummary{disk.key, disk.capacityBytes, disk.thinProvisioned, disk.eagerlyScrub});
    }
    logger()->info("VM '{}' has {} disk(s)", vmInfoResult.value().name, out.size());
    return out;
}

Result<std::string> getVmPowerState(ClientConnection& connection, const PublicVmLocator& vm)
{
    if (Error invalid = checkVmLocator(vm, "VmLocator"); !invalid.ok()) {
        return invalid;
    }
    auto vmInfoResult = describeVm(connection, vm);
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }
    return std::string(powerStateName(vmInfoResult.value().powerState));
}

Error powerOnVm(ClientConnection& connection, const PublicVmLocator& vm)
{
    return runVmTask(connection, vm, "PowerOnVM_Task", "powering on");
}

Error powerOffVm(ClientConnection& connection, const PublicVmLocator& vm)
{
    return runVmTask(connection, vm, "PowerOffVM_Task", "powering off");
}

Error destroyVm(ClientConnection& connection, const PublicVmLocator& vm)
{
    return runVmTask(connection, vm, "Destroy_Task", "destroying");
}

Error checkDatastoreWriteAccess(ClientConnection& connection, const std::string& datastoreName,
                                const std::string& resourcePoolMoref)
{
    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::VimSession& session = *open.value()->session;

    Vim::VimInventory inventory(session);
    auto datacenterResult = inventory.resolveDatacenterMoRef(Vim::MoRef{"ResourcePool", resourcePoolMoref});
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

    if (auto made = makeDatastoreDirectory(session, marker, datacenterResult.value()); !made.ok()) {
        return Error(ErrorCode::IoError,
                     "could not create a test directory on datastore '" + datastoreName +
                         "' -- this session likely lacks Datastore.FileManagement there: " + made.message());
    }
    if (auto deleted = deleteDatastoreFile(session, marker, datacenterResult.value()); !deleted.ok()) {
        logger()->warn("write-access probe left behind '{}' on datastore '{}' (cleanup failed, but the create "
                        "already proved write access): {}",
                        marker, datastoreName, deleted.toString());
    }
    return {};
}

} // namespace VSphereSoap::Detail

// One-shot wrappers: one login per call. See VSphereClient for running several operations on one login.
namespace VSphereSoap {

Result<std::vector<VSphereNetworkAdapterInfo>> listVmNetworkAdapters(const ConnectParams& params,
                                                                     const VmLocator& vm)
{
    Detail::ClientConnection connection(params);
    return Detail::listVmNetworkAdapters(connection, vm);
}

Result<std::string> getVmNotes(const ConnectParams& params, const VmLocator& vm)
{
    Detail::ClientConnection connection(params);
    return Detail::getVmNotes(connection, vm);
}

Result<std::vector<VSphereDiskSummary>> listVmDisks(const ConnectParams& params, const VmLocator& vm)
{
    Detail::ClientConnection connection(params);
    return Detail::listVmDisks(connection, vm);
}

Result<std::string> getVmPowerState(const ConnectParams& params, const VmLocator& vm)
{
    Detail::ClientConnection connection(params);
    return Detail::getVmPowerState(connection, vm);
}

Error powerOnVm(const ConnectParams& params, const VmLocator& vm)
{
    Detail::ClientConnection connection(params);
    return Detail::powerOnVm(connection, vm);
}

Error powerOffVm(const ConnectParams& params, const VmLocator& vm)
{
    Detail::ClientConnection connection(params);
    return Detail::powerOffVm(connection, vm);
}

Error destroyVm(const ConnectParams& params, const VmLocator& vm)
{
    Detail::ClientConnection connection(params);
    return Detail::destroyVm(connection, vm);
}

Error checkDatastoreWriteAccess(const ConnectParams& params, const std::string& datastoreName,
                                const std::string& resourcePoolMoref)
{
    Detail::ClientConnection connection(params);
    return Detail::checkDatastoreWriteAccess(connection, datastoreName, resourcePoolMoref);
}

} // namespace VSphereSoap
