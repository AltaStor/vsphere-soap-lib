// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// HTTPS client for the vSphere SOAP endpoint and datastore transfers

#pragma once

// Minimal synchronous HTTPS client behind every network call this library makes: SOAP envelopes to /sdk,
// ranged datastore GETs, and the long streaming PUT/POST uploads.
// Not a public header.
// Built on Boost.Beast/Boost.Asio so the same code works on Linux, Windows and macOS.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vsphere-soap/Result.hpp"

namespace VSphereSoap::Vim {

enum class HttpMethod {
    Get,
    Post,
    Put,
};

/// Pulled repeatedly by `requestPutStream` to fill the request body as it goes out over the wire: no
/// local buffering of the whole body, which is the entire point of this over `requestPutFile`.
/// Returns the next chunk of bytes to send, or an empty vector to signal "no more body" (the producer
/// itself decides chunk size; each returned chunk becomes one HTTP chunked-encoding chunk on the wire,
/// so very small chunks mean very small wire chunks).
/// A `Result` error here aborts the upload and is propagated to `requestPutStream`'s caller as-is.
///
using HttpBodyChunkProducer = std::function<Result<std::vector<std::byte>>()>;

/// Receives a streamed response body chunk by chunk, in order.
/// A non-ok `Error` stops the transfer and is returned to the caller of `requestGetStream`.
using HttpBodyChunkConsumer = std::function<Error(std::span<const std::byte>)>;

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpResponse {
    unsigned int status = 0;
    std::string body;

    /// Response headers in wire order, names as the server sent them (case preserved, not normalized):
    /// callers that need a specific header (e.g. `Set-Cookie`, `Content-Range`) should compare
    /// case-insensitively.
    /// May contain multiple entries with the same name (e.g. multiple `Set-Cookie` headers).
    ///
    std::vector<HttpHeader> headers;
};

/// Reuses one HTTP/1.1 keep-alive connection across calls to the same `HttpsRestClient` instance
/// (established lazily on the first `request()`), rather than a fresh TCP+TLS handshake per call:
/// `VmwareSoapDisk`'s SOAP-session-then-many-ranged-GETs usage pattern (session open, findVm,
/// describeVm, then one ranged GET per read; potentially hundreds for a large disk) made "new connection
/// per call" both slow (repeated handshake cost) and fragile in practice: rapid-fire new connections to
/// the same host from one source IP is exactly the pattern connection-flood protection on a firewall,
/// hostd, or vCenter's reverse proxy is designed to catch, and it did, in the field, as a `connect()`
/// timeout partway through a real transfer.
/// If a reused connection's next request fails (e.g. the server closed it after its own idle-keepalive
/// timeout, a routine occurrence), one fresh connection is established and retried automatically;
/// a failure on a *fresh* connection is reported as-is, not masked by another retry.
/// Every connect/handshake/write/read individually has a bounded timeout (see the .cpp) rather than
/// relying on the OS's own (often multi-minute) default TCP connect timeout.
///
class HttpsRestClient {
public:
    HttpsRestClient(const std::string& host, std::uint16_t port, bool verifyTlsCertificate);

    /// `pinnedThumbprint`, when non-empty, is a SHA-1 fingerprint of the peer's leaf certificate in
    /// colon-hex form (e.g. "AB:CD:...", the format vSphere/govc/ovftool print), matched instead of
    /// normal certificate-chain verification: for self-signed ESXi/vCenter certs where
    /// `verifyTlsCertificate` can't be turned on but the operator still wants to pin a specific cert
    /// rather than accept any.
    /// Takes effect regardless of `verifyTlsCertificate` (chain verification is skipped whenever
    /// a thumbprint is pinned, since the two are alternative trust mechanisms, not additive ones).
    /// Fails with `ErrorCode::IoError` if the peer's fingerprint doesn't match.
    ///
    HttpsRestClient(const std::string& host, std::uint16_t port, bool verifyTlsCertificate, const std::string& pinnedThumbprint);

    /// Out-of-line (defined where the pimpl'd Connection type is complete).
    /// Does not attempt a clean TLS shutdown of a live connection: see the .cpp for why that's fine
    /// here.
    ~HttpsRestClient();

    HttpsRestClient(const HttpsRestClient&) = delete;
    HttpsRestClient& operator=(const HttpsRestClient&) = delete;
    HttpsRestClient(HttpsRestClient&&) = delete;
    HttpsRestClient& operator=(HttpsRestClient&&) = delete;

    /// Does not throw; fails with `ErrorCode::IoError` on DNS/TCP/TLS/HTTP transport failure.
    Result<HttpResponse> request(HttpMethod method, const std::string& target,
                                                const std::vector<HttpHeader>& headers,
                                                std::string body = {}) const;

    /// Like `request(HttpMethod::Put, ...)`, but streams `localFilePath`'s contents directly as
    /// the request body (Boost.Beast's `file_body`) instead of buffering the whole file into
    /// a `std::string` first; for a multi-GB upload (e.g. a whole virtual disk, to a datastore endpoint
    /// that only accepts a complete-file PUT, not a partial/ranged one), that difference is the whole
    /// point: peak memory stays at Beast's own read-buffer size, not the file size.
    /// `timeout` overrides `request()`'s normal (60s) per-operation timeout, since a multi-GB transfer
    /// legitimately takes far longer; it applies separately to the write and the read phases (so
    /// the true worst case is up to ~2x `timeout`), matching `request()`'s own per-phase timeout shape.
    /// Fails with `ErrorCode::IoError` if `localFilePath` can't be opened, in addition to `request`'s
    /// own failure modes.
    ///
    Result<HttpResponse> requestPutFile(const std::string& target, const std::vector<HttpHeader>& headers,
                                                        const std::string& localFilePath,
                                                        std::chrono::seconds timeout) const;

