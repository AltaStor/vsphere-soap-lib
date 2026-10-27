// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere folder-put flat VMDK streaming upload target

#include "VSphereFolderPutBackend.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "HttpTransportAdapter.hpp"
#include "SoapLog.hpp"
#include "Error.hpp"

namespace VSphereSoap {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;
using VSphereSoap::Result;
using Detail::HttpHeader;
using Detail::HttpResponse;
using Detail::HttpTransportAdapter;

namespace {

using Detail::logger;

// Same batching/backpressure constants as VSphereDatastoreBackend.cpp, for the same reasons (bounded
// memory regardless of how far ahead of the network writeAt() callers get), not shared via a common
// header since each class's Impl has its own unrelated fields around them; duplicating two constants is
// cheaper to read than a shared-state header for this.
//
constexpr std::size_t kTargetHttpChunkBytes = 1024 * 1024;
constexpr std::size_t kMaxQueuedChunks = 4;

struct ParsedHttpsUrl {
    std::string host;
    std::uint16_t port = 443;
    std::string target;
};

/// Same minimal "https://host[:port]/path[?query]" parser as VSphereDatastoreBackend.cpp's own (private,
/// anonymous-namespace) parseHttpsUrl: duplicated rather than shared for the reason given above this
/// namespace's own comment.
Result<ParsedHttpsUrl> parseHttpsUrl(const std::string& url)
{
    constexpr std::string_view kScheme = "https://";
    if (url.compare(0, kScheme.size(), kScheme) != 0) {
        return Error(ErrorCode::InvalidArgument,
                     "VSphereFolderPutBackend: putUrl must start with 'https://': '" + url + "'");
    }
    const std::size_t authorityStart = kScheme.size();
    const std::size_t pathStart = url.find('/', authorityStart);
    const std::string authority =
        url.substr(authorityStart, pathStart == std::string::npos ? std::string::npos : pathStart - authorityStart);
    if (authority.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutBackend: putUrl has no host: '" + url + "'");
    }

    ParsedHttpsUrl parsed;
    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        parsed.host = authority;
    } else {
        parsed.host = authority.substr(0, colon);
        try {
            parsed.port = static_cast<std::uint16_t>(std::stoul(authority.substr(colon + 1)));
        } catch (const std::exception&) {
            return Error(ErrorCode::InvalidArgument, "VSphereFolderPutBackend: putUrl has an invalid port: '" + url + "'");
        }
    }
    if (parsed.host.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutBackend: putUrl has an empty host: '" + url + "'");
    }
    parsed.target = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    return parsed;
}

} // namespace

class VSphereFolderPutBackend::Impl {
public:
    std::unique_ptr<HttpTransportAdapter> m_transport;
    std::string m_putTarget;
    std::string m_sessionCookie;
    std::uint64_t m_virtualSizeBytes = 0;
    std::chrono::seconds m_perOperationTimeout{300};

    // Next byte offset this class has committed to the wire (queued, not necessarily sent yet): a flat
    // extent has no grain/sector alignment requirement of its own, so this is a plain running cursor,
    // not GrainAssembler's grain-aligned bookkeeping (that machinery solves a problem (fixed-size VMDK
    // grain markers) that doesn't exist for a raw byte-for-byte copy).
    //
    std::uint64_t m_nextOffset = 0;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::vector<std::byte>> m_queue;
    bool m_eofRequested = false;
    bool m_aborted = false;
    bool m_uploadThreadDone = false;
    Result<HttpResponse> m_uploadResult = Error(ErrorCode::IoError, "upload thread has not finished yet");
    std::thread m_uploadThread;

    bool m_flushed = false;

