// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for vSphere network mapping requests

// Envelope-level unit tests for the network adapters of an imported VM: HttpNfcLeaseSession::begin()
// building both the embedded OVF descriptor's <NetworkSection>/Ethernet <Item> entries
// (HttpNfcLeaseSession.cpp's buildOvfDescriptor()) and CreateImportSpec's <networkMapping> entries, from
// ImportVmSpec::networkAdapters.
// Same FakeHttpTransport-based approach as test_vsphere_disk_provisioning.cpp: script just enough of
// the VimSession bootstrap + CreateImportSpec's response to reach the call under test, then let
// ImportVApp fail unscripted; the request that matters was already recorded before that failure.

#include <gtest/gtest.h>

#include "HttpNfcLeaseSession.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "vsphere-soap/vim/VimInventory.hpp"
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

// Same handshake as test_vsphere_disk_provisioning.cpp's own scriptSessionBootstrap.
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
    return doc;
}

// Runs HttpNfcLeaseSession::begin() with `spec` against a freshly-scripted session, returning the parsed
// <cisp> node and the parsed OVF descriptor document (re-parsed from CreateImportSpec's own
// <ovfDescriptor> text); everything every test below needs to assert against.
struct BeginResult {
    pugi::xml_node cisp;
    pugi::xml_document cispDoc; // owns cisp's storage
    pugi::xml_document ovfDoc;  // owns the re-parsed OVF descriptor
};

BeginResult runBegin(SoapFixture& fixture, const ImportVmSpec& spec)
{
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForSoapAction(
        "urn:vim25/CreateImportSpec",
        xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<soapenv:Body><CreateImportSpecResponse xmlns=\"urn:vim25\"><returnval>"
                    "<importSpec xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                    "xsi:type=\"VirtualMachineImportSpec\"><configSpec><name>placeholder</name>"
                    "</configSpec></importSpec>"
                    "</returnval></CreateImportSpecResponse></soapenv:Body></soapenv:Envelope>"));

    auto session = VimSession::open(fixture.soap, "user", "pass");
    EXPECT_TRUE(session.hasValue());

    auto result = HttpNfcLeaseSession::begin(*session.value(), spec);
    EXPECT_FALSE(result.hasValue()); // ImportVApp was never scripted, as expected.

    BeginResult out;
    out.cispDoc = sentRequestBody(fixture.transport, "urn:vim25/CreateImportSpec");
    out.cisp = out.cispDoc.select_node("//cisp").node();

    const pugi::xml_node ovfDescriptorNode = out.cispDoc.select_node("//ovfDescriptor").node();
    if (ovfDescriptorNode) {
        out.ovfDoc.load_string(ovfDescriptorNode.text().as_string());
    }
    return out;
}

} // namespace

TEST(NetworkMappingEnvelopeTest, ConnectedAdapterGetsNetworkSectionEthernetItemAndMapping)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    ImportNetworkAdapterSpec nic;
    nic.sourceName = "VM Network";
    nic.ovfNetworkName = "network0";
    nic.network = MoRef{"Network", "network-52"};
    nic.adapterType = "VmxNet3";
    spec.networkAdapters.push_back(nic);

    const BeginResult result = runBegin(fixture, spec);
    ASSERT_TRUE(result.cisp) << "CreateImportSpec request had no <cisp> element";

    // networkMapping: name -> real MoRef.
    const pugi::xml_node mapping = result.cisp.child("networkMapping");
    ASSERT_TRUE(mapping);
    EXPECT_EQ(std::string(mapping.child("name").text().as_string()), "network0");
    const pugi::xml_node network = mapping.child("network");
    ASSERT_TRUE(network);
    EXPECT_EQ(std::string(network.attribute("type").as_string()), "Network");
    EXPECT_EQ(std::string(network.text().as_string()), "network-52");
    EXPECT_FALSE(mapping.next_sibling("networkMapping")) << "exactly one networkMapping entry expected";

    // OVF descriptor: <NetworkSection><Network ovf:name="network0">.
    ASSERT_TRUE(result.ovfDoc.root().first_child()) << "ovfDescriptor did not re-parse as XML";
    const pugi::xml_node networkSection = result.ovfDoc.select_node("//NetworkSection").node();
    ASSERT_TRUE(networkSection);
    const pugi::xml_node ovfNetwork = networkSection.child("Network");
    ASSERT_TRUE(ovfNetwork);
    EXPECT_EQ(std::string(ovfNetwork.attribute("ovf:name").as_string()), "network0");

    // Ethernet Item: ResourceType 10, ResourceSubType VmxNet3, Connection network0.
    const pugi::xml_node ethernetItem = result.ovfDoc.select_node("//Item[rasd:ResourceType='10']").node();
    ASSERT_TRUE(ethernetItem) << "no Ethernet Item (ResourceType 10) found in the OVF descriptor";
    EXPECT_EQ(std::string(ethernetItem.child("rasd:ResourceSubType").text().as_string()), "VmxNet3");
    EXPECT_EQ(std::string(ethernetItem.child("rasd:Connection").text().as_string()), "network0");
    EXPECT_FALSE(ethernetItem.child("rasd:Address")) << "no MAC was requested; rasd:Address must be absent";
}

