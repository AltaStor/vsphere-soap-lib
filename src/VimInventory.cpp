// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere VM lookup and property retrieval

#include "vsphere-soap/vim/VimInventory.hpp"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>

#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Vim {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

VmLocator VmLocator::byMoref(std::string moref)
{
    VmLocator locator;
    locator.moref = std::move(moref);
    return locator;
}

VmLocator VmLocator::byUuid(std::string uuid)
{
    VmLocator locator;
    locator.uuid = std::move(uuid);
    return locator;
}

VmLocator VmLocator::byInventoryPath(std::string path)
{
    VmLocator locator;
    locator.inventoryPath = std::move(path);
    return locator;
}

std::optional<std::pair<std::string, std::string>> parseDatastorePath(std::string_view backing)
{
    if (backing.empty() || backing.front() != '[') {
        return std::nullopt;
    }
    const auto closeBracket = backing.find(']');
    if (closeBracket == std::string_view::npos) {
        return std::nullopt;
    }
    const auto datastore = backing.substr(1, closeBracket - 1);
    auto relativePath = backing.substr(closeBracket + 1);
    while (!relativePath.empty() && relativePath.front() == ' ') {
        relativePath.remove_prefix(1);
    }
    if (datastore.empty() || relativePath.empty()) {
        return std::nullopt;
    }
    return std::make_pair(std::string(datastore), std::string(relativePath));
}

