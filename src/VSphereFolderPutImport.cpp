// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere folder-put import control plane

#include "vsphere-soap/VSphereFolderPutImport.hpp"

#include <algorithm>

#include "DatastorePath.hpp"
#include "DiskAttach.hpp"
#include "vsphere-soap/vim/HttpTransportAdapter.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "SoapLog.hpp"
#include "vsphere-soap/vim/VimInventory.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap {

/// Definition of the opaque handle declared in the public header.
/// Owns *all three* of the transport, the SoapClient, and the VimSession (not just the session) because
/// VimSession only ever *references* its SoapClient (`SoapClient& m_soap`, see VimSession.hpp), which in
/// turn only references its transport; none of them own each other.
/// `prepareFolderPutImport` originally built all three as plain stack locals, which happened to work for
/// every synchronous use *inside* that function (the descriptor PUT) but left `session`'s references
/// dangling the moment the function returned: `VimSession`'s destructor (best-effort `Logout`) would
/// then run against an already-destroyed `SoapClient`/transport whenever this handle itself was
/// eventually destroyed, which is undefined behavior, not just "logs out a little late".
/// Declared in the same order `VmwareSoapDisk::Impl` already uses for the identical ownership shape
/// (transport, then soap, then session) so members are destroyed in the correct reverse order: session
/// first (while soap/transport are still alive to carry its Logout call), then soap, then transport.
///
class VSphereFolderPutSessionHandle {
public:
    VSphereFolderPutSessionHandle(std::unique_ptr<Vim::HttpTransportAdapter> transport,
                                    std::unique_ptr<Vim::SoapClient> soap,
                                    std::unique_ptr<Vim::VimSession> session)
        : m_transport(std::move(transport)), m_soap(std::move(soap)), m_session(std::move(session)) {}

private:
    std::unique_ptr<Vim::HttpTransportAdapter> m_transport;
    std::unique_ptr<Vim::SoapClient> m_soap;
    std::unique_ptr<Vim::VimSession> m_session;
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

Vim::VmLocator toInternalLocator(const VmLocator& vm)
{
    if (vm.moref) return Vim::VmLocator::byMoref(*vm.moref);
    if (vm.uuid) return Vim::VmLocator::byUuid(*vm.uuid);
    if (vm.inventoryPath) return Vim::VmLocator::byInventoryPath(*vm.inventoryPath);
    return {};
}

std::string ensureVmdkExtension(const std::string& name)
{
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".vmdk") == 0) {
        return name;
    }
    return name + ".vmdk";
}

} // namespace

Result<VSphereFolderPutPrepResult> prepareFolderPutImport(const ConnectParams& params,
                                                              const VSphereFolderPutPrepSpec& spec)
{
    if (spec.fileName.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutPrepSpec::fileName is required");
    }
    if (spec.virtualSizeBytes == 0) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutPrepSpec::virtualSizeBytes must be nonzero");
    }
    const Vim::VmLocator targetLocator = toInternalLocator(spec.targetVm);
    if (targetLocator.moref.has_value() + targetLocator.uuid.has_value() + targetLocator.inventoryPath.has_value() ==
        0) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutPrepSpec::targetVm has none of moref/uuid/"
                                                  "inventoryPath set");
    }

    logger()->info("preparing folder-put import for '{}' on {}:{}", spec.fileName, params.host, params.port);

    // Heap-allocated (not stack locals): both must outlive this function; see
    // VSphereFolderPutSessionHandle's own doc comment for why.
    auto transport = std::make_unique<Vim::HttpTransportAdapter>(params.host, params.port,
                                                                        params.verifyTlsCertificate,
                                                                        params.pinnedThumbprint);
    auto soap = std::make_unique<Vim::SoapClient>(*transport, params.host);
    auto sessionResult = Vim::VimSession::open(*soap, params.username, params.password);
    if (!sessionResult) {
        logger()->warn("VIM session open against {}:{} failed: {}", params.host, params.port,
                        sessionResult.error().toString());
        return std::move(sessionResult).error();
    }
    std::unique_ptr<Vim::VimSession> session = std::move(sessionResult).value();
    Vim::VimInventory inventory(*session);

    auto vmMorefResult = inventory.findVm(targetLocator);
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

    std::vector<HttpHeader> putHeaders{{"Content-Type", "text/plain"}};
    if (!soap->sessionCookie().empty()) {
        putHeaders.push_back({"Cookie", soap->sessionCookie()});
    }
    const std::string descriptorTarget = "/folder/" + urlEncode(descriptorRelativePath) + "?" + query;
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
    // Keeps the session (and therefore result.sessionCookie) logged in until the caller is done with it;
    // see VSphereFolderPutSessionHandle's own doc comment for why this matters (confirmed live: without
    // it, the flat extent PUT gets a bare HTTP 401).
    result.keepAlive =
        std::make_unique<VSphereFolderPutSessionHandle>(std::move(transport), std::move(soap), std::move(session));

    logger()->info("folder-put descriptor written to {} ; flat extent target {}", result.descriptorDatastorePath,
                    result.flatPutUrl);
    return result;
}

