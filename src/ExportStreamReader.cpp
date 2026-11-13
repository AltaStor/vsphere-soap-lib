// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Disk reads through a snapshot's export lease

#include "ExportStreamReader.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "HttpNfcLeaseSession.hpp"
#include "SoapLog.hpp"
#include "StreamOptimizedVmdkDecoder.hpp"
#include "vsphere-soap/Error.hpp"
#include "vsphere-soap/vim/HttpTransportAdapter.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

namespace {

// Decoded grains held ahead of the reader: 256 grains of 64 KiB, 16 MiB.
constexpr std::size_t kMaxQueuedGrains = 256;

// ESXi drops a lease after about five minutes without progress; a minute between reports leaves room for a slow
// network read.
constexpr std::chrono::seconds kProgressInterval{60};
constexpr std::chrono::seconds kWaitSlice{15};

// One network read of the download; a stream that stalls longer than this is given up on.
constexpr std::chrono::seconds kReadTimeout{300};

struct HttpsUrl {
    std::string host;
    std::uint16_t port = 443;
    std::string target;
};

Result<HttpsUrl> parseHttpsUrl(const std::string& url)
{
    constexpr std::string_view kScheme = "https://";
    if (url.compare(0, kScheme.size(), kScheme) != 0) {
        return Error(ErrorCode::InvalidFormat, "export lease URL is not https: '" + url + "'");
    }
    const std::size_t pathStart = url.find('/', kScheme.size());
    const std::string authority = url.substr(kScheme.size(), pathStart == std::string::npos
                                                                 ? std::string::npos
                                                                 : pathStart - kScheme.size());
    HttpsUrl parsed;
    const std::size_t colon = authority.find(':');
    parsed.host = authority.substr(0, colon);
    if (colon != std::string::npos) {
        try {
            parsed.port = static_cast<std::uint16_t>(std::stoul(authority.substr(colon + 1)));
        } catch (const std::exception&) {
            return Error(ErrorCode::InvalidFormat, "export lease URL has an invalid port: '" + url + "'");
        }
    }
    if (parsed.host.empty()) {
        return Error(ErrorCode::InvalidFormat, "export lease URL has no host: '" + url + "'");
    }
    parsed.target = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    return parsed;
}

Error leaseCall(VimSession& session, const char* operation, const MoRef& lease,
                const std::optional<int>& percent = std::nullopt)
{
    pugi::xml_document request = makeVimRequest(operation, lease);
    if (percent) {
        request.document_element().append_child("percent").text() = *percent;
    }
    auto result = session.call(request.document_element());
    if (!result) {
        return std::move(result).error();
    }
    return {};
}

} // namespace

std::optional<ExportDeviceUrl> pickExportDeviceUrl(const std::vector<ExportDeviceUrl>& urls,
                                                   const std::string& controllerSlot)
{
    if (!controllerSlot.empty()) {
        for (const auto& url : urls) {
            if (url.key.size() >= controllerSlot.size() + 1 &&
                url.key.compare(url.key.size() - controllerSlot.size(), controllerSlot.size(), controllerSlot) == 0 &&
                url.key[url.key.size() - controllerSlot.size() - 1] == '/') {
                return url;
            }
        }
    }
    if (urls.size() == 1) {
        return urls.front();
    }
    return std::nullopt;
}

ExportStreamReader::ExportStreamReader(VimSession& session, MoRef lease, std::uint64_t capacityBytes)
    : m_session(session), m_lease(std::move(lease)), m_capacityBytes(capacityBytes),
      m_lastProgress(std::chrono::steady_clock::now())
{
}

Result<std::unique_ptr<ExportStreamReader>> ExportStreamReader::open(VimSession& session, const std::string& vcHost,
                                                                     const MoRef& snapshot,
                                                                     const std::string& controllerSlot,
                                                                     std::uint64_t capacityBytes,
                                                                     bool verifyTlsCertificate,
                                                                     const ExportTransportFactory& transportFactory)
{
    pugi::xml_document request = makeVimRequest("ExportSnapshot", snapshot);
    auto exported = session.call(request.document_element());
    if (!exported) {
        return std::move(exported).error();
    }
    const MoRef lease = parseMoRef(soapBodyContent(exported.value()).child("returnval"));
    if (lease.empty()) {
        return Error(ErrorCode::InvalidFormat, "ExportSnapshot response has no lease");
    }
    // From here on the destructor releases the lease, whatever fails next.
    std::unique_ptr<ExportStreamReader> reader(new ExportStreamReader(session, lease, capacityBytes));

    pugi::xml_document infoStorage;
    auto info = waitForLeaseReady(session, lease, infoStorage);
    if (!info) {
        return std::move(info).error();
    }
    std::vector<ExportDeviceUrl> diskUrls;
    for (pugi::xml_node du = info.value().child("deviceUrl"); du; du = du.next_sibling("deviceUrl")) {
        if (!du.child("disk").text().as_bool()) {
            continue;
        }
        diskUrls.push_back(ExportDeviceUrl{du.child("key").text().as_string(),
                                           substituteHostPlaceholder(du.child("url").text().as_string(), vcHost),
                                           du.child("sslThumbprint").text().as_string()});
    }
    const auto picked = pickExportDeviceUrl(diskUrls, controllerSlot);
    if (!picked) {
        return Error(ErrorCode::InvalidFormat, "export lease of " + snapshot.value + " has " +
                                                   std::to_string(diskUrls.size()) + " disk URL(s), none for '" +
                                                   controllerSlot + "'");
    }
    auto url = parseHttpsUrl(picked->url);
    if (!url) {
        return std::move(url).error();
    }

    logger()->info("exporting disk '{}' of snapshot {} from {}", controllerSlot, snapshot.value, url.value().host);
    const HttpsUrl& data = url.value();
    std::unique_ptr<IHttpTransport> transport =
        transportFactory
            ? transportFactory(data.host, data.port, verifyTlsCertificate, picked->sslThumbprint)
            : std::make_unique<HttpTransportAdapter>(data.host, data.port, verifyTlsCertificate, picked->sslThumbprint);
    reader->m_thread = std::thread(&ExportStreamReader::download, reader.get(), std::move(transport),
                                   url.value().target);
    return reader;
}

