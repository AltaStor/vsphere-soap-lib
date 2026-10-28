// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere HttpNfcLease import session

#include "HttpNfcLeaseSession.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

#include "vsphere-soap/vim/SoapClient.hpp"
#include "SoapLog.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

namespace {

std::string xmlEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

/// Short controller-family name -> OVF `rasd:ResourceSubType` value, for `spec.scsiControllerType`.
/// These are OVF's own subtype spellings (the ones vSphere's OVF export writes and `CreateImportSpec`
/// accepts), *not* the vim API class names `DiskAttach.cpp`'s `kControllerTypeAliases` maps to for
/// `ReconfigVM_Task`: `CreateImportSpec` rejects a class name such as "ParaVirtualSCSIController" with
/// "Unsupported virtual hardware device" (confirmed live against vCenter 8.0.3).
/// Only the SCSI-family subset applies here: this codebase never imports a VM whose *boot* controller
/// needs to be SATA/NVMe/IDE.
///
std::string scsiControllerResourceSubType(const std::string& shortName)
{
    if (shortName.empty() || shortName == "paravirtual") return "VirtualSCSI";
    if (shortName == "lsilogicsas") return "lsilogicsas";
    if (shortName == "lsilogic") return "lsilogic";
    if (shortName == "buslogic") return "buslogic";
    return shortName; // Unrecognized: pass through verbatim rather than silently substituting a default.
}

/// Builds a minimal single-VM, single-or-multi-disk OVF 1.0 descriptor, just enough for
/// `OvfManager.CreateImportSpec` to turn into a `VirtualMachineImportSpec`.
/// UTF-8 flows through unescaped except for the 5 XML metacharacters `xmlEscape` handles, so a non-ASCII
/// `spec.vmName` (Cyrillic included) round-trips correctly: OVF/XML text content is UTF-8 by default
/// with no extra work needed here.
///
std::string buildOvfDescriptor(const ImportVmSpec& spec)
{
    std::ostringstream ovf;
    ovf << R"(<?xml version="1.0" encoding="UTF-8"?>)"
           R"(<Envelope xmlns="http://schemas.dmtf.org/ovf/envelope/1")"
           R"( xmlns:ovf="http://schemas.dmtf.org/ovf/envelope/1")"
           R"( xmlns:rasd="http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData")"
           R"( xmlns:vssd="http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData")"
           R"( xmlns:vmw="http://www.vmware.com/schema/ovf">)";

    // `ovf:id`/`ovf:diskId` are seeded from the caller's own `ImportDiskSpec::key` (falling back to
    // a positional "diskN" only if a caller left it empty) rather than a purely positional "fileN":
    // `HttpNfcLeaseInfo.deviceUrl[].importKey`/`key` is expected to trace back to whatever id this
    // descriptor gave the disk (matching how ovftool/govc correlate their own file references back to
    // lease device URLs), which is the whole reason `ImportDiskSpec::key` exists as a caller-chosen
    // field in the first place: see this function's own doc comment and `ImportDiskSpec`'s.
    // Live testing found vCenter does not echo these keys back (see `begin()`): if vCenter instead
    // returns purely positional/internally-generated keys here regardless of what this descriptor
    // requests, `begin()`'s device-URL-to-disk correlation needs to fall back to request order.
    //
    ovf << "<References>";
    for (std::size_t i = 0; i < spec.disks.size(); ++i) {
        const std::string fileId = spec.disks[i].key.empty() ? "disk" + std::to_string(i) : spec.disks[i].key;
        // ovf:size="0": the real (compressed, streamOptimized) size isn't known up front; this project
        // only ever generates this descriptor for its own immediately-following ImportVApp call, never
        // writes it to a .ovf file for later use, so a placeholder size has nowhere to mislead a future
        // reader.
        //
        ovf << "<File ovf:href=\"" << xmlEscape(fileId) << ".vmdk\" ovf:id=\"" << xmlEscape(fileId)
            << "\" ovf:size=\"0\"/>";
    }
    ovf << "</References>";

    ovf << "<DiskSection><Info>Virtual disks</Info>";
    for (std::size_t i = 0; i < spec.disks.size(); ++i) {
        const std::string fileId = spec.disks[i].key.empty() ? "disk" + std::to_string(i) : spec.disks[i].key;
        ovf << "<Disk ovf:capacity=\"" << spec.disks[i].capacityBytes << "\" ovf:capacityAllocationUnits=\"byte\""
            << " ovf:diskId=\"" << xmlEscape(fileId) << "\" ovf:fileRef=\"" << xmlEscape(fileId)
            << "\" ovf:format=\"http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized\"/>";
    }
    ovf << "</DiskSection>";

    // One <Network> per distinct ovf:name actually referenced by an Ethernet Item below:
    // VSphereImportSession reuses one ovfNetworkName for every adapter mapped to the same real vCenter
    // network (the many-to-one case), so this dedupes by ovfNetworkName rather than emitting one entry
    // per adapter.
    //
    {
        std::vector<std::string> seenNetworkNames;
        for (const auto& nic : spec.networkAdapters) {
            if (nic.ovfNetworkName.empty() || nic.network.empty()) {
                continue; // disconnected adapters have no OVF network to declare
            }
            if (std::find(seenNetworkNames.begin(), seenNetworkNames.end(), nic.ovfNetworkName) !=
                seenNetworkNames.end()) {
                continue;
            }
            seenNetworkNames.push_back(nic.ovfNetworkName);
        }
        if (!seenNetworkNames.empty()) {
            ovf << "<NetworkSection><Info>Logical networks</Info>";
            for (const std::string& name : seenNetworkNames) {
                ovf << "<Network ovf:name=\"" << xmlEscape(name) << "\"><Description>" << xmlEscape(name)
                    << "</Description></Network>";
            }
            ovf << "</NetworkSection>";
        }
    }

    const std::string escapedName = xmlEscape(spec.vmName);
    ovf << "<VirtualSystem ovf:id=\"" << escapedName << "\">";
    ovf << "<Info>A virtual machine</Info><Name>" << escapedName << "</Name>";
    if (!spec.annotation.empty()) {
        ovf << "<AnnotationSection><Info>Annotation</Info><Annotation>" << xmlEscape(spec.annotation)
            << "</Annotation></AnnotationSection>";
    }
    ovf << "<OperatingSystemSection ovf:id=\"1\" vmw:osType=\"" << xmlEscape(spec.guestOsType) << "\">"
        << "<Info>Guest OS</Info></OperatingSystemSection>";

    ovf << "<VirtualHardwareSection><Info>Virtual hardware</Info>";
    ovf << "<System><vssd:ElementName>Virtual Hardware Family</vssd:ElementName>"
        << "<vssd:InstanceID>0</vssd:InstanceID>"
        << "<vssd:VirtualSystemIdentifier>" << escapedName << "</vssd:VirtualSystemIdentifier>";
    if (!spec.hardwareVersion.empty()) {
        ovf << "<vssd:VirtualSystemType>" << xmlEscape(spec.hardwareVersion) << "</vssd:VirtualSystemType>";
    }
    ovf << "</System>";

    // vmw:Config: VMware's own OVF extension mechanism for VM-level settings with no standard OVF/CIM
    // representation (firmware type, EFI Secure Boot); the same
    // `<vmw:Config ovf:required="false" vmw:key="..." vmw:value="..."/>` shape real vSphere OVF exports
    // of EFI VMs use.
    // Not independently confirmed live against this codebase's own `CreateImportSpec`/`ImportVApp` path:
    // see `ImportVmSpec::firmware`'s own doc comment.
    //
    if (spec.firmware == "efi") {
        ovf << R"(<vmw:Config ovf:required="false" vmw:key="firmware" vmw:value="efi"/>)";
        if (spec.secureBootEnabled.has_value()) {
            ovf << R"(<vmw:Config ovf:required="false" vmw:key="bootOptions.efiSecureBootEnabled" vmw:value=")"
                << (*spec.secureBootEnabled ? "true" : "false") << "\"/>";
        }
    }

