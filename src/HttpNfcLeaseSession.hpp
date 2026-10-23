// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere HttpNfcLease import session

#pragma once

// Owns an HttpNfcLease's lifecycle: the mechanism ovftool/govc import use to create a VM and get back
// HTTPS upload URLs for its disks, all through the existing hand-rolled VIM SOAP client (no VDDK).
// Disks of one lease are uploaded one after another, not in parallel.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "VimTypes.hpp"
#include "Result.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Result;

class VimSession;

/// One disk this VM is being imported with.
/// `capacityBytes` is the *logical* (guest-visible) disk size; `key` is an arbitrary caller-chosen
/// identifier (e.g. "vmdisk1") used to correlate this disk with its `LeaseDeviceUrl` afterward.
/// Originally designed assuming `HttpNfcLeaseInfo.deviceUrl[].importKey` would echo this value back
/// (matching by content, not position); confirmed live against a real vCenter that it does not:
/// `importKey` is entirely vCenter-generated (e.g. "/myVm/ParaVirtualSCSIController0:0"), and
/// the response also includes non-disk entries (e.g. the VM's .nvram, `disk=false`) mixed in with
/// the real ones.
/// `HttpNfcLeaseSession::begin()` now filters to `disk=true` entries and correlates *those* positionally
/// against `disks` in this same order (the only mechanism left), then overlays each result's
/// `LeaseDeviceUrl::importKey` with the corresponding `key` here; so from a caller's point of view
/// outside this file, the "importKey equals what you asked for" contract still holds; only this header's
/// own internal expectation of *why* was wrong.
/// See HttpNfcLeaseSession.cpp's `begin()` for the full story.
///
struct ImportDiskSpec {
    std::string key;
    std::uint64_t capacityBytes = 0;
};

/// One network adapter for the imported VM: `HttpNfcLeaseSession::begin()` turns each of these into
/// an OVF `NetworkSection` entry plus a matching `VirtualHardwareSection` Ethernet `Item`, and (when
/// `network` is set) a `CreateImportSpec` `networkMapping` entry pointing that OVF logical network at
/// the real vCenter network.
/// See `ImportVmSpec::networkAdapters`.
///
struct ImportNetworkAdapterSpec {
    /// Purely for logging: the source-side adapter/portgroup name this entry was built from, not sent to
    /// vCenter at all.
    std::string sourceName;
    /// This adapter's own OVF `NetworkSection`/`rasd:Connection` logical name (e.g. "network0"): must be
    /// unique within one `ImportVmSpec`, and is what `networkMapping` below keys off of.
    /// Never shown to a user; caller-generated (`VSphereImportSession` numbers them).
    std::string ovfNetworkName;
    /// The real vCenter network this adapter connects to.
    /// Empty means disconnected: the OVF Ethernet Item is still created (so the guest sees a NIC) but
    /// with no `rasd:Connection`/`networkMapping` entry at all.
    MoRef network;
    /// `rasd:ResourceSubType`, e.g. "VmxNet3" (the default every caller of this struct used before it
    /// existed) or "E1000e".
    std::string adapterType = "VmxNet3";
    /// `rasd:Address`: a static MAC (any text form vCenter accepts, e.g. "00:50:56:12:34:56") to request
    /// for this adapter.
    /// Empty lets vCenter assign one automatically.
    std::string macAddress;
};

