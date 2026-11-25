// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// VMware SOAP/HTTPS virtual disk source

#include "vsphere-soap/VmwareSoapDisk.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include "vsphere-soap/vim/CbtQuery.hpp"
#include "DatastorePath.hpp"
#include "ExportStreamReader.hpp"
#include "vsphere-soap/vim/HttpTransportAdapter.hpp"
#include "SnapshotScope.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "SoapLog.hpp"
#include "vsphere-soap/vim/VimInventory.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/vim/VimTypes.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;
using VSphereSoap::Result;
using Detail::deriveFlatName;
using Vim::HttpHeader;
using Vim::HttpMethod;
using Detail::parseFlatExtentFileName;
using Detail::siblingPath;
using Detail::urlEncode;

using Detail::logger;

VmLocator VmLocator::byMoref(std::string moref)
{
    VmLocator locator;
    locator.moref = std::move(moref);
    return locator;
}
VmLocator VmLocator::byUuid(std::string uuid)
{
    VmLocator locator;
    locator.uuid = std::move(uuid);
    return locator;
}
VmLocator VmLocator::byInventoryPath(std::string path)
{
    VmLocator locator;
    locator.inventoryPath = std::move(path);
    return locator;
}

DiskSelector DiskSelector::byKey(std::string key)
{
    DiskSelector selector;
    selector.key = std::move(key);
    return selector;
}
DiskSelector DiskSelector::byDatastorePath(std::string path)
{
    DiskSelector selector;
    selector.datastorePath = std::move(path);
    return selector;
}

