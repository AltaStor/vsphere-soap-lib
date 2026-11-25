// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// vSphere Web Services SOAP client

#include "vsphere-soap/vim/SoapClient.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

#include "SoapLog.hpp"

namespace VSphereSoap::Vim {

using Detail::logger;

namespace {

bool headerNameEquals(const std::string& name, std::string_view target)
{
    return name.size() == target.size() &&
           std::equal(name.begin(), name.end(), target.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

// "vmware_soap_session=\"...\"; Path=/; HttpOnly; Secure" -> "vmware_soap_session=\"...\"".
std::string firstCookiePair(const std::string& setCookieValue)
{
    const auto semicolon = setCookieValue.find(';');
    return semicolon == std::string::npos ? setCookieValue : setCookieValue.substr(0, semicolon);
}

// "soapenv:Body" -> "Body"; "Body" -> "Body".
// SOAP responses are free to use any prefix (or none) for the envelope namespace, so every name
// comparison in this file goes through this rather than matching a fixed prefix.
std::string_view localName(std::string_view qualifiedName)
{
    const auto colon = qualifiedName.find(':');
    return colon == std::string_view::npos ? qualifiedName : qualifiedName.substr(colon + 1);
}

} // namespace

pugi::xml_node soapBodyContent(const pugi::xml_document& doc)
{
    for (pugi::xml_node envelope = doc.first_child(); envelope; envelope = envelope.next_sibling()) {
        if (localName(envelope.name()) != "Envelope") {
            continue;
        }
        for (pugi::xml_node body = envelope.first_child(); body; body = body.next_sibling()) {
            if (localName(body.name()) == "Body") {
                return body.first_child();
            }
        }
    }
    return {};
}

SoapClient::SoapClient(IHttpTransport& transport, std::string host, std::string path)
    : m_transport(transport), m_host(std::move(host)), m_path(std::move(path)) {}

Result<pugi::xml_document> SoapClient::call(const pugi::xml_node& requestNode, SoapFaultInfo* faultInfo)
{
    const auto start = std::chrono::steady_clock::now();
    const std::string_view operation = localName(requestNode.name());
    logger()->debug("-> {} {} (vim25/{})", m_host, operation, m_apiVersion);
    pugi::xml_document envelope;
    pugi::xml_node envelopeNode = envelope.append_child("soapenv:Envelope");
    envelopeNode.append_attribute("xmlns:soapenv") = "http://schemas.xmlsoap.org/soap/envelope/";
    pugi::xml_node bodyNode = envelopeNode.append_child("soapenv:Body");
    bodyNode.append_copy(requestNode);

    std::ostringstream requestXml;
    envelope.save(requestXml, "", pugi::format_raw | pugi::format_no_declaration);

    std::vector<HttpHeader> headers{
        {"Content-Type", "text/xml; charset=utf-8"},
        {"SOAPAction", "urn:vim25/" + m_apiVersion},
    };
    if (!m_sessionCookie.empty()) {
        headers.push_back({"Cookie", m_sessionCookie});
    }

    auto responseResult = m_transport.request(HttpMethod::Post, m_path, headers, requestXml.str());
    if (!responseResult) {
        logger()->warn("{} {} failed: {}", m_host, operation, responseResult.error().toString());
        return std::move(responseResult).error();
    }
    const HttpResponse& response = responseResult.value();

    for (const auto& header : response.headers) {
        if (headerNameEquals(header.name, "Set-Cookie")) {
            m_sessionCookie = firstCookiePair(header.value);
            break; // vSphere sets exactly one session cookie worth carrying forward.
        }
    }

    pugi::xml_document doc;
    const pugi::xml_parse_result parseResult = doc.load_string(response.body.c_str());
    if (!parseResult) {
        return Error(ErrorCode::InvalidFormat, "invalid SOAP XML response from " + m_host + m_path + ": " +
                                                    parseResult.description());
    }

    pugi::xml_node bodyContent = soapBodyContent(doc);
    if (!bodyContent) {
        return Error(ErrorCode::InvalidFormat,
                     "SOAP response from " + m_host + m_path + " has no <Body> element (HTTP " +
                         std::to_string(response.status) + ")");
    }

    if (localName(bodyContent.name()) == "Fault") {
        const std::string faultString = bodyContent.child("faultstring").text().as_string();
        std::string typeName;
        if (pugi::xml_node detailFirstChild = bodyContent.child("detail").first_child()) {
            typeName = std::string(localName(detailFirstChild.name()));
        }
        if (faultInfo != nullptr) {
            faultInfo->typeName = typeName;
            faultInfo->faultString = faultString;
        }
        logger()->warn("{} {} returned a SOAP fault{}: {}", m_host, operation,
                        typeName.empty() ? "" : " (" + typeName + ")", faultString);
        return Error(ErrorCode::IoError, "vSphere SOAP fault" + (typeName.empty() ? "" : " (" + typeName + ")") +
                                              " from " + m_host + m_path + ": " + faultString);
    }

    logger()->debug("<- {} {} (HTTP {}) in {:.1f}ms", m_host, operation, response.status,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return doc;
}

} // namespace VSphereSoap::Vim