Result<VSphereAttachResult> finishFolderPutImport(const ConnectParams& params, const VmLocator& targetVm,
                                                     const std::string& descriptorDatastorePath,
                                                     std::uint64_t virtualSizeBytes,
                                                     const VSphereAttachControllerSpec& controller)
{
    const Vim::VmLocator targetLocator = toInternalLocator(targetVm);
    if (targetLocator.moref.has_value() + targetLocator.uuid.has_value() + targetLocator.inventoryPath.has_value() ==
        0) {
        return Error(ErrorCode::InvalidArgument, "targetVm has none of moref/uuid/inventoryPath set");
    }

    Vim::HttpTransportAdapter transport(params.host, params.port, params.verifyTlsCertificate,
                                             params.pinnedThumbprint);
    Vim::SoapClient soap(transport, params.host);
    auto sessionResult = Vim::VimSession::open(soap, params.username, params.password);
    if (!sessionResult) {
        return std::move(sessionResult).error();
    }
    std::unique_ptr<Vim::VimSession> session = std::move(sessionResult).value();
    Vim::VimInventory inventory(*session);

    auto vmMorefResult = inventory.findVm(targetLocator);
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }

    Detail::ControllerSelector selector;
    selector.controllerType = controller.controllerType;
    selector.busNumber = controller.busNumber;
    selector.createIfMissing = controller.createIfMissing;

    // See this function's own header doc comment: "eagerZeroedThick" is an accurate label here, not
    // an assumption; this transport always sends the disk's full logical size, zeros included.
    auto attached = Detail::attachDiskFile(*session, vmMorefResult.value(), descriptorDatastorePath, virtualSizeBytes,
                                             selector, "eagerZeroedThick");
    if (!attached) {
        return std::move(attached).error();
    }
    return VSphereAttachResult{attached.value().datastorePath, attached.value().controllerKey,
                                 attached.value().unitNumber};
}

Error cleanupFolderPutImport(const ConnectParams& params, const VmLocator& targetVm,
                                      const std::string& descriptorDatastorePath)
{
    const Vim::VmLocator targetLocator = toInternalLocator(targetVm);
    if (targetLocator.moref.has_value() + targetLocator.uuid.has_value() + targetLocator.inventoryPath.has_value() ==
        0) {
        return Error(ErrorCode::InvalidArgument, "targetVm has none of moref/uuid/inventoryPath set");
    }
    const auto parsedDescriptor = Vim::parseDatastorePath(descriptorDatastorePath);
    if (!parsedDescriptor) {
        return Error(ErrorCode::InvalidArgument,
                     "cleanupFolderPutImport: not a '[datastore] path' string: " + descriptorDatastorePath);
    }
    const std::string flatRelativePath = Detail::deriveFlatName(parsedDescriptor->second);
    const std::string flatDatastorePath = "[" + parsedDescriptor->first + "] " + flatRelativePath;

    Vim::HttpTransportAdapter transport(params.host, params.port, params.verifyTlsCertificate,
                                             params.pinnedThumbprint);
    Vim::SoapClient soap(transport, params.host);
    auto sessionResult = Vim::VimSession::open(soap, params.username, params.password);
    if (!sessionResult) {
        return std::move(sessionResult).error();
    }
    std::unique_ptr<Vim::VimSession> session = std::move(sessionResult).value();
    Vim::VimInventory inventory(*session);

    auto vmMorefResult = inventory.findVm(targetLocator);
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

} // namespace VSphereSoap
