// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Attaching an uploaded disk to an existing vSphere VM

#include "DiskAttach.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <string_view>
#include <thread>

#include "SoapLog.hpp"
#include "TaskWait.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

namespace {

// Short CLI-facing controller names <-> the VIM xsi:type each one actually instantiates.
// "sata" maps to VirtualAHCIController, not VirtualSATAController: the latter has no concrete device
// class in the real API (VimInventory::isControllerDeviceType still recognizes both spellings when
// *parsing*, in case a future/other server ever reports the abstract type directly, but this codebase
// only ever *creates* AHCI).
//
constexpr std::pair<std::string_view, std::string_view> kControllerTypeAliases[] = {
    {"lsilogic", "VirtualLsiLogicController"},
    {"buslogic", "VirtualBusLogicController"},
    {"lsilogicsas", "VirtualLsiLogicSASController"},
    {"paravirtual", "ParaVirtualSCSIController"},
    {"sata", "VirtualAHCIController"},
    {"nvme", "VirtualNVMEController"},
    {"ide", "VirtualIDEController"},
};

std::string xsiTypeForShortName(const std::string& shortName)
{
    for (const auto& [alias, xsiType] : kControllerTypeAliases) {
        if (alias == shortName) {
            return std::string(xsiType);
        }
    }
    return {};
}

bool isScsiFamily(std::string_view xsiType)
{
    return xsiType == "VirtualLsiLogicController" || xsiType == "VirtualBusLogicController" ||
           xsiType == "VirtualLsiLogicSASController" || xsiType == "ParaVirtualSCSIController";
}

// Per-controller-family valid unit range and reserved units.
// SCSI-family controllers reserve unit 7 for the controller device itself (a real, long-standing VIM
// convention: confirmed by every SCSI controller's own `scsiCtlrUnitNumber` default of 7); the others
// don't reserve anything.
// Not independently verified against a live server for the exact upper bound of each family; these are
// the documented VIM schema maximums.
//
bool isUnitUsable(std::string_view xsiType, int unit)
{
    if (isScsiFamily(xsiType)) {
        return unit >= 0 && unit <= 15 && unit != 7;
    }
    if (xsiType == "VirtualAHCIController" || xsiType == "VirtualSATAController") {
        return unit >= 0 && unit <= 29;
    }
    if (xsiType == "VirtualNVMEController") {
        return unit >= 0 && unit <= 14;
    }
    if (xsiType == "VirtualIDEController") {
        return unit == 0 || unit == 1;
    }
    return unit >= 0 && unit <= 15;
}

} // namespace

Result<ControllerChoice> chooseControllerAndUnit(const VimVmInfo& target, const ControllerSelector& selector)
{
    const std::string wantedXsiType =
        selector.controllerType.empty() ? std::string() : xsiTypeForShortName(selector.controllerType);
    if (!selector.controllerType.empty() && wantedXsiType.empty()) {
        return Error(ErrorCode::InvalidArgument, "unknown controller type '" + selector.controllerType +
                                                      "' (expected one of lsilogic, buslogic, lsilogicsas, "
                                                      "paravirtual, sata, nvme, ide)");
    }

    auto pickFreeUnit = [](const VimControllerInfo& controller) -> std::optional<int> {
        for (int unit = 0; unit <= 30; ++unit) {
            if (!isUnitUsable(controller.deviceType, unit)) {
                continue;
            }
            if (std::find(controller.usedUnitNumbers.begin(), controller.usedUnitNumbers.end(), unit) ==
                controller.usedUnitNumbers.end()) {
                return unit;
            }
        }
        return std::nullopt;
    };

    // Case 1: an explicit bus number was given; find that exact controller (optionally also matching
    // a requested type).
    if (selector.busNumber) {
        const auto it = std::find_if(target.controllers.begin(), target.controllers.end(),
                                       [&](const VimControllerInfo& c) {
                                           return c.busNumber == *selector.busNumber &&
                                                  (wantedXsiType.empty() || c.deviceType == wantedXsiType);
                                       });
        if (it != target.controllers.end()) {
            const auto unit = pickFreeUnit(*it);
            if (!unit) {
                return Error(ErrorCode::InvalidArgument,
                             "controller " + it->deviceType + " bus " + std::to_string(it->busNumber) +
                                 " on target VM has no free unit numbers left");
            }
            return ControllerChoice{it->key, *unit, std::nullopt};
        }
        if (!selector.createIfMissing) {
            return Error(ErrorCode::InvalidArgument,
                         "target VM has no controller at bus " + std::to_string(*selector.busNumber) +
                             (selector.controllerType.empty() ? "" : (" of type " + selector.controllerType)) +
                             " (set createIfMissing to add one)");
        }
        const std::string createType = wantedXsiType.empty() ? "ParaVirtualSCSIController" : wantedXsiType;
        return ControllerChoice{0, 0, std::make_pair(createType, *selector.busNumber)};
    }

    // Case 2: no bus given, but a type was; use that type's lowest-bus controller if one exists.
    if (!wantedXsiType.empty()) {
        const VimControllerInfo* best = nullptr;
        for (const VimControllerInfo& c : target.controllers) {
            if (c.deviceType == wantedXsiType && (best == nullptr || c.busNumber < best->busNumber)) {
                best = &c;
            }
        }
        if (best != nullptr) {
            const auto unit = pickFreeUnit(*best);
            if (!unit) {
                return Error(ErrorCode::InvalidArgument, "controller " + best->deviceType + " bus " +
                                                              std::to_string(best->busNumber) +
                                                              " on target VM has no free unit numbers left");
            }
            return ControllerChoice{best->key, *unit, std::nullopt};
        }
        if (!selector.createIfMissing) {
            return Error(ErrorCode::InvalidArgument, "target VM has no " + selector.controllerType +
                                                           " controller (set createIfMissing to add one)");
        }
        return ControllerChoice{0, 0, std::make_pair(wantedXsiType, 0)};
    }

    // Case 3: fully automatic; default to the target's existing primary controller, defined as the type
    // most of its existing disks are already attached to (ties broken by lowest controller key).
    // Falls back to the lowest-key controller of any type if the VM has disks with no resolvable
    // controller (shouldn't happen for a well-formed VM), or the lowest-key controller overall if it has
    // no disks yet.
    //
    if (target.controllers.empty()) {
        if (!selector.createIfMissing) {
            return Error(ErrorCode::InvalidArgument,
                         "target VM has no disk controller at all (set createIfMissing to add one)");
        }
        return ControllerChoice{0, 0, std::make_pair(std::string("ParaVirtualSCSIController"), 0)};
    }

    std::map<int, int> diskCountByControllerKey; // controllerKey -> how many disks use it
    for (const VimDiskInfo& disk : target.disks) {
        ++diskCountByControllerKey[disk.controllerKey];
    }

    const VimControllerInfo* primary = nullptr;
    int primaryDiskCount = -1;
    for (const VimControllerInfo& c : target.controllers) {
        const int count = diskCountByControllerKey.count(c.key) ? diskCountByControllerKey[c.key] : 0;
        if (count > primaryDiskCount || (count == primaryDiskCount && (primary == nullptr || c.key < primary->key))) {
            primary = &c;
            primaryDiskCount = count;
        }
    }
    // primary is never null here: target.controllers is non-empty (checked above) and the loop always
    // assigns it on the first iteration.
    const auto unit = pickFreeUnit(*primary);
    if (!unit) {
        return Error(ErrorCode::InvalidArgument, "target VM's primary controller (" + primary->deviceType + " bus " +
                                                       std::to_string(primary->busNumber) +
                                                       ") has no free unit numbers left; name a different controller "
                                                       "by type or bus number");
    }
    return ControllerChoice{primary->key, *unit, std::nullopt};
}

