// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// HTTPS request/response transport interface

#pragma once

// Narrow seam around one HTTPS request/response round trip: lets SoapClient's envelope/fault handling
// and VmwareSoapDisk's Range-request logic be unit-tested without a live vCenter/ESXi host or real TLS.
// The concrete HttpTransportAdapter wraps the real HttpsRestClient; test code substitutes a fake
// instead.

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "HttpsRestClient.hpp"
#include "Result.hpp"

namespace VSphereSoap::Detail {

/// One HTTPS request/response round trip against a fixed host.
/// Both SoapClient (POSTing SOAP envelopes to /sdk) and VmwareSoapDisk (ranged GETs against /folder) go
/// through this seam.
///
/// Does not throw; fails with `ErrorCode::IoError` on DNS/TCP/TLS/HTTP transport failure.
/// Does not itself require a 2xx status: callers interpret the returned status/headers/body themselves
/// (a SOAP fault or a 404/5xx datastore response is still a "successful" transport round trip, just not
/// a successful outcome for the caller).
///
class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;

    [[nodiscard]] virtual Result<HttpResponse> request(HttpMethod method, const std::string& target,
                                                          const std::vector<HttpHeader>& headers,
                                                          std::string body = {}) = 0;

    /// Like `request(HttpMethod::Put, ...)`, but streams a local file's contents as the body rather than
    /// buffering it into a `std::string` first: see `HttpsRestClient::requestPutFile()`, which this
    /// always forwards to on the concrete `HttpTransportAdapter`.
    [[nodiscard]] virtual Result<HttpResponse> requestPutFile(const std::string& target,
                                                                const std::vector<HttpHeader>& headers,
                                                                const std::string& localFilePath,
                                                                std::chrono::seconds timeout) = 0;

    /// Like `request(HttpMethod::Put, ...)`, but the body is pulled incrementally from `producer`: see
    /// `HttpsRestClient::requestPutStream()`, which this always forwards to on the concrete
    /// `HttpTransportAdapter`, for what `contentLength` controls (chunked vs a plain declared-length
    /// request) and why.
    /// This is the seam `VSphereDatastoreBackend` uploads a stream-optimized VMDK through with no local
    /// staging file, unlike `requestPutFile`.
    ///
    [[nodiscard]] virtual Result<HttpResponse> requestPutStream(const std::string& target,
                                                                   const std::vector<HttpHeader>& headers,
                                                                   HttpBodyChunkProducer producer,
                                                                   std::chrono::seconds timeout,
                                                                   std::optional<std::uint64_t> contentLength) = 0;

    /// `requestPutStream()` sent as POST: see `HttpsRestClient::requestPostStream()` for why
    /// an HttpNfcLease import disk upload needs POST rather than PUT.
    [[nodiscard]] virtual Result<HttpResponse> requestPostStream(const std::string& target,
                                                                    const std::vector<HttpHeader>& headers,
                                                                    HttpBodyChunkProducer producer,
                                                                    std::chrono::seconds timeout,
                                                                    std::optional<std::uint64_t> contentLength) = 0;
};

} // namespace VSphereSoap::Detail
