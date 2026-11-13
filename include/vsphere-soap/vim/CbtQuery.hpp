// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere changed block tracking (CBT) query

#pragma once

// Wraps QueryChangedDiskAreas: the disk's allocated-block map (changeId="*", used to skip a thin disk's
// unallocated zero regions) or its changed-block map relative to a real baseline changeId.
// Checks CBT eligibility up front (change tracking on, hardware version, RDM) so unavailability is
// reported clearly rather than discovered as an opaque fault or silently downgraded to a full read.
//
// Public so that a tool reading the disk some other way, e.g. through the VDDK, can still take the disk's
// changed areas from vSphere, over a VimSession of its own.

#include <cstdint>
#include <string>
#include <vector>

#include "vsphere-soap/vim/VimInventory.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/vim/VimTypes.hpp"
#include "vsphere-soap/Result.hpp"

namespace VSphereSoap::Vim {

using VSphereSoap::Result;

/// One allocated/changed byte range within a disk: `[start, start+length)`.
struct Extent {
    std::uint64_t start = 0;
    std::uint64_t length = 0;
};

/// Sorts `extents` and joins those that overlap or touch, dropping empty ones.
std::vector<Extent> mergeExtents(std::vector<Extent> extents);

class CbtQuery {
public:
    /// `session` must outlive this object.
    explicit CbtQuery(VimSession& session);

    /// Fails with `ErrorCode::NotImplemented` (message says which condition) if
    /// `vmInfo.changeTrackingEnabled` is false, `vmInfo.hwVersion` is below "vmx-07", `disk.backingType`
    /// is an RDM backing, or `disk.diskMode` is an independent mode (excluded from snapshots, hence from
    /// CBT too): checked here, before ever calling `QueryChangedDiskAreas`, so `VmwareSoapDisk::open`
    /// can report *why* CBT isn't available instead of guessing from a fault.
    ///
    Error checkEligible(const VimVmInfo& vmInfo, const VimDiskInfo& disk) const;

    /// `changeId="*"`: the disk's full allocated-block map.
    ///
    /// `snapshot` is the disk's state the map is taken at. Empty asks about the VM's current state, which
    /// vSphere answers only for a powered-off VM; a running one fails with the server's fault.
    ///
    Result<std::vector<Extent>> queryAllocatedExtents(const MoRef& vm, const MoRef& snapshot,
                                                                        const std::string& deviceKey,
                                                                        std::uint64_t diskCapacityBytes);

    /// A real `changeId`: the disk's changed-block map relative to that baseline.
    /// `snapshot` as for `queryAllocatedExtents`.
    Result<std::vector<Extent>> queryChangedExtents(const MoRef& vm, const MoRef& snapshot,
                                                                      const std::string& deviceKey,
                                                                      const std::string& changeId,
                                                                      std::uint64_t diskCapacityBytes);

private:
    /// `QueryChangedDiskAreas` returns a bounded set of extents per call, covering only
    /// `[response.startOffset, response.startOffset + response.length)`; this loops, resuming from where
    /// the previous response left off, until that covered range reaches `diskCapacityBytes`.
    /// Fails with `ErrorCode::InvalidFormat` if a response doesn't advance past the offset it was asked
    /// for (would otherwise loop forever).
    ///
    Result<std::vector<Extent>> queryLoop(const MoRef& vm, const MoRef& snapshot,
                                                            const std::string& deviceKey, const std::string& changeId,
                                                            std::uint64_t diskCapacityBytes);

    VimSession& m_session;
};

} // namespace VSphereSoap::Vim
