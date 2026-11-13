// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere changed block tracking (CBT) query

#include "vsphere-soap/vim/CbtQuery.hpp"

#include <algorithm>

#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Vim {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

std::vector<Extent> mergeExtents(std::vector<Extent> extents)
{
    std::sort(extents.begin(), extents.end(), [](const Extent& a, const Extent& b) { return a.start < b.start; });

    std::vector<Extent> merged;
    for (const Extent& extent : extents) {
        if (extent.length == 0) {
            continue;
        }
        if (!merged.empty() && extent.start <= merged.back().start + merged.back().length) {
            Extent& last = merged.back();
            last.length = std::max(last.start + last.length, extent.start + extent.length) - last.start;
        } else {
            merged.push_back(extent);
        }
    }
    return merged;
}

CbtQuery::CbtQuery(VimSession& session) : m_session(session) {}

Error CbtQuery::checkEligible(const VimVmInfo& vmInfo, const VimDiskInfo& disk) const
{
    if (!vmInfo.changeTrackingEnabled) {
        return Error(ErrorCode::NotImplemented, "changed block tracking is not enabled on VM '" + vmInfo.name +
                                                     "' (config.changeTrackingEnabled is false)");
    }

    int hwVersionNumber = 0;
    if (const auto dash = vmInfo.hwVersion.rfind('-'); dash != std::string::npos) {
        try {
            hwVersionNumber = std::stoi(vmInfo.hwVersion.substr(dash + 1));
        } catch (const std::exception&) {
            hwVersionNumber = 0;
        }
    }
    if (hwVersionNumber < 7) {
        return Error(ErrorCode::NotImplemented, "VM '" + vmInfo.name + "' hardware version (" + vmInfo.hwVersion +
                                                     ") is below vmx-07, which CBT requires");
    }

    if (disk.backingType == "RawDiskMappingVer1BackingInfo") {
        return Error(ErrorCode::NotImplemented,
                     "disk '" + disk.key + "' is a raw device mapping (RDM); CBT is not supported on RDM disks");
    }
    if (disk.diskMode.find("independent") != std::string::npos) {
        return Error(ErrorCode::NotImplemented, "disk '" + disk.key + "' is in independent mode (" + disk.diskMode +
                                                     "), which is excluded from snapshots and therefore from CBT");
    }

    return {};
}

Result<std::vector<Extent>> CbtQuery::queryAllocatedExtents(const MoRef& vm, const MoRef& snapshot,
                                                                const std::string& deviceKey,
                                                                std::uint64_t diskCapacityBytes)
{
    return queryLoop(vm, snapshot, deviceKey, "*", diskCapacityBytes);
}

Result<std::vector<Extent>> CbtQuery::queryChangedExtents(const MoRef& vm, const MoRef& snapshot,
                                                              const std::string& deviceKey, const std::string& changeId,
                                                              std::uint64_t diskCapacityBytes)
{
    return queryLoop(vm, snapshot, deviceKey, changeId, diskCapacityBytes);
}

Result<std::vector<Extent>> CbtQuery::queryLoop(const MoRef& vm, const MoRef& snapshot, const std::string& deviceKey,
                                                   const std::string& changeId, std::uint64_t diskCapacityBytes)
{
    std::vector<Extent> extents;
    std::uint64_t offset = 0;

    while (offset < diskCapacityBytes) {
        pugi::xml_document request = makeVimRequest("QueryChangedDiskAreas", vm);
        pugi::xml_node op = request.document_element();
        // Optional in the API: without it vSphere answers for the VM's current state.
        if (!snapshot.empty()) {
            pugi::xml_node snapshotNode = op.append_child("snapshot");
            snapshotNode.append_attribute("type") = snapshot.type.c_str();
            snapshotNode.text() = snapshot.value.c_str();
        }
        op.append_child("deviceKey").text() = deviceKey.c_str();
        op.append_child("startOffset").text() = std::to_string(offset).c_str();
        op.append_child("changeId").text() = changeId.c_str();

        auto callResult = m_session.call("urn:vim25/QueryChangedDiskAreas", op);
        if (!callResult) {
            return std::move(callResult).error();
        }

        const pugi::xml_node result = returnval(soapBodyContent(callResult.value()));
        if (!result) {
            return Error(ErrorCode::InvalidFormat, "QueryChangedDiskAreas response missing <returnval>");
        }

        const std::uint64_t coveredStart = result.child("startOffset").text().as_ullong();
        const std::uint64_t coveredLength = result.child("length").text().as_ullong();

        for (pugi::xml_node area = result.child("changedArea"); area; area = area.next_sibling("changedArea")) {
            Extent extent;
            extent.start = area.child("start").text().as_ullong();
            extent.length = area.child("length").text().as_ullong();
            if (extent.length > 0) {
                extents.push_back(extent);
            }
        }

        const std::uint64_t nextOffset = coveredStart + coveredLength;
        if (nextOffset <= offset) {
            // A non-advancing response would loop forever; treat it as a protocol error instead of
            // spinning.
            return Error(ErrorCode::InvalidFormat,
                         "QueryChangedDiskAreas made no progress from offset " + std::to_string(offset));
        }
        offset = nextOffset;
    }

    return extents;
}

} // namespace VSphereSoap::Vim
