// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for vSphere disk provisioning requests

// Envelope-level unit tests for the two SOAP request shapes that actually control a vSphere-attached
// VMDK's real provisioning:
//   1. CreateImportSpec's OvfCreateImportSpecParams.diskProvisioning: tells vCenter/ESXi how to allocate
//      the destination disk on the datastore *before* any upload byte arrives (HttpNfcLeaseSession.cpp).
//   2. ReconfigVM_Task's VirtualDiskFlatVer2BackingInfo.thinProvisioned/eagerlyScrub: what the attach
//      step reports back to vCenter about a disk that's already been written (DiskAttach.cpp's
//      reconfigAddDisk, via attachDiskFile).
//
// Both are exercised through a FakeHttpTransport the same way tests/test_cbt_extents.cpp already does
// for CbtQuery: script just enough of the VimSession bootstrap + the one response actually needed to
// reach the call under test, then let the *next* call fail unscripted; the request that matters was
// already recorded by FakeHttpTransport before that failure, so there's no need to fully script
// a multi-call sequence through to a successful end-to-end result just to inspect one envelope.

#include <gtest/gtest.h>

#include <array>

#include "DiskAttach.hpp"
#include "HttpNfcLeaseSession.hpp"
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

// Same handshake as test_cbt_extents.cpp's own scriptSessionBootstrap, plus ovfManager
// (HttpNfcLeaseSession::begin requires a non-empty one): that file's copy doesn't need it, so this is
// its own, slightly richer variant rather than a shared helper neither file fully needs.
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
                    "<ovfManager type=\"OvfManager\">OvfManager</ovfManager>"
                    "<fileManager type=\"FileManager\">FileManager</fileManager>"
                    "</returnval></RetrieveServiceContentResponse></soapenv:Body></soapenv:Envelope>"));
    transport.queueForSoapAction(
        "urn:vim25/Login",
        xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<soapenv:Body><LoginResponse xmlns=\"urn:vim25\"><returnval>"
                    "<key>session-key</key></returnval></LoginResponse></soapenv:Body></soapenv:Envelope>"));
}

struct SoapFixture {
    FakeHttpTransport transport;
    SoapClient soap{transport, "vcenter.example.com"};
};

// Finds the most recently recorded request for `soapAction` and parses its body as XML: mirrors
// test_soap_client.cpp's own "re-parse transport.m_requests.back().body" pattern for asserting on
// exactly what a caller sent, not just that a call happened.
pugi::xml_document sentRequestBody(const FakeHttpTransport& transport, const std::string& soapAction)
{
    pugi::xml_document doc;
    for (auto it = transport.m_requests.rbegin(); it != transport.m_requests.rend(); ++it) {
        for (const auto& h : it->headers) {
            if (h.name == "SOAPAction" && h.value == soapAction) {
                doc.load_string(it->body.c_str());
                return doc;
            }
        }
    }
    return doc; // empty document if never sent
}

} // namespace

