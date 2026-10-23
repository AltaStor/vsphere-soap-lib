// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// VMware SOAP/HTTPS virtual disk source

#pragma once

// The export side of vsphere-soap-lib: reads a powered-off VM's disk through vSphere Web Services (SOAP)
// for the control plane and plain HTTPS datastore GET/Range requests for the data, with no VMware SDK.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "IDisk.hpp"
#include "Result.hpp"

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

    /// When true (the default), attempt to use Changed Block Tracking's allocated-block map so
    /// `isZero()` can skip a thin disk's unallocated zero regions without reading them.
    /// Never fails `open()` on its own when CBT turns out to be unavailable; it just falls back to
    /// `IDisk::isZero()`'s base (read-and-compare) behavior; see `cbtActive()`/`cbtUnavailableReason()`.
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
    [[nodiscard]] static VmLocator byMoref(std::string moref);
    [[nodiscard]] static VmLocator byUuid(std::string uuid);
    [[nodiscard]] static VmLocator byInventoryPath(std::string path);

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
    [[nodiscard]] static DiskSelector byKey(std::string key);
    [[nodiscard]] static DiskSelector byDatastorePath(std::string path);

    std::optional<std::string> key;
    std::optional<std::string> datastorePath;
};

/// `IDisk` over a VMware virtual disk read via vSphere Web Services (SOAP) control plane and plain HTTPS
/// datastore GET/Range data plane.
/// Read-only in this phase (`writeAt`/`flush` fail/no-op accordingly) and powered-off-source-VMs-only
/// (see `open()`'s precondition); there is no live guest-consistency story here for a transfer off
/// a running VM.
///
/// No VMDK sparse/COW parsing lives here: the datastore GET always targets the disk's flat/raw extent
/// file, derived from `config.hardware.device`'s `backing.fileName` (falling back to reading the small
/// VMDK descriptor's extent line if the derived "-flat.vmdk" name 404s, rather than assuming the naming
/// convention blindly); see VmwareSoapDisk.cpp.
///
/// Not thread-safe: confine all calls, including construction/destruction, to a single thread.
///
class VmwareSoapDisk final : public VSphereSoap::IDisk {
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
    [[nodiscard]] static VSphereSoap::Result<std::unique_ptr<VmwareSoapDisk>>
    open(const ConnectParams& params, const VmLocator& vm, const DiskSelector& disk);

    /// Removes the CBT baseline snapshot (if one was taken) and logs out, both best-effort.
    ~VmwareSoapDisk() override;

    VmwareSoapDisk(const VmwareSoapDisk&) = delete;
    VmwareSoapDisk& operator=(const VmwareSoapDisk&) = delete;
    VmwareSoapDisk(VmwareSoapDisk&&) = delete;
    VmwareSoapDisk& operator=(VmwareSoapDisk&&) = delete;

    VSphereSoap::Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out) override;

    /// Always fails with `ErrorCode::NotImplemented`: this backend is read-only in this phase.
    VSphereSoap::Result<std::uint64_t> writeAt(std::uint64_t offset, std::span<const std::byte> in) override;

    [[nodiscard]] std::uint64_t virtualSize() const noexcept override;
    [[nodiscard]] std::uint64_t sectorSize() const noexcept override;

    /// No-op: nothing is buffered here.
    /// Always succeeds.
    VSphereSoap::Result<void> flush() override;

    /// Overridden per `IDisk`'s documented extension point: answers from the CBT allocated-block map
    /// without touching the data region when one is active (`cbtActive()`); falls back to the base
    /// `IDisk::isZero()` (read-and-compare) otherwise.
    VSphereSoap::Result<bool> isZero(std::uint64_t offset, std::uint64_t length) override;

    /// True once `open()` successfully obtained a CBT allocated-block map for this disk (implies
    /// `params.useCbt` was set and CBT was eligible/available).
    [[nodiscard]] bool cbtActive() const noexcept;

    /// Human-readable reason CBT isn't active, suitable for a one-time log line: empty when
    /// `cbtActive()` is true, or when `params.useCbt` was false to begin with.
    [[nodiscard]] const std::string& cbtUnavailableReason() const noexcept;

private:
    class Impl;
    explicit VmwareSoapDisk(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace VSphereSoap