    Error pushChunk(std::vector<std::byte> chunk)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_queue.size() < kMaxQueuedChunks || m_uploadThreadDone; });
        if (m_uploadThreadDone) {
            return m_uploadResult ? Error(ErrorCode::IoError, "VSphereFolderPutBackend: upload already finished; "
                                                              "cannot accept more data")
                                 : m_uploadResult.error();
        }
        m_queue.push_back(std::move(chunk));
        m_cv.notify_all();
        return {};
    }

    Result<std::vector<std::byte>> pullChunk()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return !m_queue.empty() || m_eofRequested || m_aborted; });
        if (m_aborted) {
            return Error(ErrorCode::Cancelled, "VSphereFolderPutBackend: upload aborted (destroyed without a "
                                                 "successful flush())");
        }
        if (!m_queue.empty()) {
            auto chunk = std::move(m_queue.front());
            m_queue.pop_front();
            m_cv.notify_all();
            return chunk;
        }
        return std::vector<std::byte>{};
    }

    // Pushes `bytes` to the upload queue immediately, split into at-most-kTargetHttpChunkBytes pieces:
    // deliberately *not* batched up to that size the way VSphereDatastoreBackend's own onEncoderBytes()
    // is (there, batching reduces how many tiny ~64KiB grain-marker-sized fragments become separate wire
    // writes; here, writeAt() already delivers whole copy-loop chunks, up to tens of MiB each, so
    // there's no small-fragment problem to solve).
    // Confirmed live that waiting to accumulate a full batch before pushing anything is actively harmful
    // for a small disk: a disk under one batch's worth of bytes would sit with literally nothing pushed
    // to the wire until flush(); the upload thread's connection sits open and provably idle for as long
    // as that takes, and a real vCenter resets it ("An existing connection was forcibly closed by
    // the remote host") well under the ~1s gap that produced in testing.
    // Pushing immediately keeps that gap to whatever pushChunk()'s own locking costs, not "however long
    // until this disk's next MiB of data exists".
    //
    Error appendBytes(std::span<const std::byte> bytes)
    {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t n = std::min(bytes.size() - offset, kTargetHttpChunkBytes);
            std::vector<std::byte> piece(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                           bytes.begin() + static_cast<std::ptrdiff_t>(offset + n));
            if (auto result = pushChunk(std::move(piece)); !result.ok()) {
                return result;
            }
            offset += n;
        }
        return {};
    }

    // Zero-fills [nextOffset, targetOffset) in bounded batches (never materializing the whole gap as one
    // buffer, however large) and advances nextOffset to targetOffset.
    Error zeroFillTo(std::uint64_t targetOffset)
    {
        static constexpr std::uint64_t kZeroBatchBytes = 1024 * 1024;
        while (m_nextOffset < targetOffset) {
            const std::uint64_t remaining = targetOffset - m_nextOffset;
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kZeroBatchBytes));
            const std::vector<std::byte> zeros(n, std::byte{0});
            if (auto result = appendBytes(zeros); !result.ok()) {
                return result;
            }
            m_nextOffset += n;
        }
        return {};
    }
};

VSphereFolderPutBackend::VSphereFolderPutBackend(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

VSphereFolderPutBackend::~VSphereFolderPutBackend()
{
    if (!m_impl) {
        return;
    }
    if (!m_impl->m_flushed) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_aborted = true;
        m_impl->m_cv.notify_all();
    }
    if (m_impl->m_uploadThread.joinable()) {
        m_impl->m_uploadThread.join();
    }
}

Result<std::unique_ptr<VSphereFolderPutBackend>> VSphereFolderPutBackend::open(VSphereFolderPutBackendParams params)
{
    if (params.putUrl.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutBackend::open: putUrl is required");
    }
    if (params.virtualSizeBytes == 0) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutBackend::open: virtualSizeBytes must be nonzero");
    }
    auto urlResult = parseHttpsUrl(params.putUrl);
    if (!urlResult) {
        return std::move(urlResult).error();
    }
    const ParsedHttpsUrl& url = urlResult.value();

    auto impl = std::make_unique<Impl>();
    impl->m_transport = std::make_unique<HttpTransportAdapter>(url.host, url.port, params.verifyTlsCertificate,
                                                                params.pinnedThumbprint);
    impl->m_putTarget = url.target;
    impl->m_sessionCookie = params.sessionCookie;
    impl->m_virtualSizeBytes = params.virtualSizeBytes;
    impl->m_perOperationTimeout = params.perOperationTimeout;

    logger()->info("opened folder-put backend for {}:{}{} ({} bytes, Content-Length known up front)", url.host,
                    url.port, url.target, params.virtualSizeBytes);

    Impl* rawImpl = impl.get();
    const std::chrono::seconds timeout = impl->m_perOperationTimeout;
    const std::uint64_t contentLength = impl->m_virtualSizeBytes;
    impl->m_uploadThread = std::thread([rawImpl, timeout, contentLength] {
        std::vector<HttpHeader> headers{{"Content-Type", "application/octet-stream"}};
        if (!rawImpl->m_sessionCookie.empty()) {
            headers.push_back({"Cookie", rawImpl->m_sessionCookie});
        }
        auto producer = [rawImpl]() -> Result<std::vector<std::byte>> { return rawImpl->pullChunk(); };
        auto result = rawImpl->m_transport->requestPutStream(rawImpl->m_putTarget, headers, std::move(producer), timeout,
                                                               contentLength);

        std::lock_guard<std::mutex> lock(rawImpl->m_mutex);
        rawImpl->m_uploadResult = std::move(result);
        rawImpl->m_uploadThreadDone = true;
        rawImpl->m_cv.notify_all();
    });

    return std::unique_ptr<VSphereFolderPutBackend>(new VSphereFolderPutBackend(std::move(impl)));
}