    /// Like `requestPutFile`, but the body comes from repeatedly calling `producer` rather than reading
    /// a local file: for a body that should never be staged on local disk.
    /// If `contentLength` is `std::nullopt`, sends `Transfer-Encoding: chunked` (no `Content-Length`):
    /// confirmed live, against a real ESXi NFC endpoint, to work fine for a multi-GB body once
    /// the *actual* blocker (an application-level `Overwrite: t` header the endpoint requires regardless
    /// of transfer encoding) was found and added by the caller; an earlier round of live testing had
    /// blamed chunked encoding itself for a connection reset ("An existing connection was forcibly
    /// closed by the remote host") that turned out to be that same missing header: the server rejects
    /// the request but the reset arrives while a large body is still being written, which looks
    /// identical to a transport-level chunked-encoding rejection from the client side until
    /// an independent tool (curl, with a small body) exposes the real HTTP 403 underneath.
    /// If a caller separately knows (or can precompute) the exact total up front, `contentLength` sends
    /// a plain non-chunked request with that `Content-Length` and no `Transfer-Encoding` header instead:
    /// also confirmed live to work end-to-end for a multi-GB body.
    /// `producer`'s total output must equal `*contentLength` exactly when one is given: neither more nor
    /// less; this is not re-verified against what `producer` actually produces; getting it wrong desyncs
    /// the connection (a short body hangs, a long one corrupts the next response on a reused
    /// connection).
    /// `timeout` bounds each individual network write/read the same way it does for `requestPutFile`;
    /// a slow `producer` call itself is not bounded by it (the producer's own caller is expected to
    /// apply whatever timeout makes sense for wherever its bytes come from).
    ///
    Result<HttpResponse> requestPutStream(const std::string& target,
                                                          const std::vector<HttpHeader>& headers,
                                                          HttpBodyChunkProducer producer, std::chrono::seconds timeout,
                                                          std::optional<std::uint64_t> contentLength = std::nullopt) const;

    /// `requestPutStream()`, but sent as POST; what an ESXi HttpNfcLease *import* device URL needs for
    /// a disk: POST streams into the disk ImportVApp already created (keeping its thin/thick
    /// provisioning), while PUT replaces that file outright and loses it.
    Result<HttpResponse> requestPostStream(const std::string& target,
                                                           const std::vector<HttpHeader>& headers,
                                                           HttpBodyChunkProducer producer, std::chrono::seconds timeout,
                                                           std::optional<std::uint64_t> contentLength = std::nullopt) const;

    /// GET whose body is handed to `consumer` as it arrives instead of being buffered, for a body larger
    /// than memory such as an export lease's disk stream.
    /// Only a 2xx body is streamed; any other status comes back in `HttpResponse::body` as usual.
    /// The streamed response's `body` is empty.
    /// `timeout` bounds each network read, not the whole transfer.
    /// Uses a connection of its own, closed afterwards, since a stopped transfer leaves it unusable.
    ///
    Result<HttpResponse> requestGetStream(const std::string& target, const std::vector<HttpHeader>& headers,
                                          const HttpBodyChunkConsumer& consumer, std::chrono::seconds timeout) const;

private:
    /// Shared body of `requestPutStream()`/`requestPostStream()`.
    Result<HttpResponse> requestStream(HttpMethod method, const std::string& target,
                                                       const std::vector<HttpHeader>& headers,
                                                       HttpBodyChunkProducer producer, std::chrono::seconds timeout,
                                                       std::optional<std::uint64_t> contentLength) const;

    struct Connection;

    /// Establishes a fresh TCP+TLS connection (DNS resolve, connect, handshake, and thumbprint check if
    /// pinned).
    Result<std::unique_ptr<Connection>> connect() const;

    /// Writes `req` and reads one response over an already-connected `conn`, without shutting it down
    /// afterwards (left open for reuse).
    Result<HttpResponse> requestOnce(Connection& conn, HttpMethod method, const std::string& target,
                                                    const std::vector<HttpHeader>& headers, std::string body) const;

    /// `requestOnce`'s file-streaming sibling, backing `requestPutFile()`.
    Result<HttpResponse> requestOnceFile(Connection& conn, const std::string& target,
                                                        const std::vector<HttpHeader>& headers,
                                                        const std::string& localFilePath,
                                                        std::chrono::seconds timeout) const;

    /// `requestOnceFile`'s streaming-producer sibling, backing `requestPutStream()`.
    Result<HttpResponse> requestOnceStream(Connection& conn, HttpMethod method, const std::string& target,
                                                          const std::vector<HttpHeader>& headers,
                                                          const HttpBodyChunkProducer& producer,
                                                          std::chrono::seconds timeout,
                                                          std::optional<std::uint64_t> contentLength) const;

    std::string m_host;
    std::uint16_t m_port;
    bool m_verifyTlsCertificate;
    std::string m_pinnedThumbprint;
    /// Lazily established; reused across request() calls.
    mutable std::unique_ptr<Connection> m_connection;
};

} // namespace VSphereSoap::Vim
