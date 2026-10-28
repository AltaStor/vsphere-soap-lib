// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// VMware SOAP/HTTPS virtual disk source

#pragma once

// The export side of vsphere-soap-lib: reads a powered-off VM's disk through vSphere Web Services (SOAP)
// for the control plane and plain HTTPS datastore GET/Range requests for the data, with no VMware SDK.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

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
    /// Unavailable CBT never fails `open()`; `isZero()` then just reports ranges as not zero.
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

/// Read-only disk of a powered-off VM.
/// The control plane is vSphere SOAP; data comes from ranged HTTPS GETs of the disk's flat extent file, so no
/// VMDK container is parsed here.
///
/// It has the usual disk methods but no disk base class: a tool wraps it into its own disk interface.
/// Not thread-safe: use an instance from one thread only.
///
class VmwareSoapDisk final {
public:
    /// Connects, logs in, resolves `vm`/`disk` to a datastore flat-extent path, and probes Range support
    /// (fails loudly (`ErrorCode::IoError`) if the server ignores the `Range` header rather than
    /// silently falling back to reading the whole disk per call).
    ///
    /// Precondition (checked, not assumed): the VM must be powered off; fails with
    /// `ErrorCode::InvalidArgument` otherwise.
    ///
    /// When `params.useCbt` and CBT turns out to be eligible for this VM/disk (see
    /// `cbtUnavailableReason()`), also takes a snapshot as a CBT baseline reference and queries
    /// the disk's allocated-block map; both are released when the returned object is destroyed.
    /// A CBT snapshot/query failure does not fail `open()`; it only means `cbtActive()` comes back
    /// false.
    ///
    /// Fails with `ErrorCode::IoError` on connection/auth/transport failure or if the datastore doesn't
    /// support ranged reads; `ErrorCode::InvalidArgument` if the VM is powered on, the VM/disk selector
    /// matches nothing, or the matched disk has no datastore-relative path (e.g. an RDM);
    /// `ErrorCode::InvalidFormat` on an unexpected vSphere response shape.
    /// Cleans up any session/snapshot it already acquired before returning an error.
    ///
    static VSphereSoap::Result<std::unique_ptr<VmwareSoapDisk>>
    open(const ConnectParams& params, const VmLocator& vm, const DiskSelector& disk);

    /// Removes the CBT baseline snapshot (if one was taken) and logs out, both best-effort.
    ~VmwareSoapDisk();

    VmwareSoapDisk(const VmwareSoapDisk&) = delete;
    VmwareSoapDisk& operator=(const VmwareSoapDisk&) = delete;
    VmwareSoapDisk(VmwareSoapDisk&&) = delete;
    VmwareSoapDisk& operator=(VmwareSoapDisk&&) = delete;

    /// Reads up to `out.size()` bytes at `offset`; the result is short only at the end of the disk.
    /// Fails with `ErrorCode::IoError` when the datastore GET fails after its retries.
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