// vCenter requires rasd:Connection on every Ethernet Item (confirmed live), so a disconnected adapter points
// at a placeholder OVF network and is kept off it by AutomaticAllocation.
// With nothing connected there is no real network to map the placeholder to, so it stays unmapped.
TEST(NetworkMappingEnvelopeTest, DisconnectedAdapterHasPlaceholderConnectionAndNoMapping)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    ImportNetworkAdapterSpec nic;
    nic.sourceName = "Isolated";
    // ovfNetworkName/network both left empty: a disconnected adapter.
    spec.networkAdapters.push_back(nic);

    const BeginResult result = runBegin(fixture, spec);
    ASSERT_TRUE(result.cisp);
    EXPECT_FALSE(result.cisp.child("networkMapping"))
        << "with no connected adapter there is no real network to map the placeholder to";

    pugi::xpath_node_set networkNodes = result.ovfDoc.select_nodes("//NetworkSection/Network");
    ASSERT_EQ(networkNodes.size(), 1u);
    EXPECT_EQ(std::string(networkNodes.first().node().attribute("ovf:name").as_string()), "disconnected");
    const pugi::xml_node ethernetItem = result.ovfDoc.select_node("//Item[rasd:ResourceType='10']").node();
    ASSERT_TRUE(ethernetItem) << "the Ethernet Item itself must still be created (the guest still gets a NIC)";
    EXPECT_EQ(std::string(ethernetItem.child("rasd:Connection").text().as_string()), "disconnected");
    EXPECT_EQ(std::string(ethernetItem.child("rasd:AutomaticAllocation").text().as_string()), "false");
}

TEST(NetworkMappingEnvelopeTest, DisconnectedAdapterNextToConnectedOneKeepsOrderAndMapsPlaceholder)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    ImportNetworkAdapterSpec isolated;
    isolated.sourceName = "Isolated";
    spec.networkAdapters.push_back(isolated);
    ImportNetworkAdapterSpec connected;
    connected.sourceName = "VM Network";
    connected.ovfNetworkName = "network0";
    connected.network = MoRef{"Network", "network-52"};
    spec.networkAdapters.push_back(connected);

    const BeginResult result = runBegin(fixture, spec);
    ASSERT_TRUE(result.cisp);

    // network0 -> network-52 first, then the placeholder onto the same real network.
    const pugi::xml_node first = result.cisp.child("networkMapping");
    ASSERT_TRUE(first);
    EXPECT_EQ(std::string(first.child("name").text().as_string()), "network0");
    const pugi::xml_node second = first.next_sibling("networkMapping");
    ASSERT_TRUE(second);
    EXPECT_EQ(std::string(second.child("name").text().as_string()), "disconnected");
    EXPECT_EQ(std::string(second.child("network").attribute("type").as_string()), "Network");
    EXPECT_EQ(std::string(second.child("network").text().as_string()), "network-52");
    EXPECT_FALSE(second.next_sibling("networkMapping"));

    EXPECT_EQ(result.ovfDoc.select_nodes("//NetworkSection/Network").size(), 2u);

    pugi::xpath_node_set ethernetItems = result.ovfDoc.select_nodes("//Item[rasd:ResourceType='10']");
    ASSERT_EQ(ethernetItems.size(), 2u);
    const pugi::xml_node nic0 = ethernetItems[0].node();
    const pugi::xml_node nic1 = ethernetItems[1].node();
    EXPECT_EQ(std::string(nic0.child("rasd:AddressOnParent").text().as_string()), "0");
    EXPECT_EQ(std::string(nic0.child("rasd:Connection").text().as_string()), "disconnected");
    EXPECT_EQ(std::string(nic0.child("rasd:AutomaticAllocation").text().as_string()), "false");
    EXPECT_EQ(std::string(nic1.child("rasd:AddressOnParent").text().as_string()), "1");
    EXPECT_EQ(std::string(nic1.child("rasd:Connection").text().as_string()), "network0");
    EXPECT_EQ(std::string(nic1.child("rasd:AutomaticAllocation").text().as_string()), "true");
}

