// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// vSphere folder-put import control plane

#include "vsphere-soap/VSphereFolderPutImport.hpp"

#include <algorithm>

#include "ClientOperations.hpp"
#include "DatastorePath.hpp"
#include "DiskAttach.hpp"
#include "SnapshotScope.hpp"
#include "TaskWait.hpp"
#include "VmLookup.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "SoapLog.hpp"
#include "vsphere-soap/vim/VimInventory.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap {

/// Definition of the opaque handle declared in the public header.
/// Shares ownership of the whole `Detail::VimConnection` (transport, SoapClient and VimSession, which
/// only reference one another and so must live together; see VimConnection.hpp) with the
/// `ClientConnection` the prepare call ran on: the `VSphereClient`, or the temporary one inside the
/// one-shot free function.
/// The session logs out only when the last owner goes, so the cookie stays valid while this handle lives,
/// even after the client that created it is destroyed; while the client lives, the handle adds nothing.
///
class VSphereFolderPutSessionHandle {
public:
    explicit VSphereFolderPutSessionHandle(std::shared_ptr<Detail::VimConnection> connection)
        : m_connection(std::move(connection)) {}

private:
    std::shared_ptr<Detail::VimConnection> m_connection;
};

// See VSphereFolderPutPrepResult's own declarations' doc comment: these must be defined here, not
// defaulted in the header, since VSphereFolderPutSessionHandle is only complete at this point in
// the program.
VSphereFolderPutPrepResult::VSphereFolderPutPrepResult() = default;
VSphereFolderPutPrepResult::~VSphereFolderPutPrepResult() = default;
VSphereFolderPutPrepResult::VSphereFolderPutPrepResult(VSphereFolderPutPrepResult&&) noexcept = default;
VSphereFolderPutPrepResult& VSphereFolderPutPrepResult::operator=(VSphereFolderPutPrepResult&&) noexcept = default;

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;
using VSphereSoap::Result;
using Detail::deriveFlatName;
using Vim::HttpHeader;
using Vim::HttpMethod;
using Detail::urlEncode;

namespace {

using Detail::logger;

std::string ensureVmdkExtension(const std::string& name)
{
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".vmdk") == 0) {
        return name;
    }
    return name + ".vmdk";
}

} // namespace

