// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere datastore streaming upload target via HttpNfcLease

#include "VSphereDatastoreBackend.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>

#include "GrainAssembler.hpp"
#include "HttpTransportAdapter.hpp"
#include "SoapLog.hpp"
#include "StreamOptimizedVmdkEncoder.hpp"
#include "Error.hpp"

namespace VSphereSoap {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;
using VSphereSoap::Result;
using Detail::HttpHeader;
using Detail::HttpResponse;
using Detail::GrainAssembler;
using Detail::HttpTransportAdapter;
using Detail::StreamOptimizedVmdkEncoder;
using Detail::VmdkGrain;

namespace {

using Detail::logger;

// A batched HTTP chunk targets roughly this size before being handed to the upload thread: large enough
// that a multi-GB transfer isn't thousands of tiny TCP writes (one per grain marker the encoder emits),
// small enough that writeAt() callers see reasonably prompt backpressure instead of buffering a huge
// amount of not-yet-sent data in memory if the network is slower than the disk-copy loop.
//
constexpr std::size_t kTargetHttpChunkBytes = 1024 * 1024;

// How many already-batched HTTP chunks may sit in the handoff queue before writeAt() blocks: bounds this
// backend's own memory use to roughly kMaxQueuedChunks * kTargetHttpChunkBytes regardless of how far
// ahead the disk-copy loop gets of the network.
constexpr std::size_t kMaxQueuedChunks = 4;

// How long the upload may go without handing the upload thread anything before keepAliveIfIdle() sends
// something: a third of the ~60 s ESXi tolerates (confirmed live: 45 s of silence is accepted, 70 s
// aborts the connection), leaving room for a slow source read between two checks.
constexpr std::chrono::seconds kIdleKeepAliveAfter{20};

struct ParsedHttpsUrl {
    std::string host;
    std::uint16_t port = 443;
    std::string target; // path + optional "?query", always at least "/"
};

/// Minimal parser for exactly the shape an HttpNfcLease device URL takes:
/// "https://host[:port]/path[?query]".
/// Not a general URL parser: no userinfo, no fragment, scheme must be "https".
Result<ParsedHttpsUrl> parseHttpsUrl(const std::string& url)
{
    constexpr std::string_view kScheme = "https://";
    if (url.compare(0, kScheme.size(), kScheme) != 0) {
        return Error(ErrorCode::InvalidArgument, "VSphereDatastoreBackend: deviceUrl must start with 'https://': '" +
                                                       url + "'");
    }
    const std::size_t authorityStart = kScheme.size();
    const std::size_t pathStart = url.find('/', authorityStart);
    const std::string authority =
        url.substr(authorityStart, pathStart == std::string::npos ? std::string::npos : pathStart - authorityStart);
    if (authority.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereDatastoreBackend: deviceUrl has no host: '" + url + "'");
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
            return Error(ErrorCode::InvalidArgument,
                         "VSphereDatastoreBackend: deviceUrl has an invalid port: '" + url + "'");
        }
    }
    if (parsed.host.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereDatastoreBackend: deviceUrl has an empty host: '" + url + "'");
    }
    parsed.target = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    return parsed;
}

} // namespace

class VSphereDatastoreBackend::Impl {
public:
    std::unique_ptr<HttpTransportAdapter> m_transport;
    std::string m_putTarget;
    std::uint64_t m_virtualSizeBytes = 0;
    std::uint64_t m_grainSizeBytes = 0;
    std::chrono::seconds m_perOperationTimeout{300};

    std::optional<StreamOptimizedVmdkEncoder> m_encoder;
    // Turns writeAt()-shaped input into exactly-grain-sized writeGrain() calls: see GrainAssembler.hpp
    // for why this logic lives there instead of inline here.
    // Touched only from the writeAt()/flush() caller's thread, no locking needed.
    std::optional<GrainAssembler> m_grainAssembler;

    std::vector<std::byte> m_httpChunkBuffer; // encoder sink output, batched before queueing

    // keepAliveIfIdle() state: caller's thread only, like the above.
    bool m_sparse = false;          // compressed (chunked) mode: all-zero grains are omitted
    bool m_forceZeroGrain = false;  // set around keepAliveIfIdle()'s one deliberately-sent zero grain
    std::chrono::steady_clock::time_point m_lastPushAt = std::chrono::steady_clock::now();