TEST(DiskProvisioningEnvelopeTest, CreateImportSpecCarriesRequestedDiskProvisioning)
{
    // {requested value, expected wire value}; empty means "don't set ImportVmSpec::diskProvisioning at
    // all", which must omit the element entirely rather than sending an empty one (see
    // HttpNfcLeaseSession.cpp's own comment on why: an empty element is not the same as "let vCenter
    // pick its own default" on the wire).
    //
    const std::array<std::pair<std::string, std::string>, 4> cases{{
        {"thin", "thin"},
        {"thick", "thick"},
        {"eagerZeroedThick", "eagerZeroedThick"},
        {"", ""},
    }};

    for (const auto& [requested, expectedWireValue] : cases) {
        SCOPED_TRACE("diskProvisioning='" + requested + "'");
        SoapFixture fixture;
        scriptSessionBootstrap(fixture.transport);
        // CreateImportSpec's response: just enough for begin() to move on to ImportVApp (which is
        // deliberately left unscripted; see this file's own header comment on why that's fine for this
        // test).
        fixture.transport.queueForSoapAction(
            "urn:vim25/CreateImportSpec",
            xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<soapenv:Body><CreateImportSpecResponse xmlns=\"urn:vim25\"><returnval>"
                        "<importSpec xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                        "xsi:type=\"VirtualMachineImportSpec\"><configSpec><name>placeholder</name>"
                        "</configSpec></importSpec>"
                        "</returnval></CreateImportSpecResponse></soapenv:Body></soapenv:Envelope>"));

        auto session = VimSession::open(fixture.soap, "user", "pass");
        ASSERT_TRUE(session.hasValue());

        ImportVmSpec spec;
        spec.vmName = "alta-import-test";
        spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
        spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
        spec.diskProvisioning = requested;

        auto result = HttpNfcLeaseSession::begin(*session.value(), spec);
        // ImportVApp was never scripted, so begin() is expected to fail; that's fine, CreateImportSpec
        // (the call under test) already ran.
        ASSERT_FALSE(result.hasValue());

        const pugi::xml_document sent = sentRequestBody(fixture.transport, "urn:vim25/CreateImportSpec");
        const pugi::xml_node cisp = sent.select_node("//cisp").node();
        ASSERT_TRUE(cisp) << "CreateImportSpec request had no <cisp> element";

        // diskProvisioning is the last element this codebase sends in OvfCreateImportSpecParams'
        // xsd:sequence (see HttpNfcLeaseSession.cpp): with no network adapters in this spec it directly
        // follows entityName.
        // Pinning its position matters as much as its value: a real vCenter rejects an out-of-sequence
        // element outright.
        // NetworkMappingEnvelopeTest covers the with-adapters ordering.
        //
        const pugi::xml_node entityName = cisp.child("entityName");
        ASSERT_TRUE(entityName);
        const pugi::xml_node diskProvisioningNode = cisp.child("diskProvisioning");
        if (expectedWireValue.empty()) {
            EXPECT_FALSE(diskProvisioningNode)
                << "an unset diskProvisioning must omit the element, not send it empty";
        } else {
            ASSERT_TRUE(diskProvisioningNode);
            EXPECT_EQ(std::string(diskProvisioningNode.text().as_string()), expectedWireValue);
            EXPECT_EQ(diskProvisioningNode.previous_sibling(), entityName)
                << "with no networkMapping, diskProvisioning must directly follow entityName";
            EXPECT_FALSE(diskProvisioningNode.next_sibling()) << "diskProvisioning must be the last cisp element";
        }
    }
}

