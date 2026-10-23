// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for vSphere inventory object listing

// VimInventory::listObjects(): the ContainerView-based inventory read a caller uses for a standalone
// ESXi host, which has no REST API to list VMs/datastores/networks/folders with.
// FakeHttpTransport-based, like test_vsphere_network_mapping.cpp.

#include <gtest/gtest.h>

#include "SoapClient.hpp"
#include "VimInventory.hpp"
#include "VimSession.hpp"
#include "fake_http_transport.hpp"

namespace VSphereSoap::Detail {
namespace {

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

// An ESXi host's ServiceContent: ha-* morefs, and a viewManager.
void scriptEsxiSessionBootstrap(FakeHttpTransport& transport)
{
    transport.queueForSoapAction(
        "urn:vim25/RetrieveServiceContent",
        xmlResponse("<RetrieveServiceContentResponse xmlns=\"urn:vim25\"><returnval>"
                    "<rootFolder type=\"Folder\">ha-folder-root</rootFolder>"
                    "<propertyCollector type=\"PropertyCollector\">ha-property-collector</propertyCollector>"
                    "<viewManager type=\"ViewManager\">ViewManager</viewManager>"
                    "<about><apiType>HostAgent</apiType></about>"
                    "<searchIndex type=\"SearchIndex\">ha-searchindex</searchIndex>"
                    "<sessionManager type=\"SessionManager\">ha-sessionmgr</sessionManager>"
                    "</returnval></RetrieveServiceContentResponse>"));
    transport.queueForSoapAction("urn:vim25/Login",
                                 xmlResponse("<LoginResponse xmlns=\"urn:vim25\"><returnval><key>k</key>"
                                             "</returnval></LoginResponse>"));
    transport.queueForSoapAction("urn:vim25/CreateContainerView",
                                 xmlResponse("<CreateContainerViewResponse xmlns=\"urn:vim25\">"
                                             "<returnval type=\"ContainerView\">session[1]view-1</returnval>"
                                             "</CreateContainerViewResponse>"));
    transport.queueForSoapAction("urn:vim25/DestroyView",
                                 xmlResponse("<DestroyViewResponse xmlns=\"urn:vim25\"></DestroyViewResponse>"));
}

pugi::xml_document sentRequestBody(const FakeHttpTransport& transport, const std::string& soapAction)
{
    pugi::xml_document doc;
    for (const auto& request : transport.m_requests) {
        for (const auto& h : request.headers) {
            if (h.name == "SOAPAction" && h.value == soapAction) {
                doc.load_string(request.body.c_str());
                return doc;
            }
        }
    }
    return doc;
}

bool wasSent(const FakeHttpTransport& transport, const std::string& soapAction)
{
    return static_cast<bool>(sentRequestBody(transport, soapAction).first_child());
}

struct Fixture {
    FakeHttpTransport transport;
    SoapClient soap{transport, "esxi.example.com"};
};

} // namespace

TEST(VimListObjectsTest, BuildsContainerViewOverRootFolderAndTraversesIt)
{
    Fixture fixture;
    scriptEsxiSessionBootstrap(fixture.transport);
    fixture.transport.queueForSoapAction(
        "urn:vim25/RetrievePropertiesEx",
        xmlResponse("<RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval>"
                    "<objects><obj type=\"VirtualMachine\">1</obj>"
                    "<propSet><name>name</name><val xsi:type=\"xsd:string\">web-01</val></propSet></objects>"
                    "<objects><obj type=\"VirtualMachine\">2</obj>"
                    "<propSet><name>name</name><val xsi:type=\"xsd:string\">db-01</val></propSet></objects>"
                    "</returnval></RetrievePropertiesExResponse>"));

    auto session = VimSession::open(fixture.soap, "root", "pw");
    ASSERT_TRUE(session.hasValue());
    VimInventory inventory(*session.value());
    auto objects = inventory.listObjects("VirtualMachine", {"name"});
    ASSERT_TRUE(objects.hasValue()) << objects.error().toString();

    ASSERT_EQ(objects.value().size(), 2u);
    EXPECT_EQ(objects.value()[0].moref.type, "VirtualMachine");
    EXPECT_EQ(objects.value()[0].moref.value, "1");
    EXPECT_EQ(objects.value()[0].get("name"), "web-01");
    EXPECT_EQ(objects.value()[1].get("name"), "db-01");
    EXPECT_EQ(objects.value()[1].get("missing"), "");

    const auto create = sentRequestBody(fixture.transport, "urn:vim25/CreateContainerView");
    EXPECT_STREQ(create.select_node("//_this").node().text().as_string(), "ViewManager");
    EXPECT_STREQ(create.select_node("//container").node().text().as_string(), "ha-folder-root");
    EXPECT_STREQ(create.select_node("//container").node().attribute("type").as_string(), "Folder");
    EXPECT_STREQ(create.select_node("//type").node().text().as_string(), "VirtualMachine");
    EXPECT_STREQ(create.select_node("//recursive").node().text().as_string(), "true");

    const auto retrieve = sentRequestBody(fixture.transport, "urn:vim25/RetrievePropertiesEx");
    EXPECT_STREQ(retrieve.select_node("//objectSet/obj").node().text().as_string(), "session[1]view-1");
    EXPECT_STREQ(retrieve.select_node("//objectSet/obj").node().attribute("type").as_string(), "ContainerView");
    EXPECT_STREQ(retrieve.select_node("//objectSet/skip").node().text().as_string(), "true");
    const pugi::xml_node selectSet = retrieve.select_node("//objectSet/selectSet").node();
    EXPECT_STREQ(selectSet.attribute("xsi:type").as_string(), "TraversalSpec");
    EXPECT_STREQ(selectSet.child("type").text().as_string(), "ContainerView");
    EXPECT_STREQ(selectSet.child("path").text().as_string(), "view");
    EXPECT_STREQ(retrieve.select_node("//propSet/type").node().text().as_string(), "VirtualMachine");
    EXPECT_STREQ(retrieve.select_node("//propSet/pathSet").node().text().as_string(), "name");

    EXPECT_TRUE(wasSent(fixture.transport, "urn:vim25/DestroyView"));
}

TEST(VimListObjectsTest, FlattensArrayValuesAndFollowsContinuationToken)
{
    Fixture fixture;
    scriptEsxiSessionBootstrap(fixture.transport);
    fixture.transport.queueForSoapAction(
        "urn:vim25/RetrievePropertiesEx",
        xmlResponse("<RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><token>page2</token>"
                    "<objects><obj type=\"Folder\">ha-folder-vm</obj>"
                    "<propSet><name>name</name><val xsi:type=\"xsd:string\">vm</val></propSet>"
                    "<propSet><name>childType</name><val xsi:type=\"ArrayOfString\">"
                    "<string>Folder</string><string>VirtualMachine</string><string>VirtualApp</string>"
                    "</val></propSet></objects>"
                    "</returnval></RetrievePropertiesExResponse>"));
    fixture.transport.queueForSoapAction(
        "urn:vim25/ContinueRetrievePropertiesEx",
        xmlResponse("<ContinueRetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval>"
                    "<objects><obj type=\"Folder\">ha-folder-host</obj>"
                    "<propSet><name>name</name><val xsi:type=\"xsd:string\">host</val></propSet>"
                    "<propSet><name>childType</name><val xsi:type=\"ArrayOfString\">"
                    "<string>ComputeResource</string></val></propSet></objects>"
                    "</returnval></ContinueRetrievePropertiesExResponse>"));

    auto session = VimSession::open(fixture.soap, "root", "pw");
    ASSERT_TRUE(session.hasValue());
    VimInventory inventory(*session.value());
    auto objects = inventory.listObjects("Folder", {"name", "childType"});
    ASSERT_TRUE(objects.hasValue()) << objects.error().toString();

    ASSERT_EQ(objects.value().size(), 2u);
    EXPECT_EQ(objects.value()[0].get("childType"), "Folder,VirtualMachine,VirtualApp");
    EXPECT_EQ(objects.value()[1].moref.value, "ha-folder-host");
    EXPECT_EQ(objects.value()[1].get("childType"), "ComputeResource");

    const auto continuation = sentRequestBody(fixture.transport, "urn:vim25/ContinueRetrievePropertiesEx");
    EXPECT_STREQ(continuation.select_node("//token").node().text().as_string(), "page2");
}

TEST(VimListObjectsTest, DestroysViewEvenWhenRetrieveFails)
{
    Fixture fixture;
    scriptEsxiSessionBootstrap(fixture.transport);
    // RetrievePropertiesEx deliberately unscripted: the fake fails it.

    auto session = VimSession::open(fixture.soap, "root", "pw");
    ASSERT_TRUE(session.hasValue());
    VimInventory inventory(*session.value());
    auto objects = inventory.listObjects("Datastore", {"name"});
    EXPECT_FALSE(objects.hasValue());
    EXPECT_TRUE(wasSent(fixture.transport, "urn:vim25/DestroyView"));
}

TEST(VimListObjectsTest, FailsWithoutViewManager)
{
    Fixture fixture;
    fixture.transport.queueForSoapAction(
        "urn:vim25/RetrieveServiceContent",
        xmlResponse("<RetrieveServiceContentResponse xmlns=\"urn:vim25\"><returnval>"
                    "<rootFolder type=\"Folder\">ha-folder-root</rootFolder>"
                    "<propertyCollector type=\"PropertyCollector\">ha-property-collector</propertyCollector>"
                    "<searchIndex type=\"SearchIndex\">ha-searchindex</searchIndex>"
                    "<sessionManager type=\"SessionManager\">ha-sessionmgr</sessionManager>"
                    "</returnval></RetrieveServiceContentResponse>"));
    fixture.transport.queueForSoapAction("urn:vim25/Login",
                                         xmlResponse("<LoginResponse xmlns=\"urn:vim25\"><returnval><key>k</key>"
                                                     "</returnval></LoginResponse>"));

    auto session = VimSession::open(fixture.soap, "root", "pw");
    ASSERT_TRUE(session.hasValue());
    VimInventory inventory(*session.value());
    auto objects = inventory.listObjects("Network", {"name"});
    ASSERT_FALSE(objects.hasValue());
    EXPECT_EQ(objects.error().code(), VSphereSoap::ErrorCode::InvalidFormat);
    EXPECT_FALSE(wasSent(fixture.transport, "urn:vim25/CreateContainerView"));
}

} // namespace VSphereSoap::Detail
