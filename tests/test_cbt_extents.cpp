// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for vSphere CBT changed-extent queries

#include <gtest/gtest.h>

#include "vsphere-soap/vim/CbtQuery.hpp"
#include "vsphere-soap/vim/VimInventory.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "fake_http_transport.hpp"

namespace VSphereSoap::Detail {
namespace {

using namespace VSphereSoap::Vim;

using Test::FakeHttpTransport;

HttpResponse xmlResponse(std::string body)
{
    HttpResponse response;
    response.status = 200;
    response.body = std::move(body);
    return response;
}

// Scripts the RetrieveServiceContent + Login handshake every VimSession::open needs before any other
// call can go through: shared by every test here that needs a live VimSession.
void scriptSessionBootstrap(FakeHttpTransport& transport)
{
    transport.queueForSoapAction(
        "urn:vim25/RetrieveServiceContent",
        xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<soapenv:Body><RetrieveServiceContentResponse xmlns=\"urn:vim25\"><returnval>"
                    "<rootFolder type=\"Folder\">group-d1</rootFolder>"
                    "<propertyCollector type=\"PropertyCollector\">propertyCollector</propertyCollector>"
                    "<searchIndex type=\"SearchIndex\">SearchIndex</searchIndex>"
                    "<sessionManager type=\"SessionManager\">SessionManager</sessionManager>"
                    "</returnval></RetrieveServiceContentResponse></soapenv:Body></soapenv:Envelope>"));
    transport.queueForSoapAction(
        "urn:vim25/Login",
        xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<soapenv:Body><LoginResponse xmlns=\"urn:vim25\"><returnval>"
                    "<key>session-key</key></returnval></LoginResponse></soapenv:Body></soapenv:Envelope>"));
}

std::string queryChangedDiskAreasResponse(std::uint64_t startOffset, std::uint64_t length,
                                           const std::vector<Extent>& changedAreas)
{
    std::string body = "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<soapenv:Body><QueryChangedDiskAreasResponse xmlns=\"urn:vim25\"><returnval>"
                        "<startOffset>" +
                        std::to_string(startOffset) + "</startOffset><length>" + std::to_string(length) +
                        "</length>";
    for (const auto& area : changedAreas) {
        body += "<changedArea><start>" + std::to_string(area.start) + "</start><length>" +
                std::to_string(area.length) + "</length></changedArea>";
    }
    body += "</returnval></QueryChangedDiskAreasResponse></soapenv:Body></soapenv:Envelope>";
    return body;
}

// Bundles a FakeHttpTransport with the SoapClient wrapping it, in declaration order (transport before
// soap) so the SoapClient's reference member is always valid: every test needing a live VimSession
// builds one of these, scripts responses on `.transport`, then opens a session against `.soap`.
struct SoapFixture {
    FakeHttpTransport transport;
    SoapClient soap{transport, "vcenter.example.com"};
};

TEST(CbtQueryEligibilityTest, RejectsWhenChangeTrackingDisabled)
{
    VimVmInfo vm;
    vm.name = "myvm";
    vm.changeTrackingEnabled = false;
    vm.hwVersion = "vmx-19";
    VimDiskInfo disk;
    disk.key = "2000";
    disk.diskMode = "persistent";

    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());

    CbtQuery cbt(*session.value());
    const auto result = cbt.checkEligible(vm, disk);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), VSphereSoap::ErrorCode::NotImplemented);
    EXPECT_NE(result.message().find("not enabled"), std::string::npos);
}

TEST(CbtQueryEligibilityTest, RejectsBelowHardwareVersionSeven)
{
    VimVmInfo vm;
    vm.changeTrackingEnabled = true;
    vm.hwVersion = "vmx-04";
    VimDiskInfo disk;
    disk.diskMode = "persistent";

    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());

    CbtQuery cbt(*session.value());
    const auto result = cbt.checkEligible(vm, disk);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.code(), VSphereSoap::ErrorCode::NotImplemented);
    EXPECT_NE(result.message().find("vmx-07"), std::string::npos);
}