TEST(DiskProvisioningEnvelopeTest, ReconfigAddDiskReportsThinAndEagerlyScrubMatchingWhatWasRequested)
{
    // {diskProvisioning, expected thinProvisioned, expected eagerlyScrub}
    struct Case {
        std::string diskProvisioning;
        std::string expectedThin;
        std::string expectedEager;
    };
    const std::array<Case, 4> cases{{
        // empty defaults to thin, matching every pre-existing caller
        {"", "true", "false"},
        {"thin", "true", "false"},
        {"thick", "false", "false"},             // lazy-zeroed thick: not thin, not eager-scrubbed
        {"eagerZeroedThick", "false", "true"},
    }};

    for (const auto& c : cases) {
        SCOPED_TRACE("diskProvisioning='" + c.diskProvisioning + "'");
        SoapFixture fixture;
        scriptSessionBootstrap(fixture.transport);

        // describeVm's RetrievePropertiesEx: one existing ParaVirtualSCSI controller, no disks yet, so
        // chooseControllerAndUnit resolves it as the auto-picked primary without needing
        // a controller-create sub-call.
        fixture.transport.queueForSoapAction(
            "urn:vim25/RetrievePropertiesEx",
            xmlResponse(
                "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<soapenv:Body><RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><objects>"
                "<obj type=\"VirtualMachine\">vm-1</obj>"
                "<propSet><name>config.hardware.device</name><val xsi:type=\"ArrayOfVirtualDevice\" "
                "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
                "<VirtualDevice xsi:type=\"ParaVirtualSCSIController\">"
                "<key>1000</key><busNumber>0</busNumber></VirtualDevice>"
                "</val></propSet>"
                "</objects></returnval></RetrievePropertiesExResponse></soapenv:Body></soapenv:Envelope>"));

        if (c.diskProvisioning == "eagerZeroedThick") {
            // attachDiskFile's pre-attach fixup (see DiskAttach.cpp) walks vm-1 -> its parent Datacenter
            // (resolveDatacenterMoRef) before calling EagerZeroVirtualDisk_Task: both must succeed
            // before ReconfigVM_Task, the request under test, is even reached.
            // Two hops: [1] query vm-1 itself, returning its parent (dc-1, type Datacenter); [2] query
            // dc-1 itself, whose type being "Datacenter" is what ends the walk.
            //
            fixture.transport.queueForSoapAction(
                "urn:vim25/RetrievePropertiesEx",
                xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                            "<soapenv:Body><RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><objects>"
                            "<obj type=\"VirtualMachine\">vm-1</obj>"
                            "<propSet><name>name</name><val>myvm</val></propSet>"
                            "<propSet><name>parent</name><val type=\"Datacenter\">dc-1</val></propSet>"
                            "</objects></returnval></RetrievePropertiesExResponse></soapenv:Body></soapenv:Envelope>"));
            fixture.transport.queueForSoapAction(
                "urn:vim25/RetrievePropertiesEx",
                xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                            "<soapenv:Body><RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><objects>"
                            "<obj type=\"Datacenter\">dc-1</obj>"
                            "<propSet><name>name</name><val>dc1</val></propSet>"
                            "</objects></returnval></RetrievePropertiesExResponse></soapenv:Body></soapenv:Envelope>"));
            fixture.transport.queueForSoapAction(
                "urn:vim25/EagerZeroVirtualDisk_Task",
                xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                            "<soapenv:Body><EagerZeroVirtualDisk_TaskResponse xmlns=\"urn:vim25\">"
                            "<returnval type=\"Task\">task-1</returnval>"
                            "</EagerZeroVirtualDisk_TaskResponse></soapenv:Body></soapenv:Envelope>"));
            // waitForVimTask's own RetrievePropertiesEx poll on task-1.
            fixture.transport.queueForSoapAction(
                "urn:vim25/RetrievePropertiesEx",
                xmlResponse(
                    "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<soapenv:Body><RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><objects>"
                    "<obj type=\"Task\">task-1</obj>"
                    "<propSet><name>info.state</name><val>success</val></propSet>"
                    "</objects></returnval></RetrievePropertiesExResponse></soapenv:Body></soapenv:Envelope>"));
        }
        // ReconfigVM_Task is deliberately left unscripted: the request under test is recorded by
        // FakeHttpTransport before that failure.

        auto session = VimSession::open(fixture.soap, "user", "pass");
        ASSERT_TRUE(session.hasValue());

        ControllerSelector selector; // fully automatic: no type/bus preference.
        auto result = attachDiskFile(*session.value(), MoRef{"VirtualMachine", "vm-1"}, "[ds-01] myvm/myvm.vmdk",
                                       4ull * 1024 * 1024, selector, c.diskProvisioning);
        ASSERT_FALSE(result.hasValue()); // expected: ReconfigVM_Task itself was never scripted

        const pugi::xml_document sent = sentRequestBody(fixture.transport, "urn:vim25/ReconfigVM_Task");
        const pugi::xml_node backing = sent.select_node("//device/backing").node();
        ASSERT_TRUE(backing) << "ReconfigVM_Task request had no <device><backing> element";

        const pugi::xml_node thin = backing.child("thinProvisioned");
        const pugi::xml_node eager = backing.child("eagerlyScrub");
        ASSERT_TRUE(thin);
        ASSERT_TRUE(eager);
        EXPECT_EQ(std::string(thin.text().as_string()), c.expectedThin);
        EXPECT_EQ(std::string(eager.text().as_string()), c.expectedEager);
        EXPECT_EQ(eager.previous_sibling(), thin) << "eagerlyScrub must immediately follow thinProvisioned";
    }
}

} // namespace VSphereSoap::Detail
