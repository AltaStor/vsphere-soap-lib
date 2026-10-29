// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Attaching an uploaded disk to an existing vSphere VM

#pragma once

// Net-new: attaches a disk that was uploaded via HttpNfcLeaseSession (into a disposable placeholder VM;
// see VSphereImportSession.hpp's own doc comment on why HttpNfcLease/ImportVApp always creates a VM,
// never targets an existing one) onto an already-existing target VM.
//
// Sequence: detach the disk from the placeholder (ReconfigVM_Task, no fileOperation; keep the file) ->
// move the file into the target VM's own folder (renaming the extent and the descriptor together, which
// keeps the disk's provisioning; MoveVirtualDisk_Task across datastores) -> unregister the placeholder
// (UnregisterVM, not Destroy_Task; must not delete the file we just moved out) and delete its now
// disk-less folder -> resolve a free controller/unit on the target -> attach (ReconfigVM_Task, add).
// None of these four VIM operations exist anywhere else in this codebase yet.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vsphere-soap/vim/VimInventory.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/vim/VimTypes.hpp"
#include "vsphere-soap/Result.hpp"

namespace VSphereSoap::Detail {

using namespace VSphereSoap::Vim;

using VSphereSoap::Result;

/// Which controller/unit to attach the disk to, as resolved by `chooseControllerAndUnit`.
struct ControllerSelector {
    /// Empty = no type preference (match by `busNumber` alone, or, with both empty, auto-pick
    /// the target's existing primary controller).
    /// One of "lsilogic", "buslogic", "lsilogicsas", "paravirtual", "sata", "nvme", "ide" when set.
    std::string controllerType;
    /// `std::nullopt` = no specific bus requested (auto-pick).
    std::optional<int> busNumber;
    /// If no matching controller exists (or the target has none at all), create one of `controllerType`
    /// (defaulting to "paravirtual" if that is also empty) at `busNumber` (defaulting to 0) instead of
    /// failing.
    bool createIfMissing = false;
};

struct DiskAttachSpec {
    MoRef placeholderVm;
    /// Empty leaves the disk unattached at `desiredDatastoreDir`, which is then required.
    MoRef targetVm;
    ControllerSelector controller;
    /// Filename (no directory, no "[datastore]" prefix) to give the moved disk in the target VM's
    /// folder, e.g. "mydisk.vmdk": a ".vmdk" extension is appended if missing.
    /// Empty (the default) keeps whatever name vCenter itself assigned the placeholder's disk during
    /// `ImportVApp` (typically `<placeholder-vm-name>.vmdk`): that name is an implementation detail of
    /// the placeholder, not something a caller chose, so most callers with an opinion on the final
    /// filename should set this rather than rely on it.
    ///
    std::string desiredFileName;
    /// vSphere's own wire enum ("thin"/"thick"/"eagerZeroedThick") this disk was actually provisioned
    /// with: forwarded to the final attach reconfig so its `thinProvisioned`/`eagerlyScrub` backing
    /// fields match what was really written, instead of a hardcoded guess.
    /// Empty is treated as "thin" (matches every caller of this struct before this field existed).
    ///
    std::string diskProvisioning;
    /// Overrides where the moved disk file ends up: `"[datastore] relative/dir"` (no filename;
    /// `desiredFileName`/the placeholder's own filename still supplies that).
    /// Empty (the default, matching every caller of this struct before this field existed) uses
    /// the target VM's own folder (`config.files.vmPathName`'s directory), so every attached disk lands
    /// on the same datastore as the target VM itself.
    /// Set this to place a disk on a *different* datastore than the target VM's own (e.g. to honor
    /// a per-disk `--storage-map` target): the target VM itself does not move, only this one disk's
    /// file.
    /// Must name a datastore in the same datacenter as the target VM: `attachUploadedDisk` resolves both
    /// the source and destination side of the move through the target VM's own datacenter, not a second
    /// lookup for this datastore.
    ///
    std::string desiredDatastoreDir;
};

struct DiskAttachResult {
    /// Final "[datastore] relative/path.vmdk" of the moved disk, now backing a `VirtualDisk` device on
    /// the target VM.
    std::string datastorePath;
    int controllerKey = 0;
    int unitNumber = 0;
};

/// Runs the full detach -> move -> unregister -> attach sequence described above.
/// Preconditions (checked, not assumed): the placeholder VM must have exactly one `VirtualDisk` device
/// (true by construction for every placeholder this codebase creates; `VSphereImportVmSpec::disks` is
/// always given exactly one entry by the only caller of this function); the target VM must be powered
/// off (hot-add is untested).
///
/// Failure/cleanup behavior: a failure before the disk is detached leaves the placeholder VM owning it
/// untouched (safe to retry the whole attach); `UnregisterVM` failing after a successful move is logged
/// and swallowed (cosmetic clutter, not data risk) rather than failing the whole operation; every other
/// failure surfaces the exact datastore path/VM state a human needs to recover manually, following
/// the same "(VM '...' was still created)"-style messaging callers use for the equivalent lease-side
/// failures.
///
Result<DiskAttachResult> attachUploadedDisk(VimSession& session, const DiskAttachSpec& spec);

/// The controller/unit resolution algorithm, exposed standalone (pure, no network) for unit testing over
/// a hand-built `VimVmInfo` fixture.
/// `placeholderDisk` is the (already-detached) disk being attached, needed only for its `capacityBytes`
/// (irrelevant to this function's own logic, kept for future use if capacity ever needs to factor into
/// controller choice (e.g. NVMe-preferring large disks) not yet implemented).
///
struct ControllerChoice {
    int controllerKey = 0;
    int unitNumber = 0;
    /// When set, no existing controller matched and one must be created (`ReconfigVM_Task`,
    /// `operation=add`) with this type/bus before the disk-add reconfig can use `controllerKey` (which
    /// is only meaningful once that controller exists: callers must create it and re-resolve, not use
    /// this struct's `controllerKey` directly in that case).
    ///
    std::optional<std::pair<std::string, int>> controllerToCreate;
};
Result<ControllerChoice> chooseControllerAndUnit(const VimVmInfo& target,
                                                                  const ControllerSelector& selector);

/// The "resolve controller/unit, create a controller if needed, attach" tail shared by
/// `attachUploadedDisk` (called after its detach/move/unregister steps) and the `folder-put` fallback
/// transport (called directly: that path writes straight into the target VM's own folder, so there is no
/// placeholder to detach/move/unregister at all).
/// Not just a style refactor: this is the exact same live-tested sequence both transports need, so
/// a future fix to it (like the earlier one that stopped sending `sharedBus` for non-SCSI controllers)
/// only has one place to land.
///
/// `datastorePath` must already be the disk's final "[datastore] relative/path.vmdk" location (nothing
/// is moved here).
/// Same failure-message-surfaces-recovery-state discipline as `attachUploadedDisk`.
///
/// `needsEagerZeroFixup`: whether this call should itself force `diskProvisioning == "eagerZeroedThick"`
/// disks eager-zeroed (see `eagerZeroVirtualDisk`'s doc comment on why this can't happen after
/// a `MoveDatastoreFile_Task`).
/// `attachUploadedDisk` does that fixup itself, earlier, at the file's pre-move location, and calls this
/// with `false`; `finishFolderPutImport` (no move involved at all) leaves it at the default `true`.
///
Result<DiskAttachResult> attachDiskFile(VimSession& session, const MoRef& targetVm,
                                                        const std::string& datastorePath,
                                                        std::uint64_t capacityBytes,
                                                        const ControllerSelector& controller,
                                                        const std::string& diskProvisioning = {},
                                                        bool needsEagerZeroFixup = true);

/// `DeleteDatastoreFile_Task`, for cleaning up a partially-written file left behind by
/// a failed/cancelled upload (the `folder-put` transport's descriptor and flat extent both need this,
/// since neither is inside a disposable placeholder VM that a single `Destroy_Task` could remove
/// instead).
/// `name` is a "[datastore] relative/path" string; `datacenter` may be empty (server picks based on
/// `name`'s datastore, matching `MoveDatastoreFile_Task`'s own optional datacenter parameters elsewhere
/// in this file).
/// Fails with `ErrorCode::IoError` on SOAP failure: callers doing best-effort cleanup should log rather
/// than propagate, the same discipline `unregisterVm` already uses internally.
///
Error deleteDatastoreFile(VimSession& session, const std::string& name,
                                                 const MoRef& datacenter);

/// `FileManager.MakeDirectory` (with `createParentDirectories=true`): creates a directory at `name`
/// ("[datastore] relative/path"), used by `checkDatastoreWriteAccess` (VSphereVmNetworkInfo.hpp) as
/// a real, direct, non-destructive proof that the connected session can actually create files on
/// a datastore, rather than an ACL/privilege-introspection query.
/// Synchronous (no `_Task` suffix: MakeDirectory returns once the directory exists, unlike
/// the file-move/delete/eager-zero operations elsewhere in this file).
/// Fails with `ErrorCode::IoError` on SOAP failure, most commonly `NoPermission` for a session that
/// genuinely lacks `Datastore.FileManagement` on `datacenter`.
///
Error makeDatastoreDirectory(VimSession& session, const std::string& name,
                                                     const MoRef& datacenter);

/// `VirtualDiskManager.EagerZeroVirtualDisk_Task`: forces every block of the virtual disk at `name` to
/// be physically allocated and zeroed.
/// `attachDiskFile` calls this as a post-import fixup for a `diskProvisioning == "eagerZeroedThick"`
/// request: confirmed live against a real vCenter that `HttpNfcLease`'s own `diskProvisioning` field,
/// while accepted without error, does not reliably produce a truly eager-zeroed disk on every
/// ESXi/vCenter version; this call is the actual, deterministic mechanism VMware provides for
/// guaranteeing it, used the same way a real eager-zero-after-the-fact operation (e.g. a storage vMotion
/// to eager-zeroed-thick) would end up invoking it internally.
/// Fails with `ErrorCode::IoError` on SOAP failure.
///
Error eagerZeroVirtualDisk(VimSession& session, const std::string& name,
                                                  const MoRef& datacenter);

} // namespace VSphereSoap::Detail