/// Everything `HttpNfcLeaseSession::begin()` needs to build an OVF descriptor and place the resulting
/// VM.
/// This constructs the VM itself (via `ImportVApp`); it cannot attach to a VM the vSphere Automation
/// REST API already created, since the lease and its device URLs only exist for a VM ImportVApp makes.
///
struct ImportVmSpec {
    std::string vmName;
    std::uint64_t memoryBytes = 0;
    std::uint32_t cpuCount = 1;
    std::vector<ImportDiskSpec> disks;
    /// Network adapters to give the imported VM: see `ImportNetworkAdapterSpec`.
    /// Empty (the default) produces a VM with no network adapters at all, matching every caller of this
    /// struct before this field existed.
    std::vector<ImportNetworkAdapterSpec> networkAdapters;
    /// A vSphere `VirtualSystemSettingData.VirtualSystemType` value (e.g. "vmx-19"); an empty string
    /// omits it from the descriptor and lets the target choose its own default hardware version.
    std::string hardwareVersion;
    /// `OperatingSystemSection`'s `vmw:osType` (VMware's own guest-id enum, e.g. "otherGuest64",
    /// "ubuntu64Guest"): distinct from and not sharing a value space with the REST API's `Vm.GuestOS`
    /// enum, even though both mean the same thing conceptually.
    std::string guestOsType = "otherGuest64";
    MoRef resourcePool;
    /// empty is valid: ImportVApp defaults to the resource pool's own vApp/folder context.
    MoRef folder;
    MoRef host; ///< empty is valid unless `resourcePool` spans multiple hosts ambiguously.
    /// Short controller-family name for the one SCSI controller `disks` hang off of: same short
    /// spellings `DiskAttach.hpp`'s `ControllerSelector::controllerType` accepts ("paravirtual",
    /// "lsilogicsas", "lsilogic", "buslogic"; empty defaults to "paravirtual", not vCenter's own
    /// unspecified default: see `buildOvfDescriptor`'s own comment on why this codebase picks
    /// an explicit default rather than relying on the server's).
    /// Mapped to the OVF `rasd:ResourceSubType` value using the same short-name table `DiskAttach.cpp`
    /// uses for its own (confirmed-live, `ReconfigVM_Task` -based) controller creation: the OVF-import
    /// path itself is not independently confirmed to honor this string the same way; if a future live
    /// test finds it doesn't, the fix belongs in `buildOvfDescriptor`, not in a caller.
    ///
    std::string scsiControllerType;
    /// `VirtualMachineConfigSpec.firmware` wire value: "bios" (default, matches every caller of this
    /// struct before this field existed; and vCenter's own default for a plain OVF import) or "efi".
    /// Applied via the `vmw:Config` OVF extension mechanism (`vmw:key="firmware"`): see
    /// `buildOvfDescriptor`'s own comment; not independently confirmed live.
    ///
    std::string firmware;
    /// `VirtualMachineBootOptions.efiSecureBootEnabled`, only meaningful when `firmware == "efi"`.
    /// `std::nullopt` (the default) omits the `vmw:Config` entry entirely, leaving the target's own EFI
    /// default (Secure Boot off, for every vSphere version this codebase targets).
    std::optional<bool> secureBootEnabled;
    /// `AnnotationSection`'s `Annotation` text: the VM's "Notes" field (`config.annotation`).
    /// Empty (the default) omits `AnnotationSection` entirely, leaving the target's own default (no
    /// notes).
    std::string annotation;
    /// `OvfCreateImportSpecParams.diskProvisioning`: vSphere's own wire enum controlling how
    /// the *destination* datastore actually allocates each disk in this import; "thin" (sparse, grows as
    /// data streams in), "thick" (lazy-zeroed: full size reserved immediately, physically zeroed only as
    /// blocks are first touched), or "eagerZeroedThick" (full size reserved AND physically zeroed by
    /// the host immediately, before any upload byte arrives).
    /// This is what actually determines the on-datastore disk shape: the client always streams the same
    /// streamOptimized-encoded, sparse-skipping data regardless of this value; vCenter's own NFC service
    /// decodes it into whatever shape the destination device was already provisioned with.
    /// Empty lets vCenter pick its own default (historically "thin", since that's what every caller of
    /// this class requested before this field existed).
    ///
    std::string diskProvisioning;
    /// `CreateImportSpec`'s own `datastore` parameter, only used to pick a default storage
    /// policy/profile for the generated ImportSpec, not to pin the disk's actual location
    /// (`resourcePool`/`folder`/`host` and the resource pool's own default datastore decide that).
    /// Empty is valid; confirmed live against a real vCenter to fall back to the target's own default
    /// rather than erroring.
    ///
    MoRef datastore;
};

/// One `HttpNfcLeaseInfo.deviceUrl` entry (disk entries only; non-disk ones, e.g. the VM's .nvram, never
/// appear here): where to PUT one disk's bytes.
struct LeaseDeviceUrl {
    /// Matches the corresponding `ImportDiskSpec::key` this URL is for.
    /// Set by `begin()` to that exact value (see `ImportDiskSpec::key`'s own doc comment for why this is
    /// an overlay onto vCenter's own, differently-formatted `importKey`/`key`, not that value verbatim).
    std::string importKey;
    /// The upload URL with vCenter's `*` host placeholder (if any) already substituted for the real host
    /// this session is connected to: ready to hand to an HTTPS client as-is.
    std::string url;
};

