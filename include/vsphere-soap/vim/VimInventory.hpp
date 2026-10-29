// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere VM lookup and property retrieval

#pragma once

// VM lookup and property retrieval over a VimSession.
// VMware-SOAP-specific on purpose: VIM snapshot/CBT/session concepts have no home on a generic
// hypervisor interface.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/vim/VimTypes.hpp"
#include "vsphere-soap/Result.hpp"

namespace VSphereSoap::Vim {

using VSphereSoap::Result;

/// Identifies a VM to look up.
/// Exactly one member is set; `findVm` picks the corresponding SearchIndex call (or returns the moref
/// as-is).
struct VmLocator {
    static VmLocator byMoref(std::string moref);
    static VmLocator byUuid(std::string uuid);
    static VmLocator byInventoryPath(std::string path);

    std::optional<std::string> moref;
    std::optional<std::string> uuid;
    std::optional<std::string> inventoryPath;
};

enum class VimPowerState {
    Unknown,
    PoweredOff,
    PoweredOn,
    Suspended,
};

/// One file of a disk's backing chain: the disk's own backing or one of its parents.
struct VimDiskBacking {
    std::string fileName;        ///< "[datastore] dir/name.vmdk", the descriptor.
    std::string backingType;     ///< e.g. "VirtualDiskFlatVer2BackingInfo".
    std::string deltaDiskFormat; ///< backing.deltaDiskFormat on a delta, e.g. "seSparseFormat"; empty on a base.
};

/// One `VirtualDisk` device from `config.hardware.device`.
struct VimDiskInfo {
    std::string key; ///< e.g. "2000": VimVirtualMachine::disks()' device key, stable across calls.
    std::uint64_t capacityBytes = 0;

    /// `backing`'s concrete type, e.g. "VirtualDiskFlatVer2BackingInfo", "RawDiskMappingVer1BackingInfo"
    /// (RDM): used to reject RDM/non-file backings up front (CbtQuery::checkEligible,
    /// VmwareSoapDisk::open), since only a flat/sparse file backing has a datastore path this backend
    /// can GET.
    ///
    std::string backingType;

    /// `backing.fileName` verbatim, e.g. "[datastore1] myvm/myvm.vmdk".
    /// Empty for backings that don't have one (e.g. RDM).
    std::string rawFileName;
    /// Parsed from `rawFileName` via `parseDatastorePath`; both empty if it didn't match the "[ds] path"
    /// shape.
    std::string datastoreName;
    std::string datastoreRelativePath;

    bool thinProvisioned = false;
    /// `backing.eagerlyScrub`: true only for a disk whose blocks were all physically zeroed at
    /// allocation time (an "eager-zeroed thick" disk).
    /// Absent/false for both thin and lazy-zeroed-thick backings.
    bool eagerlyScrub = false;
    std::string diskMode; ///< backing.diskMode, e.g. "persistent", "independent_persistent".
    std::string uuid;     ///< backing.uuid.

    /// backing.changeId, present only once CBT has a baseline for this disk (empty/absent otherwise,
    /// including "CBT enabled but no snapshot has ever been taken yet").
    std::optional<std::string> changeId;

    /// backing.deltaDiskFormat when this backing is a snapshot delta; empty on a base disk.
    std::string deltaDiskFormat;

    /// The backing's parents from `backing.parent`, nearest first and the base disk last.
    /// Empty when the disk has no snapshot deltas.
    std::vector<VimDiskBacking> parents;

