// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Name-based placement lookup and the streamOptimized descriptor helpers

#include <gtest/gtest.h>

#include <string>

#include "Placement.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "fake_http_transport.hpp"
#include "vsphere-soap/VSphereDatastoreBackend.hpp"

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

// One <objects> element of a RetrievePropertiesEx answer.
struct Obj {
    std::string type;
    std::string id;
    std::string props;
};

std::string prop(const std::string& name, const std::string& value)
{
    return "<propSet><name>" + name + "</name><val>" + value + "</val></propSet>";
}

std::string morefList(const std::string& type, std::initializer_list<const char*> ids)
{
    std::string out;
    for (const char* id : ids) {
        out += "<ManagedObjectReference type=\"" + type + "\">" + id + "</ManagedObjectReference>";
    }
    return out;
}

HttpResponse objects(std::initializer_list<Obj> list)
{
    std::string body = "<RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval>";
    for (const Obj& o : list) {
        body += "<objects><obj type=\"" + o.type + "\">" + o.id + "</obj>" + o.props + "</objects>";
    }
    return xmlResponse(body + "</returnval></RetrievePropertiesExResponse>");
}

// A vCenter with two datacenters' worth of objects, answering the lookups in the order the resolver
// makes them.
struct Fixture {
    FakeHttpTransport transport;
    SoapClient soap{transport, "vcenter.example.com"};