Result<VSphereFolderPutPrepResult> Detail::prepareFolderPutImport(Detail::ClientConnection& connection,
                                                                  const VSphereFolderPutPrepSpec& spec)
{
    const ConnectParams& params = connection.params();
    if (spec.fileName.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutPrepSpec::fileName is required");
    }
    if (spec.virtualSizeBytes == 0) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutPrepSpec::virtualSizeBytes must be nonzero");
    }
    if (Error invalid = Detail::checkVmLocator(spec.targetVm, "VSphereFolderPutPrepSpec::targetVm"); !invalid.ok()) {
        return invalid;
    }

    logger()->info("preparing folder-put import for '{}' on {}:{}", spec.fileName, params.host, params.port);

    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::IHttpTransport* transport = open.value()->transport.get();
    Vim::SoapClient* soap = open.value()->soap.get();
    Vim::VimSession* session = open.value()->session.get();
    Vim::VimInventory inventory(*session);

    auto vmMorefResult = Detail::findVm(inventory, spec.targetVm);
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }
    const Vim::MoRef targetMoref = vmMorefResult.value();

    auto vmInfoResult = inventory.describeVm(targetMoref);
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }
    const Vim::VimVmInfo& vmInfo = vmInfoResult.value();
    if (vmInfo.powerState != Vim::VimPowerState::PoweredOff) {
        return Error(ErrorCode::InvalidArgument, "target VM '" + vmInfo.name +
                                                      "' must be powered off to import a disk this way");
    }
    if (vmInfo.vmPathName.empty()) {
        return Error(ErrorCode::InvalidFormat, "target VM '" + vmInfo.name + "' has no config.files.vmPathName");
    }
    auto parsedVmPath = Vim::parseDatastorePath(vmInfo.vmPathName);
    if (!parsedVmPath) {
        return Error(ErrorCode::InvalidFormat, "could not parse target VM's config.files.vmPathName: " +
                                                    vmInfo.vmPathName);
    }
    const std::string& datastoreName = parsedVmPath->first;
    const auto lastSlash = parsedVmPath->second.find_last_of('/');
    const std::string vmDir = lastSlash == std::string::npos ? std::string() : parsedVmPath->second.substr(0, lastSlash);

    auto dcNameResult = inventory.resolveDatacenterName(targetMoref);
    if (!dcNameResult) {
        return std::move(dcNameResult).error();
    }

    const std::string fileName = ensureVmdkExtension(spec.fileName);
    const std::string descriptorRelativePath = vmDir.empty() ? fileName : vmDir + "/" + fileName;
    const std::string flatRelativePath = deriveFlatName(descriptorRelativePath);
    if (flatRelativePath == descriptorRelativePath) {
        // deriveFlatName() only rewrites a plain "*.vmdk" name it recognizes; this should never actually
        // happen given ensureVmdkExtension() above, but fail loudly rather than silently PUTting
        // the flat extent over the descriptor's own path if it somehow did.
        return Error(ErrorCode::InvalidFormat, "could not derive a flat-extent name for '" + fileName + "'");
    }

    const std::string query = "dcPath=" + urlEncode(dcNameResult.value()) + "&dsName=" + urlEncode(datastoreName);

    const std::uint64_t capacitySectors = spec.virtualSizeBytes / 512;
    const std::string adapterType = spec.adapterType.empty() ? "lsilogic" : spec.adapterType;
    const std::string descriptor = "# Disk DescriptorFile\n"
                                    "version=1\n"
                                    "CID=fffffffe\n"
                                    "parentCID=ffffffff\n"
                                    "createType=\"monolithicFlat\"\n"
                                    "\n"
                                    "# Extent description\n"
                                    "RW " +
                                    std::to_string(capacitySectors) + " FLAT \"" +
                                    flatRelativePath.substr(flatRelativePath.find_last_of('/') + 1) + "\" 0\n"
                                    "\n"
                                    "# The Disk Data Base\n"
                                    "#DDB\n"
                                    "\n"
                                    "ddb.adapterType = \"" +
                                    adapterType +
                                    "\"\n"
                                    "ddb.geometry.cylinders = \"" +
                                    std::to_string(std::max<std::uint64_t>(1, capacitySectors / (255 * 63))) +
                                    "\"\n"
                                    "ddb.geometry.heads = \"255\"\n"
                                    "ddb.geometry.sectors = \"63\"\n";

    // A PUT replaces whatever is there, so a name already taken is refused rather than overwritten.
    const std::string descriptorTarget = "/folder/" + urlEncode(descriptorRelativePath) + "?" + query;
    const std::string flatTarget = "/folder/" + urlEncode(flatRelativePath) + "?" + query;
    for (const std::string* target : {&descriptorTarget, &flatTarget}) {
        auto exists = Detail::datastoreFileExists(*transport, *target, soap->sessionCookie());
        if (!exists) {
            return std::move(exists).error();
        }
        if (exists.value()) {
            return Error(ErrorCode::InvalidArgument, "'" + fileName + "' already exists in the folder of VM '" +
                                                         vmInfo.name + "'; choose another name");
        }
    }

    std::vector<HttpHeader> putHeaders{{"Content-Type", "text/plain"}};
    if (!soap->sessionCookie().empty()) {
        putHeaders.push_back({"Cookie", soap->sessionCookie()});
    }
    auto descriptorPutResult = transport->request(HttpMethod::Put, descriptorTarget, putHeaders, descriptor);
    if (!descriptorPutResult) {
        return std::move(descriptorPutResult).error();
    }
    if (descriptorPutResult.value().status < 200 || descriptorPutResult.value().status >= 300) {
        return Error(ErrorCode::IoError, "PUT of VMDK descriptor to " + descriptorTarget +
                                              " failed: HTTP " + std::to_string(descriptorPutResult.value().status) +
                                              ": " + descriptorPutResult.value().body);
    }

    VSphereFolderPutPrepResult result;
    result.flatPutUrl = "https://" + params.host + ":" + std::to_string(params.port) + "/folder/" +
                          urlEncode(flatRelativePath) + "?" + query;
    result.sessionCookie = soap->sessionCookie();
    result.descriptorDatastorePath = "[" + datastoreName + "] " + descriptorRelativePath;
    result.virtualSizeBytes = spec.virtualSizeBytes;
    // Keeps the session (and therefore result.sessionCookie) logged in until the caller is done with it;
    // see VSphereFolderPutSessionHandle's own doc comment for why this matters (confirmed live: without
    // it, the flat extent PUT gets a bare HTTP 401).
    result.keepAlive =
        std::make_unique<VSphereFolderPutSessionHandle>(connection.share());

    logger()->info("folder-put descriptor written to {} ; flat extent target {}", result.descriptorDatastorePath,
                    result.flatPutUrl);
    return result;
}