    int instanceId = 1;
    const int cpuInstanceId = instanceId++;
    ovf << "<Item><rasd:AllocationUnits>hertz * 10^6</rasd:AllocationUnits>"
        << "<rasd:Description>Number of Virtual CPUs</rasd:Description>"
        << "<rasd:ElementName>" << spec.cpuCount << " virtual CPU(s)</rasd:ElementName>"
        << "<rasd:InstanceID>" << cpuInstanceId << "</rasd:InstanceID>"
        << "<rasd:ResourceType>3</rasd:ResourceType>"
        << "<rasd:VirtualQuantity>" << spec.cpuCount << "</rasd:VirtualQuantity></Item>";

    const int memInstanceId = instanceId++;
    const std::uint64_t memoryMiB = spec.memoryBytes / (1024ull * 1024ull);
    ovf << "<Item><rasd:AllocationUnits>byte * 2^20</rasd:AllocationUnits>"
        << "<rasd:Description>Memory Size</rasd:Description>"
        << "<rasd:ElementName>" << memoryMiB << "MB of memory</rasd:ElementName>"
        << "<rasd:InstanceID>" << memInstanceId << "</rasd:InstanceID>"
        << "<rasd:ResourceType>4</rasd:ResourceType>"
        << "<rasd:VirtualQuantity>" << memoryMiB << "</rasd:VirtualQuantity></Item>";