namespace {

constexpr std::uint64_t kSectorSizeBytes = 512;
constexpr int kMaxIoAttempts = 5;

bool headerNameEquals(const std::string& name, std::string_view target)
{
    return name.size() == target.size() &&
           std::equal(name.begin(), name.end(), target.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

struct RangeProbeResult {
    unsigned int status = 0;
    bool hasContentRange = false;
};

Result<RangeProbeResult> probeRange(Vim::IHttpTransport& transport, const std::string& target,
                                     const std::string& cookie)
{
    std::vector<HttpHeader> headers{{"Range", "bytes=0-15"}};
    if (!cookie.empty()) {
        headers.push_back({"Cookie", cookie});
    }
    auto responseResult = transport.request(HttpMethod::Get, target, headers);
    if (!responseResult) {
        return std::move(responseResult).error();
    }

    RangeProbeResult probe;
    probe.status = responseResult.value().status;
    for (const auto& header : responseResult.value().headers) {
        if (headerNameEquals(header.name, "Content-Range") && header.value.starts_with("bytes ")) {
            probe.hasContentRange = true;
            break;
        }
    }
    return probe;
}

/// Resolves `disk`'s flat-extent GET path (deriving "-flat.vmdk", falling back to reading the descriptor
/// if that 404s) and probes Range support.
/// Returns the resolved `{path, query}` on success: `path + "?" + query` is what `readAt` issues ranged
/// GETs against.
///
Result<std::pair<std::string, std::string>> resolveGetTarget(Vim::IHttpTransport& transport,
                                                                const std::string& cookie, const std::string& dcName,
                                                                const Vim::VimDiskInfo& disk)
{
    const std::string query = "dcPath=" + urlEncode(dcName) + "&dsName=" + urlEncode(disk.datastoreName);

    std::string relativePath = deriveFlatName(disk.datastoreRelativePath);
    std::string path = "/folder/" + urlEncode(relativePath);
    std::string target = path + "?" + query;

    auto probeResult = probeRange(transport, target, cookie);
    if (!probeResult) {
        return std::move(probeResult).error();
    }

    if (probeResult.value().status == 404) {
        // The derived "-flat.vmdk" name doesn't exist: don't assume the naming convention blindly, read
        // the descriptor's extent line instead.
        const std::string descriptorPath = "/folder/" + urlEncode(disk.datastoreRelativePath);
        const std::string descriptorTarget = descriptorPath + "?" + query;

        std::vector<HttpHeader> headers;
        if (!cookie.empty()) {
            headers.push_back({"Cookie", cookie});
        }
        auto descriptorResult = transport.request(HttpMethod::Get, descriptorTarget, headers);
        if (!descriptorResult) {
            return std::move(descriptorResult).error();
        }
        if (descriptorResult.value().status < 200 || descriptorResult.value().status >= 300) {
            return Error(ErrorCode::IoError, "could not fetch VMDK descriptor " + descriptorTarget + ": HTTP " +
                                                  std::to_string(descriptorResult.value().status));
        }

        const auto extentFileName = parseFlatExtentFileName(descriptorResult.value().body);
        if (!extentFileName) {
            return Error(ErrorCode::InvalidFormat,
                         "could not find an extent line in VMDK descriptor " + descriptorTarget);
        }

        relativePath = siblingPath(disk.datastoreRelativePath, *extentFileName);
        path = "/folder/" + urlEncode(relativePath);
        target = path + "?" + query;
        probeResult = probeRange(transport, target, cookie);
        if (!probeResult) {
            return std::move(probeResult).error();
        }
    }

    if (probeResult.value().status == 200) {
        return Error(ErrorCode::IoError, "datastore " + target +
                                              " does not support ranged reads (server returned 200 for a Range "
                                              "request); refusing to read the whole disk per call");
    }
    if (probeResult.value().status != 206) {
        return Error(ErrorCode::IoError, "datastore Range probe against " + target +
                                              " failed: HTTP " + std::to_string(probeResult.value().status));
    }
    if (!probeResult.value().hasContentRange) {
        return Error(ErrorCode::IoError, "datastore " + target +
                                              " returned 206 without a Content-Range header; refusing to trust its "
                                              "Range support");
    }

    return std::make_pair(path, query);
}

} // namespace

class VmwareSoapDisk::Impl {
public:
    Impl(std::unique_ptr<Vim::IHttpTransport> transport, std::string host)
        : m_transport(std::move(transport)), m_soap(*m_transport, std::move(host)) {}

    // Declared in the order they are acquired, so they are released in reverse: the lease before the snapshot it
    // exports, the snapshot before the session that took it.
    std::unique_ptr<Vim::IHttpTransport> m_transport;
    Vim::SoapClient m_soap;
    std::unique_ptr<Vim::VimSession> m_session;
    std::unique_ptr<Detail::SnapshotScope> m_snapshotScope; // only when open() took the snapshot itself
    std::unique_ptr<Detail::ExportStreamReader> m_export;  // only for an exported disk
    Vim::MoRef m_vm;
    Vim::VimDiskInfo m_disk;
    std::string m_getPath;
    std::string m_getQuery;
    std::uint64_t m_virtualSizeBytes = 0;
    std::vector<Vim::Extent> m_allocatedExtents; // sorted by start; only meaningful when cbtActive_
    std::vector<DiskExtent> m_dataExtents;
    std::string m_changeId;
    bool m_cbtActive = false;
    std::string m_cbtUnavailableReason;
    std::uint64_t m_bytesReadLogged = 0; // watermark for the throttled progress log in readAt()
};

namespace {
// Coarse progress logging for readAt(), which can be called thousands of times for one disk:
// trace-per-call would be too noisy even for SPDLOG_LEVEL=trace, so this only fires once per this many
// bytes crossed.
constexpr std::uint64_t kReadProgressLogIntervalBytes = 64ull * 1024 * 1024;

// Whether `path` ("dir/name.vmdk") is the disk's current file or one of its snapshot chain.
bool diskHasFile(const Vim::VimDiskInfo& disk, const std::string& path)
{
    if (disk.datastoreRelativePath == path) {
        return true;
    }
    return std::any_of(disk.parents.begin(), disk.parents.end(), [&](const Vim::VimDiskBacking& parent) {
        const auto parsed = Vim::parseDatastorePath(parent.fileName);
        return parsed && parsed->second == path;
    });
}

// "<controller type><bus>:<unit>", the tail of the disk's key in an export lease.
std::string controllerSlot(const Vim::VimVmInfo& vm, const Vim::VimDiskInfo& disk)
{
    for (const auto& controller : vm.controllers) {
        if (controller.key == disk.controllerKey) {
            return controller.deviceType + std::to_string(controller.busNumber) + ":" +
                   std::to_string(disk.unitNumber);
        }
    }
    return {};
}

std::vector<DiskExtent> toDiskExtents(const std::vector<Vim::Extent>& extents)
{
    std::vector<DiskExtent> out;
    out.reserve(extents.size());
    for (const auto& extent : Vim::mergeExtents(extents)) {
        out.push_back(DiskExtent{extent.start, extent.length});
    }
    return out;
}
} // namespace

Result<std::unique_ptr<VmwareSoapDisk>> VmwareSoapDisk::open(const ConnectParams& params, const VmLocator& vm,
                                                                const DiskSelector& disk)
{
    if (!disk.key && !disk.datastorePath) {
        return Error(ErrorCode::InvalidArgument, "DiskSelector has neither key nor datastorePath set");
    }
    if (!params.cbtChangeId.empty() && !params.useCbt) {
        return Error(ErrorCode::InvalidArgument, "a CBT change id needs useCbt");
    }
    Vim::VmLocator internalLocator;
    if (vm.moref) {
        internalLocator = Vim::VmLocator::byMoref(*vm.moref);
    } else if (vm.uuid) {
        internalLocator = Vim::VmLocator::byUuid(*vm.uuid);
    } else if (vm.inventoryPath) {
        internalLocator = Vim::VmLocator::byInventoryPath(*vm.inventoryPath);
    } else {
        return Error(ErrorCode::InvalidArgument, "VmLocator has none of moref/uuid/inventoryPath set");
    }

    logger()->info("opening disk (key={}, path={}) on {}:{}", disk.key.value_or("-"), disk.datastorePath.value_or("-"),
                    params.host, params.port);

    auto impl = std::make_unique<Impl>(
        std::make_unique<Vim::HttpTransportAdapter>(params.host, params.port, params.verifyTlsCertificate,
                                                          params.pinnedThumbprint),
        params.host);

    auto sessionResult = Vim::VimSession::open(impl->m_soap, params.username, params.password);
    if (!sessionResult) {
        logger()->warn("VIM session open against {}:{} failed: {}", params.host, params.port,
                        sessionResult.error().toString());
        return std::move(sessionResult).error();
    }
    impl->m_session = std::move(sessionResult).value();

    Vim::VimInventory inventory(*impl->m_session);

    auto vmMorefResult = inventory.findVm(internalLocator);
    if (!vmMorefResult) {
        return std::move(vmMorefResult).error();
    }
    impl->m_vm = vmMorefResult.value();

    // Must run before describeVm(): a snapshot left behind by a prior run that was killed or crashed
    // mid-export (its own SnapshotScope destructor never got a chance to run RemoveSnapshot_Task) makes
    // the VM's *current* disk backing point at that snapshot's delta file instead of the stable base
    // flat file this backend reads from. describeVm() below reads and caches that backing once; cleaning
    // up *after* it (e.g. inside a later SnapshotScope::create() call) is too late to fix what this
    // open() call itself will read from.
    // Not needed with the caller's own snapshot, since none of ours is taken then.
    //
    const std::string snapshotName =
        params.cbtSnapshotName.empty() ? std::string(Detail::kDefaultCbtBaselineSnapshotName) : params.cbtSnapshotName;
    if (params.snapshotMoref.empty()) {
        if (auto cleanup = Detail::SnapshotScope::removeIfExists(*impl->m_session, impl->m_vm, snapshotName);
            !cleanup.ok()) {
            logger()->warn("stale-snapshot cleanup for {} {} failed: {} -- proceeding anyway; disk reads may target "
                            "a leftover snapshot's delta file if one is still present",
                            impl->m_vm.type, impl->m_vm.value, cleanup.toString());
        }
    }

    auto vmInfoResult = inventory.describeVm(impl->m_vm);
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }
    const Vim::VimVmInfo& vmInfo = vmInfoResult.value();
    logger()->debug("VM '{}' has {} disk(s)", vmInfo.name, vmInfo.disks.size());

    if (vmInfo.powerState != Vim::VimPowerState::PoweredOff && vmInfo.powerState != Vim::VimPowerState::PoweredOn) {
        return Error(ErrorCode::InvalidArgument, "VM '" + vmInfo.name +
                                                      "' is suspended or in an unknown power state; only powered-off "
                                                      "and running VMs can be read");
    }

    const Vim::VimDiskInfo* matchedDisk = nullptr;
    for (const auto& candidate : vmInfo.disks) {
        if ((disk.key && candidate.key == *disk.key) ||
            (disk.datastorePath && diskHasFile(candidate, *disk.datastorePath))) {
            matchedDisk = &candidate;
            break;
        }
    }
    if (matchedDisk == nullptr) {
        return Error(ErrorCode::InvalidArgument, "no disk matching the given selector on VM '" + vmInfo.name + "'");
    }
    impl->m_disk = *matchedDisk;
    impl->m_virtualSizeBytes = matchedDisk->capacityBytes;

    if (impl->m_disk.datastoreName.empty() || impl->m_disk.datastoreRelativePath.empty()) {
        return Error(ErrorCode::InvalidArgument,
                     "disk '" + impl->m_disk.key + "' on VM '" + vmInfo.name + "' has no datastore-relative path" +
                         (impl->m_disk.backingType.empty() ? "" : " (backing type: " + impl->m_disk.backingType + ")") +
                         "; only file-backed VMDK disks are supported, not RDM/raw-device-mapped disks");
    }

    // Direct reads need the files unlocked and the data in one flat file; anything else goes through an export.
    const bool exported = !params.snapshotMoref.empty() || vmInfo.powerState == Vim::VimPowerState::PoweredOn ||
                          !impl->m_disk.parents.empty();

    Vim::CbtQuery cbt(*impl->m_session);
    const Error eligible = params.useCbt ? cbt.checkEligible(vmInfo, impl->m_disk)
                                         : Error(ErrorCode::NotImplemented, "CBT was not requested");
    if (!params.cbtChangeId.empty() && !eligible.ok()) {
        return Error(ErrorCode::NotImplemented, "an incremental read needs CBT: " + eligible.message());
    }

    // The snapshot the disk is read at: the caller's, or one taken here when an export or CBT needs it.
    Vim::MoRef snapshot;
    if (!params.snapshotMoref.empty()) {
        snapshot = Vim::MoRef{"VirtualMachineSnapshot", params.snapshotMoref};
    } else if (exported || eligible.ok()) {
        const bool quiesce = params.quiesce && vmInfo.powerState == Vim::VimPowerState::PoweredOn;
        auto snapshotResult = Detail::SnapshotScope::create(*impl->m_session, impl->m_vm, snapshotName, quiesce);
        if (!snapshotResult) {
            if (exported) {
                return Error(snapshotResult.error().code(), "failed to snapshot VM '" + vmInfo.name +
                                                                "' for export: " + snapshotResult.error().message());
            }
            impl->m_cbtUnavailableReason =
                "failed to create CBT baseline snapshot: " + snapshotResult.error().message();
        } else {
            impl->m_snapshotScope = std::move(snapshotResult).value();
            snapshot = impl->m_snapshotScope->snapshot();
        }
    }

    // The disk as the snapshot holds it: its size there, and the change id the next incremental is read against.
    if (!snapshot.empty()) {
        auto snapshotDisks = inventory.describeSnapshotDisks(snapshot);
        if (!snapshotDisks) {
            return std::move(snapshotDisks).error();
        }
        const auto atSnapshot = std::find_if(snapshotDisks.value().begin(), snapshotDisks.value().end(),
                                             [&](const Vim::VimDiskInfo& d) { return d.key == impl->m_disk.key; });
        if (atSnapshot == snapshotDisks.value().end()) {
            return Error(ErrorCode::InvalidArgument, "snapshot " + snapshot.value + " has no disk '" +
                                                         impl->m_disk.key + "'");
        }
        impl->m_virtualSizeBytes = atSnapshot->capacityBytes;
        impl->m_changeId = atSnapshot->changeId.value_or("");
    }

    if (!eligible.ok()) {
        if (impl->m_cbtUnavailableReason.empty() && params.useCbt) {
            impl->m_cbtUnavailableReason = eligible.message();
        }
    } else if (!snapshot.empty()) {
        auto allocated = cbt.queryAllocatedExtents(impl->m_vm, snapshot, impl->m_disk.key, impl->m_virtualSizeBytes);
        if (!allocated) {
            if (!params.cbtChangeId.empty()) {
                return Error(allocated.error().code(),
                             "failed to query allocated extents: " + allocated.error().message());
            }
            impl->m_cbtUnavailableReason = "failed to query allocated extents: " + allocated.error().message();
        } else {
            impl->m_allocatedExtents = Vim::mergeExtents(std::move(allocated).value());
            impl->m_cbtActive = true;
        }
    }

    if (!params.cbtChangeId.empty()) {
        auto changed = cbt.queryChangedExtents(impl->m_vm, snapshot, impl->m_disk.key, params.cbtChangeId,
                                               impl->m_virtualSizeBytes);
        if (!changed) {
            return Error(changed.error().code(), "failed to query the changes since " + params.cbtChangeId + ": " +
                                                     changed.error().message());
        }
        impl->m_dataExtents = toDiskExtents(changed.value());
    } else if (impl->m_cbtActive) {
        impl->m_dataExtents = toDiskExtents(impl->m_allocatedExtents);
    } else if (impl->m_virtualSizeBytes > 0) {
        impl->m_dataExtents = {DiskExtent{0, impl->m_virtualSizeBytes}};
    }
    if (!impl->m_cbtActive) {
        impl->m_changeId.clear(); // without CBT nothing can be read against it later
    }

    if (exported) {
        auto exportResult = Detail::ExportStreamReader::open(*impl->m_session, params.host, snapshot,
                                                             controllerSlot(vmInfo, impl->m_disk),
                                                             impl->m_virtualSizeBytes, params.verifyTlsCertificate);
        if (!exportResult) {
            logger()->warn("export of disk '{}' on {} failed: {}", impl->m_disk.key, params.host,
                           exportResult.error().toString());
            return std::move(exportResult).error();
        }
        impl->m_export = std::move(exportResult).value();
    } else {
        auto dcNameResult = inventory.resolveDatacenterName(impl->m_vm);
        if (!dcNameResult) {
            return std::move(dcNameResult).error();
        }
        auto targetResult =
            resolveGetTarget(*impl->m_transport, impl->m_soap.sessionCookie(), dcNameResult.value(), impl->m_disk);
        if (!targetResult) {
            logger()->warn("resolving ranged-GET target for disk '{}' on {} failed: {}", impl->m_disk.key, params.host,
                            targetResult.error().toString());
            return std::move(targetResult).error();
        }
        impl->m_getPath = std::move(targetResult.value().first);
        impl->m_getQuery = std::move(targetResult.value().second);
    }

    logger()->info("disk '{}' opened on {} ({} bytes, {}, cbt={})", impl->m_disk.key, params.host,
                    impl->m_virtualSizeBytes, exported ? "exported" : "direct", impl->m_cbtActive);
    return std::unique_ptr<VmwareSoapDisk>(new VmwareSoapDisk(std::move(impl)));
}

VmwareSoapDisk::VmwareSoapDisk(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

VmwareSoapDisk::~VmwareSoapDisk() = default;

Result<std::uint64_t> VmwareSoapDisk::readAt(std::uint64_t offset, std::span<std::byte> out)
{
    if (m_impl->m_export) {
        return m_impl->m_export->readAt(offset, out);
    }
    if (out.empty() || offset >= m_impl->m_virtualSizeBytes) {
        return std::uint64_t{0};
    }
    const std::uint64_t want = std::min<std::uint64_t>(m_impl->m_virtualSizeBytes - offset, out.size());
    const std::uint64_t endInclusive = offset + want - 1;

    const std::string target = m_impl->m_getPath + "?" + m_impl->m_getQuery;
    std::vector<HttpHeader> headers{{"Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(endInclusive)}};
    if (!m_impl->m_soap.sessionCookie().empty()) {
        headers.push_back({"Cookie", m_impl->m_soap.sessionCookie()});
    }

    for (int attempt = 1; attempt <= kMaxIoAttempts; ++attempt) {
        auto responseResult = m_impl->m_transport->request(HttpMethod::Get, target, headers);
        if (!responseResult) {
            // Transient connection failure: the Range header makes this request idempotent, so retrying
            // with the exact same range is safe.
            logger()->debug("readAt offset={} len={} attempt {}/{} failed: {}", offset, want, attempt,
                             kMaxIoAttempts, responseResult.error().toString());
            if (attempt == kMaxIoAttempts) {
                return std::move(responseResult).error();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
            continue;
        }

        const auto& response = responseResult.value();
        if (response.status == 206) {
            if (response.body.size() != want) {
                return Error(ErrorCode::IoError, "datastore GET " + target + " returned " +
                                                      std::to_string(response.body.size()) + " bytes, expected " +
                                                      std::to_string(want));
            }
            std::memcpy(out.data(), response.body.data(), want);
            if (offset - m_impl->m_bytesReadLogged >= kReadProgressLogIntervalBytes || m_impl->m_bytesReadLogged == 0) {
                logger()->debug("readAt progress: offset={} of {} bytes ({:.1f}%)", offset,
                                 m_impl->m_virtualSizeBytes,
                                 100.0 * static_cast<double>(offset) / static_cast<double>(m_impl->m_virtualSizeBytes));
                m_impl->m_bytesReadLogged = offset;
            }
            return want;
        }
        if (response.status >= 500 && response.status < 600 && attempt < kMaxIoAttempts) {
            logger()->debug("readAt offset={} len={} attempt {}/{} got HTTP {}, retrying", offset, want, attempt,
                             kMaxIoAttempts, response.status);
            std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
            continue;
        }
        logger()->warn("readAt offset={} len={} failed: HTTP {}", offset, want, response.status);
        return Error(ErrorCode::IoError,
                     "datastore GET " + target + " failed: HTTP " + std::to_string(response.status));
    }
    return Error(ErrorCode::IoError, "datastore GET " + target + " failed after " +
                                          std::to_string(kMaxIoAttempts) + " attempts");
}

Result<std::uint64_t> VmwareSoapDisk::writeAt(std::uint64_t /*offset*/, std::span<const std::byte> /*in*/)
{
    return Error(ErrorCode::NotImplemented, "VmwareSoapDisk is read-only in this phase");
}

std::uint64_t VmwareSoapDisk::virtualSize() const noexcept { return m_impl->m_virtualSizeBytes; }
std::uint64_t VmwareSoapDisk::sectorSize() const noexcept { return kSectorSizeBytes; }

Error VmwareSoapDisk::flush() { return {}; }

Result<bool> VmwareSoapDisk::isZero(std::uint64_t offset, std::uint64_t length)
{
    if (!m_impl->m_cbtActive) {
        // Without CBT only a network read could tell, and the caller reads this range anyway once it gets
        // false, so the range is not read twice.
        return false;
    }
    if (length == 0) {
        return true;
    }

    const std::uint64_t rangeEnd = offset + length;
    const auto it = std::lower_bound(m_impl->m_allocatedExtents.begin(), m_impl->m_allocatedExtents.end(), offset,
                                      [](const Vim::Extent& extent, std::uint64_t value) {
                                          return extent.start + extent.length <= value;
                                      });
    if (it != m_impl->m_allocatedExtents.end() && it->start < rangeEnd) {
        return false; // overlaps an allocated extent: not provably zero.
    }
    return true;
}

const std::vector<DiskExtent>& VmwareSoapDisk::dataExtents() const noexcept { return m_impl->m_dataExtents; }
const std::string& VmwareSoapDisk::changeId() const noexcept { return m_impl->m_changeId; }
bool VmwareSoapDisk::exported() const noexcept { return m_impl->m_export != nullptr; }
bool VmwareSoapDisk::cbtActive() const noexcept { return m_impl->m_cbtActive; }
const std::string& VmwareSoapDisk::cbtUnavailableReason() const noexcept { return m_impl->m_cbtUnavailableReason; }

} // namespace VSphereSoap
