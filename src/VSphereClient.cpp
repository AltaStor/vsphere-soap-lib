// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// One reusable vSphere SOAP session for several operations

#include "vsphere-soap/VSphereClient.hpp"

#include <utility>

#include "ClientOperations.hpp"
#include "SoapLog.hpp"
#include "VimConnection.hpp"

namespace VSphereSoap {

class VSphereClient::Impl {
public:
    explicit Impl(const ConnectParams& params) : m_connection(params) {}

    Detail::ClientConnection m_connection;
};

VSphereClient::VSphereClient(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
VSphereClient::~VSphereClient() = default;

Result<std::unique_ptr<VSphereClient>> VSphereClient::open(const ConnectParams& params)
{
    Detail::logger()->debug("opening vSphere client session on {}:{}", params.host, params.port);
    auto impl = std::make_unique<Impl>(params);
    auto connected = impl->m_connection.get();
    if (!connected) {
        return std::move(connected).error();
    }
    return std::unique_ptr<VSphereClient>(new VSphereClient(std::move(impl)));
}

Result<std::vector<VSphereNetworkAdapterInfo>> VSphereClient::listVmNetworkAdapters(const VmLocator& vm)
{
    return Detail::listVmNetworkAdapters(m_impl->m_connection, vm);
}

Result<std::string> VSphereClient::getVmNotes(const VmLocator& vm)
{
    return Detail::getVmNotes(m_impl->m_connection, vm);
}

Result<std::vector<VSphereDiskSummary>> VSphereClient::listVmDisks(const VmLocator& vm)
{
    return Detail::listVmDisks(m_impl->m_connection, vm);
}

Result<std::string> VSphereClient::getVmPowerState(const VmLocator& vm)
{
    return Detail::getVmPowerState(m_impl->m_connection, vm);
}

Error VSphereClient::powerOnVm(const VmLocator& vm) { return Detail::powerOnVm(m_impl->m_connection, vm); }

Error VSphereClient::powerOffVm(const VmLocator& vm) { return Detail::powerOffVm(m_impl->m_connection, vm); }

Error VSphereClient::destroyVm(const VmLocator& vm) { return Detail::destroyVm(m_impl->m_connection, vm); }

Error VSphereClient::checkDatastoreWriteAccess(const std::string& datastoreName, const std::string& resourcePoolMoref)
{
    return Detail::checkDatastoreWriteAccess(m_impl->m_connection, datastoreName, resourcePoolMoref);
}

Result<VSpherePlacement> VSphereClient::resolvePlacement(const VSpherePlacementSpec& spec)
{
    return Detail::resolvePlacement(m_impl->m_connection, spec);
}

Result<VSphereAttachResult> VSphereClient::attachImportedDisk(const VSphereAttachSpec& spec)
{
    return Detail::attachImportedDisk(m_impl->m_connection, spec);
}

Result<std::string> VSphereClient::releaseImportedDisk(const std::string& placeholderVmId,
                                                       const std::string& datastorePath,
                                                       const std::string& diskProvisioning)
{
    return Detail::releaseImportedDisk(m_impl->m_connection, placeholderVmId, datastorePath, diskProvisioning);
}

Result<bool> VSphereClient::datastoreDiskExists(const std::string& datastorePath)
{
    return Detail::datastoreDiskExists(m_impl->m_connection, datastorePath);
}

Error VSphereClient::eagerZeroAllDisks(const VmLocator& vm)
{
    return Detail::eagerZeroAllDisks(m_impl->m_connection, vm);
}

Result<VSphereFolderPutPrepResult> VSphereClient::prepareFolderPutImport(const VSphereFolderPutPrepSpec& spec)
{
    return Detail::prepareFolderPutImport(m_impl->m_connection, spec);
}

Result<VSphereFolderPutPrepResult> VSphereClient::prepareFolderPutOverwrite(const VmLocator& vm,
                                                                            const DiskSelector& disk)
{
    return Detail::prepareFolderPutOverwrite(m_impl->m_connection, vm, disk);
}

Error VSphereClient::finishFolderPutOverwrite(const VmLocator& vm)
{
    return Detail::finishFolderPutOverwrite(m_impl->m_connection, vm);
}

Result<VSphereAttachResult> VSphereClient::finishFolderPutImport(const VmLocator& targetVm,
                                                                 const std::string& descriptorDatastorePath,
                                                                 std::uint64_t virtualSizeBytes,
                                                                 const VSphereAttachControllerSpec& controller)
{
    return Detail::finishFolderPutImport(m_impl->m_connection, targetVm, descriptorDatastorePath, virtualSizeBytes,
                                         controller);
}

Error VSphereClient::cleanupFolderPutImport(const VmLocator& targetVm, const std::string& descriptorDatastorePath)
{
    return Detail::cleanupFolderPutImport(m_impl->m_connection, targetVm, descriptorDatastorePath);
}

Error VSphereClient::validateImport(const VSphereImportVmSpec& spec)
{
    return Detail::validateImport(m_impl->m_connection, spec);
}

} // namespace VSphereSoap