    const int scsiInstanceId = instanceId++;
    ovf << "<Item><rasd:Address>0</rasd:Address>"
        << "<rasd:Description>SCSI Controller</rasd:Description>"
        << "<rasd:ElementName>SCSI Controller 0</rasd:ElementName>"
        << "<rasd:InstanceID>" << scsiInstanceId << "</rasd:InstanceID>"
        << "<rasd:ResourceSubType>" << xmlEscape(scsiControllerResourceSubType(spec.scsiControllerType))
        << "</rasd:ResourceSubType>"
        << "<rasd:ResourceType>6</rasd:ResourceType></Item>";

    for (std::size_t i = 0; i < spec.disks.size(); ++i) {
        const std::string fileId = spec.disks[i].key.empty() ? "disk" + std::to_string(i) : spec.disks[i].key;
        const int diskInstanceId = instanceId++;
        ovf << "<Item><rasd:AddressOnParent>" << i << "</rasd:AddressOnParent>"
            << "<rasd:ElementName>Hard Disk " << (i + 1) << "</rasd:ElementName>"
            << "<rasd:HostResource>ovf:/disk/" << xmlEscape(fileId) << "</rasd:HostResource>"
            << "<rasd:InstanceID>" << diskInstanceId << "</rasd:InstanceID>"
            << "<rasd:Parent>" << scsiInstanceId << "</rasd:Parent>"
            << "<rasd:ResourceType>17</rasd:ResourceType></Item>";
    }

    for (std::size_t i = 0; i < spec.networkAdapters.size(); ++i) {
        const ImportNetworkAdapterSpec& nic = spec.networkAdapters[i];
        const int nicInstanceId = instanceId++;
        // rasd elements must appear in the CIM schema's (alphabetical) sequence order (CreateImportSpec
        // rejects an out-of-place one as "Unsupported element") so Address precedes AddressOnParent.
        ovf << "<Item>";
        if (!nic.macAddress.empty()) {
            ovf << "<rasd:Address>" << xmlEscape(nic.macAddress) << "</rasd:Address>";
        }
        ovf << "<rasd:AddressOnParent>" << i << "</rasd:AddressOnParent>"
            << "<rasd:AutomaticAllocation>true</rasd:AutomaticAllocation>";
        if (!nic.ovfNetworkName.empty() && !nic.network.empty()) {
            ovf << "<rasd:Connection>" << xmlEscape(nic.ovfNetworkName) << "</rasd:Connection>";
        }
        ovf << "<rasd:Description>Network adapter " << (i + 1) << "</rasd:Description>"
            << "<rasd:ElementName>Network adapter " << (i + 1) << "</rasd:ElementName>"
            << "<rasd:InstanceID>" << nicInstanceId << "</rasd:InstanceID>"
            << "<rasd:ResourceSubType>" << xmlEscape(nic.adapterType.empty() ? "VmxNet3" : nic.adapterType)
            << "</rasd:ResourceSubType>"
            << "<rasd:ResourceType>10</rasd:ResourceType></Item>";
    }

    ovf << "</VirtualHardwareSection></VirtualSystem></Envelope>";
    return ovf.str();
}

/// Substitutes vCenter's `*` device-URL host placeholder for the real host this session is talking to,
/// e.g. "https://*:443/foo" -> "https://192.168.1.1:443/foo".
/// Leaves the URL untouched if it doesn't contain the placeholder (a direct-to-ESXi lease already has
/// a real hostname/IP there).
///
std::string substituteHostPlaceholder(const std::string& url, const std::string& realHost)
{
    const auto pos = url.find("://*");
    if (pos == std::string::npos) {
        return url;
    }
    return url.substr(0, pos + 3) + realHost + url.substr(pos + 4);
}

} // namespace