    /// The device this disk is attached to and its SCSI/SATA/NVMe/IDE unit number on that controller:
    /// `0` for both is a valid real value (key/unit `0` exist), so callers needing "was this populated"
    /// should check against the owning device's own presence in `VimVmInfo::disks` rather than testing
    /// these for zero.
    ///
    int controllerKey = 0;
    int unitNumber = 0;
};

/// One `VirtualEthernetCard` subtype device (`VirtualE1000`, `VirtualE1000e`, `VirtualVmxnet2`,
/// `VirtualVmxnet3`, `VirtualPCNet32`, `VirtualSriovEthernetCard`) from `config.hardware.device`, in
/// device order: what a caller mapping a VM's networks elsewhere enumerates.
///
struct VimNetworkAdapterInfo {
    int key = 0;
    std::string deviceType; ///< The device's own xsi:type, e.g. "VirtualVmxnet3".
    std::string macAddress; ///< device.macAddress, e.g. "00:50:56:12:34:56".
    /// `backing`'s concrete type: "VirtualEthernetCardNetworkBackingInfo" (standard portgroup) or
    /// "VirtualEthernetCardDistributedVirtualPortBackingInfo" (distributed portgroup) are the two this
    /// project resolves a name for; any other backing (e.g. opaque network) leaves `networkName` empty.
    std::string backingType;
    /// The portgroup's display name: `backing.deviceName` verbatim for a standard-portgroup backing
    /// (already the real name, no extra query needed); for a distributed-portgroup backing, resolved by
    /// `describeVm()` via one extra batched `RetrievePropertiesEx` on the `DistributedVirtualPortgroup`
    /// morefs found in `backing.port.portgroupKey` (see VimInventory.cpp).
    /// Empty if the backing type wasn't one of the two above, or (distributed case) if that follow-up
    /// resolution failed.
    ///
    std::string networkName;
    /// `backing.port.portgroupKey` (a `DistributedVirtualPortgroup` moref value) when `backingType` is
    /// the distributed-portgroup one; empty otherwise.
    std::string portgroupKey;
    /// The distributed portgroup's own configured VLAN id (`config.defaultPortConfig.vlan.vlanId`),
    /// resolved by `describeVm()` in the same follow-up call as `networkName`: `std::nullopt` if this
    /// adapter isn't on a distributed portgroup, if that portgroup has no single-VLAN config (e.g.
    /// a PVLAN/trunk range spec instead of a plain `VlanIdSpec`), or if it's untagged (vlanId 0,
    /// vSphere's own "no VLAN" value).
    /// Standard-portgroup VLAN ids live on the *host's* network config, not on the portgroup/device
    /// itself, and are not resolved here, so a standard-portgroup-backed adapter never has one.
    ///
    std::optional<int> vlanId;
    bool connected = false; ///< connectable.connected: runtime state, always false while powered off.
    bool startConnected = false; ///< connectable.startConnected: the configured "connect at power on".
};

/// One controller device from `config.hardware.device`, parsed for the disk-attach flow's
/// controller/unit resolution: a `VirtualSCSIController` subtype (`VirtualLsiLogicController`,
/// `VirtualParaVirtualSCSIController`, `VirtualBusLogicController`, `VirtualLsiLogicSASController`),
/// `VirtualSATAController`/`VirtualAHCIController`, `VirtualNVMEController`, or `VirtualIDEController`.
/// Not needed by any existing caller of `describeVm()`, so this is additive.
///
struct VimControllerInfo {
    int key = 0;
    std::string deviceType; ///< The device's own xsi:type, e.g. "VirtualParaVirtualSCSIController".
    int busNumber = 0;
    /// Every `unitNumber` already in use by a device whose `controllerKey` points at this controller
    /// (gathered from the same device array in one pass, not a second query): lets a caller pick a free
    /// unit without re-scanning `VimVmInfo::disks`.
    std::vector<int> usedUnitNumbers;
};

struct VimVmInfo {
    MoRef moref;
    std::string name;
    VimPowerState powerState = VimPowerState::Unknown;
    MoRef host; ///< runtime.host.
    std::string configUuid;
    std::string firmware; ///< config.firmware: "bios" | "efi".
    std::string hwVersion; ///< config.version, e.g. "vmx-19".
    std::string guestId;
    bool changeTrackingEnabled = false;
    std::vector<VimDiskInfo> disks;
    std::vector<VimControllerInfo> controllers;
    std::vector<VimNetworkAdapterInfo> networkAdapters;
    /// `config.files.vmPathName`, e.g. "[ds-01] myvm/myvm.vmx": the VM's own folder is this path's
    /// directory component; used to build the destination path for `MoveDatastoreFile_Task` in
    /// the disk-attach flow.
    std::string vmPathName;
    /// `config.annotation`: the free-text "Notes" field shown in the vSphere Client's VM summary.
    /// Not exposed by the vSphere Automation REST API (`/api/vcenter/vm/{vm}`), only by this legacy SOAP
    /// API.
    /// Empty when the source VM has no notes set.
    ///
    std::string annotation;
    std::uint32_t numCpu = 0;     ///< config.hardware.numCPU.
    std::uint64_t memoryMb = 0;   ///< config.hardware.memoryMB.
};

/// One managed object returned by `VimInventory::listObjects()`, with the requested properties flattened
/// to text.
struct VimObjectProperties {
    MoRef moref;
    /// (property path, value): a scalar property's text, or for an array-valued one (e.g.
    /// `Folder.childType`'s ArrayOfString) its elements' texts joined with ','.
    /// Properties the server didn't return (unset on that object) are absent.
    std::vector<std::pair<std::string, std::string>> properties;