// vmw:Config must follow every Item inside VirtualHardwareSection: CreateImportSpec rejects each Item after one
// with "Unsupported element 'Item'" (confirmed live against vCenter 8.0.3).
TEST(NetworkMappingEnvelopeTest, EfiVmwConfigComesAfterEveryItem)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    spec.firmware = "efi";
    spec.secureBootEnabled = false;
    ImportNetworkAdapterSpec nic;
    nic.sourceName = "VM Network";
    nic.ovfNetworkName = "network0";
    nic.network = MoRef{"Network", "network-52"};
    spec.networkAdapters.push_back(nic);

    const BeginResult result = runBegin(fixture, spec);
    const pugi::xml_node hardware = result.ovfDoc.select_node("//VirtualHardwareSection").node();
    ASSERT_TRUE(hardware);

    std::vector<std::string> configKeys;
    bool seenConfig = false;
    for (const pugi::xml_node child : hardware.children()) {
        const std::string name = child.name();
        if (name == "vmw:Config") {
            seenConfig = true;
            configKeys.push_back(child.attribute("vmw:key").as_string());
        } else if (name == "Item") {
            EXPECT_FALSE(seenConfig) << "an Item follows a vmw:Config element";
        }
    }
    EXPECT_EQ(configKeys, (std::vector<std::string>{"firmware", "bootOptions.efiSecureBootEnabled"}));
    EXPECT_STREQ(hardware.last_child().name(), "vmw:Config");
    EXPECT_EQ(std::string(hardware.find_child_by_attribute("vmw:Config", "vmw:key", "firmware")
                              .attribute("vmw:value")
                              .as_string()),
              "efi");
}

TEST(NetworkMappingEnvelopeTest, BiosSpecEmitsNoVmwConfig)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    spec.firmware = "bios";
    spec.secureBootEnabled = false; // ignored without EFI

    const BeginResult result = runBegin(fixture, spec);
    ASSERT_TRUE(result.ovfDoc.select_node("//VirtualHardwareSection").node());
    EXPECT_TRUE(result.ovfDoc.select_nodes("//vmw:Config").empty());
}

TEST(NetworkMappingEnvelopeTest, StaticMacAddressIsCarriedAsRasdAddress)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    ImportNetworkAdapterSpec nic;
    nic.sourceName = "VM Network";
    nic.ovfNetworkName = "network0";
    nic.network = MoRef{"Network", "network-52"};
    nic.macAddress = "00:50:56:12:34:56";
    spec.networkAdapters.push_back(nic);

    const BeginResult result = runBegin(fixture, spec);
    const pugi::xml_node ethernetItem = result.ovfDoc.select_node("//Item[rasd:ResourceType='10']").node();
    ASSERT_TRUE(ethernetItem);
    EXPECT_EQ(std::string(ethernetItem.child("rasd:Address").text().as_string()), "00:50:56:12:34:56");
    // CIM sequence order: Address is the first rasd element of the Item; vCenter rejects it anywhere
    // else ("Unsupported element 'Address'").
    EXPECT_STREQ(ethernetItem.first_child().name(), "rasd:Address");
}

