// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for the vSphere SOAP client

#include <gtest/gtest.h>

#include "SoapClient.hpp"
#include "fake_http_transport.hpp"

namespace VSphereSoap::Detail {
namespace {

using Test::FakeHttpTransport;

HttpResponse xmlResponse(unsigned int status, std::string body)
{
    HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    return response;
}

TEST(SoapClientTest, WrapsRequestNodeInEnvelopeAndBody)
{
    FakeHttpTransport transport;
    transport.queueForSoapAction(
        "urn:vim25/Foo",
        xmlResponse(200, "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                         "<soapenv:Body><FooResponse xmlns=\"urn:vim25\"><returnval>ok</returnval>"
                         "</FooResponse></soapenv:Body></soapenv:Envelope>"));

    SoapClient client(transport, "vcenter.example.com");

    pugi::xml_document request;
    pugi::xml_node op = request.append_child("Foo");
    op.append_attribute("xmlns") = "urn:vim25";
    op.append_child("_this").append_attribute("type") = "ServiceInstance";

    auto result = client.call("urn:vim25/Foo", op);
    ASSERT_TRUE(result.hasValue());

    const pugi::xml_node body = soapBodyContent(result.value());
    ASSERT_TRUE(body);
    EXPECT_STREQ(body.name(), "FooResponse");
    EXPECT_STREQ(body.child("returnval").text().as_string(), "ok");

    ASSERT_EQ(transport.m_requests.size(), 1u);
    const auto& sent = transport.m_requests.front();
    EXPECT_EQ(sent.target, "/sdk");

    pugi::xml_document sentDoc;
    ASSERT_TRUE(sentDoc.load_string(sent.body.c_str()));
    const pugi::xml_node envelope = sentDoc.first_child();
    EXPECT_STREQ(envelope.name(), "soapenv:Envelope");
    const pugi::xml_node bodyNode = envelope.child("soapenv:Body");
    ASSERT_TRUE(bodyNode);
    EXPECT_STREQ(bodyNode.first_child().name(), "Foo");
    EXPECT_STREQ(bodyNode.first_child().child("_this").attribute("type").as_string(), "ServiceInstance");
}

TEST(SoapClientTest, DetectsFaultAndExtractsDetailType)
{
    FakeHttpTransport transport;
    transport.queueForSoapAction(
        "urn:vim25/Login",
        xmlResponse(500, "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                         "<soapenv:Body><soapenv:Fault>"
                         "<faultcode>ServerFaultCode</faultcode>"
                         "<faultstring>Login failure</faultstring>"
                         "<detail><InvalidLogin xmlns=\"urn:vim25\"/></detail>"
                         "</soapenv:Fault></soapenv:Body></soapenv:Envelope>"));

    SoapClient client(transport, "vcenter.example.com");
    pugi::xml_document request;
    request.append_child("Login").append_attribute("xmlns") = "urn:vim25";

    SoapFaultInfo faultInfo;
    auto result = client.call("urn:vim25/Login", request.document_element(), &faultInfo);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), VSphereSoap::ErrorCode::IoError);
    EXPECT_EQ(faultInfo.typeName, "InvalidLogin");
    EXPECT_EQ(faultInfo.faultString, "Login failure");
}

TEST(SoapClientTest, CarriesSessionCookieOnSubsequentCallsOnly)
{
    FakeHttpTransport transport;
    HttpResponse withCookie = xmlResponse(200, "<Envelope><Body><FooResponse/></Body></Envelope>");
    withCookie.headers.push_back({"Set-Cookie", "vmware_soap_session=\"abc123\"; Path=/; HttpOnly; Secure"});
    transport.queueForSoapAction("urn:vim25/Foo", withCookie);
    transport.queueForSoapAction("urn:vim25/Foo", xmlResponse(200, "<Envelope><Body><FooResponse/></Body></Envelope>"));

    SoapClient client(transport, "vcenter.example.com");
    pugi::xml_document request;
    request.append_child("Foo");

    ASSERT_TRUE(client.call("urn:vim25/Foo", request.document_element()).hasValue());
    ASSERT_EQ(transport.m_requests.size(), 1u);
    bool firstHadCookie = false;
    for (const auto& header : transport.m_requests.back().headers) {
        firstHadCookie = firstHadCookie || header.name == "Cookie";
    }
    EXPECT_FALSE(firstHadCookie);
    EXPECT_EQ(client.sessionCookie(), "vmware_soap_session=\"abc123\"");

    ASSERT_TRUE(client.call("urn:vim25/Foo", request.document_element()).hasValue());
    std::string secondCookieValue;
    for (const auto& header : transport.m_requests.back().headers) {
        if (header.name == "Cookie") {
            secondCookieValue = header.value;
        }
    }
    EXPECT_EQ(secondCookieValue, "vmware_soap_session=\"abc123\"");
}

TEST(SoapBodyContentTest, FindsBodyRegardlessOfNamespacePrefix)
{
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string("<x:Envelope xmlns:x=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                                "<x:Body><Payload>hi</Payload></x:Body></x:Envelope>"));
    const pugi::xml_node body = soapBodyContent(doc);
    ASSERT_TRUE(body);
    EXPECT_STREQ(body.name(), "Payload");
}

TEST(SoapBodyContentTest, ReturnsNullNodeWhenShapeIsUnexpected)
{
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string("<NotAnEnvelope/>"));
    EXPECT_FALSE(soapBodyContent(doc));
}

} // namespace
} // namespace VSphereSoap::Detail
