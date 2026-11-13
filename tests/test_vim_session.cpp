// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for the vSphere login session

// The session reads ServiceContent on the baseline API version, then switches the client to the version the
// server reports, so every later call is answered in the server's own version.

#include <gtest/gtest.h>

#include "vsphere-soap/vim/SoapClient.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "fake_http_transport.hpp"

namespace VSphereSoap::Detail {
namespace {

using namespace VSphereSoap::Vim;

using Test::FakeHttpTransport;

HttpResponse xmlResponse(const std::string& responseElement)
{
    HttpResponse response;
    response.status = 200;
    response.body = "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\"><soapenv:Body>" +
                    responseElement + "</soapenv:Body></soapenv:Envelope>";
    return response;
}

// Scripts a vCenter whose ServiceContent carries `aboutFields` inside <about>, and a Login that succeeds.
void scriptServer(FakeHttpTransport& transport, const std::string& aboutFields)
{
    transport.queueForOperation(
        "RetrieveServiceContent",
        xmlResponse("<RetrieveServiceContentResponse xmlns=\"urn:vim25\"><returnval>"
                    "<rootFolder type=\"Folder\">group-d1</rootFolder>"
                    "<propertyCollector type=\"PropertyCollector\">propertyCollector</propertyCollector>"
                    "<about>" + aboutFields + "</about>"
                    "<sessionManager type=\"SessionManager\">SessionManager</sessionManager>"
                    "<searchIndex type=\"SearchIndex\">SearchIndex</searchIndex>"
                    "</returnval></RetrieveServiceContentResponse>"));
    transport.queueForOperation("Login", xmlResponse("<LoginResponse xmlns=\"urn:vim25\"><returnval><key>k</key>"
                                                     "</returnval></LoginResponse>"));
}

// The SOAPAction header of the first request for `operation`, empty if it was never sent.
std::string soapActionOf(const FakeHttpTransport& transport, const std::string& operation)
{
    for (const auto& request : transport.m_requests) {
        if (FakeHttpTransport::operationOf(request.body) != operation) {
            continue;
        }
        for (const auto& header : request.headers) {
            if (header.name == "SOAPAction") {
                return header.value;
            }
        }
    }
    return {};
}

TEST(VimSessionTest, SwitchesToServerApiVersionAfterServiceContent)
{
    FakeHttpTransport transport;
    scriptServer(transport, "<apiType>VirtualCenter</apiType><apiVersion>8.0.3.0</apiVersion>");
    SoapClient soap(transport, "vcenter.example.com");

    auto session = VimSession::open(soap, "user", "pass");
    ASSERT_TRUE(session.hasValue()) << session.error().toString();

    EXPECT_EQ(soapActionOf(transport, "RetrieveServiceContent"), "urn:vim25/6.7");
    EXPECT_EQ(soapActionOf(transport, "Login"), "urn:vim25/8.0.3.0");
    EXPECT_EQ(soap.apiVersion(), "8.0.3.0");
}

TEST(VimSessionTest, StaysOnBaselineWithoutUsableApiVersion)
{
    const char* const aboutVariants[] = {
        "<apiType>HostAgent</apiType>",
        "<apiVersion></apiVersion>",
        "<apiVersion>8.0.</apiVersion>",
        "<apiVersion>8..0</apiVersion>",
        "<apiVersion>eight</apiVersion>",
        "<apiVersion>8.0.3.0\r\nX-Injected: 1</apiVersion>",
    };
    for (const char* about : aboutVariants) {
        SCOPED_TRACE(about);
        FakeHttpTransport transport;
        scriptServer(transport, about);
        SoapClient soap(transport, "vcenter.example.com");

        auto session = VimSession::open(soap, "user", "pass");
        ASSERT_TRUE(session.hasValue()) << session.error().toString();

        EXPECT_EQ(soapActionOf(transport, "Login"), "urn:vim25/6.7");
        EXPECT_EQ(soap.apiVersion(), "6.7");
    }
}

} // namespace
} // namespace VSphereSoap::Detail