namespace {

VimPowerState parsePowerState(std::string_view s)
{
    if (s == "poweredOff") {
        return VimPowerState::PoweredOff;
    }
    if (s == "poweredOn") {
        return VimPowerState::PoweredOn;
    }
    if (s == "suspended") {
        return VimPowerState::Suspended;
    }
    return VimPowerState::Unknown;
}

// A controller device's xsi:type is one of these: every subtype of
// VirtualSCSIController/VirtualSATAController plus VirtualNVMEController and VirtualIDEController.
// Checked against the raw xsi:type string rather than walking a base-type hierarchy pugixml doesn't
// expose, since the VIM WSDL's full subtype list is fixed and short.
//
bool isControllerDeviceType(std::string_view xsiType)
{
    static constexpr std::string_view kControllerTypes[] = {
        "VirtualLsiLogicController",     "VirtualBusLogicController",  "VirtualLsiLogicSASController",
        "ParaVirtualSCSIController",     "VirtualIDEController",       "VirtualSATAController",
        "VirtualAHCIController",         "VirtualNVMEController",
    };
    return std::find(std::begin(kControllerTypes), std::end(kControllerTypes), xsiType) !=
           std::end(kControllerTypes);
}

// Every VirtualEthernetCard subtype the VIM WSDL defines: same "fixed, short, checked against the raw
// xsi:type" reasoning as isControllerDeviceType.
bool isEthernetCardDeviceType(std::string_view xsiType)
{
    static constexpr std::string_view kEthernetTypes[] = {
        "VirtualE1000", "VirtualE1000e", "VirtualVmxnet2", "VirtualVmxnet3",
        "VirtualPCNet32", "VirtualSriovEthernetCard", "VirtualVmxnet3Vrdma",
    };
    return std::find(std::begin(kEthernetTypes), std::end(kEthernetTypes), xsiType) != std::end(kEthernetTypes);
}

// config.hardware.device's <val> is an ArrayOfVirtualDevice; each child is one device, discriminated by
// its own xsi:type attribute (VirtualDisk, VirtualE1000, ParaVirtualSCSIController, ...).
// Two passes over the same array: disks first (recording each one's controllerKey/unitNumber, parsed
// straight off the device; no separate controller lookup needed for that), then controllers, so each
// controller's `usedUnitNumbers` can be filled from the disks already collected in pass one regardless
// of which order the server listed the devices in.
//
void parseDevices(pugi::xml_node deviceArrayVal, std::vector<VimDiskInfo>& disks,
                   std::vector<VimControllerInfo>& controllers,
                   std::vector<VimNetworkAdapterInfo>& networkAdapters)
{
    for (pugi::xml_node device = deviceArrayVal.first_child(); device; device = device.next_sibling()) {
        const pugi::xml_attribute xsiType = device.attribute("xsi:type");
        if (!xsiType || std::string_view(xsiType.as_string()) != "VirtualDisk") {
            continue;
        }

        VimDiskInfo disk;
        disk.key = device.child("key").text().as_string();
        disk.controllerKey = device.child("controllerKey").text().as_int();
        disk.unitNumber = device.child("unitNumber").text().as_int();

        if (const pugi::xml_node capBytes = device.child("capacityInBytes")) {
            disk.capacityBytes = capBytes.text().as_ullong();
        } else {
            disk.capacityBytes = device.child("capacityInKB").text().as_ullong() * 1024ull;
        }

        const pugi::xml_node backing = device.child("backing");
        if (const pugi::xml_attribute backingType = backing.attribute("xsi:type")) {
            disk.backingType = backingType.as_string();
        }
        disk.rawFileName = backing.child("fileName").text().as_string();
        disk.thinProvisioned = backing.child("thinProvisioned").text().as_bool();
        disk.eagerlyScrub = backing.child("eagerlyScrub").text().as_bool();
        disk.diskMode = backing.child("diskMode").text().as_string();
        disk.uuid = backing.child("uuid").text().as_string();
        if (const pugi::xml_node changeId = backing.child("changeId");
            changeId && changeId.text().as_string()[0] != '\0') {
            disk.changeId = changeId.text().as_string();
        }

        if (auto parsed = parseDatastorePath(disk.rawFileName)) {
            disk.datastoreName = std::move(parsed->first);
            disk.datastoreRelativePath = std::move(parsed->second);
        }

        disks.push_back(std::move(disk));
    }

    for (pugi::xml_node device = deviceArrayVal.first_child(); device; device = device.next_sibling()) {
        const pugi::xml_attribute xsiType = device.attribute("xsi:type");
        if (!xsiType || !isControllerDeviceType(xsiType.as_string())) {
            continue;
        }

        VimControllerInfo controller;
        controller.key = device.child("key").text().as_int();
        controller.deviceType = xsiType.as_string();
        controller.busNumber = device.child("busNumber").text().as_int();
        for (const VimDiskInfo& disk : disks) {
            if (disk.controllerKey == controller.key) {
                controller.usedUnitNumbers.push_back(disk.unitNumber);
            }
        }
        controllers.push_back(std::move(controller));
    }

    for (pugi::xml_node device = deviceArrayVal.first_child(); device; device = device.next_sibling()) {
        const pugi::xml_attribute xsiType = device.attribute("xsi:type");
        if (!xsiType || !isEthernetCardDeviceType(xsiType.as_string())) {
            continue;
        }

        VimNetworkAdapterInfo adapter;
        adapter.key = device.child("key").text().as_int();
        adapter.deviceType = xsiType.as_string();
        adapter.macAddress = device.child("macAddress").text().as_string();
        adapter.connected = device.child("connectable").child("connected").text().as_bool();
        adapter.startConnected = device.child("connectable").child("startConnected").text().as_bool();

        const pugi::xml_node backing = device.child("backing");
        if (const pugi::xml_attribute backingType = backing.attribute("xsi:type")) {
            adapter.backingType = backingType.as_string();
        }
        if (adapter.backingType == "VirtualEthernetCardNetworkBackingInfo") {
            adapter.networkName = backing.child("deviceName").text().as_string();
        } else if (adapter.backingType == "VirtualEthernetCardDistributedVirtualPortBackingInfo") {
            adapter.portgroupKey = backing.child("port").child("portgroupKey").text().as_string();
        }

        networkAdapters.push_back(std::move(adapter));
    }
}

void parseObjectContentInto(pugi::xml_node objectContent, VimVmInfo& info)
{
    for (pugi::xml_node prop = objectContent.child("propSet"); prop; prop = prop.next_sibling("propSet")) {
        const std::string name = prop.child("name").text().as_string();
        const pugi::xml_node val = prop.child("val");

        if (name == "name") {
            info.name = val.text().as_string();
        } else if (name == "runtime.powerState") {
            info.powerState = parsePowerState(val.text().as_string());
        } else if (name == "runtime.host") {
            info.host = parseMoRef(val);
        } else if (name == "config.uuid") {
            info.configUuid = val.text().as_string();
        } else if (name == "config.firmware") {
            info.firmware = val.text().as_string();
        } else if (name == "config.version") {
            info.hwVersion = val.text().as_string();
        } else if (name == "guest.guestId") {
            info.guestId = val.text().as_string();
        } else if (name == "config.changeTrackingEnabled") {
            info.changeTrackingEnabled = val.text().as_bool();
        } else if (name == "config.hardware.device") {
            parseDevices(val, info.disks, info.controllers, info.networkAdapters);
        } else if (name == "config.files.vmPathName") {
            info.vmPathName = val.text().as_string();
        } else if (name == "config.annotation") {
            info.annotation = val.text().as_string();
        } else if (name == "config.hardware.numCPU") {
            info.numCpu = val.text().as_uint();
        } else if (name == "config.hardware.memoryMB") {
            info.memoryMb = val.text().as_ullong();
        }
    }
}

/// One batched `RetrievePropertiesEx` covering every distinct distributed portgroup referenced by
/// `adapters` (several `objectSet` entries sharing one `propSet`, since they're all the same type:
/// `RetrievePropertiesEx` supports this directly, no per-portgroup round trip needed), filling in each
/// matching adapter's `networkName`/`vlanId`.
/// Best-effort: a failed call here is logged and swallowed rather than failing the whole `describeVm()`,
/// since a name/VLAN lookup gap for one portgroup shouldn't break inventory of a VM that otherwise
/// described just fine (matches this file's `warnings`-style tolerance elsewhere in the codebase for
/// non-essential detail).
///
void resolveDistributedPortgroupDetails(VimSession& session, std::vector<VimNetworkAdapterInfo>& adapters)
{
    std::vector<std::string> keys;
    for (const VimNetworkAdapterInfo& adapter : adapters) {
        if (!adapter.portgroupKey.empty() &&
            std::find(keys.begin(), keys.end(), adapter.portgroupKey) == keys.end()) {
            keys.push_back(adapter.portgroupKey);
        }
    }
    if (keys.empty()) {
        return;
    }

    pugi::xml_document request = makeVimRequest("RetrievePropertiesEx", session.propertyCollector());
    pugi::xml_node op = request.document_element();
    pugi::xml_node specSet = op.append_child("specSet");
    pugi::xml_node propSet = specSet.append_child("propSet");
    propSet.append_child("type").text() = "DistributedVirtualPortgroup";
    propSet.append_child("all").text() = "false";
    propSet.append_child("pathSet").text() = "name";
    propSet.append_child("pathSet").text() = "config.defaultPortConfig.vlan.vlanId";
    for (const std::string& key : keys) {
        pugi::xml_node objectSet = specSet.append_child("objectSet");
        pugi::xml_node obj = objectSet.append_child("obj");
        obj.append_attribute("type") = "DistributedVirtualPortgroup";
        obj.text() = key.c_str();
    }
    op.append_child("options");

    auto callResult = session.call("urn:vim25/RetrievePropertiesEx", op);
    if (!callResult) {
        return;
    }

    std::vector<std::pair<std::string, std::string>> names;       // (portgroupKey, name)
    std::vector<std::pair<std::string, int>> vlanIds;              // (portgroupKey, vlanId)
    const pugi::xml_node result = returnval(soapBodyContent(callResult.value()));
    for (pugi::xml_node objectContent = result.child("objects"); objectContent;
         objectContent = objectContent.next_sibling("objects")) {
        const std::string key = parseMoRef(objectContent.child("obj")).value;
        if (key.empty()) {
            continue;
        }
        for (pugi::xml_node prop = objectContent.child("propSet"); prop; prop = prop.next_sibling("propSet")) {
            const std::string propName = prop.child("name").text().as_string();
            const pugi::xml_node val = prop.child("val");
            if (propName == "name") {
                names.emplace_back(key, val.text().as_string());
            } else if (propName == "config.defaultPortConfig.vlan.vlanId") {
                // Only a plain VmwareDistributedVirtualSwitchVlanIdSpec has a scalar vlanId text node;
                // a PVLAN/trunk-range spec doesn't, and val.text().as_int() harmlessly reads as 0
                // (vSphere's own "no VLAN" value) for those, which VimNetworkAdapterInfo::vlanId's own
                // doc comment already documents as "leave unset" below.
                //
                if (const int vlanId = val.text().as_int(); vlanId > 0) {
                    vlanIds.emplace_back(key, vlanId);
                }
            }
        }
    }

    for (VimNetworkAdapterInfo& adapter : adapters) {
        if (adapter.portgroupKey.empty()) {
            continue;
        }
        for (const auto& [key, name] : names) {
            if (key == adapter.portgroupKey) {
                adapter.networkName = name;
                break;
            }
        }
        for (const auto& [key, vlanId] : vlanIds) {
            if (key == adapter.portgroupKey) {
                adapter.vlanId = vlanId;
                break;
            }
        }
    }
}

// Sends `op` (a `RetrievePropertiesEx` request) and then `ContinueRetrievePropertiesEx` for as long as
// a page comes back with a `token`, handing every returned `ObjectContent` to `onObject`.
template <typename OnObject>
Error retrievePropertiesPaged(VimSession& session, const pugi::xml_node& op, OnObject&& onObject)
{
    std::string token;
    bool firstPage = true;
    for (;;) {
        Result<pugi::xml_document> callResult = [&]() -> Result<pugi::xml_document> {
            if (firstPage) {
                return session.call("urn:vim25/RetrievePropertiesEx", op);
            }
            pugi::xml_document continueRequest =
                makeVimRequest("ContinueRetrievePropertiesEx", session.propertyCollector());
            continueRequest.document_element().append_child("token").text() = token.c_str();
            return session.call("urn:vim25/ContinueRetrievePropertiesEx", continueRequest.document_element());
        }();
        firstPage = false;

        if (!callResult) {
            return std::move(callResult).error();
        }

        const pugi::xml_node result = returnval(soapBodyContent(callResult.value()));
        token = result.child("token").text().as_string();

        for (pugi::xml_node objectContent = result.child("objects"); objectContent;
             objectContent = objectContent.next_sibling("objects")) {
            onObject(objectContent);
        }

        if (token.empty()) {
            return {};
        }
    }
}

// A property value as text: a scalar's own text, or an array's elements' texts joined with ',' (e.g.
// Folder.childType's ArrayOfString).
std::string flattenPropertyValue(pugi::xml_node val)
{
    if (!val.first_child() || val.first_child().type() != pugi::node_element) {
        return val.text().as_string();
    }
    std::string joined;
    for (pugi::xml_node element = val.first_child(); element; element = element.next_sibling()) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += element.text().as_string();
    }
    return joined;
}

} // namespace