    Fixture()
    {
        transport.queueForSoapAction(
            "urn:vim25/RetrieveServiceContent",
            xmlResponse("<RetrieveServiceContentResponse xmlns=\"urn:vim25\"><returnval>"
                        "<rootFolder type=\"Folder\">group-d1</rootFolder>"
                        "<propertyCollector type=\"PropertyCollector\">propertyCollector</propertyCollector>"
                        "<viewManager type=\"ViewManager\">ViewManager</viewManager>"
                        "<searchIndex type=\"SearchIndex\">SearchIndex</searchIndex>"
                        "<sessionManager type=\"SessionManager\">SessionManager</sessionManager>"
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

    void answer(HttpResponse response)
    {
        transport.queueForSoapAction("urn:vim25/RetrievePropertiesEx", std::move(response));
    }

    void answerDatastores()
    {
        answer(objects({{"Datastore", "datastore-11", prop("name", "ssd-01")},
                        {"Datastore", "datastore-12", prop("name", "shared-nfs")}}));
    }

    // cluster-A sees both datastores, host-B (a standalone host) only the shared one.
    void answerComputes()
    {
        answer(objects({{"ClusterComputeResource", "domain-c7",
                         prop("name", "cluster-A") +
                             prop("datastore", morefList("Datastore", {"datastore-11", "datastore-12"})) +
                             prop("resourcePool", "resgroup-8")},
                        {"ComputeResource", "domain-s20",
                         prop("name", "host-B") + prop("datastore", morefList("Datastore", {"datastore-12"})) +
                             prop("resourcePool", "resgroup-21")}}));
    }

    void answerDatacenters()
    {
        answer(objects({{"Datacenter", "datacenter-3",
                         prop("name", "DC1") +
                             prop("datastore", morefList("Datastore", {"datastore-11", "datastore-12"})) +
                             prop("vmFolder", "group-v4")}}));
    }

    Result<VSpherePlacement> resolve(const VSpherePlacementSpec& spec)
    {
        auto session = VimSession::open(soap, "administrator@vsphere.local", "pw");
        if (!session) {
            return std::move(session).error();
        }
        return resolvePlacement(*session.value(), spec);
    }
};

} // namespace

TEST(PlacementTest, DatastoreAloneResolvesToItsOnlyClusterRootPoolAndDatacenterFolder)
{
    Fixture fixture;
    fixture.answerDatastores();
    fixture.answerComputes();
    fixture.answerDatacenters();

    auto placement = fixture.resolve({.datastoreName = "ssd-01"});

    ASSERT_TRUE(placement.hasValue()) << placement.error().toString();
    EXPECT_EQ(placement.value().datastoreId, "datastore-11");
    EXPECT_EQ(placement.value().resourcePoolId, "resgroup-8");
    EXPECT_EQ(placement.value().folderId, "group-v4");
}

TEST(PlacementTest, DatastoreSeenByTwoComputeResourcesNeedsOneNamed)
{
    Fixture fixture;
    fixture.answerDatastores();
    fixture.answerComputes();

    auto placement = fixture.resolve({.datastoreName = "shared-nfs"});

    ASSERT_FALSE(placement.hasValue());
    EXPECT_EQ(placement.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(placement.error().message().find("'cluster-A' (domain-c7)"), std::string::npos);
    EXPECT_NE(placement.error().message().find("'host-B' (domain-s20)"), std::string::npos);
}

TEST(PlacementTest, NamedStandaloneHostPicksItsOwnRootPool)
{
    Fixture fixture;
    fixture.answerDatastores();
    fixture.answerComputes();
    fixture.answerDatacenters();

    auto placement = fixture.resolve({.datastoreName = "shared-nfs", .computeName = "host-B"});

    ASSERT_TRUE(placement.hasValue()) << placement.error().toString();
    EXPECT_EQ(placement.value().resourcePoolId, "resgroup-21");
}

TEST(PlacementTest, NamedResourcePoolIsTakenFromTheChosenClusterOnly)
{
    Fixture fixture;
    fixture.answerDatastores();
    fixture.answerComputes();
    fixture.answer(objects({{"ResourcePool", "resgroup-30", prop("name", "imports") + prop("owner", "domain-s20")},
                            {"ResourcePool", "resgroup-31", prop("name", "imports") + prop("owner", "domain-c7")}}));
    fixture.answerDatacenters();

    auto placement = fixture.resolve({.datastoreName = "ssd-01", .resourcePoolName = "imports"});

    ASSERT_TRUE(placement.hasValue()) << placement.error().toString();
    EXPECT_EQ(placement.value().resourcePoolId, "resgroup-31");
}

TEST(PlacementTest, NamedFolderMustHoldVirtualMachines)
{
    Fixture fixture;
    fixture.answerDatastores();
    fixture.answerComputes();
    fixture.answer(objects({{"Folder", "group-h5", prop("name", "imports") + prop("childType", "<string>Folder</string>"
                                                                                              "<string>ComputeResource</string>")},
                            {"Folder", "group-v40", prop("name", "imports") + prop("childType", "<string>Folder</string>"
                                                                                               "<string>VirtualMachine</string>")}}));

    auto placement = fixture.resolve({.datastoreName = "ssd-01", .folderName = "imports"});

    ASSERT_TRUE(placement.hasValue()) << placement.error().toString();
    EXPECT_EQ(placement.value().folderId, "group-v40");
}

TEST(PlacementTest, UnknownDatastoreIsAnInvalidArgument)
{
    Fixture fixture;
    fixture.answerDatastores();

    auto placement = fixture.resolve({.datastoreName = "missing"});

    ASSERT_FALSE(placement.hasValue());
    EXPECT_EQ(placement.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(placement.error().message(), "no datastore named 'missing'");
}

TEST(PlacementTest, EmptyDatastoreNameIsRefusedBeforeAnyLookup)
{
    Fixture fixture;

    auto placement = fixture.resolve({});

    ASSERT_FALSE(placement.hasValue());
    EXPECT_EQ(placement.error().code(), ErrorCode::InvalidArgument);
}

TEST(StreamOptimizedDescriptorTest, RoundsUpToWholeGrains)
{
    EXPECT_EQ(roundUpToGrain(0), 0u);
    EXPECT_EQ(roundUpToGrain(1), 65536u);
    EXPECT_EQ(roundUpToGrain(65536), 65536u);
    EXPECT_EQ(roundUpToGrain(65537), 131072u);
    EXPECT_EQ(roundUpToGrain(1000, 8), 4096u);
}

TEST(StreamOptimizedDescriptorTest, NamesSectorsExtentAndAdapter)
{
    const std::uint64_t capacity = 10ull * 1024 * 1024 * 1024;
    const std::string descriptor = makeStreamOptimizedDescriptor(capacity, "disk0.vmdk", "lsilogic");

    EXPECT_EQ(descriptor.rfind("# Disk DescriptorFile\n", 0), 0u);
    EXPECT_NE(descriptor.find("createType=\"streamOptimized\"\n"), std::string::npos);
    EXPECT_NE(descriptor.find("RW 20971520 SPARSE \"disk0.vmdk\"\n"), std::string::npos);
    EXPECT_NE(descriptor.find("ddb.adapterType = \"lsilogic\"\n"), std::string::npos);
    EXPECT_NE(descriptor.find("ddb.geometry.cylinders = \"1305\"\n"), std::string::npos);
}

} // namespace VSphereSoap::Detail