namespace {

/// `ReconfigVM_Task` with one `deviceChange`: shared shape for every add/remove call this file makes.
/// `deviceXml` is a caller-built callback so each call site controls exactly which child elements
/// `<device>` gets (remove needs far fewer than add).
Result<MoRef> reconfigVm(VimSession& session, const MoRef& vm, const char* operation,
                          const std::function<void(pugi::xml_node)>& buildDevice)
{
    pugi::xml_document request = makeVimRequest("ReconfigVM_Task", vm);
    pugi::xml_node op = request.document_element();
    // Required for the device's own xsi:type attribute below (VirtualDisk is a polymorphic VirtualDevice
    // subtype, conveyed the standard XML Schema way); confirmed live: vCenter rejects this call with
    // InvalidRequestFault ("undefined namespace prefix xsi") without it.
    // Matches HttpNfcLeaseSession.cpp's own ImportVApp request, which needs the same declaration for
    // the same reason (its own xsi:type comes from a copied CreateImportSpec response node instead of
    // a literal here, but the requirement is identical).
    //
    op.append_attribute("xmlns:xsi") = "http://www.w3.org/2001/XMLSchema-instance";
    pugi::xml_node spec = op.append_child("spec");
    pugi::xml_node deviceChange = spec.append_child("deviceChange");
    deviceChange.append_child("operation").text() = operation;
    pugi::xml_node device = deviceChange.append_child("device");
    device.append_attribute("xsi:type") = "VirtualDisk";
    buildDevice(device);

    auto callResult = session.call(op);
    if (!callResult) {
        return std::move(callResult).error();
    }
    const MoRef task = parseMoRef(soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "ReconfigVM_Task response missing task MoRef");
    }
    return task;
}

Error reconfigRemoveDisk(VimSession& session, const MoRef& vm, const VimDiskInfo& disk)
{
    logger()->info("detaching disk key={} from placeholder VM {} (keeping its backing file)", disk.key, vm.value);
    Result<MoRef> task = reconfigVm(session, vm, "remove", [&](pugi::xml_node device) {
        // Element order matters: VIM's XML Schema declares VirtualDevice/VirtualDisk as an ordered
        // <sequence>, and a real vCenter rejects an out-of-order element with "Unexpected element tag
        // ... seen" rather than silently accepting it reordered.
        // Confirmed live: this exact order (key, backing, controllerKey, unitNumber, capacityInKB) is
        // required; `backing` comes from the VirtualDevice base type, ahead of controllerKey/unitNumber,
        // and capacityInKB is a VirtualDisk-specific field that comes after.
        //
        device.append_child("key").text() = disk.key.c_str();
        pugi::xml_node backing = device.append_child("backing");
        backing.append_attribute("xsi:type") = disk.backingType.empty() ? "VirtualDiskFlatVer2BackingInfo"
                                                                          : disk.backingType.c_str();
        backing.append_child("fileName").text() = disk.rawFileName.c_str();
        // Required by the same strict schema validation as capacityInKB below: reuse whatever diskMode
        // the placeholder's disk already has (per its own describeVm() read) rather than assuming
        // a literal, since this is a remove, not a fresh disk.
        backing.append_child("diskMode").text() = disk.diskMode.empty() ? "persistent" : disk.diskMode.c_str();
        device.append_child("controllerKey").text() = disk.controllerKey;
        device.append_child("unitNumber").text() = disk.unitNumber;
        // Confirmed live: a real vCenter's schema validation requires capacityInKB on a VirtualDisk
        // device spec even for operation="remove"; a full, schema-valid device, not just enough fields
        // to identify which one to remove by key.
        device.append_child("capacityInKB").text() = static_cast<long long>(disk.capacityBytes / 1024);
    });
    if (!task) {
        return std::move(task).error();
    }
    Result<MoRef> waited = waitForVimTask(session, task.value());
    if (!waited) {
        return std::move(waited).error();
    }
    return {};
}