std::string VimObjectProperties::get(std::string_view path) const
{
    for (const auto& [name, value] : properties) {
        if (name == path) {
            return value;
        }
    }
    return {};
}

VimInventory::VimInventory(VimSession& session) : m_session(session) {}

Result<MoRef> VimInventory::findVm(const VmLocator& locator)
{
    if (locator.moref) {
        return MoRef{"VirtualMachine", *locator.moref};
    }

    if (locator.uuid) {
        pugi::xml_document request = makeVimRequest("FindByUuid", m_session.searchIndex());
        pugi::xml_node op = request.document_element();
        op.append_child("uuid").text() = locator.uuid->c_str();
        op.append_child("vmSearch").text() = "true";
        op.append_child("instanceUuid").text() = "false";

        auto callResult = m_session.call("urn:vim25/FindByUuid", op);
        if (!callResult) {
            return std::move(callResult).error();
        }
        const MoRef found = parseMoRef(soapBodyContent(callResult.value()).child("returnval"));
        if (found.empty()) {
            return Error(ErrorCode::InvalidArgument, "no VM with UUID " + *locator.uuid);
        }
        return found;
    }

    if (locator.inventoryPath) {
        pugi::xml_document request = makeVimRequest("FindByInventoryPath", m_session.searchIndex());
        pugi::xml_node op = request.document_element();
        op.append_child("inventoryPath").text() = locator.inventoryPath->c_str();

        auto callResult = m_session.call("urn:vim25/FindByInventoryPath", op);
        if (!callResult) {
            return std::move(callResult).error();
        }
        const MoRef found = parseMoRef(soapBodyContent(callResult.value()).child("returnval"));
        if (found.empty()) {
            return Error(ErrorCode::InvalidArgument, "no object at inventory path " + *locator.inventoryPath);
        }
        return found;
    }

    return Error(ErrorCode::InvalidArgument, "VmLocator has none of moref/uuid/inventoryPath set");
}