/// RAII owner of one `HttpNfcLease`: `begin()` builds the OVF descriptor, calls `CreateImportSpec` then
/// `ImportVApp`, and polls (`RetrievePropertiesEx` on the lease, mirroring
/// `SnapshotScope::waitForTask`'s pattern) until the lease is `ready` (or fails loudly on `error`).
/// While a caller is uploading against the returned device URLs, `HttpNfcLeaseProgress` must keep being
/// sent periodically or the lease expires from under the upload: `startKeepAlive()`/`stopKeepAlive()`
/// run that on a background thread sharing this session's `VimSession` (see the class's own doc comment
/// on why that needs explicit synchronization, not "don't worry about it").
///
/// The destructor calls `HttpNfcLeaseAbort` if `complete()` was never called successfully: a lease is
/// never simply leaked.
/// Not copyable/movable (owns a background thread and a live server-side lease, neither of which has
/// a sensible mid-lifetime handoff).
///
class HttpNfcLeaseSession {
public:
    /// `session` must outlive this object (and must outlive any in-flight keepalive thread:
    /// `stopKeepAlive()`/the destructor join it before returning, so this is automatically satisfied as
    /// long as the caller doesn't destroy `session` from inside a callback running on that thread).
    ///
    /// Fails with `ErrorCode::InvalidArgument` if `spec.disks` is empty or `session.ovfManager()` is
    /// empty (see `VimSession::ovfManager()`'s doc comment); `ErrorCode::IoError` on any SOAP call
    /// failure or if the lease transitions to `error`; `ErrorCode::InvalidFormat` on an unexpected
    /// response shape.
    /// On any failure after `ImportVApp` itself succeeded, aborts the lease before returning rather than
    /// leaving it dangling.
    ///
    [[nodiscard]] static Result<std::unique_ptr<HttpNfcLeaseSession>> begin(VimSession& session,
                                                                               const ImportVmSpec& spec);

    /// Aborts the lease if `complete()` was never called successfully (best-effort, matching
    /// `SnapshotScope`'s destructor discipline); stops and joins the keepalive thread first if still
    /// running.
    ~HttpNfcLeaseSession();

    HttpNfcLeaseSession(const HttpNfcLeaseSession&) = delete;
    HttpNfcLeaseSession& operator=(const HttpNfcLeaseSession&) = delete;
    HttpNfcLeaseSession(HttpNfcLeaseSession&&) = delete;
    HttpNfcLeaseSession& operator=(HttpNfcLeaseSession&&) = delete;

    [[nodiscard]] const MoRef& vm() const noexcept { return m_vm; }
    [[nodiscard]] const std::vector<LeaseDeviceUrl>& deviceUrls() const noexcept { return m_deviceUrls; }

    /// Starts a background thread sending `HttpNfcLeaseProgress` every `interval` (default 30s, well
    /// under vCenter's own lease timeout) until `stopKeepAlive()` is called or this object is destroyed.
    /// `percentComplete` is polled from this thread each tick; it must be safe to call concurrently with
    /// whatever thread is driving the actual upload (e.g. a simple `std::atomic<int>` the upload loop
    /// updates), since that's the whole point of running this on its own thread rather than requiring
    /// the upload loop to interleave progress calls itself.
    /// Calling this twice without an intervening `stopKeepAlive()` is a no-op (logged, not an error)
    /// rather than starting a second thread.
    ///
    void startKeepAlive(std::function<int()> percentComplete,
                          std::chrono::seconds interval = std::chrono::seconds(30));

    /// Stops the keepalive thread and joins it (blocks until its current tick, if any, finishes).
    /// Safe to call when no thread is running.
    void stopKeepAlive();

    /// `HttpNfcLeaseComplete`.
    /// Stops the keepalive thread first (a progress call racing a complete call is exactly the kind of
    /// thing worth not risking).
    /// Precondition: every device URL's upload has actually finished; this does not itself verify that
    /// data made it to the destination, only that the lease is being told it did.
    /// Idempotent: a second call after a successful first one is a no-op.
    ///
    [[nodiscard]] Result<void> complete();

private:
    HttpNfcLeaseSession(VimSession& session, MoRef lease, MoRef vm, std::vector<LeaseDeviceUrl> deviceUrls);

    VimSession& m_session;
    MoRef m_lease;
    MoRef m_vm;
    std::vector<LeaseDeviceUrl> m_deviceUrls;
    bool m_completed = false;

    std::thread m_keepAliveThread;
    std::atomic<bool> m_keepAliveRunning{false};
};

} // namespace VSphereSoap::Detail
