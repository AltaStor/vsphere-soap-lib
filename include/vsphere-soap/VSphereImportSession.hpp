// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Public wrapper for HttpNfcLease-based vSphere VM import

#pragma once

// The import side of vsphere-soap-lib, control plane: a string/id-based wrapper around the private
// HttpNfcLeaseSession, so callers drive an HttpNfcLease-based VM import without touching vim25 MoRef or
// pugixml types, the same way VmwareSoapDisk wraps its own internals.
//
// Creates the destination VM itself (via ImportVApp), unlike the vSphere Automation REST API's POST
// /api/vcenter/vm.
// The two are alternative VM-creation mechanisms, not composable; pick one per import.
// Pairs with VSphereDatastoreBackend.hpp: this class gets device URLs, that class streams bytes to one
// of them.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vsphere-soap/Result.hpp"
#include "vsphere-soap/VmwareSoapDisk.hpp" // reuses ConnectParams

namespace VSphereSoap {

struct VSphereImportDiskSpec {
    /// Caller-chosen identifier correlating this disk with its `VSphereImportDeviceUrl` afterward:
    /// `VSphereImportDeviceUrl::key` is guaranteed to equal this value exactly (see
    /// `HttpNfcLeaseSession.hpp`'s `ImportDiskSpec::key` for how that's actually arranged internally,
    /// since vCenter's own wire-level importKey turned out not to be caller-controlled).
    ///
    std::string key;
    std::uint64_t capacityBytes = 0;
};

/// One network adapter for the imported VM: the string/id-based public mirror of
/// `HttpNfcLeaseSession.hpp`'s `Detail::ImportNetworkAdapterSpec`, same reasoning as
/// `VSphereImportDiskSpec` vs. `Detail::ImportDiskSpec`.
struct VSphereImportNetworkAdapterSpec {
    /// For logging only: see `ImportNetworkAdapterSpec::sourceName`.
    std::string sourceName;
    /// Managed-object-reference *value* (e.g. "network-52", "dvportgroup-101") of the real vCenter
    /// network this adapter connects to: the same identifier space the vSphere Automation REST API uses
    /// for networks.
    /// Empty creates a disconnected adapter (see `ImportNetworkAdapterSpec::network`'s own doc comment).
    ///
    std::string networkId;
    /// The MoRef *type* that pairs with `networkId`: "Network", "DistributedVirtualPortgroup", or
    /// "OpaqueNetwork": see `VmwareNetworkRef::morefType`.
    /// Ignored when `networkId` is empty.
    std::string networkMorefType = "Network";
    /// `rasd:ResourceSubType`, e.g. "VmxNet3" (default) or "E1000e".
    std::string adapterType = "VmxNet3";
    /// A static MAC to request (any text form vCenter accepts).
    /// Empty lets vCenter assign one automatically.
    std::string macAddress;
};

struct VSphereImportVmSpec {
    std::string vmName;
    std::uint64_t memoryBytes = 0;
    std::uint32_t cpuCount = 1;
    std::vector<VSphereImportDiskSpec> disks;
    /// Network adapters to give the imported VM.
    /// Empty (the default) produces a VM with no network adapters, matching every caller of this struct
    /// before this field existed.
    std::vector<VSphereImportNetworkAdapterSpec> networkAdapters;
    /// A vSphere `VirtualSystemSettingData.VirtualSystemType` value (e.g. "vmx-19"); empty lets
    /// the target choose its own default.
    std::string hardwareVersion;
    /// VMware's own guest-id enum (e.g. "otherGuest64", "ubuntu64Guest"): distinct from
    /// `VmwareVmCreateSpec::guestOs`'s REST-API enum, even though both mean the same thing conceptually.
    std::string guestOsType = "otherGuest64";
    /// Managed-object-reference *values* (e.g. "resgroup-21", "group-v3"): vSphere's REST and SOAP APIs
    /// share one underlying object-id space, so ids from either work here.
    /// `resourcePoolId` is required; `folderId`/`hostId` empty is valid (see `HttpNfcLeaseSession.hpp`'s
    /// `ImportVmSpec` for what each empty case defaults to).
    ///
    std::string resourcePoolId;
    std::string folderId;
    std::string hostId;
    /// `CreateImportSpec`'s own `datastore` parameter: see `HttpNfcLeaseSession.hpp`'s
    /// `ImportVmSpec::datastore` for what it's actually used for.
    /// Empty is valid.
    std::string datastoreId;
    /// Forwarded verbatim to `HttpNfcLeaseSession.hpp`'s `ImportVmSpec::diskProvisioning`: see that
    /// field's doc comment.
    /// One of "thin", "thick", "eagerZeroedThick", or empty for vCenter's own default.
    std::string diskProvisioning;
    /// Forwarded verbatim to `HttpNfcLeaseSession.hpp`'s `ImportVmSpec::scsiControllerType`: see that
    /// field's doc comment.
    /// One of "paravirtual" (the default, whether empty or explicit), "lsilogicsas", "lsilogic",
    /// "buslogic".
    ///
    std::string scsiControllerType;
    /// Forwarded verbatim to `ImportVmSpec::firmware`: "bios" (default) or "efi".
    std::string firmware;
    /// Forwarded verbatim to `ImportVmSpec::secureBootEnabled`.
    std::optional<bool> secureBootEnabled;
    /// Forwarded verbatim to `ImportVmSpec::annotation`: the VM's "Notes" field (`config.annotation`).
    std::string annotation;
};

struct VSphereImportDeviceUrl {
    std::string key; ///< Matches the corresponding `VSphereImportDiskSpec::key`.
    std::string url; ///< Ready to hand to `VSphereDatastoreBackendParams::deviceUrl`.
};

/// RAII owner of one HttpNfcLease-based VM import.
/// See this header's own top comment.
/// Not copyable/movable (owns a live server-side lease and, once `startKeepAlive()` is called,
/// a background thread).
///
class VSphereImportSession {
public:
    /// Connects, logs in, builds an OVF descriptor for `spec`, and drives `CreateImportSpec` +
    /// `ImportVApp` through to a ready lease: see `HttpNfcLeaseSession::begin()`'s doc comment for
    /// the exact failure modes and abort-on-failure behavior, which this forwards.
    static VSphereSoap::Result<std::unique_ptr<VSphereImportSession>>
    begin(const ConnectParams& params, const VSphereImportVmSpec& spec);

