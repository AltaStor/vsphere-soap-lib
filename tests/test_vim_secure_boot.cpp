// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for reading a VM's Secure Boot setting

// VimInventory::describeSecureBoot() reads config.bootOptions.efiSecureBootEnabled alone.
// FakeHttpTransport-based, like test_vim_list_objects.cpp.

#include <gtest/gtest.h>

#include <optional>

#include "vsphere-soap/vim/SoapClient.hpp"
#include "vsphere-soap/vim/VimInventory.hpp"
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
    response.body = "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\""
                    " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><soapenv:Body>" +
                    responseElement + "</soapenv:Body></soapenv:Envelope>";
    return response;
}

void scriptSessionBootstrap(FakeHttpTransport& transport)
{
    transport.queueForOperation(
        "RetrieveServiceContent",
        xmlResponse("<RetrieveServiceContentResponse xmlns=\"urn:vim25\"><returnval>"
                    "<rootFolder type=\"Folder\">ha-folder-root</rootFolder>"
                    "<propertyCollector type=\"PropertyCollector\">ha-property-collector</propertyCollector>"
                    "<about><apiType>HostAgent</apiType></about>"
                    "<searchIndex type=\"SearchIndex\">ha-searchindex</searchIndex>"
                    "<sessionManager type=\"SessionManager\">ha-sessionmgr</sessionManager>"
                    "</returnval></RetrieveServiceContentResponse>"));
    transport.queueForOperation("Login",
                                 xmlResponse("<LoginResponse xmlns=\"urn:vim25\"><returnval><key>k</key>"
                                             "</returnval></LoginResponse>"));
}

// The RetrievePropertiesEx answer for VM "1" with `propSets` as its properties.
HttpResponse vmProperties(const std::string& propSets)
{
    return xmlResponse("<RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval>"
                       "<objects><obj type=\"VirtualMachine\">1</obj>" +
                       propSets + "</objects></returnval></RetrievePropertiesExResponse>");
}

pugi::xml_document sentRequestBody(const FakeHttpTransport& transport, const std::string& operation)
{
    pugi::xml_document doc;
    for (const auto& request : transport.m_requests) {
        if (FakeHttpTransport::operationOf(request.body) == operation) {
            doc.load_string(request.body.c_str());
            return doc;
        }
    }
    return doc;
}

struct Fixture {
    FakeHttpTransport transport;
    SoapClient soap{transport, "esxi.example.com"};
};

Result<std::optional<bool>> describeSecureBootOfVm1(Fixture& fixture)
{
    auto session = VimSession::open(fixture.soap, "root", "pw");
    if (!session) {
        return std::move(session).error();
    }
    VimInventory inventory(*session.value());
    return inventory.describeSecureBoot(MoRef{"VirtualMachine", "1"});
}

} // namespace

TEST(VimSecureBootTest, ReadsSecureBootOnWithThatPropertyAlone)
{
    Fixture fixture;
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForOperation(
        "RetrievePropertiesEx",
        vmProperties("<propSet><name>config.bootOptions.efiSecureBootEnabled</name>"
                     "<val xsi:type=\"xsd:boolean\">true</val></propSet>"));

    auto secureBoot = describeSecureBootOfVm1(fixture);
    ASSERT_TRUE(secureBoot.hasValue()) << secureBoot.error().toString();
    EXPECT_EQ(secureBoot.value(), std::optional<bool>(true));

    const auto sent = sentRequestBody(fixture.transport, "RetrievePropertiesEx");
    const auto paths = sent.select_nodes("//pathSet");
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_STREQ(paths.first().node().text().as_string(), "config.bootOptions.efiSecureBootEnabled");
    EXPECT_STREQ(sent.select_node("//objectSet/obj").node().text().as_string(), "1");
}

TEST(VimSecureBootTest, ReadsSecureBootOff)
{
    Fixture fixture;
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForOperation(
        "RetrievePropertiesEx",
        vmProperties("<propSet><name>config.bootOptions.efiSecureBootEnabled</name>"
                     "<val xsi:type=\"xsd:boolean\">false</val></propSet>"));

    auto secureBoot = describeSecureBootOfVm1(fixture);
    ASSERT_TRUE(secureBoot.hasValue()) << secureBoot.error().toString();
    EXPECT_EQ(secureBoot.value(), std::optional<bool>(false));
}

TEST(VimSecureBootTest, PropertyTheServerDidNotReturnIsUnknown)
{
    Fixture fixture;
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForOperation("RetrievePropertiesEx", vmProperties(""));

    auto secureBoot = describeSecureBootOfVm1(fixture);
    ASSERT_TRUE(secureBoot.hasValue()) << secureBoot.error().toString();
    EXPECT_EQ(secureBoot.value(), std::nullopt);
}

TEST(VimSecureBootTest, FailsForAVmTheServerDoesNotReturn)
{
    Fixture fixture;
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForOperation(
        "RetrievePropertiesEx",
        xmlResponse("<RetrievePropertiesExResponse xmlns=\"urn:vim25\"></RetrievePropertiesExResponse>"));

    auto secureBoot = describeSecureBootOfVm1(fixture);
    ASSERT_FALSE(secureBoot.hasValue());
    EXPECT_EQ(secureBoot.error().code(), ErrorCode::InvalidArgument);
}

TEST(VimSecureBootTest, PassesASoapFaultOnAsAnError)
{
    Fixture fixture;
    scriptSessionBootstrap(fixture.transport);
    HttpResponse fault = xmlResponse("<soapenv:Fault><faultcode>ServerFaultCode</faultcode>"
                                     "<faultstring>a fault</faultstring>"
                                     "<detail><RuntimeFaultFault xmlns=\"urn:vim25\"/></detail></soapenv:Fault>");
    fault.status = 500;
    fixture.transport.queueForOperation("RetrievePropertiesEx", fault);

    auto secureBoot = describeSecureBootOfVm1(fixture);
    ASSERT_FALSE(secureBoot.hasValue());
    EXPECT_EQ(secureBoot.error().code(), ErrorCode::IoError);
}

} // namespace VSphereSoap::Detail
