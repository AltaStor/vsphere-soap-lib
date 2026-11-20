// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// HTTPS client for the vSphere SOAP endpoint and datastore transfers

#include "vsphere-soap/vim/HttpsRestClient.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/file_body.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/optional.hpp>

#include <openssl/x509.h>

#include "SoapLog.hpp"

namespace VSphereSoap::Vim {

using Detail::httpLogger;

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

std::string_view methodName(HttpMethod method)
{
    switch (method) {
    case HttpMethod::Get:
        return "GET";
    case HttpMethod::Post:
        return "POST";
    case HttpMethod::Put:
        return "PUT";
    }
    return "?";
}

using Clock = std::chrono::steady_clock;
double elapsedMs(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Bounded, explicit timeouts rather than relying on the OS's own TCP connect timeout (often
// multi-minute) or an unbounded blocking read.
//
// NOTE: this is deliberately *not* implemented via beast::tcp_stream's own
// expires_after()/synchronous-call timeout support.
// That was tried first and empirically does not bound a synchronous connect()/http::read() on
// an ssl::stream<tcp_stream> in this codebase's usage (verified with a standalone repro against an RFC
// 5737 TEST-NET address that never responds: expires_after(15s) plus a synchronous connect() call
// blocked for well over 30s with no error).
// Instead, every operation below is driven explicitly as async_*, with a steady_timer racing it and
// closing the stream out from under it on expiry, rather than trusting a synchronous convenience API's
// internal timeout wiring a second time without being able to verify it.
//
constexpr std::chrono::seconds kConnectTimeout{15};
constexpr std::chrono::seconds kRequestTimeout{60};

http::verb toBeastVerb(HttpMethod method)
{
    switch (method) {
    case HttpMethod::Get:
        return http::verb::get;
    case HttpMethod::Post:
        return http::verb::post;
    case HttpMethod::Put:
        return http::verb::put;
    }
    return http::verb::get;
}

// Strips ':'/whitespace and upper-cases, so "ab:CD:..." and "ABCD..." compare equal regardless of which
// form the caller pasted in.
std::string normalizeThumbprint(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

// SHA-1 fingerprint of `cert`'s DER encoding, e.g. "AB:CD:EF:...".
std::string sha1Fingerprint(X509* cert)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    if (X509_digest(cert, EVP_sha1(), digest, &digestLen) != 1) {
        return {};
    }
    std::string out;
    out.reserve(digestLen * 3);
    for (unsigned int i = 0; i < digestLen; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X", digest[i]);
        if (i != 0) {
            out.push_back(':');
        }
        out += buf;
    }
    return out;
}

asio::ssl::context makeSslContext(bool verifyChain)
{
    // Built and fully configured before any ssl::stream references it: an ssl::stream's underlying SSL*
    // is created from the context's state at stream-construction time, so
    // verify_mode/default_verify_paths set *after* a stream already exists would silently not apply to
    // it.
    //
    asio::ssl::context ctx(asio::ssl::context::tlsv12_client);
    ctx.set_verify_mode(verifyChain ? asio::ssl::verify_peer : asio::ssl::verify_none);
    if (verifyChain) {
        ctx.set_default_verify_paths();
    }
    return ctx;
}

// Runs exactly one async operation to completion, racing it against `timeout`.
// `starter(handler)` must kick off one async op that calls `handler(ec)` exactly once on completion.
// `ioc` is restart()ed here so the same Connection's io_context can be reused across many of these calls
// (one per connect/handshake/write/read).
// On timeout, force-closes `stream`'s lowest layer, which unblocks the pending op with an error (rather
// than trying to "cancel" it, since that name/behavior is exactly what didn't reliably bound things when
// left to a library's own synchronous wrapper); and returns a timeout error.
//
template <typename Stream, typename Starter>
Error runWithTimeout(asio::io_context& ioc, Stream& stream, std::chrono::seconds timeout,
                             std::string_view opLabel, Starter&& starter)
{
    const auto start = Clock::now();
    ioc.restart();

    // Declared before starter() runs so the operation's own completion handler (below) can cancel it:
    // io_context::run() doesn't return until *every* outstanding handler has run, a pending
    // steady_timer::async_wait() included; without this cancel(), a successful operation would still
    // make run() block for the full `timeout` on every single call, waiting out a timer nothing needs
    // anymore.
    // (This is exactly the bug an earlier version of this function had: it measured correctly in
    // a timeout-reproduction test, because the timeout and "wait for the uncancelled timer" duration
    // were the same number by coincidence, but silently added the full timeout to every *successful*
    // call too.)
    //
    asio::steady_timer timer(ioc);

    boost::system::error_code opEc;
    bool opDone = false;
    starter([&](boost::system::error_code ec) {
        opEc = ec;
        opDone = true;
        timer.cancel();
    });

    timer.expires_after(timeout);
    bool timedOut = false;
    timer.async_wait([&](const boost::system::error_code& timerEc) {
        // timerEc is operation_aborted when the op finished first and cancelled us; that's success, not
        // a timeout.
        if (!timerEc && !opDone) {
            timedOut = true;
            beast::error_code closeEc;
            beast::get_lowest_layer(stream).socket().close(closeEc);
        }
    });

    ioc.run();

    if (timedOut) {
        httpLogger()->warn("{} timed out after {}s", opLabel, timeout.count());
        return Error(ErrorCode::IoError,
                     "operation timed out after " + std::to_string(timeout.count()) + "s");
    }
    if (opEc) {
        httpLogger()->debug("{} failed after {:.1f}ms: {}", opLabel, elapsedMs(start), opEc.message());
        return Error(ErrorCode::IoError, opEc.message());
    }
    httpLogger()->trace("{} completed in {:.1f}ms", opLabel, elapsedMs(start));
    return {};
}

} // namespace

// One TCP+TLS connection, kept alive across HttpsRestClient::request() calls (see the class doc comment
// for why). io_context/ssl_context are held alongside the stream that was built from them, all torn down
// together when a connection is dropped/replaced.
struct HttpsRestClient::Connection {
    asio::io_context ioc;
    asio::ssl::context sslCtx;
    beast::ssl_stream<beast::tcp_stream> stream;