Error reconfigAddDisk(VimSession& session, const MoRef& vm, int controllerKey, int unitNumber,
                              std::uint64_t capacityBytes, const std::string& datastorePath,
                              const std::string& diskProvisioning)
{
    // Empty means "thin" (matches every caller before this parameter existed); anything else must be one
    // of vSphere's own three `diskProvisioning` wire values.
    const bool thin = diskProvisioning.empty() || diskProvisioning == "thin";
    const bool eager = diskProvisioning == "eagerZeroedThick";
    logger()->info("attaching {} to target VM {} (controllerKey={}, unitNumber={}, thinProvisioned={}, "
                    "eagerlyScrub={})",
                    datastorePath, vm.value, controllerKey, unitNumber, thin, eager);
    Result<MoRef> task = reconfigVm(session, vm, "add", [&](pugi::xml_node device) {
        // Element order matters: see reconfigRemoveDisk()'s comment on the same requirement.
        // Negative keys are the standard VIM convention for "a new device in this same reconfig",
        // distinguishing it from any real (positive, server-assigned) device key already present
        // elsewhere in the same request.
        //
        device.append_child("key").text() = -100;
        pugi::xml_node backing = device.append_child("backing");
        backing.append_attribute("xsi:type") = "VirtualDiskFlatVer2BackingInfo";
        backing.append_child("fileName").text() = datastorePath.c_str();
        backing.append_child("diskMode").text() = "persistent";
        backing.append_child("thinProvisioned").text() = thin ? "true" : "false";
        // eagerlyScrub matters only for a non-thin disk (a thin disk is never eager-zeroed by
        // definition; it has no physically allocated blocks to have scrubbed yet); reporting it
        // accurately for the eagerZeroedThick case, rather than always "false", is exactly what
        // distinguishes "actually zeroed on the datastore side" from silently mislabeling it thin/lazy.
        //
        backing.append_child("eagerlyScrub").text() = eager ? "true" : "false";
        device.append_child("controllerKey").text() = controllerKey;
        device.append_child("unitNumber").text() = unitNumber;
        // capacityInKB (not capacityInBytes) for the request: universally accepted across schema
        // versions, unlike capacityInBytes which is newer; matches how parseDevices() above already
        // tolerates either on the *read* side. capacityBytes is always a multiple of the encoder's 64
        // KiB grain size (StreamOptimizedVmdkEncoder::create's own precondition), hence always exactly
        // divisible here.
        //
        device.append_child("capacityInKB").text() = static_cast<long long>(capacityBytes / 1024);
    });
    if (!task) {
        return std::move(task).error();
    }
    Result<MoRef> waited = waitForVimTask(session, task.value());
    if (!waited) {
        return std::move(waited).error();
    }
    return {};
}

/// Adds a new controller device (`operation=add`, no `key`/`unitNumber`; those are VirtualDisk-only
/// concepts) and returns its server-assigned key, found by re-describing the VM afterward and matching
/// on `deviceType`+`busNumber` (the two values this call itself requested: the only thing
/// `ReconfigVM_Task`'s own task result doesn't hand back directly).
/// Not verified whether a controller-add and a disk-add can be combined into one `ReconfigVM_Task` call,
/// so this deliberately runs as two sequential reconfigs (this one, then the caller's own disk-add)
/// rather than one.
///
Result<int> reconfigAddController(VimSession& session, VimInventory& inventory, const MoRef& vm,
                                    const std::string& xsiType, int busNumber)
{
    logger()->info("target VM {} has no usable controller; adding a new {} at bus {}", vm.value, xsiType, busNumber);
    pugi::xml_document request = makeVimRequest("ReconfigVM_Task", vm);
    pugi::xml_node op = request.document_element();
    // see reconfigVm()'s own comment
    op.append_attribute("xmlns:xsi") = "http://www.w3.org/2001/XMLSchema-instance";
    pugi::xml_node spec = op.append_child("spec");
    pugi::xml_node deviceChange = spec.append_child("deviceChange");
    deviceChange.append_child("operation").text() = "add";
    pugi::xml_node device = deviceChange.append_child("device");
    device.append_attribute("xsi:type") = xsiType.c_str();
    device.append_child("key").text() = -101;
    device.append_child("busNumber").text() = busNumber;
    // sharedBus is SCSI-family-only (VirtualSCSIController's own schema field); confirmed live that it
    // is *not* harmless to include unconditionally as originally assumed: a real vCenter rejects
    // VirtualAHCIController (and, by the same schema logic, presumably
    // VirtualNVMEController/VirtualIDEController too) with "Unexpected element tag 'sharedBus' seen" if
    // it's present at all.
    // "noSharing" is the correct default for a controller that will only ever back one VM's own disks
    // (this codebase never creates a clustered/shared-disk setup) when it does apply.
    //
    if (isScsiFamily(xsiType)) {
        device.append_child("sharedBus").text() = "noSharing";
    }

    auto callResult = session.call(op);
    if (!callResult) {
        return std::move(callResult).error();
    }
    const MoRef task = parseMoRef(soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "ReconfigVM_Task (add controller) response missing task MoRef");
    }
    if (auto waited = waitForVimTask(session, task); !waited) {
        return std::move(waited).error();
    }

    Result<VimVmInfo> refreshed = inventory.describeVm(vm);
    if (!refreshed) {
        return std::move(refreshed).error();
    }
    for (const VimControllerInfo& c : refreshed.value().controllers) {
        if (c.deviceType == xsiType && c.busNumber == busNumber) {
            return c.key;
        }
    }
    return Error(ErrorCode::InvalidFormat,
                 "ReconfigVM_Task (add controller) succeeded but no matching controller was found afterward");
}