HttpNfcLeaseSession::HttpNfcLeaseSession(VimSession& session, MoRef lease, MoRef vm,
                                            std::vector<LeaseDeviceUrl> deviceUrls)
    : m_session(session), m_lease(std::move(lease)), m_vm(std::move(vm)), m_deviceUrls(std::move(deviceUrls)) {}

HttpNfcLeaseSession::~HttpNfcLeaseSession()
{
    stopKeepAlive();
    if (!m_completed && !m_lease.empty()) {
        try {
            pugi::xml_document request = makeVimRequest("HttpNfcLeaseAbort", m_lease);
            (void)m_session.call("urn:vim25/HttpNfcLeaseAbort", request.document_element());
        } catch (...) {
            // Never propagate out of a destructor: matches SnapshotScope.
        }
    }
}

void HttpNfcLeaseSession::startKeepAlive(std::function<int()> percentComplete, std::chrono::seconds interval)
{
    if (m_keepAliveRunning.exchange(true)) {
        logger()->debug("startKeepAlive called while already running -- ignoring");
        return;
    }
    m_keepAliveThread = std::thread([this, percentComplete = std::move(percentComplete), interval]() {
        while (m_keepAliveRunning.load()) {
            const int percent = percentComplete ? percentComplete() : 0;
            pugi::xml_document request = makeVimRequest("HttpNfcLeaseProgress", m_lease);
            request.document_element().append_child("percent").text() = std::to_string(percent).c_str();
            // Best-effort: a transient failure here shouldn't kill the upload that's actually in flight
            // on another thread; the *next* tick trying again is the real mitigation, the same way
            // a missed heartbeat elsewhere in this codebase just waits for the next one rather than
            // escalating on the first miss.
            //
            if (auto result = m_session.call("urn:vim25/HttpNfcLeaseProgress", request.document_element()); !result) {
                logger()->debug("HttpNfcLeaseProgress failed (will retry next tick): {}", result.error().toString());
            }
            for (int waited = 0; waited < interval.count() && m_keepAliveRunning.load(); ++waited) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void HttpNfcLeaseSession::stopKeepAlive()
{
    m_keepAliveRunning = false;
    if (m_keepAliveThread.joinable()) {
        m_keepAliveThread.join();
    }
}

Error HttpNfcLeaseSession::complete()
{
    if (m_completed) {
        return {};
    }
    stopKeepAlive();
    pugi::xml_document request = makeVimRequest("HttpNfcLeaseComplete", m_lease);
    auto result = m_session.call("urn:vim25/HttpNfcLeaseComplete", request.document_element());
    if (!result) {
        return std::move(result).error();
    }
    m_completed = true;
    return {};
}

namespace {

/// `SnapshotScope::waitForTask`'s polling shape, adapted for `HttpNfcLease.state`/`info` instead of
/// `Task.info.state`/`info.result`: see that function's own comments for the retry/backoff reasoning,
/// which applies identically here.
Result<pugi::xml_node> waitForLeaseReady(VimSession& session, const MoRef& lease, pugi::xml_document& storage)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    auto backoff = std::chrono::milliseconds(250);
    constexpr auto kMaxBackoff = std::chrono::milliseconds(5000);

    for (;;) {
        pugi::xml_document request = makeVimRequest("RetrievePropertiesEx", session.propertyCollector());
        pugi::xml_node op = request.document_element();
        pugi::xml_node specSet = op.append_child("specSet");
        pugi::xml_node propSet = specSet.append_child("propSet");
        propSet.append_child("type").text() = "HttpNfcLease";
        propSet.append_child("all").text() = "false";
        propSet.append_child("pathSet").text() = "state";
        propSet.append_child("pathSet").text() = "info";
        propSet.append_child("pathSet").text() = "error";
        pugi::xml_node objectSet = specSet.append_child("objectSet");
        pugi::xml_node obj = objectSet.append_child("obj");
        obj.append_attribute("type") = lease.type.c_str();
        obj.text() = lease.value.c_str();
        op.append_child("options");

        auto callResult = session.call("urn:vim25/RetrievePropertiesEx", op);
        if (!callResult) {
            return std::move(callResult).error();
        }

        const pugi::xml_node objectContent = returnval(soapBodyContent(callResult.value())).child("objects");
        std::string state;
        pugi::xml_node info;
        std::string errorMessage;
        for (pugi::xml_node prop = objectContent.child("propSet"); prop; prop = prop.next_sibling("propSet")) {
            const std::string name = prop.child("name").text().as_string();
            if (name == "state") {
                state = prop.child("val").text().as_string();
            } else if (name == "info") {
                info = prop.child("val");
            } else if (name == "error") {
                if (const pugi::xml_node msg = prop.child("val").child("localizedMessage")) {
                    errorMessage = msg.text().as_string();
                }
            }
        }

        if (state == "ready") {
            if (!info) {
                return Error(ErrorCode::InvalidFormat, "HttpNfcLease is ready but reported no info");
            }
            // Deep-copy `info` into caller-owned storage so it outlives this function's own
            // `callResult`/`request` documents.
            storage.reset();
            pugi::xml_node root = storage.append_child("info");
            for (pugi::xml_node child = info.first_child(); child; child = child.next_sibling()) {
                root.append_copy(child);
            }
            return storage.child("info");
        }
        if (state == "error") {
            return Error(ErrorCode::IoError,
                         "HttpNfcLease failed: " + (errorMessage.empty() ? "unknown error" : errorMessage));
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return Error(ErrorCode::IoError, "HttpNfcLease " + lease.value + " timed out waiting to become ready");
        }
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, kMaxBackoff);
    }
}

} // namespace

Result<std::unique_ptr<HttpNfcLeaseSession>> HttpNfcLeaseSession::begin(VimSession& session,
                                                                            const ImportVmSpec& spec)
{
    if (spec.disks.empty()) {
        return Error(ErrorCode::InvalidArgument, "HttpNfcLeaseSession::begin: spec.disks is empty");
    }
    if (session.ovfManager().empty()) {
        return Error(ErrorCode::InvalidArgument,
                     "HttpNfcLeaseSession::begin: this vCenter/ESXi's ServiceContent had no ovfManager");
    }

    const std::string ovfDescriptor = buildOvfDescriptor(spec);
    logger()->trace("generated OVF descriptor for '{}':\n{}", spec.vmName, ovfDescriptor);

    // --- CreateImportSpec ---
    pugi::xml_document cisRequest = makeVimRequest("CreateImportSpec", session.ovfManager());
    {
        pugi::xml_node op = cisRequest.document_element();
        op.append_child("ovfDescriptor").text() = ovfDescriptor.c_str();
        pugi::xml_node resourcePool = op.append_child("resourcePool");
        resourcePool.append_attribute("type") = spec.resourcePool.type.c_str();
        resourcePool.text() = spec.resourcePool.value.c_str();
        pugi::xml_node datastore = op.append_child("datastore");
        datastore.append_attribute("type") = spec.datastore.empty() ? "Datastore" : spec.datastore.type.c_str();
        // empty is valid: see ImportVmSpec::datastore's doc comment
        datastore.text() = spec.datastore.value.c_str();
        // OvfCreateImportSpecParams: `locale`/`deploymentOption`/`entityName` are all mandatory fields
        // on the wire (confirmed live: vCenter rejects CreateImportSpec outright, with
        // an InvalidRequestFault naming exactly these three, if any is omitted) even though nothing
        // about this project's minimal OVF descriptor uses locales or deployment options; "US"/empty are
        // the same "there's nothing to select" values ovftool itself sends.
        // `entityName` is what actually names the imported VM; it's not inferred from the descriptor's
        // own VirtualSystem/Name element.
        //
        pugi::xml_node cisp = op.append_child("cisp");
        cisp.append_child("locale").text() = "US";
        cisp.append_child("deploymentOption");
        cisp.append_child("entityName").text() = spec.vmName.c_str();
        // Element order below is significant: vim25's OvfCreateImportSpecParams is an xsd:sequence
        // (entityName, hostSystem, networkMapping, ipAllocationPolicy, ipProtocol, propertyMapping,
        // resourceMapping, diskProvisioning, ...), and vCenter rejects the whole call with
        // InvalidRequestFault ("Unexpected element tag") when an element is out of place; so
        // diskProvisioning must come after every networkMapping.
        //
        // OvfNetworkMapping: pairs each OVF logical network name declared in the descriptor's own
        // <NetworkSection> (built above from spec.networkAdapters) with the real vCenter network to
        // connect it to; without this, CreateImportSpec leaves every Ethernet Item's network unresolved
        // (confirmed by the OVF spec itself: NetworkSection names are OVF-local until something maps
        // them to a real network on import).
        // Deduped by ovfNetworkName the same way buildOvfDescriptor's own <NetworkSection> emission is,
        // so a many-to-one mapping doesn't produce contradictory duplicate entries.
        //
        {
            std::vector<std::string> seenNetworkNames;
            for (const auto& nic : spec.networkAdapters) {
                if (nic.ovfNetworkName.empty() || nic.network.empty()) {
                    continue;
                }
                if (std::find(seenNetworkNames.begin(), seenNetworkNames.end(), nic.ovfNetworkName) !=
                    seenNetworkNames.end()) {
                    continue;
                }
                seenNetworkNames.push_back(nic.ovfNetworkName);
                pugi::xml_node mapping = cisp.append_child("networkMapping");
                mapping.append_child("name").text() = nic.ovfNetworkName.c_str();
                pugi::xml_node networkNode = mapping.append_child("network");
                networkNode.append_attribute("type") = nic.network.type.c_str();
                networkNode.text() = nic.network.value.c_str();
            }
        }
        // Controls the destination disk's actual on-datastore shape: see
        // ImportVmSpec::diskProvisioning's own doc comment.
        // Omitted (not an empty element) when unset, letting vCenter fall back to its own default,
        // the same as every field above that already conditions on non-emptiness elsewhere in this file
        // (e.g. `folder`/`host` below).
        //
        if (!spec.diskProvisioning.empty()) {
            cisp.append_child("diskProvisioning").text() = spec.diskProvisioning.c_str();
        }
    }
    auto cisResult = session.call("urn:vim25/CreateImportSpec", cisRequest.document_element());
    if (!cisResult) {
        return std::move(cisResult).error();
    }
    const pugi::xml_node cisReturn = returnval(soapBodyContent(cisResult.value()));
    if (!cisReturn) {
        return Error(ErrorCode::InvalidFormat, "CreateImportSpec response missing <returnval>");
    }
    for (pugi::xml_node err = cisReturn.child("error"); err; err = err.next_sibling("error")) {
        const std::string msg = err.child("localizedMessage").text().as_string();
        return Error(ErrorCode::IoError,
                     "CreateImportSpec reported an error: " + (msg.empty() ? std::string("(no message)") : msg));
    }
    const pugi::xml_node importSpecNode = cisReturn.child("importSpec");
    if (!importSpecNode) {
        return Error(ErrorCode::InvalidFormat, "CreateImportSpec response missing importSpec");
    }

    // --- ImportVApp ---
    pugi::xml_document importRequest = makeVimRequest("ImportVApp", spec.resourcePool);
    {
        pugi::xml_node op = importRequest.document_element();
        // CreateImportSpec's returnval always carries its importSpec as a concrete subtype of
        // the polymorphic vim.ImportSpec base (e.g. VirtualMachineImportSpec), serialized with
        // an xsi:type attribute on the top-level node; confirmed live: vCenter rejects ImportVApp with
        // "undefined namespace prefix xsi" without this, since append_copy() below only copies
        // the attribute itself, not whatever ancestor of the *original* response document declared
        // the xsi prefix (that ancestor isn't part of what's being copied into this brand-new request
        // document).
        //
        op.append_attribute("xmlns:xsi") = "http://www.w3.org/2001/XMLSchema-instance";
        pugi::xml_node specCopy = op.append_child("spec");
        for (pugi::xml_attribute attr = importSpecNode.first_attribute(); attr; attr = attr.next_attribute()) {
            specCopy.append_copy(attr);
        }
        for (pugi::xml_node child = importSpecNode.first_child(); child; child = child.next_sibling()) {
            specCopy.append_copy(child);
        }
        if (!spec.folder.empty()) {
            pugi::xml_node folderNode = op.append_child("folder");
            folderNode.append_attribute("type") = spec.folder.type.c_str();
            folderNode.text() = spec.folder.value.c_str();
        }
        if (!spec.host.empty()) {
            pugi::xml_node hostNode = op.append_child("host");
            hostNode.append_attribute("type") = spec.host.type.c_str();
            hostNode.text() = spec.host.value.c_str();
        }
    }
    auto importResult = session.call("urn:vim25/ImportVApp", importRequest.document_element());
    if (!importResult) {
        return std::move(importResult).error();
    }
    const MoRef lease = parseMoRef(returnval(soapBodyContent(importResult.value())));
    if (lease.empty()) {
        return Error(ErrorCode::InvalidFormat, "ImportVApp response missing HttpNfcLease MoRef");
    }

    // From here on, any failure must abort the lease rather than just returning an error: wrap the rest
    // in a lambda so every early return can funnel through one abort-then-propagate path instead of
    // repeating it at each call site.
    auto fail = [&](Error err) -> Result<std::unique_ptr<HttpNfcLeaseSession>> {
        pugi::xml_document abortRequest = makeVimRequest("HttpNfcLeaseAbort", lease);
        (void)session.call("urn:vim25/HttpNfcLeaseAbort", abortRequest.document_element());
        return err;
    };

    pugi::xml_document infoStorage;
    auto infoResult = waitForLeaseReady(session, lease, infoStorage);
    if (!infoResult) {
        return fail(std::move(infoResult).error());
    }
    const pugi::xml_node info = infoResult.value();

    MoRef importedVm = parseMoRef(info.child("entity"));
    if (importedVm.empty()) {
        return fail(Error(ErrorCode::InvalidFormat, "HttpNfcLeaseInfo missing entity (the imported VM)"));
    }

    // HttpNfcLeaseInfo.deviceUrl[] is not one entry per requested disk; confirmed live: a real vCenter
    // returned 2 entries for a 1-disk import, the second being the VM's .nvram file (disk=false).
    // Worse, `importKey`/`key` turned out to be entirely vCenter-generated (e.g.
    // "/<vmName>/ParaVirtualSCSIController0:0"), not an echo of the caller-chosen `ImportDiskSpec::key`
    // this header originally assumed would round-trip; so importKey cannot be used as the correlation
    // key it was designed to be.
    // `disk` (true/false) is vim25's own way of saying whether an entry is a virtual disk at all;
    // filtering to just those and correlating the remainder positionally against `spec.disks` (in
    // the order ImportSpec listed them, which live testing confirms is also the order vCenter reports
    // their device URLs in) is the only correlation mechanism left.
    // Every entry is still logged before filtering, since that's the only way this project learns what
    // a given vCenter actually sends.
    //
    std::vector<LeaseDeviceUrl> diskDeviceUrls;
    for (pugi::xml_node du = info.child("deviceUrl"); du; du = du.next_sibling("deviceUrl")) {
        const std::string key = du.child("key").text().as_string();
        std::string importKey = du.child("importKey").text().as_string();
        if (importKey.empty()) {
            importKey = key;
        }
        const std::string rawUrl = du.child("url").text().as_string();
        const std::string url = substituteHostPlaceholder(rawUrl, session.soapHost());
        const bool isDisk = du.child("disk").text().as_bool();
        logger()->info("HttpNfcLease device URL: key='{}' importKey='{}' disk='{}' url='{}'", key, importKey, isDisk,
                        url);
        if (isDisk) {
            diskDeviceUrls.push_back(LeaseDeviceUrl{importKey, url});
        }
    }
    if (diskDeviceUrls.size() != spec.disks.size()) {
        return fail(Error(ErrorCode::InvalidFormat, "HttpNfcLeaseInfo returned " +
                                                          std::to_string(diskDeviceUrls.size()) +
                                                          " disk device URL(s) for " +
                                                          std::to_string(spec.disks.size()) + " requested disk(s)"));
    }
    // Overlay the caller's own key onto each positionally-matched entry: from here on,
    // `deviceUrls()[i].importKey` is `spec.disks[i].key` exactly as this class's own public contract
    // promises, regardless of whatever vCenter-internal string actually travelled over the wire.
    for (std::size_t i = 0; i < diskDeviceUrls.size(); ++i) {
        diskDeviceUrls[i].importKey = spec.disks[i].key;
    }

    return std::unique_ptr<HttpNfcLeaseSession>(
        new HttpNfcLeaseSession(session, lease, importedVm, std::move(diskDeviceUrls)));
}

} // namespace VSphereSoap::Detail