    explicit Connection(bool verifyChain) : sslCtx(makeSslContext(verifyChain)), stream(ioc, sslCtx) {}
};

HttpsRestClient::HttpsRestClient(const std::string& host, std::uint16_t port, bool verifyTlsCertificate)
    : HttpsRestClient(host, port, verifyTlsCertificate, {}) {}

HttpsRestClient::HttpsRestClient(const std::string& host, std::uint16_t port, bool verifyTlsCertificate,
                                  const std::string& pinnedThumbprint)
    : m_host{host}, m_port(port), m_verifyTlsCertificate(verifyTlsCertificate),
      m_pinnedThumbprint(normalizeThumbprint(pinnedThumbprint)) {}

// Defined here, not defaulted in the header: Connection must be a complete type for unique_ptr's
// destructor to be instantiated.
// Deliberately doesn't attempt stream_.shutdown(): a clean TLS close_notify is a nicety, not
// a correctness requirement, and attempting one on a connection that might already be in a bad state
// (the reason we're being destroyed, sometimes) risks throwing/blocking during teardown for no real
// benefit; the peer sees an abrupt close either way, same as a crashed client.
//
HttpsRestClient::~HttpsRestClient() = default;
Result<std::unique_ptr<HttpsRestClient::Connection>> HttpsRestClient::connect() const
{
    const auto start = Clock::now();
    httpLogger()->debug("connecting to {}:{} (verifyTls={}, pinned={})", m_host, m_port, m_verifyTlsCertificate,
                     !m_pinnedThumbprint.empty());
    try {
        const bool verifyChain = m_verifyTlsCertificate && m_pinnedThumbprint.empty();
        auto conn = std::make_unique<Connection>(verifyChain);

        if (!SSL_set_tlsext_host_name(conn->stream.native_handle(), m_host.c_str())) {
            httpLogger()->warn("failed to set TLS SNI hostname for {}", m_host);
            return makeIoError(m_host, 0, "failed to set TLS SNI hostname");
        }

        tcp::resolver resolver(conn->ioc);
        boost::system::error_code resolveEc;
        const auto endpoints = resolver.resolve(m_host, std::to_string(m_port), resolveEc);
        if (resolveEc) {
            httpLogger()->warn("DNS resolve failed for {}:{}: {}", m_host, m_port, resolveEc.message());
            return makeIoError(m_host, 0, "DNS resolve failed: " + resolveEc.message());
        }
        httpLogger()->trace("resolved {}:{} to {} endpoint(s)", m_host, m_port, std::distance(endpoints.begin(), endpoints.end()));

        if (auto result = runWithTimeout(conn->ioc, conn->stream, kConnectTimeout, "TCP connect", [&](auto handler) {
                beast::get_lowest_layer(conn->stream)
                    .async_connect(endpoints, [handler = std::move(handler)](
                                                   const beast::error_code& ec, const tcp::endpoint&) { handler(ec); });
            });
            !result.ok()) {
            httpLogger()->warn("TCP connect to {}:{} failed: {}", m_host, m_port, result.toString());
            return makeIoError(m_host, 0, "connect: " + result.toString());
        }

        if (auto result = runWithTimeout(conn->ioc, conn->stream, kConnectTimeout, "TLS handshake", [&](auto handler) {
                conn->stream.async_handshake(asio::ssl::stream_base::client, std::move(handler));
            });
            !result.ok()) {
            httpLogger()->warn("TLS handshake with {}:{} failed: {}", m_host, m_port, result.toString());
            return makeIoError(m_host, 0, "TLS handshake: " + result.toString());
        }

        if (!m_pinnedThumbprint.empty()) {
            X509* peerCert = SSL_get_peer_certificate(conn->stream.native_handle());
            if (peerCert == nullptr) {
                httpLogger()->warn("TLS thumbprint pinning failed for {}: no peer certificate presented", m_host);
                return Error(ErrorCode::IoError, "TLS thumbprint pinning failed: " + m_host +
                                                      " presented no peer certificate");
            }
            // Compared in the pinned form, without separators.
            const std::string actual = normalizeThumbprint(sha1Fingerprint(peerCert));
            X509_free(peerCert);
            if (actual != m_pinnedThumbprint) {
                httpLogger()->warn("TLS thumbprint mismatch for {}: expected {}, got {}", m_host, m_pinnedThumbprint, actual);
                return Error(ErrorCode::IoError, "TLS thumbprint mismatch for " + m_host + ": expected " +
                                                      m_pinnedThumbprint + ", got " + actual);
            }
        }

        httpLogger()->info("connected to {}:{} in {:.1f}ms", m_host, m_port, elapsedMs(start));
        return conn;
    } catch (const std::exception& e) {
        httpLogger()->warn("HTTPS connect to {}:{} failed: {}", m_host, m_port, e.what());
        return makeIoError(m_host, 0, std::string("HTTPS connect failed: ") + e.what());
    }
}

Result<HttpResponse> HttpsRestClient::requestOnce(Connection& conn, HttpMethod method, const std::string& target,
                                                   const std::vector<HttpHeader>& headers, std::string body) const
{
    const auto start = Clock::now();
    httpLogger()->debug("-> {} {} ({} byte body)", methodName(method), target, body.size());
    try {
        auto req = std::make_shared<http::request<http::string_body>>(toBeastVerb(method), target, 11);
        req->set(http::field::host, m_host);
        req->set(http::field::user_agent, "vsphere-soap-lib");
        for (const auto& header : headers) {
            req->set(header.name, header.value);
        }
        if (!body.empty()) {
            req->body() = std::move(body);
            req->prepare_payload();
        }

        if (auto result = runWithTimeout(conn.ioc, conn.stream, kRequestTimeout, "HTTPS write", [&](auto handler) {
                http::async_write(conn.stream, *req, [req, handler = std::move(handler)](
                                                          const beast::error_code& ec, std::size_t) { handler(ec); });
            });
            !result.ok()) {
            httpLogger()->warn("HTTPS write to {}{} failed: {}", m_host, target, result.toString());
            return makeIoError(m_host, 0, "HTTPS write: " + result.toString());
        }

        // A plain http::response<string_body> read via the free async_read overload is stuck with
        // Beast's built-in 8MB response body_limit, with no way to raise it: fine for the small JSON
        // REST bodies this client was written for, but VmwareSoapDisk's ranged-GET disk reads (chunk
        // size configurable, 32MB by default) blow right through it and fail with "body limit exceeded".
        // Using an explicit response_parser instead exposes body_limit() to override.
        //
        auto buffer = std::make_shared<beast::flat_buffer>();
        auto parser = std::make_shared<http::response_parser<http::string_body>>();
        // trusted endpoint (the caller-supplied vCenter/ESXi host); response size is already bounded by
        // the Range we asked for
        parser->body_limit(boost::none);
        if (auto result = runWithTimeout(conn.ioc, conn.stream, kRequestTimeout, "HTTPS read", [&](auto handler) {
                http::async_read(conn.stream, *buffer, *parser,
                                  [buffer, parser, handler = std::move(handler)](const beast::error_code& ec,
                                                                                  std::size_t) { handler(ec); });
            });
            !result.ok()) {
            httpLogger()->warn("HTTPS read from {}{} failed: {}", m_host, target, result.toString());
            return makeIoError(m_host, 0, "HTTPS read: " + result.toString());
        }

        // Deliberately no stream shutdown here; see the class doc comment: this connection stays open
        // for the next request().

        auto res = parser->release();
        HttpResponse response;
        response.status = res.result_int();
        response.body = std::move(res.body());
        response.headers.reserve(std::distance(res.begin(), res.end()));
        for (const auto& field : res) {
            response.headers.push_back({std::string(field.name_string()), std::string(field.value())});
        }
        httpLogger()->debug("<- {} {} ({} bytes) in {:.1f}ms", response.status, target, response.body.size(),
                         elapsedMs(start));
        return response;
    } catch (const std::exception& e) {
        httpLogger()->warn("HTTPS request {} {} failed: {}", methodName(method), target, e.what());
        return makeIoError(m_host, 0, std::string("HTTPS request failed: ") + e.what());
    }
}

Result<HttpResponse> HttpsRestClient::request(HttpMethod method, const std::string& target,
                                               const std::vector<HttpHeader>& headers, std::string body) const
{
    if (m_connection) {
        httpLogger()->trace("reusing existing connection to {}:{}", m_host, m_port);
        auto result = requestOnce(*m_connection, method, target, headers, body);
        if (result) {
            return result;
        }
        // A reused connection can legitimately fail if the server closed it (its own idle keep-alive
        // timeout, or just because it felt like it) between our calls: routine, not an error worth
        // surfacing.
        // Drop it and fall through to connect a fresh one, exactly like the no-connection-yet case
        // below.
        //
        httpLogger()->info("reused connection to {}:{} failed ({}), reconnecting", m_host, m_port,
                        result.error().toString());
        m_connection.reset();
    }

    auto connectResult = connect();
    if (!connectResult) {
        return std::move(connectResult).error();
    }
    m_connection = std::move(connectResult).value();

    // A failure here is on a connection that was *just* established: report it as-is rather than masking
    // it with another silent retry.
    return requestOnce(*m_connection, method, target, headers, std::move(body));
}

Result<HttpResponse> HttpsRestClient::requestOnceFile(Connection& conn, const std::string& target,
                                                        const std::vector<HttpHeader>& headers,
                                                        const std::string& localFilePath,
                                                        std::chrono::seconds timeout) const
{
    const auto start = Clock::now();
    try {
        beast::error_code fileEc;
        http::file_body::value_type fileBody;
        fileBody.open(localFilePath.c_str(), beast::file_mode::scan, fileEc);
        if (fileEc) {
            return Error(ErrorCode::IoError, "failed to open '" + localFilePath + "' for upload: " + fileEc.message());
        }
        const auto fileSize = fileBody.size();
        httpLogger()->debug("-> PUT {} ({} byte file body from {})", target, fileSize, localFilePath);

        auto req = std::make_shared<http::request<http::file_body>>(http::verb::put, target, 11, std::move(fileBody));
        req->set(http::field::host, m_host);
        req->set(http::field::user_agent, "vsphere-soap-lib");
        for (const auto& header : headers) {
            req->set(header.name, header.value);
        }
        req->prepare_payload();

        if (auto result = runWithTimeout(conn.ioc, conn.stream, timeout, "HTTPS file PUT write", [&](auto handler) {
                http::async_write(conn.stream, *req, [req, handler = std::move(handler)](
                                                          const beast::error_code& ec, std::size_t) { handler(ec); });
            });
            !result.ok()) {
            httpLogger()->warn("HTTPS file PUT write to {}{} failed: {}", m_host, target, result.toString());
            return makeIoError(m_host, 0, "HTTPS write: " + result.toString());
        }

        auto buffer = std::make_shared<beast::flat_buffer>();
        auto parser = std::make_shared<http::response_parser<http::string_body>>();
        // trusted endpoint; response is just a status/error body, not disk data
        parser->body_limit(boost::none);
        if (auto result = runWithTimeout(conn.ioc, conn.stream, timeout, "HTTPS file PUT read", [&](auto handler) {
                http::async_read(conn.stream, *buffer, *parser,
                                  [buffer, parser, handler = std::move(handler)](const beast::error_code& ec,
                                                                                  std::size_t) { handler(ec); });
            });
            !result.ok()) {
            httpLogger()->warn("HTTPS file PUT read from {}{} failed: {}", m_host, target, result.toString());
            return makeIoError(m_host, 0, "HTTPS read: " + result.toString());
        }

        // Deliberately no stream shutdown here; see the class doc comment: this connection stays open
        // for the next request().

        auto res = parser->release();
        HttpResponse response;
        response.status = res.result_int();
        response.body = std::move(res.body());
        response.headers.reserve(std::distance(res.begin(), res.end()));
        for (const auto& field : res) {
            response.headers.push_back({std::string(field.name_string()), std::string(field.value())});
        }
        httpLogger()->debug("<- {} {} ({} bytes) in {:.1f}ms", response.status, target, response.body.size(),
                         elapsedMs(start));
        return response;
    } catch (const std::exception& e) {
        httpLogger()->warn("HTTPS file PUT {} failed: {}", target, e.what());
        return makeIoError(m_host, 0, std::string("HTTPS file PUT failed: ") + e.what());
    }
}

Result<HttpResponse> HttpsRestClient::requestPutFile(const std::string& target, const std::vector<HttpHeader>& headers,
                                                        const std::string& localFilePath,
                                                        std::chrono::seconds timeout) const
{
    if (m_connection) {
        httpLogger()->trace("reusing existing connection to {}:{}", m_host, m_port);
        auto result = requestOnceFile(*m_connection, target, headers, localFilePath, timeout);
        if (result) {
            return result;
        }
        httpLogger()->info("reused connection to {}:{} failed ({}), reconnecting", m_host, m_port,
                        result.error().toString());
        m_connection.reset();
    }

    auto connectResult = connect();
    if (!connectResult) {
        return std::move(connectResult).error();
    }
    m_connection = std::move(connectResult).value();

    return requestOnceFile(*m_connection, target, headers, localFilePath, timeout);
}

Result<HttpResponse> HttpsRestClient::requestOnceStream(Connection& conn, HttpMethod method, const std::string& target,
                                                          const std::vector<HttpHeader>& headers,
                                                          const HttpBodyChunkProducer& producer,
                                                          std::chrono::seconds timeout,
                                                          std::optional<std::uint64_t> contentLength) const
{
    const auto start = Clock::now();
    httpLogger()->debug("-> {} {} ({} stream body, no local staging)", methodName(method), target,
                     contentLength ? "Content-Length " + std::to_string(*contentLength) : std::string("chunked"));
    try {
        http::request<http::buffer_body> req{toBeastVerb(method), target, 11};
        req.set(http::field::host, m_host);
        req.set(http::field::user_agent, "vsphere-soap-lib");
        for (const auto& header : headers) {
            req.set(header.name, header.value);
        }
        // See requestPutStream's doc comment: chunked encoding was tried first and confirmed live to not
        // be universally accepted, so a caller that can precompute the exact total passes it here
        // instead, getting a plain Content-Length request with no Transfer-Encoding header at all.
        if (contentLength) {
            req.content_length(*contentLength);
        } else {
            req.chunked(true);
        }
        req.body().data = nullptr;
        req.body().size = 0;
        req.body().more = true; // there IS a body coming, just not fetched yet

        http::request_serializer<http::buffer_body> sr{req};

        // Kept alive across write_some calls: buffer_body's writer only points at these bytes, it
        // doesn't own/copy them, so this must outlive every write that references it.
        std::vector<std::byte> currentChunk;
        beast::error_code lastWriteEc; // default-constructed = success

        while (!sr.is_done()) {
            if (lastWriteEc == http::error::need_buffer) {
                auto chunk = producer();
                if (!chunk) {
                    return std::move(chunk).error();
                }
                currentChunk = std::move(chunk).value();
                if (currentChunk.empty()) {
                    req.body().data = nullptr;
                    req.body().size = 0;
                    req.body().more = false; // signals true end-of-body, not "need more"
                } else {
                    req.body().data = currentChunk.data();
                    req.body().size = currentChunk.size();
                    req.body().more = true;
                }
            } else if (lastWriteEc) {
                httpLogger()->warn("HTTPS streamed {} write to {}{} failed: {}", methodName(method), m_host, target, lastWriteEc.message());
                return makeIoError(m_host, 0, "HTTPS write: " + lastWriteEc.message());
            }

            if (auto result = runWithTimeout(
                    conn.ioc, conn.stream, timeout, "HTTPS streamed upload write",
                    [&](auto handler) {
                        http::async_write_some(conn.stream, sr,
                                                [handler = std::move(handler), &lastWriteEc](beast::error_code ec,
                                                                                              std::size_t) mutable {
                                                    lastWriteEc = ec;
                                                    if (ec == http::error::need_buffer) {
                                                        // not a real failure: see the loop above
                                                        ec = {};
                                                    }
                                                    handler(ec);
                                                });
                    });
                !result.ok()) {
                // A real transport-level failure (not need_buffer, which never reaches here since it's
                // translated to success above).
                httpLogger()->warn("HTTPS streamed {} write to {}{} failed: {}", methodName(method), m_host, target, result.toString());
                return makeIoError(m_host, 0, "HTTPS write: " + result.toString());
            }
        }

        auto buffer = std::make_shared<beast::flat_buffer>();
        auto parser = std::make_shared<http::response_parser<http::string_body>>();
        parser->body_limit(boost::none); // trusted endpoint; response is just a status/error body
        if (auto result = runWithTimeout(conn.ioc, conn.stream, timeout, "HTTPS streamed upload read", [&](auto handler) {
                http::async_read(conn.stream, *buffer, *parser,
                                  [buffer, parser, handler = std::move(handler)](const beast::error_code& ec,
                                                                                  std::size_t) { handler(ec); });
            });
            !result.ok()) {
            httpLogger()->warn("HTTPS streamed {} read from {}{} failed: {}", methodName(method), m_host, target, result.toString());
            return makeIoError(m_host, 0, "HTTPS read: " + result.toString());
        }

        // Deliberately no stream shutdown here; see the class doc comment: this connection stays open
        // for the next request().

        auto res = parser->release();
        HttpResponse response;
        response.status = res.result_int();
        response.body = std::move(res.body());
        response.headers.reserve(std::distance(res.begin(), res.end()));
        for (const auto& field : res) {
            response.headers.push_back({std::string(field.name_string()), std::string(field.value())});
        }
        httpLogger()->debug("<- {} {} ({} bytes) in {:.1f}ms", response.status, target, response.body.size(),
                         elapsedMs(start));
        return response;
    } catch (const std::exception& e) {
        httpLogger()->warn("HTTPS streamed {} {} failed: {}", methodName(method), target, e.what());
        return makeIoError(m_host, 0, std::string("HTTPS streamed upload failed: ") + e.what());
    }
}

Result<HttpResponse> HttpsRestClient::requestPutStream(const std::string& target,
                                                          const std::vector<HttpHeader>& headers,
                                                          HttpBodyChunkProducer producer, std::chrono::seconds timeout,
                                                          std::optional<std::uint64_t> contentLength) const
{
    return requestStream(HttpMethod::Put, target, headers, std::move(producer), timeout, contentLength);
}

Result<HttpResponse> HttpsRestClient::requestPostStream(const std::string& target,
                                                           const std::vector<HttpHeader>& headers,
                                                           HttpBodyChunkProducer producer, std::chrono::seconds timeout,
                                                           std::optional<std::uint64_t> contentLength) const
{
    return requestStream(HttpMethod::Post, target, headers, std::move(producer), timeout, contentLength);
}

Result<HttpResponse> HttpsRestClient::requestStream(HttpMethod method, const std::string& target,
                                                       const std::vector<HttpHeader>& headers,
                                                       HttpBodyChunkProducer producer, std::chrono::seconds timeout,
                                                       std::optional<std::uint64_t> contentLength) const
{
    // Deliberately no "retry on a fresh connection if the reused one turned out to be dead" fallback,
    // unlike request()/requestPutFile()/requestOnceFile(): `producer` is a one-shot, non-replayable
    // stream (it drains a queue fed by whatever's generating the upload bytes), and a failure partway
    // through this method's write loop may already have pulled and discarded some of it; silently
    // retrying from a fresh connection would call `producer` again from wherever it now is, not from
    // the start, corrupting the upload rather than resending it.
    // A single connection attempt (reusing one already open, or establishing one fresh) means a failure
    // here is always reported as-is; the caller decides whether/how to retry the whole upload with
    // a fresh producer.
    //
    if (!m_connection) {
        auto connectResult = connect();
        if (!connectResult) {
            return std::move(connectResult).error();
        }
        m_connection = std::move(connectResult).value();
    }
    auto result = requestOnceStream(*m_connection, method, target, headers, producer, timeout, contentLength);
    if (!result) {
        // Whatever state this connection is in now, it's not safe to reuse for the next call regardless
        // of what that call turns out to be.
        m_connection.reset();
    }
    return result;
}

Result<HttpResponse> HttpsRestClient::requestGetStream(const std::string& target,
                                                       const std::vector<HttpHeader>& headers,
                                                       const HttpBodyChunkConsumer& consumer,
                                                       std::chrono::seconds timeout) const
{
    // A connection of its own: a consumer that stops early leaves the rest of the body unread on it.
    auto connectResult = connect();
    if (!connectResult) {
        return std::move(connectResult).error();
    }
    Connection& conn = *connectResult.value();

    const auto start = Clock::now();
    httpLogger()->debug("-> GET {} (streamed)", target);
    try {
        http::request<http::empty_body> req(http::verb::get, target, 11);
        req.set(http::field::host, m_host);
        req.set(http::field::user_agent, "vsphere-soap-lib");
        for (const auto& header : headers) {
            req.set(header.name, header.value);
        }
        if (auto result = runWithTimeout(conn.ioc, conn.stream, kRequestTimeout, "HTTPS write", [&](auto handler) {
                http::async_write(conn.stream, req, [handler = std::move(handler)](const beast::error_code& ec,
                                                                                   std::size_t) { handler(ec); });
            });
            !result.ok()) {
            return makeIoError(m_host, 0, "HTTPS write: " + result.toString());
        }

        beast::flat_buffer buffer;
        http::response_parser<http::buffer_body> parser;
        parser.body_limit(boost::none);
        if (auto result = runWithTimeout(conn.ioc, conn.stream, timeout, "HTTPS read header", [&](auto handler) {
                http::async_read_header(conn.stream, buffer, parser,
                                        [handler = std::move(handler)](const beast::error_code& ec, std::size_t) {
                                            handler(ec);
                                        });
            });
            !result.ok()) {
            return makeIoError(m_host, 0, "HTTPS read: " + result.toString());
        }

        HttpResponse response;
        response.status = parser.get().result_int();
        for (const auto& field : parser.get()) {
            response.headers.push_back({std::string(field.name_string()), std::string(field.value())});
        }
        const bool streamed = response.status >= 200 && response.status < 300;

        std::vector<std::byte> chunk(1 << 20);
        std::uint64_t total = 0;
        while (!parser.is_done()) {
            parser.get().body().data = chunk.data();
            parser.get().body().size = chunk.size();
            if (auto result = runWithTimeout(conn.ioc, conn.stream, timeout, "HTTPS read body", [&](auto handler) {
                    http::async_read(conn.stream, buffer, parser,
                                     [handler = std::move(handler)](const beast::error_code& ec, std::size_t) {
                                         // need_buffer only means the chunk is full.
                                         handler(ec == http::error::need_buffer ? beast::error_code{} : ec);
                                     });
                });
                !result.ok()) {
                return makeIoError(m_host, 0, "HTTPS read: " + result.toString());
            }
            const std::size_t filled = chunk.size() - parser.get().body().size;
            total += filled;
            if (filled == 0) {
                continue;
            }
            if (!streamed) {
                // An error page: kept, within reason, for the caller's message.
                if (response.body.size() < 64 * 1024) {
                    response.body.append(reinterpret_cast<const char*>(chunk.data()), filled);
                }
                continue;
            }
            if (Error consumed = consumer(std::span<const std::byte>(chunk.data(), filled)); !consumed.ok()) {
                httpLogger()->debug("GET {} stopped by its consumer after {} bytes", target, total);
                return consumed;
            }
        }
        httpLogger()->debug("<- {} {} ({} bytes streamed) in {:.1f}ms", response.status, target, total,
                            elapsedMs(start));
        return response;
    } catch (const std::exception& e) {
        httpLogger()->warn("HTTPS streamed GET {} failed: {}", target, e.what());
        return makeIoError(m_host, 0, std::string("HTTPS streamed GET failed: ") + e.what());
    }
}

} // namespace VSphereSoap::Vim