// Everything vCenter's strict schema validation rejects on the wire, in one realistic spec:
// networkMapping + diskProvisioning ordering inside <cisp>, alphabetical rasd element order inside every
// OVF Item, and OVF's own SCSI controller subtype spelling.
// Each of these failed a live CreateImportSpec against vCenter 8.0.3 before being fixed.
//
TEST(NetworkMappingEnvelopeTest, CreateImportSpecElementOrderMatchesVimAndCimSchemas)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    spec.diskProvisioning = "thin";
    spec.hardwareVersion = "vmx-14";
    ImportNetworkAdapterSpec nic;
    nic.sourceName = "VM Network";
    nic.ovfNetworkName = "network0";
    nic.network = MoRef{"Network", "network-52"};
    nic.adapterType = "VmxNet3";
    nic.macAddress = "00:50:56:12:34:56";
    spec.networkAdapters.push_back(nic);

    const BeginResult result = runBegin(fixture, spec);
    ASSERT_TRUE(result.cisp);
    const pugi::xml_node mapping = result.cisp.child("networkMapping");
    const pugi::xml_node provisioning = result.cisp.child("diskProvisioning");
    ASSERT_TRUE(mapping);
    ASSERT_TRUE(provisioning);
    EXPECT_EQ(mapping.next_sibling(), provisioning) << "diskProvisioning must come after networkMapping";

    for (const pugi::xpath_node& item : result.ovfDoc.select_nodes("//Item")) {
        std::string previous;
        for (const pugi::xml_node child : item.node().children()) {
            const std::string name = child.name();
            EXPECT_LT(previous, name) << "rasd elements out of CIM sequence order in an OVF Item";
            previous = name;
        }
    }

    const pugi::xml_node scsiItem = result.ovfDoc.select_node("//Item[rasd:ResourceType='6']").node();
    ASSERT_TRUE(scsiItem);
    EXPECT_EQ(std::string(scsiItem.child("rasd:ResourceSubType").text().as_string()), "VirtualSCSI");
    EXPECT_EQ(std::string(result.ovfDoc.select_node("//vssd:VirtualSystemType").node().text().as_string()), "vmx-14");
}

TEST(NetworkMappingEnvelopeTest, E1000eAdapterTypeIsCarriedAsResourceSubType)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    ImportNetworkAdapterSpec nic;
    nic.sourceName = "VM Network";
    nic.ovfNetworkName = "network0";
    nic.network = MoRef{"Network", "network-52"};
    nic.adapterType = "E1000e";
    spec.networkAdapters.push_back(nic);

    const BeginResult result = runBegin(fixture, spec);
    const pugi::xml_node ethernetItem = result.ovfDoc.select_node("//Item[rasd:ResourceType='10']").node();
    ASSERT_TRUE(ethernetItem);
    EXPECT_EQ(std::string(ethernetItem.child("rasd:ResourceSubType").text().as_string()), "E1000e");
}

TEST(NetworkMappingEnvelopeTest, ManyToOneMappingProducesOneNetworkSectionEntryAndOneMapping)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};

    ImportNetworkAdapterSpec nic0;
    nic0.sourceName = "VLAN10";
    nic0.ovfNetworkName = "network0";
    nic0.network = MoRef{"Network", "network-52"};
    spec.networkAdapters.push_back(nic0);

    ImportNetworkAdapterSpec nic1;
    nic1.sourceName = "VLAN20";
    // Two different source networks explicitly mapped to the same target: VSphereImportSession is what
    // actually dedupes ovfNetworkName in real use (see VSphereImportSession.cpp); at this
    // internal-linkage level, a caller reusing the same ovfNetworkName/network is exactly what that
    // dedup produces, and is what this test exercises directly.
    //
    nic1.ovfNetworkName = "network0";
    nic1.network = MoRef{"Network", "network-52"};
    spec.networkAdapters.push_back(nic1);

    const BeginResult result = runBegin(fixture, spec);
    ASSERT_TRUE(result.cisp);
    const pugi::xml_node mapping = result.cisp.child("networkMapping");
    ASSERT_TRUE(mapping);
    EXPECT_FALSE(mapping.next_sibling("networkMapping"))
        << "two adapters mapped to the same real network must produce exactly one networkMapping entry";

    pugi::xpath_node_set networkNodes = result.ovfDoc.select_nodes("//NetworkSection/Network");
    EXPECT_EQ(networkNodes.size(), 1u) << "two adapters on the same target network must share one NetworkSection entry";

    pugi::xpath_node_set ethernetItems = result.ovfDoc.select_nodes("//Item[rasd:ResourceType='10']");
    EXPECT_EQ(ethernetItems.size(), 2u) << "each adapter still gets its own Ethernet Item";
}

TEST(NetworkMappingEnvelopeTest, CyrillicNetworkNameSurvivesIntact)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    ImportNetworkAdapterSpec nic;
    nic.sourceName = "Коммутатор №1";
    nic.ovfNetworkName = "network0";
    nic.network = MoRef{"Network", "network-52"};
    spec.networkAdapters.push_back(nic);

    const BeginResult result = runBegin(fixture, spec);
    // ovfNetworkName itself is always plain ASCII ("networkN", caller-generated): what actually needs to
    // survive Cyrillic intact end to end is the *source* name, which never appears in the wire request
    // at all (it's a caller-side/logging-only field: see ImportNetworkAdapterSpec::sourceName's doc
    // comment).
    // This test confirms the OVF descriptor still parses and builds correctly when spec.vmName itself is
    // Cyrillic, since that's the field that actually flows into the descriptor's own escaped text
    // content.
    //
    ASSERT_TRUE(result.ovfDoc.select_node("//Item[rasd:ResourceType='10']").node());
}