Result<VSphereFolderPutPrepResult> Detail::prepareFolderPutOverwrite(Detail::ClientConnection& connection,
                                                                     const Detail::PublicVmLocator& vm,
                                                                     const DiskSelector& disk)
{
    const ConnectParams& params = connection.params();
    if (Error invalid = Detail::checkVmLocator(vm, "vm"); !invalid.ok()) {
        return invalid;
    }
    if (!disk.key && !disk.datastorePath) {
        return Error(ErrorCode::InvalidArgument, "DiskSelector has neither key nor datastorePath set");
    }

    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::IHttpTransport* transport = open.value()->transport.get();
    Vim::SoapClient* soap = open.value()->soap.get();
    Vim::VimSession* session = open.value()->session.get();
    Vim::VimInventory inventory(*session);

    auto vmMoref = Detail::findVm(inventory, vm);
    if (!vmMoref) {
        return std::move(vmMoref).error();
    }
    auto vmInfo = inventory.describeVm(vmMoref.value());
    if (!vmInfo) {
        return std::move(vmInfo).error();
    }
    const std::string& vmName = vmInfo.value().name;
    if (vmInfo.value().powerState != Vim::VimPowerState::PoweredOff) {
        return Error(ErrorCode::InvalidArgument, "VM '" + vmName + "' must be powered off to overwrite its disk");
    }

    const auto& disks = vmInfo.value().disks;
    const auto target = std::find_if(disks.begin(), disks.end(), [&](const Vim::VimDiskInfo& candidate) {
        return (disk.key && candidate.key == *disk.key) ||
               (disk.datastorePath && candidate.datastoreRelativePath == *disk.datastorePath);
    });
    if (target == disks.end()) {
        return Error(ErrorCode::InvalidArgument, "VM '" + vmName + "' has no disk matching the given selector");
    }
    if (!target->parents.empty()) {
        return Error(ErrorCode::InvalidArgument, "disk '" + target->rawFileName + "' of VM '" + vmName +
                                                     "' has snapshots; overwriting its base would corrupt them");
    }
    if (target->backingType != "VirtualDiskFlatVer2BackingInfo") {
        return Error(ErrorCode::NotImplemented, "disk '" + target->rawFileName + "' has a " + target->backingType +
                                                    " backing, not a flat file that can be overwritten");
    }

    auto dcName = inventory.resolveDatacenterName(vmMoref.value());
    if (!dcName) {
        return std::move(dcName).error();
    }
    const std::string query = "dcPath=" + urlEncode(dcName.value()) + "&dsName=" + urlEncode(target->datastoreName);

    // The flat file is named by the descriptor, which also says whether the disk is one at all.
    std::vector<HttpHeader> headers;
    if (!soap->sessionCookie().empty()) {
        headers.push_back({"Cookie", soap->sessionCookie()});
    }
    const std::string descriptorTarget = "/folder/" + urlEncode(target->datastoreRelativePath) + "?" + query;
    auto descriptor = transport->request(HttpMethod::Get, descriptorTarget, headers, {});
    if (!descriptor) {
        return std::move(descriptor).error();
    }
    if (descriptor.value().status != 200) {
        return Error(ErrorCode::IoError, "reading the descriptor " + target->rawFileName + " failed: HTTP " +
                                             std::to_string(descriptor.value().status));
    }
    const auto createType = Detail::parseDescriptorCreateType(descriptor.value().body);
    if (!createType || (*createType != "vmfs" && *createType != "monolithicFlat")) {
        return Error(ErrorCode::NotImplemented, "disk '" + target->rawFileName + "' is of type '" +
                                                    createType.value_or("unknown") +
                                                    "', not a flat disk that can be overwritten");
    }
    const auto extent = Detail::parseFlatExtentFileName(descriptor.value().body);
    if (!extent) {
        return Error(ErrorCode::InvalidFormat, "the descriptor " + target->rawFileName + " names no extent file");
    }
    const std::string flatRelativePath = Detail::siblingPath(target->datastoreRelativePath, *extent);

    VSphereFolderPutPrepResult result;
    result.flatPutUrl = "https://" + params.host + ":" + std::to_string(params.port) + "/folder/" +
                        urlEncode(flatRelativePath) + "?" + query;
    result.sessionCookie = soap->sessionCookie();
    result.descriptorDatastorePath = target->rawFileName;
    result.virtualSizeBytes = target->capacityBytes;
    result.keepAlive =
        std::make_unique<VSphereFolderPutSessionHandle>(connection.share());

    logger()->info("disk {} of VM '{}' ready to be overwritten through {}", result.descriptorDatastorePath, vmName,
                   flatRelativePath);
    return result;
}