TEST(CbtQueryEligibilityTest, RejectsRdmBacking)
{
    VimVmInfo vm;
    vm.changeTrackingEnabled = true;
    vm.hwVersion = "vmx-19";
    VimDiskInfo disk;
    disk.diskMode = "persistent";
    disk.backingType = "RawDiskMappingVer1BackingInfo";

    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());

    CbtQuery cbt(*session.value());
    const auto result = cbt.checkEligible(vm, disk);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.message().find("RDM"), std::string::npos);
}

TEST(CbtQueryEligibilityTest, RejectsIndependentDiskMode)
{
    VimVmInfo vm;
    vm.changeTrackingEnabled = true;
    vm.hwVersion = "vmx-19";
    VimDiskInfo disk;
    disk.diskMode = "independent_persistent";

    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());

    CbtQuery cbt(*session.value());
    const auto result = cbt.checkEligible(vm, disk);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.message().find("independent"), std::string::npos);
}

TEST(CbtQueryEligibilityTest, AcceptsEligibleDisk)
{
    VimVmInfo vm;
    vm.changeTrackingEnabled = true;
    vm.hwVersion = "vmx-19";
    VimDiskInfo disk;
    disk.diskMode = "persistent";
    disk.backingType = "VirtualDiskFlatVer2BackingInfo";

    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());

    CbtQuery cbt(*session.value());
    EXPECT_TRUE(cbt.checkEligible(vm, disk).ok());
}

TEST(CbtQueryLoopTest, LoopsUntilDiskCapacityIsCoveredAndPreservesExtentOrder)
{
    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);

    // Page 1 covers [0, 6'000'000); page 2 covers [6'000'000, 10'000'000), reaching the 10'000'000-byte
    // disk capacity: queryLoop must issue a second QueryChangedDiskAreas with startOffset=6'000'000, not
    // assume one call covers the whole disk.
    fixture.transport.queueForSoapAction(
        "urn:vim25/QueryChangedDiskAreas",
        xmlResponse(queryChangedDiskAreasResponse(0, 6'000'000, {{0, 1'000'000}, {2'000'000, 500'000}})));
    fixture.transport.queueForSoapAction(
        "urn:vim25/QueryChangedDiskAreas",
        xmlResponse(queryChangedDiskAreasResponse(6'000'000, 4'000'000, {{8'000'000, 2'000'000}})));

    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());

    CbtQuery cbt(*session.value());
    const MoRef vm{"VirtualMachine", "vm-1"};
    const MoRef snapshot{"VirtualMachineSnapshot", "snapshot-1"};
    auto result = cbt.queryAllocatedExtents(vm, snapshot, "2000", 10'000'000);
    ASSERT_TRUE(result.hasValue());

    const std::vector<Extent> expected{{0, 1'000'000}, {2'000'000, 500'000}, {8'000'000, 2'000'000}};
    ASSERT_EQ(result.value().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(result.value()[i].start, expected[i].start) << "extent " << i;
        EXPECT_EQ(result.value()[i].length, expected[i].length) << "extent " << i;
    }

    // Every page is asked at the snapshot it was given.
    EXPECT_NE(fixture.transport.m_requests.back().body.find("snapshot-1"), std::string::npos);
}

TEST(CbtQueryLoopTest, LeavesTheSnapshotOutWhenThereIsNone)
{
    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForSoapAction(
        "urn:vim25/QueryChangedDiskAreas",
        xmlResponse(queryChangedDiskAreasResponse(0, 10'000'000, {{4096, 8192}})));

    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());

    CbtQuery cbt(*session.value());
    auto result = cbt.queryChangedExtents(MoRef{"VirtualMachine", "vm-1"}, MoRef{}, "2000", "52 aa/7", 10'000'000);
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0].start, 4096u);
    EXPECT_EQ(result.value()[0].length, 8192u);

    // vSphere refuses an empty <snapshot>, so a query of the VM's current state has to send none at all.
    const std::string& body = fixture.transport.m_requests.back().body;
    EXPECT_NE(body.find("<changeId>52 aa/7</changeId>"), std::string::npos);
    EXPECT_EQ(body.find("snapshot"), std::string::npos);
}

TEST(CbtQueryLoopTest, FailsRatherThanLoopingForeverOnNoProgress)
{
    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    // startOffset + length == 0, i.e. no progress past offset 0.
    fixture.transport.queueForSoapAction("urn:vim25/QueryChangedDiskAreas",
                                          xmlResponse(queryChangedDiskAreasResponse(0, 0, {})));

    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());

    CbtQuery cbt(*session.value());
    const MoRef vm{"VirtualMachine", "vm-1"};
    const MoRef snapshot{"VirtualMachineSnapshot", "snapshot-1"};
    auto result = cbt.queryAllocatedExtents(vm, snapshot, "2000", 10'000'000);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), VSphereSoap::ErrorCode::InvalidFormat);
}