/// FileManager.MoveDatastoreFile_Task: moves one file as-is; a plain rename when source and destination
/// are on the same datastore.
/// On a .vmdk it moves only the descriptor, never the extent it references (see moveUploadedDisk()).
Error moveDatastoreFile(VimSession& session, const std::string& sourceName, const MoRef& sourceDc,
                                 const std::string& destinationName, const MoRef& destinationDc)
{
    logger()->info("moving {} -> {}", sourceName, destinationName);
    pugi::xml_document request = makeVimRequest("MoveDatastoreFile_Task", session.fileManager());
    pugi::xml_node op = request.document_element();
    op.append_child("sourceName").text() = sourceName.c_str();
    if (!sourceDc.empty()) {
        pugi::xml_node node = op.append_child("sourceDatacenter");
        node.append_attribute("type") = sourceDc.type.c_str();
        node.text() = sourceDc.value.c_str();
    }
    op.append_child("destinationName").text() = destinationName.c_str();
    if (!destinationDc.empty()) {
        pugi::xml_node node = op.append_child("destinationDatacenter");
        node.append_attribute("type") = destinationDc.type.c_str();
        node.text() = destinationDc.value.c_str();
    }
    op.append_child("force").text() = "false";

    auto callResult = session.call(op);
    if (!callResult) {
        return std::move(callResult).error();
    }
    const MoRef task = parseMoRef(soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "MoveDatastoreFile_Task response missing task MoRef");
    }
    Result<MoRef> waited = waitForVimTask(session, task);
    if (!waited) {
        return std::move(waited).error();
    }
    return {};
}

/// Moves a virtual disk (its descriptor *and* its extent file(s)) via
/// VirtualDiskManager.MoveVirtualDisk_Task.
/// Works across datastores, but physically rewrites the disk: a lazy-zeroed thick disk comes out
/// eager-zeroed (confirmed live), and the time taken grows with disk size.
///
Error moveVirtualDisk(VimSession& session, const std::string& sourceName, const MoRef& sourceDc,
                               const std::string& destinationName, const MoRef& destinationDc)
{
    logger()->info("moving virtual disk {} -> {}", sourceName, destinationName);
    pugi::xml_document request = makeVimRequest("MoveVirtualDisk_Task", session.virtualDiskManager());
    pugi::xml_node op = request.document_element();
    op.append_child("sourceName").text() = sourceName.c_str();
    if (!sourceDc.empty()) {
        pugi::xml_node node = op.append_child("sourceDatacenter");
        node.append_attribute("type") = sourceDc.type.c_str();
        node.text() = sourceDc.value.c_str();
    }
    op.append_child("destName").text() = destinationName.c_str();
    if (!destinationDc.empty()) {
        pugi::xml_node node = op.append_child("destDatacenter");
        node.append_attribute("type") = destinationDc.type.c_str();
        node.text() = destinationDc.value.c_str();
    }
    op.append_child("force").text() = "false";

    auto callResult = session.call(op);
    if (!callResult) {
        return std::move(callResult).error();
    }
    const MoRef task = parseMoRef(soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "MoveVirtualDisk_Task response missing task MoRef");
    }
    Result<MoRef> waited = waitForVimTask(session, task);
    if (!waited) {
        return std::move(waited).error();
    }
    return {};
}

