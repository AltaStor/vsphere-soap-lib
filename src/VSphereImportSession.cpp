// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Public wrapper for HttpNfcLease-based vSphere VM import

#include "vsphere-soap/VSphereImportSession.hpp"

#include <algorithm>
#include <utility>

#include "HttpNfcLeaseSession.hpp"
#include "vsphere-soap/vim/HttpTransportAdapter.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "SoapLog.hpp"
#include "TaskWait.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/vim/VimTypes.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;
using VSphereSoap::Result;

namespace {
using Detail::logger;

Vim::MoRef moRefOf(const char* type, const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    return Vim::MoRef{type, value};
}
} // namespace

class VSphereImportSession::Impl {
public:
    Impl(std::unique_ptr<Vim::IHttpTransport> transport, std::string host)
        : m_transport(std::move(transport)), m_soap(*m_transport, std::move(host)) {}

    std::unique_ptr<Vim::IHttpTransport> m_transport;
    Vim::SoapClient m_soap;
    std::unique_ptr<Vim::VimSession> m_session;
    std::unique_ptr<Detail::HttpNfcLeaseSession> m_lease;
    std::string m_vmId;
    std::vector<VSphereImportDeviceUrl> m_deviceUrls;
};

VSphereImportSession::VSphereImportSession(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
VSphereImportSession::~VSphereImportSession() = default;

Result<std::unique_ptr<VSphereImportSession>> VSphereImportSession::begin(const ConnectParams& params,
                                                                              const VSphereImportVmSpec& spec)
{
    if (spec.disks.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereImportSession::begin: spec.disks is empty");
    }
    if (spec.resourcePoolId.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereImportSession::begin: spec.resourcePoolId is required");
    }

    logger()->info("opening VIM session for HttpNfcLease import of '{}' on {}:{}", spec.vmName, params.host,
                    params.port);
    auto impl = std::make_unique<Impl>(
        std::make_unique<Vim::HttpTransportAdapter>(params.host, params.port, params.verifyTlsCertificate,
                                                          params.pinnedThumbprint),
        params.host);

    auto sessionResult = Vim::VimSession::open(impl->m_soap, params.username, params.password);
    if (!sessionResult) {
        logger()->warn("VIM session open against {}:{} failed: {}", params.host, params.port,
                        sessionResult.error().toString());
        return std::move(sessionResult).error();
    }
    impl->m_session = std::move(sessionResult).value();

    Detail::ImportVmSpec internalSpec;
    internalSpec.vmName = spec.vmName;
    internalSpec.memoryBytes = spec.memoryBytes;
    internalSpec.cpuCount = spec.cpuCount;
    internalSpec.hardwareVersion = spec.hardwareVersion;
    internalSpec.guestOsType = spec.guestOsType;
    internalSpec.resourcePool = moRefOf("ResourcePool", spec.resourcePoolId);
    internalSpec.folder = moRefOf("Folder", spec.folderId);
    internalSpec.host = moRefOf("HostSystem", spec.hostId);
    internalSpec.datastore = moRefOf("Datastore", spec.datastoreId);
    internalSpec.diskProvisioning = spec.diskProvisioning;
    internalSpec.scsiControllerType = spec.scsiControllerType;
    internalSpec.firmware = spec.firmware;
    internalSpec.secureBootEnabled = spec.secureBootEnabled;
    internalSpec.annotation = spec.annotation;
    internalSpec.disks.reserve(spec.disks.size());
    for (const auto& disk : spec.disks) {
        internalSpec.disks.push_back(Detail::ImportDiskSpec{disk.key, disk.capacityBytes});
    }

    // ovfNetworkName is purely an internal OVF-descriptor detail (never seen by a caller): one generated
    // name per distinct target network, so two adapters mapped to the same real network (the many-to-one
    // case) share one <NetworkSection>/networkMapping entry instead of vCenter seeing two OVF-local
    // networks that both happen to resolve to the same real one.
    //
    std::vector<std::pair<std::string, std::string>> networkIdToOvfName; // (networkId, ovfNetworkName)
    internalSpec.networkAdapters.reserve(spec.networkAdapters.size());
    for (const auto& nic : spec.networkAdapters) {
        Detail::ImportNetworkAdapterSpec internalNic;
        internalNic.sourceName = nic.sourceName;
        internalNic.adapterType = nic.adapterType;
        internalNic.macAddress = nic.macAddress;
        if (!nic.networkId.empty()) {
            const auto existing =
                std::find_if(networkIdToOvfName.begin(), networkIdToOvfName.end(),
                              [&](const auto& kv) { return kv.first == nic.networkId; });
            if (existing != networkIdToOvfName.end()) {
                internalNic.ovfNetworkName = existing->second;
            } else {
                internalNic.ovfNetworkName = "network" + std::to_string(networkIdToOvfName.size());
                networkIdToOvfName.emplace_back(nic.networkId, internalNic.ovfNetworkName);
            }
            internalNic.network = moRefOf(nic.networkMorefType.empty() ? "Network" : nic.networkMorefType.c_str(),
                                            nic.networkId);
        }
        internalSpec.networkAdapters.push_back(std::move(internalNic));
    }

    auto leaseResult = Detail::HttpNfcLeaseSession::begin(*impl->m_session, internalSpec);
    if (!leaseResult) {
        logger()->warn("HttpNfcLeaseSession::begin for '{}' failed: {}", spec.vmName,
                        leaseResult.error().toString());
        return std::move(leaseResult).error();
    }
    impl->m_lease = std::move(leaseResult).value();
    impl->m_vmId = impl->m_lease->vm().value;
    impl->m_deviceUrls.reserve(impl->m_lease->deviceUrls().size());
    for (const auto& du : impl->m_lease->deviceUrls()) {
        impl->m_deviceUrls.push_back(VSphereImportDeviceUrl{du.importKey, du.url});
    }

    logger()->info("HttpNfcLease import ready: VM '{}' -> {} ({} device URL(s))", spec.vmName, impl->m_vmId,
                    impl->m_deviceUrls.size());
    return std::unique_ptr<VSphereImportSession>(new VSphereImportSession(std::move(impl)));
}

const std::string& VSphereImportSession::vmId() const noexcept { return m_impl->m_vmId; }

const std::vector<VSphereImportDeviceUrl>& VSphereImportSession::deviceUrls() const noexcept
{
    return m_impl->m_deviceUrls;
}

void VSphereImportSession::startKeepAlive(std::function<int()> percentComplete, std::chrono::seconds interval)
{
    m_impl->m_lease->startKeepAlive(std::move(percentComplete), interval);
}

void VSphereImportSession::stopKeepAlive() { m_impl->m_lease->stopKeepAlive(); }

Error VSphereImportSession::complete() { return m_impl->m_lease->complete(); }

Error VSphereImportSession::destroyPlaceholderVm()
{
    const Vim::MoRef vm{"VirtualMachine", m_impl->m_vmId};
    logger()->info("destroying placeholder VM {} (upload failed or was cancelled -- deleting its files too)",
                    vm.value);
    pugi::xml_document request = Vim::makeVimRequest("Destroy_Task", vm);
    auto callResult = m_impl->m_session->call("urn:vim25/Destroy_Task", request.document_element());
    if (!callResult) {
        return std::move(callResult).error();
    }
    const Vim::MoRef task = Vim::parseMoRef(Vim::soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "Destroy_Task response missing task MoRef");
    }
    Result<Vim::MoRef> waited = Detail::waitForVimTask(*m_impl->m_session, task);
    if (!waited) {
        return std::move(waited).error();
    }
    return {};
}

} // namespace VSphereSoap