Result<VimVmInfo> VimInventory::describeVm(const MoRef& vm)
{
    VimVmInfo info;
    info.moref = vm;

    pugi::xml_document request = makeVimRequest("RetrievePropertiesEx", m_session.propertyCollector());
    pugi::xml_node op = request.document_element();
    pugi::xml_node specSet = op.append_child("specSet");
    pugi::xml_node propSet = specSet.append_child("propSet");
    propSet.append_child("type").text() = "VirtualMachine";
    propSet.append_child("all").text() = "false";
    for (const char* path : {"name", "runtime.powerState", "runtime.host", "config.uuid", "config.firmware",
                              "config.version", "guest.guestId", "config.changeTrackingEnabled",
                              "config.hardware.device", "config.files.vmPathName", "config.annotation",
                              "config.hardware.numCPU", "config.hardware.memoryMB"}) {
        propSet.append_child("pathSet").text() = path;
    }
    pugi::xml_node objectSet = specSet.append_child("objectSet");
    pugi::xml_node obj = objectSet.append_child("obj");
    obj.append_attribute("type") = vm.type.c_str();
    obj.text() = vm.value.c_str();
    op.append_child("options");

    bool found = false;
    if (Error retrieved = retrievePropertiesPaged(m_session, op,
                                                         [&](pugi::xml_node objectContent) {
                                                             found = true;
                                                             parseObjectContentInto(objectContent, info);
                                                         });
        !retrieved.ok()) {
        return retrieved;
    }

    if (!found) {
        return Error(ErrorCode::InvalidArgument, "no such VirtualMachine: " + vm.value);
    }
    resolveDistributedPortgroupDetails(m_session, info.networkAdapters);
    return info;
}