ExportStreamReader::~ExportStreamReader()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_changed.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }

    // Best-effort, like every cleanup here: a lease left behind expires on its own.
    const bool complete = m_streamComplete && m_downloadError.ok();
    if (Error released = leaseCall(m_session, complete ? "HttpNfcLeaseComplete" : "HttpNfcLeaseAbort", m_lease);
        !released.ok()) {
        logger()->warn("releasing export lease {} failed: {}", m_lease.value, released.toString());
    }
}

void ExportStreamReader::download(std::unique_ptr<IHttpTransport> transport, std::string target)
{
    StreamOptimizedVmdkDecoder decoder([this](std::uint64_t offset, std::span<const std::byte> data) -> Error {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [this] { return m_stop || m_queue.size() < kMaxQueuedGrains; });
        if (m_stop) {
            return Error(ErrorCode::Cancelled, "export stream reader closed");
        }
        m_queue.push_back(Grain{offset, std::vector<std::byte>(data.begin(), data.end())});
        lock.unlock();
        m_changed.notify_all();
        return {};
    });

    auto response = transport->requestGetStream(
        target, {}, [&decoder](std::span<const std::byte> bytes) { return decoder.feed(bytes); }, kReadTimeout);

    Error error;
    if (!response) {
        error = std::move(response).error();
    } else if (response.value().status < 200 || response.value().status >= 300) {
        error = Error(ErrorCode::IoError, "export stream GET failed: HTTP " + std::to_string(response.value().status) +
                                              " " + response.value().body);
    } else if (!decoder.finished()) {
        error = Error(ErrorCode::IoError, "export stream ended before its end-of-stream marker");
    }

    {
        std::lock_guard lock(m_mutex);
        m_downloadDone = true;
        m_streamComplete = decoder.finished();
        if (error.code() != ErrorCode::Cancelled) {
            m_downloadError = std::move(error);
        }
    }
    m_changed.notify_all();
}

Result<std::optional<ExportStreamReader::Grain>> ExportStreamReader::nextGrain()
{
    std::unique_lock lock(m_mutex);
    while (m_queue.empty() && !m_downloadDone) {
        if (!m_changed.wait_for(lock, kWaitSlice, [this] { return !m_queue.empty() || m_downloadDone; })) {
            lock.unlock();
            keepLeaseAlive(m_progressOffset);
            lock.lock();
        }
    }
    if (m_queue.empty()) {
        if (!m_downloadError.ok()) {
            return m_downloadError;
        }
        return std::optional<Grain>{};
    }
    Grain grain = std::move(m_queue.front());
    m_queue.pop_front();
    lock.unlock();
    m_changed.notify_all();
    return std::optional<Grain>(std::move(grain));
}

void ExportStreamReader::keepLeaseAlive(std::uint64_t offset)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastProgress < kProgressInterval) {
        return;
    }
    m_lastProgress = now;
    const int percent =
        m_capacityBytes == 0 ? 0 : static_cast<int>(std::min<std::uint64_t>(99, offset * 100 / m_capacityBytes));
    if (Error sent = leaseCall(m_session, "HttpNfcLeaseProgress", m_lease, percent); !sent.ok()) {
        logger()->warn("export lease {} progress report failed: {}", m_lease.value, sent.toString());
    }
}

Result<std::uint64_t> ExportStreamReader::readAt(std::uint64_t offset, std::span<std::byte> out)
{
    if (out.empty() || offset >= m_capacityBytes) {
        return std::uint64_t{0};
    }
    if (offset < m_passedUpTo) {
        return Error(ErrorCode::InvalidArgument, "export stream reads must ascend: offset " + std::to_string(offset) +
                                                     " lies before " + std::to_string(m_passedUpTo) +
                                                     ", which has been passed over");
    }
    const std::uint64_t want = std::min<std::uint64_t>(out.size(), m_capacityBytes - offset);
    const std::uint64_t end = offset + want;
    std::memset(out.data(), 0, want);
    m_progressOffset = offset;
    keepLeaseAlive(offset);

    for (;;) {
        if (!m_held) {
            auto next = nextGrain();
            if (!next) {
                return std::move(next).error();
            }
            if (!next.value()) {
                m_passedUpTo = end;
                break; // the stream ended: the rest of the disk has no data
            }
            m_held = std::move(next.value());
        }

        const std::uint64_t grainStart = m_held->offset;
        const std::uint64_t grainEnd = grainStart + m_held->data.size();
        if (grainStart >= end) {
            break; // a later read's
        }
        const std::uint64_t from = std::max(grainStart, offset);
        const std::uint64_t to = std::min(grainEnd, end);
        if (from < to) {
            std::memcpy(out.data() + (from - offset), m_held->data.data() + (from - grainStart), to - from);
        }
        if (grainEnd > end) {
            break; // its tail is a later read's
        }
        m_passedUpTo = grainEnd;
        m_held.reset();
    }
    return want;
}

} // namespace VSphereSoap::Detail