TEST(MergeExtentsTest, SortsAndJoinsOverlappingAndTouchingExtents)
{
    const auto merged = mergeExtents({{100, 50}, {0, 10}, {10, 20}, {140, 30}, {300, 0}, {200, 10}});
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(merged[0].start, 0u);
    EXPECT_EQ(merged[0].length, 30u);
    EXPECT_EQ(merged[1].start, 100u);
    EXPECT_EQ(merged[1].length, 70u);
    EXPECT_EQ(merged[2].start, 200u);
    EXPECT_EQ(merged[2].length, 10u);
}

TEST(DescribeSnapshotDisksTest, ReadsTheDiskChainAndChangeIdAtTheSnapshot)
{
    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForSoapAction(
        "urn:vim25/RetrievePropertiesEx",
        xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><soapenv:Body>"
                    "<RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><objects>"
                    "<obj type=\"VirtualMachineSnapshot\">snapshot-2</obj>"
                    "<propSet><name>config.hardware.device</name><val>"
                    "<VirtualDevice xsi:type=\"VirtualDisk\"><key>2000</key><capacityInBytes>1048576</capacityInBytes>"
                    "<backing xsi:type=\"VirtualDiskFlatVer2BackingInfo\">"
                    "<fileName>[ds] vm/vm-000001.vmdk</fileName><changeId>52 aa/7</changeId>"
                    "<deltaDiskFormat>seSparseFormat</deltaDiskFormat>"
                    "<parent xsi:type=\"VirtualDiskFlatVer2BackingInfo\"><fileName>[ds] vm/vm.vmdk</fileName></parent>"
                    "</backing></VirtualDevice>"
                    "</val></propSet></objects></returnval></RetrievePropertiesExResponse>"
                    "</soapenv:Body></soapenv:Envelope>"));

    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());
    VimInventory inventory(*session.value());
    auto disks = inventory.describeSnapshotDisks(MoRef{"VirtualMachineSnapshot", "snapshot-2"});
    ASSERT_TRUE(disks) << disks.error().toString();
    ASSERT_EQ(disks.value().size(), 1u);
    const VimDiskInfo& disk = disks.value().front();
    EXPECT_EQ(disk.key, "2000");
    EXPECT_EQ(disk.capacityBytes, 1048576u);
    EXPECT_EQ(disk.changeId.value_or(""), "52 aa/7");
    EXPECT_EQ(disk.deltaDiskFormat, "seSparseFormat");
    ASSERT_EQ(disk.parents.size(), 1u);
    EXPECT_EQ(disk.parents.front().fileName, "[ds] vm/vm.vmdk");
    EXPECT_TRUE(disk.parents.front().deltaDiskFormat.empty());
}

} // namespace
} // namespace VSphereSoap::Detail
