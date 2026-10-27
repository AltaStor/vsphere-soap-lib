// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// VMware SOAP/HTTPS virtual disk source

#include "VmwareSoapDisk.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include "CbtQuery.hpp"
#include "DatastorePath.hpp"
#include "HttpTransportAdapter.hpp"
#include "SnapshotScope.hpp"
#include "SoapClient.hpp"
#include "SoapLog.hpp"
#include "VimInventory.hpp"
#include "VimSession.hpp"
#include "VimTypes.hpp"
#include "Error.hpp"

namespace VSphereSoap {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;
using VSphereSoap::Result;
using Detail::deriveFlatName;
using Detail::HttpHeader;
using Detail::HttpMethod;
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

Result<RangeProbeResult> probeRange(Detail::IHttpTransport& transport, const std::string& target,
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
Result<std::pair<std::string, std::string>> resolveGetTarget(Detail::IHttpTransport& transport,
                                                                const std::string& cookie, const std::string& dcName,
                                                                const Detail::VimDiskInfo& disk)
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
    Impl(std::unique_ptr<Detail::IHttpTransport> transport, std::string host)
        : m_transport(std::move(transport)), m_soap(*m_transport, std::move(host)) {}

    std::unique_ptr<Detail::IHttpTransport> m_transport;
    Detail::SoapClient m_soap;
    std::unique_ptr<Detail::VimSession> m_session;
    std::unique_ptr<Detail::SnapshotScope> m_snapshotScope; // only set when cbtActive_
    Detail::MoRef m_vm;
    Detail::VimDiskInfo m_disk;
    std::string m_getPath;
    std::string m_getQuery;
    std::uint64_t m_virtualSizeBytes = 0;
    std::vector<Detail::Extent> m_allocatedExtents; // sorted by start; only meaningful when cbtActive_
    bool m_cbtActive = false;
    std::string m_cbtUnavailableReason;
    std::uint64_t m_bytesReadLogged = 0; // watermark for the throttled progress log in readAt()
};

namespace {
// Coarse progress logging for readAt(), which can be called thousands of times for one disk:
// trace-per-call would be too noisy even for SPDLOG_LEVEL=trace, so this only fires once per this many
// bytes crossed.
constexpr std::uint64_t kReadProgressLogIntervalBytes = 64ull * 1024 * 1024;
} // namespace

Result<std::unique_ptr<VmwareSoapDisk>> VmwareSoapDisk::open(const ConnectParams& params, const VmLocator& vm,
                                                                const DiskSelector& disk)
{
    if (!disk.key && !disk.datastorePath) {
        return Error(ErrorCode::InvalidArgument, "DiskSelector has neither key nor datastorePath set");
    }
    Detail::VmLocator internalLocator;
    if (vm.moref) {
        internalLocator = Detail::VmLocator::byMoref(*vm.moref);
    } else if (vm.uuid) {
        internalLocator = Detail::VmLocator::byUuid(*vm.uuid);
    } else if (vm.inventoryPath) {
        internalLocator = Detail::VmLocator::byInventoryPath(*vm.inventoryPath);
    } else {
        return Error(ErrorCode::InvalidArgument, "VmLocator has none of moref/uuid/inventoryPath set");
    }

    logger()->info("opening disk (key={}, path={}) on {}:{}", disk.key.value_or("-"), disk.datastorePath.value_or("-"),
                    params.host, params.port);

    auto impl = std::make_unique<Impl>(
        std::make_unique<Detail::HttpTransportAdapter>(params.host, params.port, params.verifyTlsCertificate,
                                                          params.pinnedThumbprint),
        params.host);

    auto sessionResult = Detail::VimSession::open(impl->m_soap, params.username, params.password);
    if (!sessionResult) {
        logger()->warn("VIM session open against {}:{} failed: {}", params.host, params.port,
                        sessionResult.error().toString());
        return std::move(sessionResult).error();
    }
    impl->m_session = std::move(sessionResult).value();

    Detail::VimInventory inventory(*impl->m_session);

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
    //
    const std::string snapshotName =
        params.cbtSnapshotName.empty() ? std::string(Detail::kDefaultCbtBaselineSnapshotName) : params.cbtSnapshotName;
    if (auto cleanup = Detail::SnapshotScope::removeIfExists(*impl->m_session, impl->m_vm, snapshotName);
        !cleanup.ok()) {
        logger()->warn("stale-snapshot cleanup for {} {} failed: {} -- proceeding anyway; disk reads may target "
                        "a leftover snapshot's delta file if one is still present",
                        impl->m_vm.type, impl->m_vm.value, cleanup.toString());
    }

    auto vmInfoResult = inventory.describeVm(impl->m_vm);
    if (!vmInfoResult) {
        return std::move(vmInfoResult).error();
    }
    const Detail::VimVmInfo& vmInfo = vmInfoResult.value();
    logger()->debug("VM '{}' has {} disk(s)", vmInfo.name, vmInfo.disks.size());

    if (vmInfo.powerState != Detail::VimPowerState::PoweredOff) {
        return Error(ErrorCode::InvalidArgument, "VM '" + vmInfo.name +
                                                      "' is not powered off; this backend only supports "
                                                      "powered-off source VMs");
    }

    const Detail::VimDiskInfo* matchedDisk = nullptr;
    for (const auto& candidate : vmInfo.disks) {
        if (disk.key && candidate.key == *disk.key) {
            matchedDisk = &candidate;
            break;
        }
        if (disk.datastorePath && candidate.datastoreRelativePath == *disk.datastorePath) {
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

    auto dcNameResult = inventory.resolveDatacenterName(impl->m_vm);
    if (!dcNameResult) {
        return std::move(dcNameResult).error();
    }

    if (params.useCbt) {
        Detail::CbtQuery cbt(*impl->m_session);
        auto eligibleResult = cbt.checkEligible(vmInfo, impl->m_disk);
        if (!eligibleResult.ok()) {
            impl->m_cbtUnavailableReason = eligibleResult.message();
        } else {
            auto snapshotResult =
                Detail::SnapshotScope::create(*impl->m_session, impl->m_vm, snapshotName, /*quiesce=*/false);
            if (!snapshotResult) {
                impl->m_cbtUnavailableReason = "failed to create CBT baseline snapshot: " + snapshotResult.error().message();
            } else {
                impl->m_snapshotScope = std::move(snapshotResult).value();
                auto extentsResult = cbt.queryAllocatedExtents(impl->m_vm, impl->m_snapshotScope->snapshot(),
                                                                  impl->m_disk.key, impl->m_virtualSizeBytes);
                if (!extentsResult) {
                    impl->m_cbtUnavailableReason = "failed to query allocated extents: " + extentsResult.error().message();
                    impl->m_snapshotScope.reset();
                } else {
                    impl->m_allocatedExtents = std::move(extentsResult).value();
                    std::sort(impl->m_allocatedExtents.begin(), impl->m_allocatedExtents.end(),
                              [](const Detail::Extent& a, const Detail::Extent& b) { return a.start < b.start; });
                    impl->m_cbtActive = true;
                }
            }
        }
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

    logger()->info("disk '{}' opened on {} ({} bytes, cbt={})", impl->m_disk.key, params.host,
                    impl->m_virtualSizeBytes, impl->m_cbtActive);
    return std::unique_ptr<VmwareSoapDisk>(new VmwareSoapDisk(std::move(impl)));
}

VmwareSoapDisk::VmwareSoapDisk(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

VmwareSoapDisk::~VmwareSoapDisk() = default;

Result<std::uint64_t> VmwareSoapDisk::readAt(std::uint64_t offset, std::span<std::byte> out)
{
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
        // IDisk::isZero()'s generic default reads the range over the network to inspect it: pointless
        // here, since convert() is about to read() this exact range anyway the moment this returns
        // false.
        // Without CBT there's no cheap way to know allocation state without a network round-trip, so
        // don't pay for one twice: just say "not zero" and let the real read happen once, in convert()'s
        // own chunk size rather than isZero()'s much smaller internal one.
        //
        return false;
    }
    if (length == 0) {
        return true;
    }

    const std::uint64_t rangeEnd = offset + length;
    const auto it = std::lower_bound(m_impl->m_allocatedExtents.begin(), m_impl->m_allocatedExtents.end(), offset,
                                      [](const Detail::Extent& extent, std::uint64_t value) {
                                          return extent.start + extent.length <= value;
                                      });
    if (it != m_impl->m_allocatedExtents.end() && it->start < rangeEnd) {
        return false; // overlaps an allocated extent: not provably zero.
    }
    return true;
}

bool VmwareSoapDisk::cbtActive() const noexcept { return m_impl->m_cbtActive; }
const std::string& VmwareSoapDisk::cbtUnavailableReason() const noexcept { return m_impl->m_cbtUnavailableReason; }

} // namespace VSphereSoap