/// Moves an uploaded (placeholder-owned) disk to `destinationPath` without changing its provisioning.
/// On the same datastore: the normal case, since the placeholder is always created on the disk's own
/// target datastore: the extent and then the descriptor are *renamed* (FileManager), which keeps every
/// allocation property, including a lazy-zeroed disk's not-yet-zeroed blocks, and is instant regardless
/// of size.
/// The extent keeps its own file name: the descriptor references it by that name, relative to
/// the descriptor's folder.
/// ImportVApp always names it "<descriptor>-flat.vmdk"; should that ever not hold, the extent rename
/// fails before anything has moved and this falls back to MoveVirtualDisk_Task (correct, but rewrites
/// the disk).
/// Across datastores, MoveVirtualDisk_Task is the only option.
///
Error moveUploadedDisk(VimSession& session, const std::string& sourcePath, const MoRef& sourceDc,
                                const std::string& destinationPath, const MoRef& destinationDc)
{
    const auto source = parseDatastorePath(sourcePath);
    const auto destination = parseDatastorePath(destinationPath);
    const auto slash = [](const std::string& relative) { return relative.find_last_of('/'); };
    if (!source || !destination || source->first != destination->first ||
        source->second.size() < 5 || source->second.compare(source->second.size() - 5, 5, ".vmdk") != 0) {
        return moveVirtualDisk(session, sourcePath, sourceDc, destinationPath, destinationDc);
    }

    const std::string& datastore = source->first;
    const auto sourceSlash = slash(source->second);
    const auto destinationSlash = slash(destination->second);
    const std::string sourceDir = sourceSlash == std::string::npos ? std::string() : source->second.substr(0, sourceSlash + 1);
    const std::string destinationDir =
        destinationSlash == std::string::npos ? std::string() : destination->second.substr(0, destinationSlash + 1);
    const std::string sourceBase =
        // no ".vmdk"
        source->second.substr(sourceDir.size(), source->second.size() - sourceDir.size() - 5);
    const std::string extentName = sourceBase + "-flat.vmdk";

    if (!destinationDir.empty()) {
        // Idempotent in effect: fails harmlessly when the folder already exists (e.g. the target VM's
        // own), and a genuinely missing one is then reported by the move itself.
        (void)makeDatastoreDirectory(session, "[" + datastore + "] " + destinationDir, destinationDc);
    }
    const std::string sourceExtent = "[" + datastore + "] " + sourceDir + extentName;
    const std::string destinationExtent = "[" + datastore + "] " + destinationDir + extentName;
    if (auto moved = moveDatastoreFile(session, sourceExtent, sourceDc, destinationExtent, destinationDc);
        !moved.ok()) {
        logger()->info("renaming extent {} failed ({}); moving the disk with MoveVirtualDisk_Task instead",
                        sourceExtent, moved.toString());
        return moveVirtualDisk(session, sourcePath, sourceDc, destinationPath, destinationDc);
    }
    if (auto moved = moveDatastoreFile(session, sourcePath, sourceDc, destinationPath, destinationDc); !moved.ok()) {
        // Put the extent back so the disk stays usable at its original path.
        (void)moveDatastoreFile(session, destinationExtent, destinationDc, sourceExtent, sourceDc);
        return moved;
    }
    return {};
}

} // namespace

Error deleteDatastoreFile(VimSession& session, const std::string& name, const MoRef& datacenter)
{
    logger()->info("deleting datastore file {}", name);
    pugi::xml_document request = makeVimRequest("DeleteDatastoreFile_Task", session.fileManager());
    pugi::xml_node op = request.document_element();
    op.append_child("name").text() = name.c_str();
    if (!datacenter.empty()) {
        pugi::xml_node node = op.append_child("datacenter");
        node.append_attribute("type") = datacenter.type.c_str();
        node.text() = datacenter.value.c_str();
    }

    auto callResult = session.call(op);
    if (!callResult) {
        return std::move(callResult).error();
    }
    const MoRef task = parseMoRef(soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "DeleteDatastoreFile_Task response missing task MoRef");
    }
    Result<MoRef> waited = waitForVimTask(session, task);
    if (!waited) {
        return std::move(waited).error();
    }
    return {};
}

Error makeDatastoreDirectory(VimSession& session, const std::string& name, const MoRef& datacenter)
{
    logger()->info("creating datastore directory {}", name);
    pugi::xml_document request = makeVimRequest("MakeDirectory", session.fileManager());
    pugi::xml_node op = request.document_element();
    op.append_child("name").text() = name.c_str();
    if (!datacenter.empty()) {
        pugi::xml_node node = op.append_child("datacenter");
        node.append_attribute("type") = datacenter.type.c_str();
        node.text() = datacenter.value.c_str();
    }
    op.append_child("createParentDirectories").text() = "true";

    auto callResult = session.call(op);
    if (!callResult) {
        return std::move(callResult).error();
    }
    return {}; // synchronous: no task to wait for, see this function's own doc comment.
}

Error eagerZeroVirtualDisk(VimSession& session, const std::string& name, const MoRef& datacenter)
{
    logger()->info("eager-zeroing {} (forcing physical zero-fill to match the requested eagerZeroedThick "
                    "provisioning)", name);

    // VirtualDiskManager's own datastore-file index lags briefly behind a file that another subsystem
    // (FileManager's MoveDatastoreFile_Task, in this codebase's only caller) just moved into place;
    // confirmed live: an immediate EagerZeroVirtualDisk_Task on a path that every other operation
    // already treats as real fails with "File ... was not found", but the exact same call against
    // the exact same path succeeds a moment later.
    // Retried with capped exponential backoff, the same shape TaskWait.cpp's own polling already uses
    // for a conceptually identical "not ready yet, not actually wrong" wait.
    //
    constexpr int kMaxAttempts = 6;
    auto backoff = std::chrono::milliseconds(500);
    constexpr auto kMaxBackoff = std::chrono::milliseconds(5000);

    for (int attempt = 1;; ++attempt) {
        pugi::xml_document request = makeVimRequest("EagerZeroVirtualDisk_Task", session.virtualDiskManager());
        pugi::xml_node op = request.document_element();
        op.append_child("name").text() = name.c_str();
        if (!datacenter.empty()) {
            pugi::xml_node node = op.append_child("datacenter");
            node.append_attribute("type") = datacenter.type.c_str();
            node.text() = datacenter.value.c_str();
        }

        Error outcome = [&]() -> Error {
            auto callResult = session.call(op);
            if (!callResult) {
                return std::move(callResult).error();
            }
            const MoRef task = parseMoRef(soapBodyContent(callResult.value()).child("returnval"));
            if (task.empty()) {
                return Error(ErrorCode::InvalidFormat, "EagerZeroVirtualDisk_Task response missing task MoRef");
            }
            Result<MoRef> waited = waitForVimTask(session, task);
            if (!waited) {
                return std::move(waited).error();
            }
            return {};
        }();

        if (outcome.ok() || attempt >= kMaxAttempts ||
            outcome.message().find("was not found") == std::string::npos) {
            return outcome;
        }
        logger()->debug("EagerZeroVirtualDisk_Task on {} reported the file was not found (attempt {}/{}); retrying "
                         "after {}ms -- likely a brief datastore-index propagation lag, not a real absence",
                         name, attempt, kMaxAttempts, backoff.count());
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, kMaxBackoff);
    }
}