Error Detail::finishFolderPutOverwrite(Detail::ClientConnection& connection, const Detail::PublicVmLocator& vm)
{
    if (Error invalid = Detail::checkVmLocator(vm, "vm"); !invalid.ok()) {
        return invalid;
    }
    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::VimSession& session = *open.value()->session;
    Vim::VimInventory inventory(session);

    auto vmMoref = Detail::findVm(inventory, vm);
    if (!vmMoref) {
        return std::move(vmMoref).error();
    }
    auto vmInfo = inventory.describeVm(vmMoref.value());
    if (!vmInfo) {
        return std::move(vmInfo).error();
    }
    if (!vmInfo.value().changeTrackingEnabled) {
        return {};
    }

    const auto setChangeTracking = [&](bool enabled) -> Error {
        pugi::xml_document request = Vim::makeVimRequest("ReconfigVM_Task", vmMoref.value());
        request.document_element().append_child("spec").append_child("changeTrackingEnabled").text() = enabled;
        auto call = session.call(request.document_element());
        if (!call) {
            return std::move(call).error();
        }
        const Vim::MoRef task = Vim::parseMoRef(Vim::soapBodyContent(call.value()).child("returnval"));
        auto waited = Detail::waitForVimTask(session, task);
        if (!waited) {
            return std::move(waited).error();
        }
        return {};
    };
    // A snapshot taken and removed is what makes ESXi apply the change to the disk's tracking files.
    const auto applyAtSnapshot = [&]() -> Error {
        auto snapshot = Detail::SnapshotScope::create(session, vmMoref.value(), "vsphere-soap-cbt-reset");
        if (!snapshot) {
            return std::move(snapshot).error();
        }
        return {}; // removed by the scope here
    };

    logger()->info("resetting changed block tracking of VM '{}' after its disk was overwritten", vmInfo.value().name);
    if (Error off = setChangeTracking(false); !off.ok()) {
        return Error(ErrorCode::IoError, "disk overwritten, but turning CBT off to reset it failed: " + off.message());
    }
    if (Error applied = applyAtSnapshot(); !applied.ok()) {
        return Error(ErrorCode::IoError, "disk overwritten and CBT turned off, but the snapshot that applies it "
                                         "failed: " + applied.message());
    }
    if (Error on = setChangeTracking(true); !on.ok()) {
        return Error(ErrorCode::IoError,
                     "disk overwritten and CBT reset, but turning it back on failed: " + on.message());
    }
    if (Error applied = applyAtSnapshot(); !applied.ok()) {
        return Error(ErrorCode::IoError, "disk overwritten and CBT turned back on, but the snapshot that applies it "
                                         "failed: " + applied.message());
    }
    return {};
}

Result<VSphereAttachResult> Detail::finishFolderPutImport(Detail::ClientConnection& connection,
                                                         const Detail::PublicVmLocator& targetVm,
                                                         const std::string& descriptorDatastorePath,
                                                         std::uint64_t virtualSizeBytes,
                                                         const VSphereAttachControllerSpec& controller)
{
    if (Error invalid = Detail::checkVmLocator(targetVm, "targetVm"); !invalid.ok()) {
        return invalid;
    }

    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::VimSession* session = open.value()->session.get();
    Vim::VimInventory inventory(*session);

    auto vmMorefResult = Detail::findVm(inventory, targetVm);
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }

    Detail::ControllerSelector selector;
    selector.controllerType = controller.controllerType;
    selector.busNumber = controller.busNumber;
    selector.createIfMissing = controller.createIfMissing;

    auto attached = Detail::attachDiskFile(*session, vmMorefResult.value(), descriptorDatastorePath, virtualSizeBytes,
                                             selector, "thin");
    if (!attached) {
        return std::move(attached).error();
    }
    return VSphereAttachResult{attached.value().datastorePath, attached.value().controllerKey,
                                 attached.value().unitNumber};
}