namespace {
struct DatacenterRef {
    std::string name;
    MoRef moref;
};
} // namespace

/// Shared upward walk backing both `resolveDatacenterName` and `resolveDatacenterMoRef`: `entity`'s
/// `parent` chain, followed until a `Datacenter` is reached, capturing both its name and its own MoRef
/// in the same pass so neither public accessor needs a second round trip.
static Result<DatacenterRef> walkToDatacenter(VimSession& session, const MoRef& entity)
{
    MoRef current = entity;

    for (int hop = 0; hop < 32; ++hop) {
        pugi::xml_document request = makeVimRequest("RetrievePropertiesEx", session.propertyCollector());
        pugi::xml_node op = request.document_element();
        pugi::xml_node specSet = op.append_child("specSet");
        pugi::xml_node propSet = specSet.append_child("propSet");
        // "ManagedEntity" (rather than current.type) so this one query shape works at every hop
        // regardless of whether current is a Folder, Datacenter, or anything else in between: both name
        // and parent are ManagedEntity properties.
        propSet.append_child("type").text() = "ManagedEntity";
        propSet.append_child("all").text() = "false";
        propSet.append_child("pathSet").text() = "name";
        propSet.append_child("pathSet").text() = "parent";
        pugi::xml_node objectSet = specSet.append_child("objectSet");
        pugi::xml_node obj = objectSet.append_child("obj");
        obj.append_attribute("type") = current.type.c_str();
        obj.text() = current.value.c_str();
        op.append_child("options");

        auto callResult = session.call("urn:vim25/RetrievePropertiesEx", op);
        if (!callResult) {
            return std::move(callResult).error();
        }

        const pugi::xml_node objectContent = returnval(soapBodyContent(callResult.value())).child("objects");
        if (!objectContent) {
            return Error(ErrorCode::InvalidFormat,
                         "could not retrieve name/parent for " + current.type + " " + current.value);
        }

        std::string name;
        MoRef parent;
        for (pugi::xml_node prop = objectContent.child("propSet"); prop; prop = prop.next_sibling("propSet")) {
            const std::string propName = prop.child("name").text().as_string();
            const pugi::xml_node val = prop.child("val");
            if (propName == "name") {
                name = val.text().as_string();
            } else if (propName == "parent") {
                parent = parseMoRef(val);
            }
        }

        if (current.type == "Datacenter") {
            return DatacenterRef{std::move(name), current};
        }
        if (parent.empty()) {
            break;
        }
        current = parent;
    }

    return Error(ErrorCode::InvalidFormat,
                 "could not resolve a containing Datacenter for " + entity.type + " " + entity.value);
}

