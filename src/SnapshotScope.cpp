// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// RAII vSphere snapshot scope

#include "SnapshotScope.hpp"

#include <optional>

#include "SoapLog.hpp"
#include "TaskWait.hpp"
#include "Error.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

namespace {

/// Issues `RemoveSnapshot_Task` and returns the task MoRef: shared by the destructor's best-effort
/// cleanup and `create()`'s stale-snapshot cleanup, which additionally needs the actual `Result` (not
/// swallowed) since it must fail loudly if it can't clear the way for a new baseline.
Result<MoRef> beginRemoveSnapshot(VimSession& session, const MoRef& snapshot)
{
    pugi::xml_document request = makeVimRequest("RemoveSnapshot_Task", snapshot);
    pugi::xml_node op = request.document_element();
    op.append_child("removeChildren").text() = "false";
    op.append_child("consolidate").text() = "true";

    auto callResult = session.call("urn:vim25/RemoveSnapshot_Task", op);
    if (!callResult) {
        return std::move(callResult).error();
    }
    const MoRef task = parseMoRef(soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "RemoveSnapshot_Task response missing task MoRef");
    }
    return task;
}

/// Depth-first search of a `VirtualMachineSnapshotTree` node (a `rootSnapshotList`/`childSnapshotList`
/// element) for one named `targetName`.
/// Returns its `snapshot` MoRef.
std::optional<MoRef> findSnapshotTreeNodeByName(pugi::xml_node treeNode, const std::string& targetName)
{
    if (std::string(treeNode.child("name").text().as_string()) == targetName) {
        return parseMoRef(treeNode.child("snapshot"));
    }
    for (pugi::xml_node child = treeNode.child("childSnapshotList"); child;
         child = child.next_sibling("childSnapshotList")) {
        if (auto found = findSnapshotTreeNodeByName(child, targetName)) {
            return found;
        }
    }
    return std::nullopt;
}

/// Looks up `vm`'s current snapshot tree (`VirtualMachine.snapshot`, absent entirely if the VM has never
/// had one) for a snapshot named `name`.
/// A prior run of this same process crashing or being killed mid-export (no chance for `SnapshotScope`'s
/// destructor to run its `RemoveSnapshot_Task`) leaves exactly this behind: an orphaned same-named
/// snapshot that silently redirects every later ranged-GET at its delta file instead of the base flat
/// disk, producing confusing short-read errors far from the actual cause.
/// `create()` checks for and clears this before adding another snapshot on top.
///
Result<std::optional<MoRef>> findExistingSnapshotByName(VimSession& session, const MoRef& vm,
                                                           const std::string& name)
{
    pugi::xml_document request = makeVimRequest("RetrievePropertiesEx", session.propertyCollector());
    pugi::xml_node op = request.document_element();
    pugi::xml_node specSet = op.append_child("specSet");
    pugi::xml_node propSet = specSet.append_child("propSet");
    propSet.append_child("type").text() = "VirtualMachine";
    propSet.append_child("all").text() = "false";
    propSet.append_child("pathSet").text() = "snapshot";
    pugi::xml_node objectSet = specSet.append_child("objectSet");
    pugi::xml_node obj = objectSet.append_child("obj");
    obj.append_attribute("type") = vm.type.c_str();
    obj.text() = vm.value.c_str();
    op.append_child("options");

    auto callResult = session.call("urn:vim25/RetrievePropertiesEx", op);
    if (!callResult) {
        return std::move(callResult).error();
    }

    const pugi::xml_node objectContent = returnval(soapBodyContent(callResult.value())).child("objects");
    for (pugi::xml_node prop = objectContent.child("propSet"); prop; prop = prop.next_sibling("propSet")) {
        if (std::string(prop.child("name").text().as_string()) != "snapshot") {
            continue;
        }
        const pugi::xml_node val = prop.child("val");
        for (pugi::xml_node root = val.child("rootSnapshotList"); root; root = root.next_sibling("rootSnapshotList")) {
            if (auto found = findSnapshotTreeNodeByName(root, name)) {
                return found;
            }
        }
    }
    return std::optional<MoRef>{};
}

} // namespace

Error SnapshotScope::removeIfExists(VimSession& session, const MoRef& vm, std::string name)
{
    logger()->debug("checking {} {} for a pre-existing '{}' snapshot", vm.type, vm.value, name);
    auto existingResult = findExistingSnapshotByName(session, vm, name);
    if (!existingResult) {
        return std::move(existingResult).error();
    }
    if (!existingResult.value()) {
        logger()->debug("no pre-existing '{}' snapshot found on {} {}", name, vm.type, vm.value);
        return {};
    }

    logger()->warn(
        "found a pre-existing '{}' snapshot on {} {} -- likely left behind by a prior run that was killed "
        "or crashed mid-export before its cleanup could run; removing it",
        name, vm.type, vm.value);
    Result<MoRef> removeTask = beginRemoveSnapshot(session, *existingResult.value());
    if (!removeTask) {
        return std::move(removeTask).error();
    }
    Result<MoRef> removeWait = waitForVimTask(session, removeTask.value());
    if (!removeWait) {
        return Error(ErrorCode::IoError,
                     "failed to remove stale '" + name + "' snapshot: " + removeWait.error().message());
    }
    return {};
}

Result<std::unique_ptr<SnapshotScope>> SnapshotScope::create(VimSession& session, const MoRef& vm, std::string name,
                                                                bool quiesce)
{
    if (auto cleanup = removeIfExists(session, vm, name); !cleanup.ok()) {
        return cleanup;
    }

    pugi::xml_document request = makeVimRequest("CreateSnapshot_Task", vm);
    pugi::xml_node op = request.document_element();
    op.append_child("name").text() = name.c_str();
    op.append_child("description").text() = "";
    op.append_child("memory").text() = "false";
    op.append_child("quiesce").text() = quiesce ? "true" : "false";

    auto callResult = session.call("urn:vim25/CreateSnapshot_Task", op);
    if (!callResult) {
        return std::move(callResult).error();
    }

    const MoRef task = parseMoRef(soapBodyContent(callResult.value()).child("returnval"));
    if (task.empty()) {
        return Error(ErrorCode::InvalidFormat, "CreateSnapshot_Task response missing task MoRef");
    }

    Result<MoRef> snapshotResult = waitForVimTask(session, task);
    if (!snapshotResult) {
        return std::move(snapshotResult).error();
    }
    if (snapshotResult.value().empty()) {
        return Error(ErrorCode::InvalidFormat, "CreateSnapshot_Task completed without a snapshot result");
    }

    return std::unique_ptr<SnapshotScope>(new SnapshotScope(session, vm, std::move(snapshotResult).value()));
}

SnapshotScope::SnapshotScope(VimSession& session, MoRef vm, MoRef snapshot)
    : m_session(session), m_vm(std::move(vm)), m_snapshot(std::move(snapshot)) {}

SnapshotScope::~SnapshotScope()
{
    try {
        Result<MoRef> task = beginRemoveSnapshot(m_session, m_snapshot);
        if (task) {
            // Wait so the snapshot is actually gone before this object's caller potentially tears
            // the whole session down.
            (void)waitForVimTask(m_session, task.value());
        }
    } catch (...) {
        // Never propagate out of a destructor.
    }
}

} // namespace VSphereSoap::Detail
