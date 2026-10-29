// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for disk reads through a snapshot's export lease

#include <algorithm>
#include <map>
#include <vector>

#include <gtest/gtest.h>

#include "ExportStreamReader.hpp"
#include "StreamOptimizedVmdkEncoder.hpp"
#include "fake_http_transport.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "vsphere-soap/vim/VimSession.hpp"

namespace VSphereSoap::Detail {
namespace {

using namespace VSphereSoap::Vim;

using Test::FakeHttpTransport;
using VSphereSoap::ErrorCode;

constexpr std::uint64_t kGrain = StreamOptimizedVmdkEncoder::kDefaultGrainSizeSectors * 512;
constexpr std::uint64_t kCapacity = 8 * kGrain;
constexpr const char* kDiskPath = "/nfc/ticket/disk-0.vmdk";

HttpResponse ok(std::string body)
{
    HttpResponse response;
    response.status = 200;
    response.body = std::move(body);
    return response;
}

std::string envelope(const std::string& inner)
{
    return "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\"><soapenv:Body>" + inner +
           "</soapenv:Body></soapenv:Envelope>";
}

std::vector<std::byte> grainData(unsigned seed)
{
    std::vector<std::byte> out(kGrain);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>((seed * 13 + i) & 0xFF);
    }
    return out;
}

// What vSphere would stream for a disk holding data in grains 1, 2 and 6.
std::string exportStream(const std::map<std::uint64_t, std::vector<std::byte>>& grains)
{
    std::string stream;
    auto encoder = StreamOptimizedVmdkEncoder::create(
        [&](std::span<const std::byte> bytes) -> Error {
            stream.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return {};
        },
        kCapacity, "# Disk DescriptorFile\n", StreamOptimizedVmdkEncoder::kDefaultGrainSizeSectors, true);
    for (const auto& [offset, data] : grains) {
        EXPECT_TRUE(encoder.value().writeGrain(VmdkGrain{offset, data}).ok());
    }
    EXPECT_TRUE(encoder.value().finish().ok());
    return stream;
}

const std::map<std::uint64_t, std::vector<std::byte>> kGrains{
    {1 * kGrain, grainData(1)}, {2 * kGrain, grainData(2)}, {6 * kGrain, grainData(6)}};

// A VIM session that hands out an export lease with one disk URL, and the data side that serves `data`.
struct ExportFixture {
    FakeHttpTransport soapTransport;
    SoapClient soap{soapTransport, "vcenter.example.com"};
    FakeHttpTransport* dataTransport = nullptr;

    ExportFixture(HttpResponse data)
    {
        soapTransport.queueForSoapAction(
            "urn:vim25/RetrieveServiceContent",
            ok(envelope("<RetrieveServiceContentResponse xmlns=\"urn:vim25\"><returnval>"
                        "<rootFolder type=\"Folder\">group-d1</rootFolder>"
                        "<propertyCollector type=\"PropertyCollector\">propertyCollector</propertyCollector>"
                        "<searchIndex type=\"SearchIndex\">SearchIndex</searchIndex>"
                        "<sessionManager type=\"SessionManager\">SessionManager</sessionManager>"
                        "</returnval></RetrieveServiceContentResponse>")));
        soapTransport.queueForSoapAction(
            "urn:vim25/Login",
            ok(envelope("<LoginResponse xmlns=\"urn:vim25\"><returnval><key>k</key></returnval></LoginResponse>")));
        soapTransport.queueForSoapAction(
            "urn:vim25/ExportSnapshot",
            ok(envelope("<ExportSnapshotResponse xmlns=\"urn:vim25\"><returnval type=\"HttpNfcLease\">lease-1"
                        "</returnval></ExportSnapshotResponse>")));
        soapTransport.queueForSoapAction(
            "urn:vim25/RetrievePropertiesEx",
            ok(envelope("<RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><objects>"
                        "<obj type=\"HttpNfcLease\">lease-1</obj>"
                        "<propSet><name>state</name><val>ready</val></propSet>"
                        "<propSet><name>info</name><val>"
                        "<deviceUrl><key>/vm-1/ParaVirtualSCSIController0:0</key>"
                        "<url>https://*/nfc/ticket/disk-0.vmdk</url><sslThumbprint>AA:BB</sslThumbprint>"
                        "<disk>true</disk></deviceUrl>"
                        "<deviceUrl><key>/vm-1/nvram</key><url>https://*/nfc/ticket/disk-1.nvram</url>"
                        "<disk>false</disk></deviceUrl>"
                        "</val></propSet></objects></returnval></RetrievePropertiesExResponse>")));
        for (const char* action : {"urn:vim25/HttpNfcLeaseComplete", "urn:vim25/HttpNfcLeaseAbort",
                                   "urn:vim25/HttpNfcLeaseProgress", "urn:vim25/Logout"}) {
            soapTransport.queueForSoapAction(action, ok(envelope("<Response xmlns=\"urn:vim25\"/>")));
        }
        m_data = std::move(data);
    }

    ExportTransportFactory factory()
    {
        return [this](const std::string& host, std::uint16_t, bool, const std::string& thumbprint)
                   -> std::unique_ptr<IHttpTransport> {
            m_dataHost = host;
            m_dataThumbprint = thumbprint;
            auto transport = std::make_unique<FakeHttpTransport>();
            transport->queueForTarget(kDiskPath, m_data);
            dataTransport = transport.get();
            return transport;
        };
    }

    bool called(const std::string& action) const
    {
        return std::any_of(soapTransport.m_requests.begin(), soapTransport.m_requests.end(), [&](const auto& request) {
            return std::any_of(request.headers.begin(), request.headers.end(), [&](const HttpHeader& header) {
                return header.name == "SOAPAction" && header.value == action;
            });
        });
    }