Error Detail::cleanupFolderPutImport(Detail::ClientConnection& connection, const Detail::PublicVmLocator& targetVm,
                                     const std::string& descriptorDatastorePath)
{
    if (Error invalid = Detail::checkVmLocator(targetVm, "targetVm"); !invalid.ok()) {
        return invalid;
    }
    const auto parsedDescriptor = Vim::parseDatastorePath(descriptorDatastorePath);
    if (!parsedDescriptor) {
        return Error(ErrorCode::InvalidArgument,
                     "cleanupFolderPutImport: not a '[datastore] path' string: " + descriptorDatastorePath);
    }
    const std::string flatRelativePath = Detail::deriveFlatName(parsedDescriptor->second);
    const std::string flatDatastorePath = "[" + parsedDescriptor->first + "] " + flatRelativePath;

    auto open = connection.get();
    if (!open) {
        return std::move(open).error();
    }
    Vim::VimSession* session = open.value()->session.get();
    Vim::VimInventory inventory(*session);

    auto vmMorefResult = Detail::findVm(inventory, targetVm);
    // Left empty (server infers from the path's own datastore) if this lookup fails.
    Vim::MoRef datacenter;
    if (vmMorefResult) {
        if (auto dc = inventory.resolveDatacenterMoRef(vmMorefResult.value()); dc) {
            datacenter = dc.value();
        }
    }

    // Best-effort for both files: a delete failing here (e.g. the flat extent's PUT never got far enough
    // to create it at all) is logged, not propagated; there is already a more important upload failure
    // being reported by the caller.
    if (auto deletedFlat = Detail::deleteDatastoreFile(*session, flatDatastorePath, datacenter); !deletedFlat.ok()) {
        logger()->warn("cleanup: failed to delete flat extent {} after a failed folder-put upload: {}",
                        flatDatastorePath, deletedFlat.toString());
    }
    if (auto deletedDescriptor = Detail::deleteDatastoreFile(*session, descriptorDatastorePath, datacenter);
        !deletedDescriptor.ok()) {
        logger()->warn("cleanup: failed to delete descriptor {} after a failed folder-put upload: {}",
                        descriptorDatastorePath, deletedDescriptor.toString());
    }
    return {};
}

// One-shot wrappers: one login per call. See VSphereClient for running several operations on one login.

Result<VSphereFolderPutPrepResult> prepareFolderPutImport(const ConnectParams& params,
                                                          const VSphereFolderPutPrepSpec& spec)
{
    Detail::ClientConnection connection(params);
    return Detail::prepareFolderPutImport(connection, spec);
}

Result<VSphereFolderPutPrepResult> prepareFolderPutOverwrite(const ConnectParams& params, const VmLocator& vm,
                                                             const DiskSelector& disk)
{
    Detail::ClientConnection connection(params);
    return Detail::prepareFolderPutOverwrite(connection, vm, disk);
}

Error finishFolderPutOverwrite(const ConnectParams& params, const VmLocator& vm)
{
    Detail::ClientConnection connection(params);
    return Detail::finishFolderPutOverwrite(connection, vm);
}

Result<VSphereAttachResult> finishFolderPutImport(const ConnectParams& params, const VmLocator& targetVm,
                                                  const std::string& descriptorDatastorePath,
                                                  std::uint64_t virtualSizeBytes,
                                                  const VSphereAttachControllerSpec& controller)
{
    Detail::ClientConnection connection(params);
    return Detail::finishFolderPutImport(connection, targetVm, descriptorDatastorePath, virtualSizeBytes,
                                         controller);
}

Error cleanupFolderPutImport(const ConnectParams& params, const VmLocator& targetVm,
                             const std::string& descriptorDatastorePath)
{
    Detail::ClientConnection connection(params);
    return Detail::cleanupFolderPutImport(connection, targetVm, descriptorDatastorePath);
}

} // namespace VSphereSoap
