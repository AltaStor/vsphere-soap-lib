// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Scripted fake HTTP transport for SOAP tests

#pragma once

// Test double for VSphereSoap::Detail::IHttpTransport: returns scripted responses keyed by the request's
// SOAPAction header (for SOAP calls) or by exact target (for plain GETs, e.g. datastore reads/descriptor
// fetches).
// Each call pops the next scripted response for its key, except the last one queued for a key, which is
// returned repeatedly: lets a test script "page 1, page 2, then whatever comes after just keeps
// returning page 2" without needing an exact call count.

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "IHttpTransport.hpp"
#include "Error.hpp"

namespace VSphereSoap::Test {

class FakeHttpTransport final : public Detail::IHttpTransport {
public:
    void queueForSoapAction(const std::string& soapAction, Detail::HttpResponse response)
    {
        m_bySoapAction[soapAction].push_back(std::move(response));
    }

    void queueForTarget(const std::string& target, Detail::HttpResponse response)
    {
        m_byTarget[target].push_back(std::move(response));
    }

    struct RecordedRequest {
        Detail::HttpMethod method;
        std::string target;
        std::vector<Detail::HttpHeader> headers;
        std::string body;
    };
    std::vector<RecordedRequest> m_requests;

    VSphereSoap::Result<Detail::HttpResponse> request(Detail::HttpMethod method,
                                                                          const std::string& target,
                                                                          const std::vector<Detail::HttpHeader>& headers,
                                                                          std::string body) override
    {
        m_requests.push_back({method, target, headers, body});

        for (const auto& header : headers) {
            if (header.name == "SOAPAction") {
                if (auto next = pop(m_bySoapAction, header.value)) {
                    return *next;
                }
            }
        }
        if (auto next = pop(m_byTarget, target)) {
            return *next;
        }

        return VSphereSoap::Error(VSphereSoap::ErrorCode::IoError,
                                     "FakeHttpTransport: no response scripted for target " + target);
    }

    VSphereSoap::Result<Detail::HttpResponse> requestPutFile(
        const std::string& target, const std::vector<Detail::HttpHeader>& headers, const std::string& localFilePath,
        std::chrono::seconds /*timeout*/) override
    {
        m_requests.push_back({Detail::HttpMethod::Put, target, headers, "(file:" + localFilePath + ")"});
        if (auto next = pop(m_byTarget, target)) {
            return *next;
        }
        return VSphereSoap::Error(VSphereSoap::ErrorCode::IoError,
                                     "FakeHttpTransport: no response scripted for target " + target);
    }

    // Drains `producer` fully (as the real chunked-PUT transport eventually would over the wire) into
    // one concatenated buffer: good enough for tests that care about *what bytes* an upload produced,
    // since nothing here needs to actually exercise HTTP chunk framing (that's HttpsRestClient's own
    // job, not something a test double should re-implement).
    // Each chunk pulled is also recorded individually in `m_streamedChunks` for tests that care about
    // chunking boundaries themselves (e.g. "did the backend flush per grain, not just at the end").
    //
    std::vector<std::vector<std::byte>> m_streamedChunks;
    /// Records the `contentLength` each `requestPutStream()` call was given (`std::nullopt` means
    /// chunked): tests that care whether a caller precomputed a length rather than relying on chunked
    /// encoding can check this.
    std::vector<std::optional<std::uint64_t>> m_streamedContentLengths;

    VSphereSoap::Result<Detail::HttpResponse> requestPutStream(
        const std::string& target, const std::vector<Detail::HttpHeader>& headers,
        Detail::HttpBodyChunkProducer producer, std::chrono::seconds /*timeout*/,
        std::optional<std::uint64_t> contentLength) override
    {
        return stream(Detail::HttpMethod::Put, target, headers, std::move(producer), contentLength);
    }

    VSphereSoap::Result<Detail::HttpResponse> requestPostStream(
        const std::string& target, const std::vector<Detail::HttpHeader>& headers,
        Detail::HttpBodyChunkProducer producer, std::chrono::seconds /*timeout*/,
        std::optional<std::uint64_t> contentLength) override
    {
        return stream(Detail::HttpMethod::Post, target, headers, std::move(producer), contentLength);
    }

private:
    // Shared body of requestPutStream()/requestPostStream(); the method is recorded in `m_requests` so
    // tests can tell which one a caller used.
    VSphereSoap::Result<Detail::HttpResponse> stream(Detail::HttpMethod method, const std::string& target,
                                                        const std::vector<Detail::HttpHeader>& headers,
                                                        Detail::HttpBodyChunkProducer producer,
                                                        std::optional<std::uint64_t> contentLength)
    {
        m_streamedContentLengths.push_back(contentLength);
        std::string body;
        for (;;) {
            auto chunk = producer();
            if (!chunk) {
                return std::move(chunk).error();
            }
            if (chunk.value().empty()) {
                break;
            }
            m_streamedChunks.push_back(chunk.value());
            body.append(reinterpret_cast<const char*>(chunk.value().data()), chunk.value().size());
        }
        m_requests.push_back({method, target, headers, body});
        if (auto next = pop(m_byTarget, target)) {
            return *next;
        }
        return VSphereSoap::Error(VSphereSoap::ErrorCode::IoError,
                                     "FakeHttpTransport: no response scripted for target " + target);
    }

    static std::optional<Detail::HttpResponse> pop(std::map<std::string, std::deque<Detail::HttpResponse>>& table,
                                                     const std::string& key)
    {
        auto it = table.find(key);
        if (it == table.end() || it->second.empty()) {
            return std::nullopt;
        }
        Detail::HttpResponse response = it->second.front();
        if (it->second.size() > 1) {
            it->second.pop_front(); // keep the last one queued as the sticky default.
        }
        return response;
    }

    std::map<std::string, std::deque<Detail::HttpResponse>> m_bySoapAction;
    std::map<std::string, std::deque<Detail::HttpResponse>> m_byTarget;
};

} // namespace VSphereSoap::Test