    HttpResponse m_data;
    std::string m_dataHost;
    std::string m_dataThumbprint;
};

TEST(ExportStreamReaderTest, ReadsGrainsAndZerosInAscendingOrder)
{
    ExportFixture fixture(ok(exportStream(kGrains)));
    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session) << session.error().toString();
    {
        auto reader = ExportStreamReader::open(*session.value(), "vcenter.example.com",
                                               MoRef{"VirtualMachineSnapshot", "snapshot-1"},
                                               "ParaVirtualSCSIController0:0", kCapacity, true, fixture.factory());
        ASSERT_TRUE(reader) << reader.error().toString();
        EXPECT_EQ(fixture.m_dataHost, "vcenter.example.com"); // the `*` placeholder replaced
        EXPECT_EQ(fixture.m_dataThumbprint, "AA:BB");

        // Half a grain of zeros, then across grains 1 and 2 in one read, then a read that skips ahead to grain 6.
        std::vector<std::byte> buffer(kGrain / 2);
        ASSERT_EQ(reader.value()->readAt(0, buffer).value(), kGrain / 2);
        EXPECT_TRUE(std::all_of(buffer.begin(), buffer.end(), [](std::byte b) { return b == std::byte{0}; }));

        std::vector<std::byte> across(kGrain);
        ASSERT_EQ(reader.value()->readAt(kGrain + kGrain / 2, across).value(), kGrain);
        EXPECT_TRUE(std::equal(across.begin(), across.begin() + kGrain / 2, grainData(1).begin() + kGrain / 2));
        EXPECT_TRUE(std::equal(across.begin() + kGrain / 2, across.end(), grainData(2).begin()));

        std::vector<std::byte> last(2 * kGrain);
        ASSERT_EQ(reader.value()->readAt(6 * kGrain, last).value(), 2 * kGrain);
        EXPECT_TRUE(std::equal(last.begin(), last.begin() + kGrain, grainData(6).begin()));
        EXPECT_TRUE(std::all_of(last.begin() + kGrain, last.end(), [](std::byte b) { return b == std::byte{0}; }));

        std::vector<std::byte> past(16);
        EXPECT_EQ(reader.value()->readAt(kCapacity, past).value(), 0u);
    }
    EXPECT_TRUE(fixture.called("urn:vim25/HttpNfcLeaseComplete"));
    EXPECT_FALSE(fixture.called("urn:vim25/HttpNfcLeaseAbort"));
}

TEST(ExportStreamReaderTest, RefusesAReadBehindWhatWasPassed)
{
    ExportFixture fixture(ok(exportStream(kGrains)));
    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session) << session.error().toString();
    auto reader = ExportStreamReader::open(*session.value(), "vcenter.example.com",
                                           MoRef{"VirtualMachineSnapshot", "snapshot-1"},
                                           "ParaVirtualSCSIController0:0", kCapacity, true, fixture.factory());
    ASSERT_TRUE(reader);
    std::vector<std::byte> buffer(kGrain);
    ASSERT_TRUE(reader.value()->readAt(3 * kGrain, buffer));
    const auto behind = reader.value()->readAt(kGrain, buffer);
    ASSERT_FALSE(behind);
    EXPECT_EQ(behind.error().code(), ErrorCode::InvalidArgument);
}

TEST(ExportStreamReaderTest, ReportsAFailedDownloadAndAbortsTheLease)
{
    HttpResponse failed;
    failed.status = 500;
    failed.body = "Failed to open disk";
    ExportFixture fixture(failed);
    auto session = VimSession::open(fixture.soap, "user", "pass");
    ASSERT_TRUE(session) << session.error().toString();
    {
        auto reader = ExportStreamReader::open(*session.value(), "vcenter.example.com",
                                               MoRef{"VirtualMachineSnapshot", "snapshot-1"},
                                               "ParaVirtualSCSIController0:0", kCapacity, true, fixture.factory());
        ASSERT_TRUE(reader);
        std::vector<std::byte> buffer(kGrain);
        const auto read = reader.value()->readAt(0, buffer);
        ASSERT_FALSE(read);
        EXPECT_EQ(read.error().code(), ErrorCode::IoError);
        EXPECT_NE(read.error().message().find("HTTP 500"), std::string::npos);
    }
    EXPECT_TRUE(fixture.called("urn:vim25/HttpNfcLeaseAbort"));
    EXPECT_FALSE(fixture.called("urn:vim25/HttpNfcLeaseComplete"));
}

TEST(ExportStreamReaderTest, PicksTheDiskUrlByControllerSlot)
{
    const std::vector<ExportDeviceUrl> urls{{"/vm-1/ParaVirtualSCSIController0:0", "a", ""},
                                            {"/vm-1/ParaVirtualSCSIController0:1", "b", ""},
                                            {"/vm-1/VirtualLsiLogicController1:10", "c", ""}};
    EXPECT_EQ(pickExportDeviceUrl(urls, "ParaVirtualSCSIController0:1")->url, "b");
    EXPECT_EQ(pickExportDeviceUrl(urls, "VirtualLsiLogicController1:10")->url, "c");
    // "Controller0:0" is not a slot of its own: the match has to start at a path separator.
    EXPECT_FALSE(pickExportDeviceUrl(urls, "Controller0:0"));
    EXPECT_FALSE(pickExportDeviceUrl(urls, ""));
    EXPECT_EQ(pickExportDeviceUrl({urls.front()}, "")->url, "a"); // the only disk there is
}

} // namespace
} // namespace VSphereSoap::Detail
