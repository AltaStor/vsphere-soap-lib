// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// VMware SOAP/HTTPS virtual disk source

#pragma once

// The export side of vsphere-soap-lib: reads a VM's disk through vSphere Web Services (SOAP) for the control plane
// and plain HTTPS for the data, with no VMware SDK.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vsphere-soap/Result.hpp"

namespace VSphereSoap {

/// Connection parameters for `VmwareSoapDisk::open`.
struct ConnectParams {
    std::string host;
    std::uint16_t port = 443;
    std::string username;
    std::string password;
    bool verifyTlsCertificate = true;

    /// SHA-1 fingerprint of the peer's leaf certificate, colon-hex (e.g. "AB:CD:..."), the format
    /// vSphere/govc/ovftool print.
    /// When set, this replaces certificate-chain verification rather than adding to it: the usual way to
    /// talk to a self-signed ESXi/vCenter without turning `verifyTlsCertificate` off blindly.
    ///
    std::string pinnedThumbprint;

    /// When true (the default), the Changed Block Tracking allocated-block map lets `isZero()` skip a thin
    /// disk's unallocated regions without reading them.
    /// Unavailable CBT fails `open()` only with `cbtChangeId`; otherwise `isZero()` just reports ranges as not zero.
    ///
    bool useCbt = true;

    /// Name of the CBT baseline snapshot `VmwareSoapDisk::open()` takes, and of the leftover one it
    /// removes first (a snapshot a killed run never got to delete would otherwise redirect reads to its
    /// delta file).
    /// Give each tool a name of its own: two tools sharing one would delete each other's baseline, and
    /// a tool that changes its name can no longer clean up what its earlier runs left behind.
    /// Empty uses "vsphere-soap-cbt-baseline".
    ///
    std::string cbtSnapshotName;

    /// A CBT change id from an earlier read, for an incremental read: `dataExtents()` then lists only what
    /// changed since that id.
    /// `open()` fails when CBT is not available, rather than falling back to the whole disk.
    /// Empty reads the allocated blocks.
    ///
    std::string cbtChangeId;

    /// A snapshot's moref ("snapshot-123") to read the disk as that snapshot holds it, on a running VM too.
    /// No snapshot is taken or removed then, and `cbtSnapshotName` is unused.
    /// Empty reads the disk as it is now, through a snapshot `open()` takes itself when it needs one.
    std::string snapshotMoref;

    /// Quiesce the guest file systems through VMware Tools when `open()` snapshots a running VM.
    bool quiesce = false;
};

/// One byte range of a disk: `[offset, offset + length)`.
struct DiskExtent {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

/// Identifies which VM to read from.
/// Exactly one member should be set; `open()` fails with `ErrorCode::InvalidArgument` if none are.
struct VmLocator {
    static VmLocator byMoref(std::string moref);
    static VmLocator byUuid(std::string uuid);
    static VmLocator byInventoryPath(std::string path);

    std::optional<std::string> moref;
    std::optional<std::string> uuid;
    std::optional<std::string> inventoryPath;
};

/// Identifies which disk on the resolved VM to read: either its VMware device key (e.g. "2000", as
/// reported by `config.hardware.device`) or its datastore-relative path (e.g. "myvm/myvm.vmdk", without
/// the leading "[datastore]"; the datastore itself is implied by the VM's own disk backing, this isn't
/// a general datastore-browsing path).
/// Exactly one member should be set.
///
struct DiskSelector {
    static DiskSelector byKey(std::string key);
    static DiskSelector byDatastorePath(std::string path);

    std::optional<std::string> key;
    std::optional<std::string> datastorePath;
};

/// Read-only disk of a VM, read one of two ways:
/// - Directly: ranged HTTPS GETs of the disk's flat file, for a powered-off VM whose disk has no snapshot deltas.
///   Reads may come in any order.
/// - Exported: the disk of a snapshot through its export lease, for a running VM, a disk with deltas, or
///   a `snapshotMoref`.
///   vSphere streams the disk whole, so reads must come in ascending order.
///
/// It has the usual disk methods but no disk base class: a tool wraps it into its own disk interface.
/// Not thread-safe: use an instance from one thread only.
///
class VmwareSoapDisk final {
public:
    /// Connects, logs in, resolves `vm` and `disk`, and gets ready to read the disk one of the two ways above.
    /// `disk.datastorePath` may name the disk's current file or any file of its snapshot chain.
    ///
    /// The snapshot it needs, for CBT or for an export, is taken under `cbtSnapshotName` and removed when the
    /// object is destroyed; none is taken with `snapshotMoref`.
    /// CBT, when `useCbt` and the disk is eligible, gives `isZero()` and `dataExtents()` the allocated map, or the
    /// changes since `cbtChangeId`.
    ///
    /// Fails with `ErrorCode::InvalidArgument` if the VM is suspended, the selector matches nothing, or the disk
    /// has no datastore file (e.g. an RDM); `ErrorCode::NotImplemented` if `cbtChangeId` is set but CBT is not
    /// available; `ErrorCode::IoError` on connection, auth, lease or transport failure.
    /// Cleans up any session, snapshot or lease it already acquired before returning an error.
    ///
    static VSphereSoap::Result<std::unique_ptr<VmwareSoapDisk>>
    open(const ConnectParams& params, const VmLocator& vm, const DiskSelector& disk);

    /// Releases the export lease and removes the snapshot it took, if any, then logs out; all best-effort.
    ~VmwareSoapDisk();

    VmwareSoapDisk(const VmwareSoapDisk&) = delete;
    VmwareSoapDisk& operator=(const VmwareSoapDisk&) = delete;
    VmwareSoapDisk(VmwareSoapDisk&&) = delete;
    VmwareSoapDisk& operator=(VmwareSoapDisk&&) = delete;

    /// Reads up to `out.size()` bytes at `offset`; the result is short only at the end of the disk.
    /// Fails with `ErrorCode::IoError` when the datastore GET fails after its retries or the export stream fails,
    /// and with `ErrorCode::InvalidArgument` for an exported disk read out of ascending order.
    VSphereSoap::Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out);

    /// Always fails with `ErrorCode::NotImplemented`: the disk is read-only.
    VSphereSoap::Result<std::uint64_t> writeAt(std::uint64_t offset, std::span<const std::byte> in);

    std::uint64_t virtualSize() const noexcept;
    std::uint64_t sectorSize() const noexcept;

    /// No-op that always succeeds: nothing is buffered.
    VSphereSoap::Error flush();

    /// Whether `[offset, offset + length)` is provably all zero, answered from the CBT map without reading data.
    /// Without CBT it is always false, so the caller reads the range once instead of twice.
    VSphereSoap::Result<bool> isZero(std::uint64_t offset, std::uint64_t length);

    /// The ranges worth reading, sorted and merged.
    /// With `cbtChangeId` the ranges changed since that id, with CBT the allocated ranges, and the whole
    /// disk without CBT.
    const std::vector<DiskExtent>& dataExtents() const noexcept;

    /// The disk's change id at the CBT baseline snapshot, which the next incremental is read against.
    /// Empty when CBT is not active or the server reported none.
    const std::string& changeId() const noexcept;

    /// True when the disk is read through an export lease, so reads must ascend.
    bool exported() const noexcept;

    /// True when `open()` obtained a CBT allocated-block map for this disk.
    bool cbtActive() const noexcept;

    /// Why CBT is not active, for a one-time log line.
    /// Empty when CBT is active or was not requested.
    const std::string& cbtUnavailableReason() const noexcept;

private:
    class Impl;
    explicit VmwareSoapDisk(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace VSphereSoap