    // --- handoff to the background upload thread ---
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::vector<std::byte>> m_queue;
    bool m_eofRequested = false;
    bool m_aborted = false;
    bool m_uploadThreadDone = false;
    Result<HttpResponse> m_uploadResult = Error(ErrorCode::IoError, "upload thread has not finished yet");
    std::thread m_uploadThread;

    bool m_flushed = false;

    // Called from writeAt()/flush() (the caller's thread) with a fully batched chunk ready to go out
    // over the wire.
    // Blocks under backpressure; returns an error immediately (without blocking forever) if the upload
    // thread has already given up.
    //
    Error pushChunk(std::vector<std::byte> chunk)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_queue.size() < kMaxQueuedChunks || m_uploadThreadDone; });
        if (m_uploadThreadDone) {
            return m_uploadResult ? Error(ErrorCode::IoError, "VSphereDatastoreBackend: upload already finished; "
                                                              "cannot accept more data")
                                 : m_uploadResult.error();
        }
        m_queue.push_back(std::move(chunk));
        m_cv.notify_all();
        m_lastPushAt = std::chrono::steady_clock::now();
        return {};
    }

    // Called from the background upload thread (via requestPutStream's producer callback) whenever it
    // needs the next body chunk.
    Result<std::vector<std::byte>> pullChunk()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return !m_queue.empty() || m_eofRequested || m_aborted; });
        if (m_aborted) {
            return Error(ErrorCode::Cancelled, "VSphereDatastoreBackend: upload aborted (destroyed without a "
                                                 "successful flush())");
        }
        if (!m_queue.empty()) {
            auto chunk = std::move(m_queue.front());
            m_queue.pop_front();
            m_cv.notify_all();
            return chunk;
        }
        return std::vector<std::byte>{}; // eofRequested, queue drained: signal end-of-body
    }

    // The encoder's sink: appends to httpChunkBuffer, flushing it to the queue once it reaches
    // the batching target.
    Error onEncoderBytes(std::span<const std::byte> bytes)
    {
        m_httpChunkBuffer.insert(m_httpChunkBuffer.end(), bytes.begin(), bytes.end());
        if (m_httpChunkBuffer.size() >= kTargetHttpChunkBytes) {
            return pushChunk(std::exchange(m_httpChunkBuffer, std::vector<std::byte>{}));
        }
        return {};
    }

};

std::uint64_t roundUpToGrain(std::uint64_t virtualSizeBytes, std::uint64_t grainSizeSectors)
{
    const std::uint64_t grainBytes = grainSizeSectors * StreamOptimizedVmdkEncoder::kSectorSizeBytes;
    return (virtualSizeBytes + grainBytes - 1) / grainBytes * grainBytes;
}

std::string makeStreamOptimizedDescriptor(std::uint64_t capacityBytes, const std::string& fileName,
                                          const std::string& adapterType)
{
    const std::uint64_t sectors = capacityBytes / StreamOptimizedVmdkEncoder::kSectorSizeBytes;
    // The usual 255 heads x 63 sectors translation; only legacy BIOS paths ever read it.
    const std::uint64_t cylinders = std::max<std::uint64_t>(1, sectors / (255 * 63));
    return "# Disk DescriptorFile\n"
           "version=1\n"
           "CID=fffffffe\n"
           "parentCID=ffffffff\n"
           "createType=\"streamOptimized\"\n"
           "\n"
           "# Extent description\n"
           "RW " + std::to_string(sectors) + " SPARSE \"" + fileName + "\"\n"
           "\n"
           "# The Disk Data Base\n"
           "#DDB\n"
           "\n"
           "ddb.adapterType = \"" + adapterType + "\"\n"
           "ddb.geometry.cylinders = \"" + std::to_string(cylinders) + "\"\n"
           "ddb.geometry.heads = \"255\"\n"
           "ddb.geometry.sectors = \"63\"\n";
}