Result<std::uint64_t> VSphereFolderPutBackend::readAt(std::uint64_t, std::span<std::byte>)
{
    return Error(ErrorCode::NotImplemented, "VSphereFolderPutBackend is write-only");
}

Result<std::uint64_t> VSphereFolderPutBackend::writeAt(std::uint64_t offset, std::span<const std::byte> in)
{
    if (m_impl->m_flushed) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutBackend::writeAt called after flush()");
    }
    if (in.empty()) {
        return 0;
    }
    if (offset < m_impl->m_nextOffset) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutBackend::writeAt: offset " +
                                                       std::to_string(offset) + " is before the current write "
                                                       "cursor " + std::to_string(m_impl->m_nextOffset) +
                                                       " (writes must arrive in non-decreasing offset order)");
    }
    if (offset + in.size() > m_impl->m_virtualSizeBytes) {
        return Error(ErrorCode::InvalidArgument, "VSphereFolderPutBackend::writeAt: range [" +
                                                       std::to_string(offset) + ", " +
                                                       std::to_string(offset + in.size()) +
                                                       ") exceeds virtualSize " +
                                                       std::to_string(m_impl->m_virtualSizeBytes));
    }

    if (offset > m_impl->m_nextOffset) {
        if (auto result = m_impl->zeroFillTo(offset); !result.ok()) {
            return result;
        }
    }
    if (auto result = m_impl->appendBytes(in); !result.ok()) {
        return result;
    }
    m_impl->m_nextOffset += in.size();
    return in.size();
}

std::uint64_t VSphereFolderPutBackend::virtualSize() const noexcept { return m_impl->m_virtualSizeBytes; }
std::uint64_t VSphereFolderPutBackend::sectorSize() const noexcept { return 512; }

Error VSphereFolderPutBackend::flush()
{
    if (m_impl->m_flushed) {
        return {};
    }
    m_impl->m_flushed = true;

    // Zero-fill any remaining tail so the total byte count sent matches virtualSizeBytes exactly, even
    // if the caller's last writeAt() didn't reach the very end (e.g. a trailing all-zero region the copy
    // loop skipped).
    // Nothing else to push here, unlike VSphereDatastoreBackend, appendBytes() never leaves a partial
    // batch sitting unflushed (see its own doc comment).
    //
    Error tailFillResult = m_impl->zeroFillTo(m_impl->m_virtualSizeBytes);

    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_eofRequested = true;
        m_impl->m_cv.notify_all();
    }
    if (m_impl->m_uploadThread.joinable()) {
        m_impl->m_uploadThread.join();
    }

    if (!tailFillResult.ok()) {
        return tailFillResult;
    }
    if (!m_impl->m_uploadResult) {
        return m_impl->m_uploadResult.error();
    }
    const HttpResponse& response = m_impl->m_uploadResult.value();
    if (response.status < 200 || response.status >= 300) {
        return Error(ErrorCode::IoError, "VSphereFolderPutBackend::flush: datastore PUT to " + m_impl->m_putTarget +
                                              " failed: HTTP " + std::to_string(response.status) + ": " +
                                              response.body);
    }
    logger()->info("folder-put upload to {} completed: HTTP {}", m_impl->m_putTarget, response.status);
    return {};
}

} // namespace VSphereSoap