    /// Dry run of `begin()`: connects, logs in and has the target check the VM's OVF descriptor and placement
    /// without creating anything.
    /// Fails with the error `begin()` would fail with at that step.
    static VSphereSoap::Error validate(const ConnectParams& params, const VSphereImportVmSpec& spec);

    /// Aborts the lease (best-effort) if `complete()` was never called successfully, then logs out: see
    /// `HttpNfcLeaseSession`'s own destructor doc comment.
    ~VSphereImportSession();

    VSphereImportSession(const VSphereImportSession&) = delete;
    VSphereImportSession& operator=(const VSphereImportSession&) = delete;
    VSphereImportSession(VSphereImportSession&&) = delete;
    VSphereImportSession& operator=(VSphereImportSession&&) = delete;

    /// The newly-created VM's moref value (e.g. "vm-123").
    const std::string& vmId() const noexcept;
    const std::vector<VSphereImportDeviceUrl>& deviceUrls() const noexcept;

    /// See `HttpNfcLeaseSession::startKeepAlive()`/`stopKeepAlive()`: forwarded verbatim.
    void startKeepAlive(std::function<int()> percentComplete, std::chrono::seconds interval = std::chrono::seconds(30));
    void stopKeepAlive();

    /// See `HttpNfcLeaseSession::complete()`: forwarded verbatim.
    VSphereSoap::Error complete();

    /// `Destroy_Task` on the placeholder VM, unlike `UnregisterVM` (used on the success path, after
    /// the disk has already been moved out into the target VM's own folder), this also deletes its files
    /// from the datastore.
    /// Callers must use this, not rely on any implicit cleanup, when the upload itself failed or was
    /// cancelled and nothing useful was ever produced: calling it after a successful `complete()` + disk
    /// move would delete the very disk this whole operation just built.
    /// Idempotent is not guaranteed if called twice; callers own calling this at most once, on the one
    /// failure/cancellation path that needs it.
    ///
    VSphereSoap::Error destroyPlaceholderVm();

private:
    class Impl;
    explicit VSphereImportSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VSphereSoap
