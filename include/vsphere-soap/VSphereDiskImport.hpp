// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Public wrapper for attaching uploaded disks to vSphere VMs

#pragma once

// The import side of vsphere-soap-lib, attach step: a string/id-based wrapper around the private
// Detail::attachUploadedDisk, so callers don't touch vim25 or pugixml types directly.
//
// This is the last step of the "local disk -> VMware, attached to an existing VM" scenario: after
// `VSphereImportSession` creates a disposable placeholder VM and `VSphereDatastoreBackend` streams
// the disk into it (both unchanged, existing classes; see
// VSphereImportSession.hpp/VSphereDatastoreBackend.hpp), this function moves that disk onto
// a caller-chosen, already-existing target VM and deletes the placeholder.

#include <optional>
#include <string>

#include "vsphere-soap/Result.hpp"
#include "vsphere-soap/VmwareSoapDisk.hpp" // reuses ConnectParams/VmLocator

namespace VSphereSoap {

/// Which controller/unit on the target VM to attach the disk to.
/// Without a type or bus, the controller most of the VM's disks are on is used, and the lowest free unit
/// on it (skipping SCSI unit 7).
struct VSphereAttachControllerSpec {
    /// Empty = no type preference.
    /// One of "lsilogic", "buslogic", "lsilogicsas", "paravirtual", "sata", "nvme", "ide" when set.
    std::string controllerType;
    /// `std::nullopt` = no specific bus requested.
    std::optional<int> busNumber;
    /// Create a new controller of `controllerType` (default "paravirtual" if also empty) at `busNumber`
    /// (default 0) if no existing controller matches, instead of failing.
    bool createIfMissing = false;
};

struct VSphereAttachSpec {
    /// The placeholder VM's moref value, e.g. `VSphereImportSession::vmId()`'s return value.
    std::string placeholderVmId;
    /// Identifies the already-existing VM to attach the disk to.
    VmLocator targetVm;
    VSphereAttachControllerSpec controller;
    /// Filename to give the moved disk, e.g. "mydisk.vmdk": see `DiskAttachSpec::desiredFileName`'s doc
    /// comment (this forwards to it verbatim).
    /// Empty keeps vCenter's own placeholder-assigned name.
    std::string desiredFileName;
    /// vSphere's own `diskProvisioning` wire value ("thin"/"thick"/"eagerZeroedThick") this disk was
    /// actually provisioned with when the placeholder VM was created (see
    /// `VSphereImportVmSpec::diskProvisioning`): callers should pass the exact same value here so
    /// the attach reconfig's `thinProvisioned`/`eagerlyScrub` backing fields match reality instead of
    /// defaulting to "thin" regardless.
    ///
    std::string diskProvisioning;
    /// Forwarded verbatim to `DiskAttachSpec::desiredDatastoreDir`: see that field's own doc comment.
    /// Empty (the default) keeps the disk on the target VM's own datastore.
    std::string desiredDatastoreDir;
};

struct VSphereAttachResult {
    /// Final "[datastore] relative/path.vmdk" of the moved disk.
    std::string datastorePath;
    int controllerKey = 0;
    int unitNumber = 0;
};

/// Connects, logs in, and runs the full detach (from the placeholder) -> move (into the target VM's own
/// folder) -> unregister (the placeholder) -> attach (to the target VM) sequence.
///
/// Precondition (checked, not assumed): the placeholder VM identified by `spec.placeholderVmId` must
/// have exactly one virtual disk device (true by construction for every placeholder
/// `VSphereImportSession` creates); the target VM must be powered off (hot-add is out of scope for this
/// pass).
///
/// Failure/cleanup behavior: a failure before the disk is detached from the placeholder leaves it fully
/// intact there (safe to retry this whole call); every failure after that point is surfaced with enough
/// detail (the disk's current datastore path, and the target VM/controller/unit if already resolved) for
/// a human to finish the job manually without re-uploading, since re-running the upload is far more
/// expensive than one more SOAP call.
///
/// One login per call; see VSphereClient (VSphereClient.hpp) to run several calls on one login.
VSphereSoap::Result<VSphereAttachResult> attachImportedDisk(const ConnectParams& params,
                                                                              const VSphereAttachSpec& spec);

/// Moves the disk of a placeholder VM to `datastorePath` ("[datastore] dir/name.vmdk"), unattached, then unregisters
/// the placeholder and deletes its folder: the end of an import that leaves a disk without a VM.
/// The detach, move and unregister steps are those of `attachImportedDisk`, with the same recovery details in its
/// errors; an existing file at `datastorePath` is never overwritten.
/// `diskProvisioning` is the value the placeholder was created with.
/// Returns the disk's final path.
///
/// One login per call; see VSphereClient (VSphereClient.hpp) to run several calls on one login.
VSphereSoap::Result<std::string> releaseImportedDisk(const ConnectParams& params, const std::string& placeholderVmId,
                                                     const std::string& datastorePath,
                                                     const std::string& diskProvisioning);

/// Whether a disk at `datastorePath` ("[datastore] dir/name.vmdk") already exists: its descriptor or its flat
/// extent.
/// An import checks its destination with this before uploading, since the final move refuses a taken name only
/// after the whole disk has been sent.
///
/// One login per call; see VSphereClient (VSphereClient.hpp) to run several calls on one login.
VSphereSoap::Result<bool> datastoreDiskExists(const ConnectParams& params, const std::string& datastorePath);

/// Checks, without changing anything, that a disk can be attached to `targetVm` as `controller` asks: the
/// controller type is known, the VM has a matching controller (or `createIfMissing` is set), and that
/// controller has a free unit, counting every device on it (CD/DVD drives included).
/// The same choice `attachImportedDisk`/`finishFolderPutImport` make at the end, so an import can refuse an
/// unusable attach point before the upload instead of after it.
/// Fails with `ErrorCode::InvalidArgument` and the reason when no attach point can be resolved.
///
/// One login per call; see VSphereClient (VSphereClient.hpp) to run several calls on one login.
VSphereSoap::Error checkAttachPoint(const ConnectParams& params, const VmLocator& targetVm,
                                    const VSphereAttachControllerSpec& controller);

/// Same as `checkAttachPoint`, and on success says where the disk would go: "scsi0:0 (lsilogicsas)", or
/// "scsi0:0 (a new paravirtual controller)" when `createIfMissing` would add one. A dry run shows it.
VSphereSoap::Result<std::string> resolveAttachPoint(const ConnectParams& params, const VmLocator& targetVm,
                                                    const VSphereAttachControllerSpec& controller);

/// Forces every disk currently attached to `vm` eager-zeroed
/// (`VirtualDiskManager.EagerZeroVirtualDisk_Task`): the same post-import fixup `attachImportedDisk`
/// applies for a disk it moves onto an existing VM (see `DiskAttach.hpp`'s `eagerZeroVirtualDisk` doc
/// comment for why HttpNfcLease's own `diskProvisioning` cannot be trusted for "eagerZeroedThick"),
/// exposed standalone for a VM whose disk was never moved after upload at all: namely
/// `VSphereImportSession`'s own created VM, which keeps its disk(s) exactly where `HttpNfcLease` put
/// them.
/// A caller that created such a VM with `diskProvisioning == "eagerZeroedThick"` should call this once,
/// right after the corresponding `complete()` succeeds and before anything else touches the VM's disks.
///
/// Connects, logs in, and eager-zeros every disk `describeVm(vm)` reports: callers with only some disks
/// needing this (a mixed-provisioning VM) should not use this function for that VM; it is only correct
/// when every disk currently on `vm` genuinely wants "eagerZeroedThick".
/// Fails with `ErrorCode::IoError` on any connection/SOAP failure.
///
/// One login per call; see VSphereClient (VSphereClient.hpp) to run several calls on one login.
VSphereSoap::Error eagerZeroAllDisks(const ConnectParams& params, const VmLocator& vm);

} // namespace VSphereSoap
