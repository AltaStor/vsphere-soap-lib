// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// HTTPS transport adapter over the REST client

#pragma once

// Concrete IHttpTransport backed by HttpsRestClient (Boost.Beast/Asio).
// See IHttpTransport.hpp for why this seam exists.

#include <cstdint>
#include <optional>
#include <string>

#include "vsphere-soap/vim/HttpsRestClient.hpp"
#include "vsphere-soap/vim/IHttpTransport.hpp"

namespace VSphereSoap::Vim {

class HttpTransportAdapter final : public IHttpTransport {
public:
    HttpTransportAdapter(std::string host, std::uint16_t port, bool verifyTlsCertificate,
                          std::string pinnedThumbprint)
        : m_client(std::move(host), port, verifyTlsCertificate, std::move(pinnedThumbprint)) {}

    Result<HttpResponse> request(HttpMethod method, const std::string& target,
                                                  const std::vector<HttpHeader>& headers,
                                                  std::string body) override
    {
        return m_client.request(method, target, headers, std::move(body));
    }

    Result<HttpResponse> requestPutFile(const std::string& target, const std::vector<HttpHeader>& headers,
                                                        const std::string& localFilePath,
                                                        std::chrono::seconds timeout) override
    {
        return m_client.requestPutFile(target, headers, localFilePath, timeout);
    }

    Result<HttpResponse> requestPutStream(const std::string& target, const std::vector<HttpHeader>& headers,
                                                          HttpBodyChunkProducer producer, std::chrono::seconds timeout,
                                                          std::optional<std::uint64_t> contentLength) override
    {
        return m_client.requestPutStream(target, headers, std::move(producer), timeout, contentLength);
    }

    Result<HttpResponse> requestPostStream(const std::string& target, const std::vector<HttpHeader>& headers,
                                                           HttpBodyChunkProducer producer, std::chrono::seconds timeout,
                                                           std::optional<std::uint64_t> contentLength) override
    {
        return m_client.requestPostStream(target, headers, std::move(producer), timeout, contentLength);
    }

private:
    VSphereSoap::Vim::HttpsRestClient m_client;
};

} // namespace VSphereSoap::Vim
