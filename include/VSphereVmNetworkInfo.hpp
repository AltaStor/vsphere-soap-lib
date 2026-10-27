// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere VM network adapter query

#pragma once

// Part of vsphere-soap-lib's public API: one-shot SOAP queries and actions on an existing VM; its
// network adapters (what a network mapping on export needs), notes, disks, power state (mirrors
// VmwareSoapDisk.hpp's own connect/findVm/describeVm pattern, but is a single request/response call
// rather than a long-lived disk-reading object; nothing here needs to outlive the returned vector).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Result.hpp"
#include "VmwareSoapDisk.hpp" // reuses ConnectParams/VmLocator

namespace VSphereSoap {

/// One network adapter on a vSphere VM, in `config.hardware.device` order (ascending device key:
/// the order a NIC was added in).
struct VSphereNetworkAdapterInfo {
    /// For logging/mapping-lookup only.
    std::string sourceName;
    /// The portgroup (standard or distributed) this adapter is connected to, resolved to its display
    /// name: empty if the backing wasn't one this project resolves a name for (see
    /// `VimNetworkAdapterInfo::networkName`'s own doc comment).
    std::string networkName;
    std::string macAddress;
    /// The portgroup's own VLAN id, when resolved: see `VimNetworkAdapterInfo::vlanId`'s doc comment on
    /// when this is left unset (untagged, unresolved, or a standard-portgroup backing).
    std::optional<int> vlanId;
    /// Runtime connection state: always false for a powered-off VM.
    bool connected = false;
    /// Configured to connect at power-on (`connectable.startConnected`): what "this adapter is on
    /// network X" means for a powered-off VM.
    bool startConnected = false;
};

/// Connects, logs in, resolves `vm`, and returns its network adapters in device order.
/// Fails with `ErrorCode::InvalidArgument` if `vm` matches no VM; `ErrorCode::IoError` on
/// connection/auth/transport failure; `ErrorCode::InvalidFormat` on an unexpected vSphere response
/// shape.
///
VSphereSoap::Result<std::vector<VSphereNetworkAdapterInfo>> listVmNetworkAdapters(
    const ConnectParams& params, const VmLocator& vm);

/// Connects, logs in, resolves `vm`, and returns its "Notes" text (`config.annotation`), not exposed by
/// the vSphere Automation REST API, only by this legacy SOAP API.
/// Empty (not an error) when the VM has no notes set.
/// Same failure modes as `listVmNetworkAdapters`.
/// A separate one-shot call rather than folded into `listVmNetworkAdapters`'s return value: the two are
/// unrelated pieces of VM metadata a caller may want independently, and keeping this function
/// single-purpose matches this header's own existing shape.
///
VSphereSoap::Result<std::string> getVmNotes(const ConnectParams& params, const VmLocator& vm);

/// One virtual disk on a vSphere VM, as reported by `config.hardware.device` (SOAP):
/// the independent-of-creation-mechanism verification a test wants for a VM this project itself created
/// via `HttpNfcLease`/`ReconfigVM_Task` (see `VSphereImportSession.hpp`/`DiskAttach.hpp`): reading it
/// back through the same VIM SOAP API those write paths use, but through none of the same code, confirms
/// the disk genuinely exists with the expected size on the server, not just that this process believes
/// it uploaded one.
///
struct VSphereDiskSummary {
    std::string key;
    std::uint64_t capacityBytes = 0;
    /// `backing.thinProvisioned`/`backing.eagerlyScrub`; together disambiguate all three provisioning
    /// shapes an import can request: thin (`thinProvisioned`), lazy-zeroed-thick
    /// (`!thinProvisioned && !eagerlyScrub`), eager-zeroed-thick (`!thinProvisioned && eagerlyScrub`).
    bool thinProvisioned = false;
    bool eagerlyScrub = false;
};

/// Connects, logs in, resolves `vm`, and returns every virtual disk currently attached to it (in
/// `config.hardware.device` order).
/// Same failure modes as `listVmNetworkAdapters`.
VSphereSoap::Result<std::vector<VSphereDiskSummary>> listVmDisks(const ConnectParams& params,
                                                                                     const VmLocator& vm);

/// `runtime.powerState`, as one of "poweredOn", "poweredOff", "suspended", or "unknown": the SOAP API's
/// own spelling, which callers that also use the REST API may need to map from its "POWERED_ON"-style
/// values.
/// Same failure modes as `listVmNetworkAdapters`.
///
VSphereSoap::Result<std::string> getVmPowerState(const ConnectParams& params, const VmLocator& vm);

/// `PowerOnVM_Task`: starts `vm` and waits for the task to complete (this only means vCenter accepted
/// and finished the power-on request; it says nothing about whether a guest OS inside then actually
/// boots: see `getVmPowerState()` for confirming the resulting state, and this project's own testing
/// discipline elsewhere for why "reached a power -on-able configuration at all" is the floor this can
/// practically prove without guest tools/an OS image to boot).
/// A no-op (not an error) if `vm` is already powered on.
/// Same failure modes as `listVmNetworkAdapters`, plus `ErrorCode::IoError` if the task itself fails or
/// times out.
///
VSphereSoap::Error powerOnVm(const ConnectParams& params, const VmLocator& vm);

/// Creates a small marker directory on `datastoreName` (its datacenter resolved via `resourcePoolMoref`,
/// which must name a resource pool in that same datacenter) and immediately deletes it: a direct,
/// non-destructive proof that this session can actually create and remove files on the target datastore
/// (`Datastore.FileManagement`), the same permission class the real conversion's own disk upload/attach
/// needs.
/// Meant for an import pre-flight check, as a direct test rather than an ACL/privilege-introspection
/// query.
/// A failure to delete the marker after a successful create is logged, not returned as an error (write
/// access is already proven at that point; a leftover empty marker directory is cosmetic, not a real
/// problem).
/// Fails with `ErrorCode::IoError` on connection/auth/transport failure or if the create itself fails
/// (most commonly `NoPermission`); `ErrorCode::InvalidArgument` if `resourcePoolMoref`'s datacenter
/// can't be resolved.
///
VSphereSoap::Error checkDatastoreWriteAccess(const ConnectParams& params,
                                                                        const std::string& datastoreName,
                                                                        const std::string& resourcePoolMoref);

/// `PowerOffVM_Task`: stops `vm` and waits for the task to complete.
/// A no-op (not an error) if `vm` is already powered off.
/// Same failure modes as `powerOnVm`.
VSphereSoap::Error powerOffVm(const ConnectParams& params, const VmLocator& vm);

/// `Destroy_Task`: deletes `vm` and its files.
/// Precondition: `vm` must already be powered off (this does not power it off itself; callers wanting
/// an unconditional teardown should call `powerOffVm()` first and ignore its result, since "already off"
/// is reported the same way as "successfully powered off").
/// Same failure modes as `powerOnVm`.
///
VSphereSoap::Error destroyVm(const ConnectParams& params, const VmLocator& vm);

} // namespace VSphereSoap