    /// The value of `path`, or empty if it wasn't returned.
    std::string get(std::string_view path) const;
};

class VimInventory {
public:
    /// `session` must outlive this object.
    explicit VimInventory(VimSession& session);

    /// Resolves `locator` to a VM MoRef: `FindByUuid`/`FindByInventoryPath` on `SearchIndex` for
    /// the uuid/inventoryPath cases, or `locator.moref` wrapped as a `{"VirtualMachine", moref}` MoRef
    /// directly (no round trip) for the moref case.
    /// Fails with `ErrorCode::InvalidArgument` if no object matches, or if `locator` has none of its
    /// members set.
    ///
    Result<MoRef> findVm(const VmLocator& locator);

    /// One `RetrievePropertiesEx` call (looping on `token` continuation via
    /// `ContinueRetrievePropertiesEx` until a page comes back without one) gathering everything
    /// `VmwareSoapDisk::open` needs about `vm`.
    /// Fails with `ErrorCode::InvalidArgument` if `vm` doesn't exist, `ErrorCode::InvalidFormat` on
    /// an unexpected response shape.
    ///
    Result<VimVmInfo> describeVm(const MoRef& vm);

    /// The disks of a snapshot as they were when it was taken, from its `config.hardware.device`.
    /// Their `changeId` is what an incremental read after that snapshot is taken against.
    /// Fails with `ErrorCode::InvalidArgument` if `snapshot` doesn't exist.
    ///
    Result<std::vector<VimDiskInfo>> describeSnapshotDisks(const MoRef& snapshot);

    /// Walks `parent` upward from `entity` (polymorphically, via the `ManagedEntity` base type's
    /// `name`/`parent` properties) until reaching a `Datacenter`, returning its name: the `dcPath`
    /// the datastore HTTP access URL needs (`VmwareSoapDisk`), which nothing else in the VM's own
    /// properties exposes directly.
    /// Fails with `ErrorCode::InvalidFormat` if no containing Datacenter is found within a bounded
    /// number of hops (guards against an inventory traversal bug looping forever, not a real inventory
    /// shape).
    ///
    Result<std::string> resolveDatacenterName(const MoRef& entity);

    /// Same upward walk as `resolveDatacenterName`, returning the `Datacenter`'s own MoRef instead of
    /// its name: needed by `MoveDatastoreFile_Task`'s `sourceDatacenter`/`destinationDatacenter`
    /// parameters, which take a MoRef, not a name.
    /// Same failure modes.
    ///
    Result<MoRef> resolveDatacenterMoRef(const MoRef& entity);

    /// Every object of managed type `type` (subtypes included, e.g. "Network" also yields
    /// DistributedVirtualPortgroup/OpaqueNetwork; `VimObjectProperties::moref.type` says which) anywhere
    /// under the root folder, with the `pathSet` properties of each: a recursive ContainerView read with
    /// one paged `RetrievePropertiesEx`, then destroyed.
    /// Works the same on vCenter and on a standalone ESXi host.
    /// Fails with `ErrorCode::InvalidFormat` if the server has no ViewManager or answers with
    /// an unexpected shape.
    ///
    Result<std::vector<VimObjectProperties>> listObjects(const std::string& type,
                                                                          const std::vector<std::string>& pathSet);

private:
    VimSession& m_session;
};

/// Parses "[datastore] relative/path.vmdk" into {datastoreName, relativePath}.
/// Returns `std::nullopt` if `backing` doesn't match that shape.
/// Pure function, exposed for unit testing without a live server (mirrors
/// `VmwareVirtualMachine::resolveDatastorePath`'s existing style).
///
[[nodiscard]] std::optional<std::pair<std::string, std::string>> parseDatastorePath(std::string_view backing);

} // namespace VSphereSoap::Vim