TEST(NetworkMappingEnvelopeTest, CyrillicVmNameSurvivesIntactInOvfDescriptor)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "Виртуальная машина №1";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    ImportNetworkAdapterSpec nic;
    nic.sourceName = "VM Network";
    nic.ovfNetworkName = "network0";
    nic.network = MoRef{"Network", "network-52"};
    spec.networkAdapters.push_back(nic);

    const BeginResult result = runBegin(fixture, spec);
    const pugi::xml_node name = result.ovfDoc.select_node("//VirtualSystem/Name").node();
    ASSERT_TRUE(name);
    EXPECT_EQ(std::string(name.text().as_string()), "Виртуальная машина №1");
}

// --- VimInventory::describeVm() network adapter parsing ---

TEST(NetworkMappingEnvelopeTest, DescribeVmParsesStandardPortgroupNetworkAdapterDirectly)
{
    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForSoapAction(
        "urn:vim25/RetrievePropertiesEx",
        xmlResponse(
            "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<soapenv:Body><RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><objects>"
            "<obj type=\"VirtualMachine\">vm-1</obj>"
            "<propSet><name>config.hardware.device</name><val xsi:type=\"ArrayOfVirtualDevice\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
            "<VirtualDevice xsi:type=\"VirtualVmxnet3\">"
            "<key>4000</key><macAddress>00:50:56:aa:bb:cc</macAddress>"
            "<backing xsi:type=\"VirtualEthernetCardNetworkBackingInfo\"><deviceName>Порт-группа A</deviceName>"
            "</backing>"
            // A powered-off VM's shape: configured to connect, not connected right now.
            "<connectable><startConnected>true</startConnected><connected>false</connected></connectable>"
            "</VirtualDevice>"
            "</val></propSet>"
            "</objects></returnval></RetrievePropertiesExResponse></soapenv:Body></soapenv:Envelope>"));

    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());
    VimInventory inventory(*session.value());
    auto info = inventory.describeVm(MoRef{"VirtualMachine", "vm-1"});
    ASSERT_TRUE(info.hasValue());
    ASSERT_EQ(info.value().networkAdapters.size(), 1u);
    const VimNetworkAdapterInfo& adapter = info.value().networkAdapters.front();
    EXPECT_EQ(adapter.key, 4000);
    EXPECT_EQ(adapter.macAddress, "00:50:56:aa:bb:cc");
    EXPECT_EQ(adapter.networkName, "Порт-группа A");
    EXPECT_FALSE(adapter.connected);
    EXPECT_TRUE(adapter.startConnected);
    EXPECT_FALSE(adapter.vlanId.has_value()) << "standard-portgroup VLAN resolution is not implemented";
}

TEST(NetworkMappingEnvelopeTest, DescribeVmResolvesDistributedPortgroupNameAndVlanViaFollowUpCall)
{
    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForSoapAction(
        "urn:vim25/RetrievePropertiesEx",
        xmlResponse(
            "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<soapenv:Body><RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><objects>"
            "<obj type=\"VirtualMachine\">vm-1</obj>"
            "<propSet><name>config.hardware.device</name><val xsi:type=\"ArrayOfVirtualDevice\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
            "<VirtualDevice xsi:type=\"VirtualVmxnet3\">"
            "<key>4000</key><macAddress>00:50:56:aa:bb:dd</macAddress>"
            "<backing xsi:type=\"VirtualEthernetCardDistributedVirtualPortBackingInfo\">"
            "<port><switchUuid>50 1a 2b</switchUuid><portgroupKey>dvportgroup-100</portgroupKey></port>"
            "</backing>"
            "</VirtualDevice>"
            "</val></propSet>"
            "</objects></returnval></RetrievePropertiesExResponse></soapenv:Body></soapenv:Envelope>"));
    // The follow-up batched RetrievePropertiesEx on the DistributedVirtualPortgroup itself.
    fixture.transport.queueForSoapAction(
        "urn:vim25/RetrievePropertiesEx",
        xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<soapenv:Body><RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><objects>"
                    "<obj type=\"DistributedVirtualPortgroup\">dvportgroup-100</obj>"
                    "<propSet><name>name</name><val>Коммутатор-DVPG №1</val></propSet>"
                    "<propSet><name>config.defaultPortConfig.vlan.vlanId</name><val>25</val></propSet>"
                    "</objects></returnval></RetrievePropertiesExResponse></soapenv:Body></soapenv:Envelope>"));

    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());
    VimInventory inventory(*session.value());
    auto info = inventory.describeVm(MoRef{"VirtualMachine", "vm-1"});
    ASSERT_TRUE(info.hasValue());
    ASSERT_EQ(info.value().networkAdapters.size(), 1u);
    const VimNetworkAdapterInfo& adapter = info.value().networkAdapters.front();
    EXPECT_EQ(adapter.portgroupKey, "dvportgroup-100");
    EXPECT_EQ(adapter.networkName, "Коммутатор-DVPG №1");
    ASSERT_TRUE(adapter.vlanId.has_value());
    EXPECT_EQ(*adapter.vlanId, 25);
}