Result<std::string> VimInventory::resolveDatacenterName(const MoRef& entity)
{
    Result<DatacenterRef> result = walkToDatacenter(m_session, entity);
    if (!result) {
        return std::move(result).error();
    }
    return std::move(result).value().name;
}

Result<MoRef> VimInventory::resolveDatacenterMoRef(const MoRef& entity)
{
    Result<DatacenterRef> result = walkToDatacenter(m_session, entity);
    if (!result) {
        return std::move(result).error();
    }
    return std::move(result).value().moref;
}

Result<std::vector<VimObjectProperties>> VimInventory::listObjects(const std::string& type,
                                                                    const std::vector<std::string>& pathSet)
{
    if (m_session.viewManager().empty()) {
        return Error(ErrorCode::InvalidFormat, "RetrieveServiceContent response has no viewManager");
    }

    pugi::xml_document createRequest = makeVimRequest("CreateContainerView", m_session.viewManager());
    pugi::xml_node createOp = createRequest.document_element();
    pugi::xml_node container = createOp.append_child("container");
    container.append_attribute("type") = m_session.rootFolder().type.c_str();
    container.text() = m_session.rootFolder().value.c_str();
    createOp.append_child("type").text() = type.c_str();
    createOp.append_child("recursive").text() = "true";

    auto createResult = m_session.call("urn:vim25/CreateContainerView", createOp);
    if (!createResult) {
        return std::move(createResult).error();
    }
    const MoRef view = parseMoRef(returnval(soapBodyContent(createResult.value())));
    if (view.empty()) {
        return Error(ErrorCode::InvalidFormat, "CreateContainerView response missing <returnval>");
    }

    pugi::xml_document request = makeVimRequest("RetrievePropertiesEx", m_session.propertyCollector());
    pugi::xml_node op = request.document_element();
    // The selectSet's TraversalSpec needs an xsi:type; see DiskAttach.cpp's reconfigVm() for why
    // the namespace is declared on the operation.
    op.append_attribute("xmlns:xsi") = "http://www.w3.org/2001/XMLSchema-instance";
    pugi::xml_node specSet = op.append_child("specSet");
    pugi::xml_node propSet = specSet.append_child("propSet");
    propSet.append_child("type").text() = type.c_str();
    propSet.append_child("all").text() = "false";
    for (const std::string& path : pathSet) {
        propSet.append_child("pathSet").text() = path.c_str();
    }
    pugi::xml_node objectSet = specSet.append_child("objectSet");
    pugi::xml_node obj = objectSet.append_child("obj");
    obj.append_attribute("type") = view.type.c_str();
    obj.text() = view.value.c_str();
    objectSet.append_child("skip").text() = "true";
    pugi::xml_node selectSet = objectSet.append_child("selectSet");
    selectSet.append_attribute("xsi:type") = "TraversalSpec";
    selectSet.append_child("name").text() = "traverseView";
    selectSet.append_child("type").text() = "ContainerView";
    selectSet.append_child("path").text() = "view";
    selectSet.append_child("skip").text() = "false";
    op.append_child("options");

    std::vector<VimObjectProperties> objects;
    Error retrieved = retrievePropertiesPaged(m_session, op, [&](pugi::xml_node objectContent) {
        VimObjectProperties object;
        object.moref = parseMoRef(objectContent.child("obj"));
        for (pugi::xml_node prop = objectContent.child("propSet"); prop; prop = prop.next_sibling("propSet")) {
            object.properties.emplace_back(prop.child("name").text().as_string(),
                                           flattenPropertyValue(prop.child("val")));
        }
        if (!object.moref.empty()) {
            objects.push_back(std::move(object));
        }
    });

    // Best-effort, like VimSession's Logout: a leftover view goes away with the session anyway.
    const pugi::xml_document destroyRequest = makeVimRequest("DestroyView", view);
    (void)m_session.call("urn:vim25/DestroyView", destroyRequest.document_element());

    if (!retrieved.ok()) {
        return retrieved;
    }
    return objects;
}

} // namespace VSphereSoap::Vim
