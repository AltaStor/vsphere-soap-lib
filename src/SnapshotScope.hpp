// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// RAII vSphere snapshot scope

#pragma once

// RAII vSphere snapshot: CreateSnapshot_Task on create(), RemoveSnapshot_Task on destruction, both
// waited-for via task polling.
// Exists as a CBT baseline reference for CbtQuery, not for guest-visible read consistency; this backend
// only ever targets powered-off source VMs (see VmwareSoapDisk), so there is no live guest state
// a snapshot needs to freeze.

#include <memory>
#include <string>

#include "VimSession.hpp"
#include "VimTypes.hpp"
#include "Result.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Result;

/// The CBT baseline snapshot name used when `ConnectParams::cbtSnapshotName` is left empty.
/// `VmwareSoapDisk::open()` both removes a leftover snapshot of this name and creates a new one under
/// it, so each consuming tool should pass a name of its own (see that field's doc comment).
inline constexpr const char* kDefaultCbtBaselineSnapshotName = "vsphere-soap-cbt-baseline";

class SnapshotScope {
public:
    /// If a snapshot named `name` already exists on `vm`, removes it and waits for completion; a no-op
    /// if none does.
    /// Exposed as its own call (not just folded into `create()`) because `VmwareSoapDisk::open()` needs
    /// to run this *before* it calls `describeVm()`: a snapshot left behind by a prior run that was
    /// killed or crashed mid-export (no chance for this class's own destructor to remove it) makes
    /// the VM's *current* disk backing point at that snapshot's delta file, and `describeVm()` reads
    /// that backing once and caches it: by the time `create()` would otherwise run (after
    /// `describeVm()`, per `VmwareSoapDisk::open()`'s existing call order), the stale path is already
    /// captured and removing the snapshot no longer helps that call's own read target.
    ///
    [[nodiscard]] static Result<void> removeIfExists(VimSession& session, const MoRef& vm, std::string name);

    /// Calls `removeIfExists` first (see above: belt-and-suspenders for any caller that doesn't already
    /// do this ahead of its own disk-info read), then `CreateSnapshot_Task` and waits for it to finish.
    /// `memory` is always false in the request (the VM is powered off, so there is no memory state to
    /// capture).
    /// `quiesce` defaults to false: quiescing is a running-guest-tools concept that does not apply to
    /// a powered-off VM; the parameter exists only so this isn't a dead end if a later phase adds
    /// powered-on support.
    /// Fails with `ErrorCode::IoError` if the task faults or times out, `ErrorCode::InvalidFormat` if it
    /// succeeds without a snapshot result.
    ///
    [[nodiscard]] static Result<std::unique_ptr<SnapshotScope>> create(VimSession& session, const MoRef& vm,
                                                                          std::string name, bool quiesce = false);

    /// Calls `RemoveSnapshot_Task` and waits for it, best-effort: any failure is swallowed (never
    /// thrown, never surfaced).
    /// Leaving a snapshot behind on a customer's vCenter is unacceptable, so this always attempts
    /// removal even when the snapshot was never actually used for anything.
    ///
    ~SnapshotScope();

    SnapshotScope(const SnapshotScope&) = delete;
    SnapshotScope& operator=(const SnapshotScope&) = delete;
    SnapshotScope(SnapshotScope&&) = delete;
    SnapshotScope& operator=(SnapshotScope&&) = delete;

    [[nodiscard]] const MoRef& snapshot() const noexcept { return m_snapshot; }

private:
    SnapshotScope(VimSession& session, MoRef vm, MoRef snapshot);

    VimSession& m_session;
    MoRef m_vm;
    MoRef m_snapshot;
};

} // namespace VSphereSoap::Detail