namespace {

Error unregisterVm(VimSession& session, const MoRef& vm)
{
    logger()->info("unregistering placeholder VM {} (files left in place)", vm.value);
    const pugi::xml_document request = makeVimRequest("UnregisterVM", vm);

    // UnregisterVM is a plain (non-Task) operation; it either faults or returns synchronously, no task to poll.
    auto callResult = session.call(request.document_element());
    if (!callResult) {
        return std::move(callResult).error();
    }
    return {};
}

} // namespace

Result<DiskAttachResult> attachUploadedDisk(VimSession& session, const DiskAttachSpec& spec)
{
    VimInventory inventory(session);

    Result<VimVmInfo> placeholderInfo = inventory.describeVm(spec.placeholderVm);
    if (!placeholderInfo) {
        return std::move(placeholderInfo).error();
    }
    if (placeholderInfo.value().disks.size() != 1) {
        return Error(ErrorCode::InvalidArgument, "placeholder VM " + spec.placeholderVm.value + " has " +
                                                       std::to_string(placeholderInfo.value().disks.size()) +
                                                       " disk(s), expected exactly 1");
    }
    const VimDiskInfo uploadedDisk = placeholderInfo.value().disks.front();

    // Without a target VM the disk is only moved out, to desiredDatastoreDir, and stays unattached.
    const bool attach = !spec.targetVm.empty();
    if (!attach && spec.desiredDatastoreDir.empty()) {
        return Error(ErrorCode::InvalidArgument, "a disk that is not attached needs a destination directory");
    }
    VimVmInfo targetInfo;
    if (attach) {
        Result<VimVmInfo> targetInfoBefore = inventory.describeVm(spec.targetVm);
        if (!targetInfoBefore) {
            return std::move(targetInfoBefore).error();
        }
        targetInfo = std::move(targetInfoBefore).value();
        if (targetInfo.powerState != VimPowerState::PoweredOff) {
            return Error(ErrorCode::InvalidArgument,
                         "target VM " + spec.targetVm.value +
                             " must be powered off to attach a disk (hot-add is not supported by this pass)");
        }
        if (targetInfo.vmPathName.empty()) {
            return Error(ErrorCode::InvalidFormat, "target VM " + spec.targetVm.value + " has no config.files.vmPathName");
        }
    }

    // --- detach from the placeholder (file stays where it is for now) ---
    if (auto detached = reconfigRemoveDisk(session, spec.placeholderVm, uploadedDisk); !detached.ok()) {
        return Error(ErrorCode::IoError, "failed to detach uploaded disk from placeholder VM " +
                                              spec.placeholderVm.value + " (placeholder left in place for manual "
                                              "recovery): " +
                                              detached.message());
    }

    // --- eager-zero fixup, if requested, *before* the move below --------
    //
    // Must happen here, not after the move (or inside attachDiskFile's own pre-attach step, which runs
    // post-move for this call path): confirmed live that EagerZeroVirtualDisk_Task succeeds immediately
    // against the placeholder's original (pre-move) path, but fails with "File ... was not found"
    // against the exact same file at its post-move destination, every time, indefinitely (not a transient
    // propagation lag; see eagerZeroVirtualDisk's own retry loop, which doesn't help here either).
    // That was observed while the move below still used FileManager.MoveDatastoreFile_Task, which left
    // the disk's extent behind (see moveVirtualDisk()); zeroing before the move stays the safe order
    // regardless. finishFolderPutImport's own call to attachDiskFile has no such move (the file is
    // already at its final location), so that path's fixup stays where it is.
    //
    if (spec.diskProvisioning == "eagerZeroedThick") {
        Result<MoRef> placeholderDc = inventory.resolveDatacenterMoRef(spec.placeholderVm);
        if (!placeholderDc) {
            return Error(ErrorCode::IoError,
                         "disk detached from placeholder VM " + spec.placeholderVm.value +
                             " but its datacenter could not be resolved to force eager-zeroing before it's moved: " +
                             placeholderDc.error().message());
        }
        if (auto zeroed = eagerZeroVirtualDisk(session, uploadedDisk.rawFileName, placeholderDc.value());
            !zeroed.ok()) {
            return Error(ErrorCode::IoError, "disk detached from placeholder VM " + spec.placeholderVm.value +
                                                  " but forcing it eager-zeroed (before moving it) failed: " +
                                                  zeroed.message());
        }
    }

    const auto fileNameOf = [](const std::string& path) {
        const auto slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    };
    const auto dirOf = [](const std::string& path) {
        const auto slash = path.find_last_of('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    };

    // The uploaded disk's own filename is an implementation detail of the placeholder VM (vCenter names
    // it during ImportVApp, typically "<placeholder-vm-name>.vmdk"): a caller-supplied desiredFileName
    // takes precedence, per DiskAttachSpec::desiredFileName's own doc comment.
    // Confirmed live: the streamOptimized VMDK's *embedded* descriptor extent-filename text (set by
    // whoever built it, e.g. VSphereDatastoreBackendParams::vmdkDescriptor) does not itself drive
    // the on-datastore filename vCenter assigns, only this move step does, so a caller wanting
    // a specific final filename must go through desiredFileName, not the descriptor text.
    //
    std::string sourceFileName = fileNameOf(uploadedDisk.rawFileName);
    if (!spec.desiredFileName.empty()) {
        sourceFileName = spec.desiredFileName;
        if (sourceFileName.size() < 5 || sourceFileName.compare(sourceFileName.size() - 5, 5, ".vmdk") != 0) {
            sourceFileName += ".vmdk";
        }
    }

    // --- move the file into the target VM's own folder (or, when spec.desiredDatastoreDir is set, into
    // that caller-chosen datastore directory instead: see DiskAttachSpec::desiredDatastoreDir's own doc
    // comment) ---
    std::string destinationDatastore;
    // relative directory, no leading/trailing '/', possibly empty (datastore root)
    std::string destinationDir;
    if (spec.desiredDatastoreDir.empty()) {
        const auto targetDir = parseDatastorePath(targetInfo.vmPathName);
        if (!targetDir) {
            return Error(ErrorCode::InvalidFormat, "could not parse target VM's config.files.vmPathName: " +
                                                         targetInfo.vmPathName);
        }
        // targetDir->second is config.files.vmPathName's relative part, e.g.
        // "import-test-01/import-test-01.vmx": its *directory* is the target VM's own folder,
        // not the whole path (a real vCenter's MoveDatastoreFile_Task 404s with "File ....vmx/
        // was not found" if the .vmx filename is left in the destination directory, confirmed live).
        //
        destinationDatastore = targetDir->first;
        destinationDir = dirOf(targetDir->second);
    } else {
        if (spec.desiredDatastoreDir.front() != '[') {
            return Error(ErrorCode::InvalidFormat,
                         "DiskAttachSpec::m_desiredDatastoreDir must be of the form '[datastore] dir': " +
                             spec.desiredDatastoreDir);
        }
        const auto closeBracket = spec.desiredDatastoreDir.find(']');
        if (closeBracket == std::string::npos) {
            return Error(ErrorCode::InvalidFormat,
                         "DiskAttachSpec::m_desiredDatastoreDir must be of the form '[datastore] dir': " +
                             spec.desiredDatastoreDir);
        }
        destinationDatastore = spec.desiredDatastoreDir.substr(1, closeBracket - 1);
        std::string rest = spec.desiredDatastoreDir.substr(closeBracket + 1);
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        while (!rest.empty() && rest.back() == '/') rest.pop_back();
        destinationDir = rest;
    }
    const std::string destinationPath =
        "[" + destinationDatastore + "] " + (destinationDir.empty() ? sourceFileName : destinationDir + "/" + sourceFileName);

    Result<MoRef> sourceDc = inventory.resolveDatacenterMoRef(spec.placeholderVm);
    if (!sourceDc) {
        return Error(ErrorCode::IoError,
                     "disk detached from placeholder VM " + spec.placeholderVm.value +
                         " but its file (" + uploadedDisk.rawFileName +
                         ") could not be moved -- failed to resolve source datacenter: " + sourceDc.error().message());
    }
    Result<MoRef> destinationDc = attach ? inventory.resolveDatacenterMoRef(spec.targetVm) : sourceDc;
    if (!destinationDc) {
        return Error(ErrorCode::IoError,
                     "disk detached from placeholder VM " + spec.placeholderVm.value +
                         " but its file (" + uploadedDisk.rawFileName +
                         ") could not be moved -- failed to resolve destination datacenter: " +
                         destinationDc.error().message());
    }
    if (auto moved = moveUploadedDisk(session, uploadedDisk.rawFileName, sourceDc.value(), destinationPath,
                                       destinationDc.value());
        !moved.ok()) {
        return Error(ErrorCode::IoError, "disk detached from placeholder VM " + spec.placeholderVm.value +
                                              " but failed to move its file from " + uploadedDisk.rawFileName +
                                              " to " + destinationPath + " (file left at its original path for "
                                              "manual recovery): " +
                                              moved.message());
    }

    // --- unregister the (now disk-less) placeholder, then delete its folder (its .vmx/.nvram/log
    // files): best-effort both ---
    if (auto unregistered = unregisterVm(session, spec.placeholderVm); !unregistered.ok()) {
        logger()->warn("failed to unregister placeholder VM {} after moving its disk out (leaving it registered, "
                        "harmless but should be cleaned up manually): {}",
                        spec.placeholderVm.value, unregistered.toString());
    } else {
        // The uploaded disk lived in the placeholder's own folder, which ImportVApp created for it
        // alone.
        // Never the destination folder: that would delete the disk just moved into it.
        const std::string placeholderDir = dirOf(uploadedDisk.rawFileName);
        const std::string destinationDirPath = dirOf(destinationPath);
        if (!placeholderDir.empty() && placeholderDir.back() != ']' && placeholderDir != destinationDirPath) {
            if (auto removed = deleteDatastoreFile(session, placeholderDir, sourceDc.value()); !removed.ok()) {
                logger()->warn("failed to delete placeholder VM {}'s folder {} (harmless, but should be cleaned "
                                "up manually): {}",
                                spec.placeholderVm.value, placeholderDir, removed.toString());
            }
        }
    }

    if (!attach) {
        return DiskAttachResult{destinationPath, 0, 0};
    }

    // --- resolve controller/unit and attach on the target ---
    // needsEagerZeroFixup=false: already handled above, before the move.
    return attachDiskFile(session, spec.targetVm, destinationPath, uploadedDisk.capacityBytes, spec.controller,
                           spec.diskProvisioning, /*needsEagerZeroFixup=*/false);
}

Result<DiskAttachResult> attachDiskFile(VimSession& session, const MoRef& targetVm, const std::string& datastorePath,
                                          std::uint64_t capacityBytes, const ControllerSelector& controller,
                                          const std::string& diskProvisioning, bool needsEagerZeroFixup)
{
    VimInventory inventory(session);

    Result<VimVmInfo> targetInfo = inventory.describeVm(targetVm);
    if (!targetInfo) {
        return Error(ErrorCode::IoError, "disk file is at " + datastorePath +
                                              " but failed to read target VM before attaching: " +
                                              targetInfo.error().message());
    }
    Result<ControllerChoice> choice = chooseControllerAndUnit(targetInfo.value(), controller);
    if (!choice) {
        return Error(ErrorCode::IoError, "disk file is at " + datastorePath +
                                              " but no attach point could be resolved: " + choice.error().message());
    }

    int controllerKey = choice.value().controllerKey;
    int unitNumber = choice.value().unitNumber;
    if (choice.value().controllerToCreate) {
        const auto& [createType, createBus] = *choice.value().controllerToCreate;
        Result<int> newKey = reconfigAddController(session, inventory, targetVm, createType, createBus);
        if (!newKey) {
            return Error(ErrorCode::IoError, "disk file is at " + datastorePath + " but failed to create a " +
                                                  createType + " controller on the target VM: " +
                                                  newKey.error().message());
        }
        controllerKey = newKey.value();
        unitNumber = 0;
    }

    // --- pre-attach provisioning fixup ----------------------------------
    //
    // HttpNfcLease's own diskProvisioning (see VSphereImportVmSpec's doc comment) is accepted without
    // error by every ESXi/vCenter version this codebase has been tested against, but confirmed live NOT
    // to be reliably honored for "eagerZeroedThick": a real vCenter produced a correctly
    // lazy-zeroed-thick disk for "thick", but silently left an "eagerZeroedThick" request non-eager.
    // EagerZeroVirtualDisk_Task fixes this deterministically; but only while `datastorePath` has never
    // been through a `MoveDatastoreFile_Task` (confirmed live: EagerZeroVirtualDisk_Task fails with
    // "File ... was not found" against a path that FileManager.MoveDatastoreFile_Task produced,
    // indefinitely, not just a transient propagation lag; apparently that move doesn't update whatever
    // internal index VirtualDiskManager itself resolves paths through, at least on this server).
    // `attachUploadedDisk` (the lease-transport path, which does move the file) does this fixup itself,
    // earlier, at the file's pre-move location, and passes `needsEagerZeroFixup=false` here;
    // `finishFolderPutImport` attaches thin and needs no fixup.
    //
    if (needsEagerZeroFixup && diskProvisioning == "eagerZeroedThick") {
        auto datacenter = inventory.resolveDatacenterMoRef(targetVm);
        if (!datacenter) {
            return Error(ErrorCode::IoError, "disk file is at " + datastorePath +
                                                  " but its datacenter could not be resolved to force "
                                                  "eager-zeroing before attaching it: " +
                                                  datacenter.error().message());
        }
        if (auto zeroed = eagerZeroVirtualDisk(session, datastorePath, datacenter.value()); !zeroed.ok()) {
            return Error(ErrorCode::IoError, "disk file is at " + datastorePath +
                                                  " but forcing it eager-zeroed (before attaching it) failed: " +
                                                  zeroed.message());
        }
    }

    if (auto attached = reconfigAddDisk(session, targetVm, controllerKey, unitNumber, capacityBytes, datastorePath,
                                         diskProvisioning);
        !attached.ok()) {
        return Error(ErrorCode::IoError, "disk file is at " + datastorePath +
                                              " (controllerKey=" + std::to_string(controllerKey) +
                                              ", unitNumber=" + std::to_string(unitNumber) +
                                              ") but the final attach reconfig failed -- retry the attach without "
                                              "re-uploading: " +
                                              attached.message());
    }

    // --- post-attach provisioning verification --------------------------
    //
    // Same underlying HttpNfcLease limitation as above, but "thin" has no corrective call the way
    // eagerZeroedThick does (there is no un-allocate-a-block API, once a block is physically written,
    // only a real storage migration, e.g. storage vMotion to a thin-capable datastore, could undo that,
    // which is out of scope here).
    // All this can honestly do is verify and report, rather than silently claim success for a disk that
    // isn't actually thin.
    //
    if (diskProvisioning.empty() || diskProvisioning == "thin") {
        auto refreshed = inventory.describeVm(targetVm);
        if (refreshed) {
            for (const VimDiskInfo& d : refreshed.value().disks) {
                if (d.controllerKey == controllerKey && d.unitNumber == unitNumber && !d.thinProvisioned) {
                    return Error(ErrorCode::IoError,
                                 "disk " + datastorePath +
                                     " was attached and has your data, but this vCenter/datastore did not actually "
                                     "provision it thin (it is fully allocated on the datastore) -- this appears to "
                                     "be a server-side HttpNfcLease import limitation, not a problem with the "
                                     "upload itself; the disk is intact and usable, just not thin");
                }
            }
        }
        // A failure to re-describe the VM here doesn't fail the attach itself: the disk is already
        // genuinely attached; this is only a best-effort extra check on top of that real success.
    }

    return DiskAttachResult{datastorePath, controllerKey, unitNumber};
}

} // namespace VSphereSoap::Detail