TEST(NetworkMappingEnvelopeTest, NoNetworkAdaptersProducesNoNetworkSectionAndNoEthernetItem)
{
    SoapFixture fixture;
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};

    const BeginResult result = runBegin(fixture, spec);
    ASSERT_TRUE(result.cisp);
    EXPECT_FALSE(result.cisp.child("networkMapping"));
    EXPECT_FALSE(result.ovfDoc.select_node("//NetworkSection").node());
    EXPECT_FALSE(result.ovfDoc.select_node("//Item[rasd:ResourceType='10']").node());
}

bool sentSoapAction(const FakeHttpTransport& transport, const std::string& soapAction)
{
    for (const auto& request : transport.m_requests) {
        for (const auto& h : request.headers) {
            if (h.name == "SOAPAction" && h.value == soapAction) return true;
        }
    }
    return false;
}

// The dry run sends the same CreateImportSpec as begin(), fails with the error the target reports in its result,
// and never gets as far as ImportVApp.
TEST(NetworkMappingEnvelopeTest, ValidateReportsCreateImportSpecErrorWithoutImporting)
{
    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForSoapAction(
        "urn:vim25/CreateImportSpec",
        xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<soapenv:Body><CreateImportSpecResponse xmlns=\"urn:vim25\"><returnval>"
                    "<error><localizedMessage>Line 1: Unsupported element 'Item'</localizedMessage></error>"
                    "</returnval></CreateImportSpecResponse></soapenv:Body></soapenv:Envelope>"));
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};
    spec.firmware = "efi";

    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());
    const Error validated = HttpNfcLeaseSession::validate(*session.value(), spec);
    ASSERT_FALSE(validated.ok());
    EXPECT_NE(validated.toString().find("CreateImportSpec reported an error: Line 1: Unsupported element 'Item'"),
              std::string::npos)
        << validated.toString();
    EXPECT_TRUE(sentSoapAction(fixture.transport, "urn:vim25/CreateImportSpec"));
    EXPECT_FALSE(sentSoapAction(fixture.transport, "urn:vim25/ImportVApp"));
}

TEST(NetworkMappingEnvelopeTest, ValidateAcceptsASpecTheTargetAccepts)
{
    SoapFixture fixture;
    scriptSessionBootstrap(fixture.transport);
    fixture.transport.queueForSoapAction(
        "urn:vim25/CreateImportSpec",
        xmlResponse("<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<soapenv:Body><CreateImportSpecResponse xmlns=\"urn:vim25\"><returnval>"
                    "<importSpec xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                    "xsi:type=\"VirtualMachineImportSpec\"><configSpec><name>x</name></configSpec></importSpec>"
                    "</returnval></CreateImportSpecResponse></soapenv:Body></soapenv:Envelope>"));
    ImportVmSpec spec;
    spec.vmName = "alta-import-test";
    spec.disks.push_back(ImportDiskSpec{"disk0", 4ull * 1024 * 1024});
    spec.resourcePool = MoRef{"ResourcePool", "resgroup-1"};

    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session.hasValue());
    EXPECT_TRUE(HttpNfcLeaseSession::validate(*session.value(), spec).ok());
    EXPECT_FALSE(sentSoapAction(fixture.transport, "urn:vim25/ImportVApp"));
}

} // namespace VSphereSoap::Detail