VSphereDatastoreBackend::VSphereDatastoreBackend(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

VSphereDatastoreBackend::~VSphereDatastoreBackend()
{
    if (!m_impl) {
        // moved-from Impl pointer can't happen (class is non-movable), but open() failure paths return
        // early
        return;
    }
    if (!m_impl->m_flushed) {
        {
            std::lock_guard<std::mutex> lock(m_impl->m_mutex);
            m_impl->m_aborted = true;
            m_impl->m_cv.notify_all();
        }
    }
    if (m_impl->m_uploadThread.joinable()) {
        m_impl->m_uploadThread.join();
    }
}

Result<std::unique_ptr<VSphereDatastoreBackend>> VSphereDatastoreBackend::open(VSphereDatastoreBackendParams params)
{
    if (params.deviceUrl.empty()) {
        return Error(ErrorCode::InvalidArgument, "VSphereDatastoreBackend::open: deviceUrl is required");
    }
    auto urlResult = parseHttpsUrl(params.deviceUrl);
    if (!urlResult) {
        return std::move(urlResult).error();
    }
    const ParsedHttpsUrl& url = urlResult.value();

    // Confirmed live against a real ESXi HttpNfcLease endpoint: an uncompressed streamOptimized-shaped
    // VMDK is rejected outright once fully uploaded (HTTP 500 "Not a supported disk format (uncompressed
    // disk)"); compression is not optional here, whatever this class's own earlier doc comments assumed.
    // That forces the two modes below: compressed output size is data-dependent, so there is no
    // Content-Length to precompute for it, and sparse (not dense) encoding is both possible again
    // (nothing forces every grain to be written anymore) and worth having back, since it's genuinely
    // smaller.
    // Chunked transfer encoding, once blamed for this endpoint resetting the connection, turned out not
    // to be the culprit at all; that reset was this same "Overwrite: t" header being missing (see
    // the upload thread below); a real 10GB Content-Length upload with it present completed cleanly, so
    // chunked was never actually proven broken.
    // It's used here for the compressed path simply because there's no alternative, not because it's
    // independently confirmed: worth retesting explicitly if this ever needs revisiting.
    //
    std::optional<std::uint64_t> contentLength;
    if (!params.compressGrains) {
        // Must be computed before a single real byte is sent: Content-Length has to be in the PUT's
        // headers, which go out before the body.
        // Reuses the real encoder (via a dry run) rather than re-deriving its sector arithmetic
        // separately: see computeDenseStreamSizeBytes()'s own doc comment.
        // Real uploads always set compressGrains = true, per the finding above; this mode is kept
        // working and tested as a documented alternative for an endpoint that does accept uncompressed
        // content, should one ever be used through this class.
        //
        auto contentLengthResult =
            StreamOptimizedVmdkEncoder::computeDenseStreamSizeBytes(params.virtualSizeBytes, params.vmdkDescriptor,
                                                                        params.grainSizeSectors);
        if (!contentLengthResult) {
            return std::move(contentLengthResult).error();
        }
        contentLength = contentLengthResult.value();
    }

    auto impl = std::make_unique<Impl>();
    impl->m_transport = std::make_unique<HttpTransportAdapter>(url.host, url.port, params.verifyTlsCertificate,
                                                                params.pinnedThumbprint);
    impl->m_putTarget = url.target;
    impl->m_virtualSizeBytes = params.virtualSizeBytes;
    impl->m_grainSizeBytes = params.grainSizeSectors * StreamOptimizedVmdkEncoder::kSectorSizeBytes;
    impl->m_perOperationTimeout = params.perOperationTimeout;
    impl->m_sparse = params.compressGrains;

    auto sink = [rawImplForSink = impl.get()](std::span<const std::byte> bytes) -> Error {
        return rawImplForSink->onEncoderBytes(bytes);
    };
    auto encoderResult = StreamOptimizedVmdkEncoder::create(std::move(sink), params.virtualSizeBytes,
                                                                params.vmdkDescriptor, params.grainSizeSectors,
                                                                params.compressGrains);
    if (!encoderResult) {
        return std::move(encoderResult).error();
    }
    impl->m_encoder.emplace(std::move(encoderResult).value());
    // denseCapacityBytes only set (forcing every grain through, gaps zero-filled) in
    // the uncompressed/Content-Length mode, where a sparse-skipped gap would make the already-committed
    // `contentLength` wrong: see this function's own comment above.
    // In compressed/chunked mode there's no such constraint, so this reverts to GrainAssembler's default
    // sparse behavior (a real efficiency win: an all-zero region costs nothing on the wire, not even
    // a compressed placeholder).
    //
    // In that sparse mode an all-zero grain is dropped here, per the encoder's own contract
    // (StreamOptimizedVmdkEncoder: "an all-zero grain is simply omitted").
    // A copy loop only skips chunks that are zero end to end, so a chunk with any data in it arrives
    // here with its zero grains too; sending those makes ESXi physically write every such block, which
    // turns a lazy-zeroed thick target into one vCenter reports as eager-zeroed (confirmed live) and
    // inflates the upload for nothing.
    //
    impl->m_grainAssembler.emplace(
        impl->m_grainSizeBytes,
        [rawImplForGrains = impl.get()](std::uint64_t grainOffset, std::span<const std::byte> grainData) -> Error {
            if (rawImplForGrains->m_sparse && !rawImplForGrains->m_forceZeroGrain &&
                std::all_of(grainData.begin(), grainData.end(), [](std::byte b) { return b == std::byte{0}; })) {
                return {};
            }
            return rawImplForGrains->m_encoder->writeGrain(VmdkGrain{grainOffset, grainData});
        },
        params.compressGrains ? std::nullopt : std::optional<std::uint64_t>(params.virtualSizeBytes));

    logger()->info(
        "opened datastore backend for {}:{}{} ({} bytes logical, {}, grain={} sectors, compress={})", url.host,
        url.port, url.target, params.virtualSizeBytes,
        contentLength ? std::to_string(*contentLength) + " bytes on the wire" : std::string("chunked transfer"),
        params.grainSizeSectors, params.compressGrains);

    // Push the header+descriptor bytes the encoder already wrote into the queue right now, before
    // the upload thread even connects, rather than waiting for kTargetHttpChunkBytes to accumulate:
    // confirmed live against a real ESXi NFC endpoint that it resets an otherwise-idle connection well
    // under a second after the request headers if no body bytes have arrived yet.
    // The first *real* grain data can easily take longer than that to become ready (a whole disk-copy
    // chunk has to be read from the source and encoded before writeAt() ever calls this class), so this
    // small priming write exists purely to keep the connection alive while that happens; it is not
    // a batching decision, just a "don't let the pipe look idle" one.
    //
    if (!impl->m_httpChunkBuffer.empty()) {
        if (auto result = impl->pushChunk(std::exchange(impl->m_httpChunkBuffer, std::vector<std::byte>{}));
            !result.ok()) {
            return result;
        }
    }

    Impl* rawImpl = impl.get();
    const std::chrono::seconds timeout = impl->m_perOperationTimeout;
    impl->m_uploadThread = std::thread([rawImpl, timeout, contentLength] {
        // POST, not PUT.
        // ImportVApp has already created the target disk (thin, lazy- or eager-zeroed thick, per
        // CreateImportSpec's diskProvisioning) by the time the lease is ready: POST streams the VMDK
        // *into* that disk, keeping its provisioning, the way ovftool uploads.
        // PUT instead *replaces* the file; it needs "Overwrite: t" just to be accepted (HTTP 403 "File
        // exists, but overwrite was not requested" otherwise) and the replacement comes out thick no
        // matter what was requested.
        // Confirmed live against ESXi 8.0.3: the same stream-optimized VMDK into the same thin import
        // spec stays thin via POST and becomes thick via PUT + Overwrite.
        //
        std::vector<HttpHeader> headers{{"Content-Type", "application/x-vnd.vmware-streamVmdk"}};
        auto producer = [rawImpl]() -> Result<std::vector<std::byte>> { return rawImpl->pullChunk(); };
        auto result = rawImpl->m_transport->requestPostStream(rawImpl->m_putTarget, headers, std::move(producer), timeout,
                                                              contentLength);

        std::lock_guard<std::mutex> lock(rawImpl->m_mutex);
        rawImpl->m_uploadResult = std::move(result);
        rawImpl->m_uploadThreadDone = true;
        rawImpl->m_cv.notify_all();
    });

    return std::unique_ptr<VSphereDatastoreBackend>(new VSphereDatastoreBackend(std::move(impl)));
}

Result<std::uint64_t> VSphereDatastoreBackend::readAt(std::uint64_t /*offset*/, std::span<std::byte> /*out*/)
{
    return Error(ErrorCode::NotImplemented, "VSphereDatastoreBackend is write-only");
}

Result<std::uint64_t> VSphereDatastoreBackend::writeAt(std::uint64_t offset, std::span<const std::byte> in)
{
    if (m_impl->m_flushed) {
        return Error(ErrorCode::InvalidArgument, "VSphereDatastoreBackend::writeAt called after flush()");
    }
    if (in.empty()) {
        return 0;
    }
    if (offset + in.size() > m_impl->m_virtualSizeBytes) {
        return Error(ErrorCode::InvalidArgument, "VSphereDatastoreBackend::writeAt: range [" +
                                                       std::to_string(offset) + ", " +
                                                       std::to_string(offset + in.size()) +
                                                       ") exceeds virtualSize " +
                                                       std::to_string(m_impl->m_virtualSizeBytes));
    }
    if (auto result = m_impl->m_grainAssembler->consume(offset, in); !result.ok()) {
        return result;
    }
    return in.size();
}

std::uint64_t VSphereDatastoreBackend::virtualSize() const noexcept { return m_impl->m_virtualSizeBytes; }
std::uint64_t VSphereDatastoreBackend::sectorSize() const noexcept { return StreamOptimizedVmdkEncoder::kSectorSizeBytes; }

Error VSphereDatastoreBackend::keepAliveIfIdle(std::uint64_t knownFinalUpTo)
{
    if (m_impl->m_flushed || !m_impl->m_sparse ||
        std::chrono::steady_clock::now() - m_impl->m_lastPushAt < kIdleKeepAliveAfter) {
        return {};
    }
    // Encoded bytes still waiting for the batching threshold go first: they're real stream content, just
    // not sent yet.
    if (!m_impl->m_httpChunkBuffer.empty()) {
        return m_impl->pushChunk(std::exchange(m_impl->m_httpChunkBuffer, std::vector<std::byte>{}));
    }
    // Otherwise one zero grain from [nextExpectedOffset, knownFinalUpTo): nothing was written there
    // (the disk-copy loop skipped it as zero), and nothing ever will be, since writeAt() offsets only
    // increase; so it can't collide with a later real grain, and keeps the encoder's ascending-offset
    // contract.
    //
    const std::uint64_t grain = m_impl->m_grainSizeBytes;
    const std::uint64_t start = (m_impl->m_grainAssembler->nextExpectedOffset() + grain - 1) / grain * grain;
    if (start + grain > knownFinalUpTo || start + grain > m_impl->m_virtualSizeBytes) {
        return {};
    }
    logger()->debug("upload idle for {}s; sending a zero grain at offset {} to keep the connection alive",
                     kIdleKeepAliveAfter.count(), start);
    const std::vector<std::byte> zeroGrain(grain, std::byte{0});
    m_impl->m_forceZeroGrain = true;
    Error consumed = m_impl->m_grainAssembler->consume(start, zeroGrain);
    m_impl->m_forceZeroGrain = false;
    if (!consumed.ok()) {
        return consumed;
    }
    if (m_impl->m_httpChunkBuffer.empty()) {
        return {};
    }
    return m_impl->pushChunk(std::exchange(m_impl->m_httpChunkBuffer, std::vector<std::byte>{}));
}

Error VSphereDatastoreBackend::flush()
{
    if (m_impl->m_flushed) {
        return {};
    }
    m_impl->m_flushed = true;

    Error pendingResult = m_impl->m_grainAssembler->finish();
    Error finishResult = m_impl->m_encoder->finish();
    Error tailResult =
        m_impl->m_httpChunkBuffer.empty() ? Error{} : m_impl->pushChunk(std::move(m_impl->m_httpChunkBuffer));

    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_eofRequested = true;
        m_impl->m_cv.notify_all();
    }
    if (m_impl->m_uploadThread.joinable()) {
        m_impl->m_uploadThread.join();
    }

    if (!pendingResult.ok()) {
        return pendingResult;
    }
    if (!finishResult.ok()) {
        return finishResult;
    }
    if (!tailResult.ok()) {
        return tailResult;
    }
    if (!m_impl->m_uploadResult) {
        return m_impl->m_uploadResult.error();
    }
    const HttpResponse& response = m_impl->m_uploadResult.value();
    if (response.status < 200 || response.status >= 300) {
        return Error(ErrorCode::IoError, "VSphereDatastoreBackend::flush: datastore PUT to " + m_impl->m_putTarget +
                                              " failed: HTTP " + std::to_string(response.status) + ": " +
                                              response.body);
    }
    logger()->info("datastore upload to {} completed: HTTP {}", m_impl->m_putTarget, response.status);
    return {};
}

} // namespace VSphereSoap
