// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// VSphereClient session reuse, and the one-login-per-call free functions

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include "VimConnection.hpp"
#include "fake_http_transport.hpp"
#include "vsphere-soap/VSphereClient.hpp"
#include "vsphere-soap/VSphereFolderPutImport.hpp"
#include "vsphere-soap/VSphereVmNetworkInfo.hpp"

namespace VSphereSoap {
namespace {

using Test::FakeHttpTransport;
using Vim::HttpResponse;

HttpResponse soapResponse(const std::string& responseElement)
{
    HttpResponse response;
    response.status = 200;
    response.body = "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<soapenv:Body>" +
                    responseElement + "</soapenv:Body></soapenv:Envelope>";
    return response;
}

HttpResponse httpStatus(unsigned status)
{
    HttpResponse response;
    response.status = status;
    return response;
}

std::string prop(const std::string& name, const std::string& value)
{
    return "<propSet><name>" + name + "</name><val>" + value + "</val></propSet>";
}

HttpResponse objectResponse(const std::string& type, const std::string& id, const std::string& props)
{
    return soapResponse("<RetrievePropertiesExResponse xmlns=\"urn:vim25\"><returnval><objects><obj type=\"" + type +
                        "\">" + id + "</obj>" + props + "</objects></returnval></RetrievePropertiesExResponse>");
}

// The library owns every transport it opens and destroys it with the connection, while the test needs
// to read the recorded requests afterwards: each connection gets one of these, all forwarding to one
// FakeHttpTransport the fixture owns.
class ForwardingTransport final : public Vim::IHttpTransport {
public:
    explicit ForwardingTransport(FakeHttpTransport& target) : m_target(target) {}

    Result<HttpResponse> request(Vim::HttpMethod method, const std::string& target,
                                 const std::vector<Vim::HttpHeader>& headers, std::string body) override
    {
        return m_target.request(method, target, headers, std::move(body));
    }
    Result<HttpResponse> requestPutFile(const std::string& target, const std::vector<Vim::HttpHeader>& headers,
                                        const std::string& localFilePath, std::chrono::seconds timeout) override
    {
        return m_target.requestPutFile(target, headers, localFilePath, timeout);
    }
    Result<HttpResponse> requestPutStream(const std::string& target, const std::vector<Vim::HttpHeader>& headers,
                                          Vim::HttpBodyChunkProducer producer, std::chrono::seconds timeout,
                                          std::optional<std::uint64_t> contentLength) override
    {
        return m_target.requestPutStream(target, headers, std::move(producer), timeout, contentLength);
    }
    Result<HttpResponse> requestPostStream(const std::string& target, const std::vector<Vim::HttpHeader>& headers,
                                           Vim::HttpBodyChunkProducer producer, std::chrono::seconds timeout,
                                           std::optional<std::uint64_t> contentLength) override
    {
        return m_target.requestPostStream(target, headers, std::move(producer), timeout, contentLength);
    }

private:
    FakeHttpTransport& m_target;
};

class VSphereClientTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Detail::setTransportFactoryForTesting([this](const ConnectParams&) {
            ++m_connectionsOpened;
            return std::make_unique<ForwardingTransport>(m_fake);
        });
        m_params.host = "vcenter.example.com";
        m_params.username = "user";
        m_params.password = "pass";
        m_vm.moref = "vm-42"; // a moref needs no SearchIndex lookup
    }

    void TearDown() override { Detail::setTransportFactoryForTesting({}); }

    void scriptLogin()
    {
        m_fake.queueForOperation(
            "RetrieveServiceContent",
            soapResponse("<RetrieveServiceContentResponse xmlns=\"urn:vim25\"><returnval>"
                         "<rootFolder type=\"Folder\">group-d1</rootFolder>"
                         "<propertyCollector type=\"PropertyCollector\">propertyCollector</propertyCollector>"
                         "<searchIndex type=\"SearchIndex\">SearchIndex</searchIndex>"
                         "<sessionManager type=\"SessionManager\">SessionManager</sessionManager>"
                         "</returnval></RetrieveServiceContentResponse>"));
        m_fake.queueForOperation("Login",
                                 soapResponse("<LoginResponse xmlns=\"urn:vim25\"><returnval><key>k</key>"
                                              "</returnval></LoginResponse>"));
        m_fake.queueForOperation("Logout", soapResponse("<LogoutResponse xmlns=\"urn:vim25\"/>"));
    }

    // One sticky RetrievePropertiesEx answer that serves both describeVm (VM properties) and the task
    // wait after PowerOnVM_Task (info.state), so the operations below can run in any order.
    void scriptVm()
    {
        m_fake.queueForOperation(
            "RetrievePropertiesEx",
            objectResponse("VirtualMachine", "vm-42",
                           prop("name", "web01") + prop("runtime.powerState", "poweredOff") +
                               prop("config.annotation", "owned by the web team") + prop("info.state", "success")));
        m_fake.queueForOperation("PowerOnVM_Task",
                                 soapResponse("<PowerOnVM_TaskResponse xmlns=\"urn:vim25\">"
                                              "<returnval type=\"Task\">task-7</returnval></PowerOnVM_TaskResponse>"));
    }

    int count(const std::string& operation) const
    {
        int n = 0;
        for (const auto& request : m_fake.m_requests) {
            if (FakeHttpTransport::operationOf(request.body) == operation) {
                ++n;
            }
        }
        return n;
    }

    FakeHttpTransport m_fake;
    int m_connectionsOpened = 0;
    ConnectParams m_params;
    VmLocator m_vm;
};

TEST_F(VSphereClientTest, SeveralOperationsShareOneLogin)
{
    scriptLogin();
    scriptVm();

    auto client = VSphereClient::open(m_params);
    ASSERT_TRUE(client) << client.error().toString();

    auto notes = client.value()->getVmNotes(m_vm);
    ASSERT_TRUE(notes) << notes.error().toString();
    EXPECT_EQ(notes.value(), "owned by the web team");

    auto power = client.value()->getVmPowerState(m_vm);
    ASSERT_TRUE(power) << power.error().toString();
    EXPECT_EQ(power.value(), "poweredOff");

    Error poweredOn = client.value()->powerOnVm(m_vm);
    EXPECT_TRUE(poweredOn.ok()) << poweredOn.toString();
    EXPECT_EQ(count("PowerOnVM_Task"), 1);

    EXPECT_EQ(m_connectionsOpened, 1);
    EXPECT_EQ(count("RetrieveServiceContent"), 1);
    EXPECT_EQ(count("Login"), 1);
    EXPECT_EQ(count("Logout"), 0);

    client.value().reset();
    EXPECT_EQ(count("Login"), 1);
    EXPECT_EQ(count("Logout"), 1);
}

TEST_F(VSphereClientTest, FreeFunctionLogsInAndOutOncePerCall)
{
    scriptLogin();
    scriptVm();

    auto notes = getVmNotes(m_params, m_vm);
    ASSERT_TRUE(notes) << notes.error().toString();
    EXPECT_EQ(notes.value(), "owned by the web team");
    EXPECT_EQ(count("Login"), 1);
    EXPECT_EQ(count("Logout"), 1);

    auto power = getVmPowerState(m_params, m_vm);
    ASSERT_TRUE(power) << power.error().toString();
    EXPECT_EQ(count("Login"), 2);
    EXPECT_EQ(count("Logout"), 2);
    EXPECT_EQ(m_connectionsOpened, 2);
}

TEST_F(VSphereClientTest, FreeFunctionRejectsEmptyLocatorWithoutConnecting)
{
    auto disks = listVmDisks(m_params, VmLocator{});
    ASSERT_FALSE(disks);
    EXPECT_EQ(disks.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(disks.error().message(), "VmLocator has none of moref/uuid/inventoryPath set");
    EXPECT_EQ(m_connectionsOpened, 0);
    EXPECT_TRUE(m_fake.m_requests.empty());
}

TEST_F(VSphereClientTest, OpenFailsWithTheSameErrorAsAFreeFunction)
{
    // RetrieveServiceContent answers but Login is never scripted, so the login itself fails.
    m_fake.queueForOperation(
        "RetrieveServiceContent",
        soapResponse("<RetrieveServiceContentResponse xmlns=\"urn:vim25\"><returnval>"
                     "<rootFolder type=\"Folder\">group-d1</rootFolder>"
                     "<propertyCollector type=\"PropertyCollector\">propertyCollector</propertyCollector>"
                     "<searchIndex type=\"SearchIndex\">SearchIndex</searchIndex>"
                     "<sessionManager type=\"SessionManager\">SessionManager</sessionManager>"
                     "</returnval></RetrieveServiceContentResponse>"));

    auto client = VSphereClient::open(m_params);
    ASSERT_FALSE(client);
    auto notes = getVmNotes(m_params, m_vm);
    ASSERT_FALSE(notes);

    EXPECT_EQ(client.error().code(), notes.error().code());
    EXPECT_EQ(client.error().message(), notes.error().message());
}

// prepareFolderPutImport's keepAlive shares the client's login: the cookie it hands out has to stay valid
// for the flat-extent PUT even when the client is gone first, so Logout waits for the last owner.
class FolderPutKeepAliveTest : public VSphereClientTest {
protected:
    void scriptPrepare()
    {
        scriptLogin();
        // describeVm, then the walk from the VM up to its datacenter.
        m_fake.queueForOperation("RetrievePropertiesEx",
                                 objectResponse("VirtualMachine", "vm-42",
                                                prop("name", "web01") + prop("runtime.powerState", "poweredOff") +
                                                    prop("config.files.vmPathName", "[ds1] web01/web01.vmx")));
        m_fake.queueForOperation(
            "RetrievePropertiesEx",
            objectResponse("VirtualMachine", "vm-42",
                           prop("name", "web01") + "<propSet><name>parent</name>"
                                                   "<val type=\"Datacenter\">datacenter-1</val></propSet>"));
        m_fake.queueForOperation("RetrievePropertiesEx",
                                 objectResponse("Datacenter", "datacenter-1", prop("name", "DC1")));

        // Neither file exists yet; then the descriptor PUT succeeds.
        const std::string query = "?dcPath=DC1&dsName=ds1";
        m_fake.queueForTarget("/folder/web01/data.vmdk" + query, httpStatus(404));
        m_fake.queueForTarget("/folder/web01/data.vmdk" + query, httpStatus(201));
        m_fake.queueForTarget("/folder/web01/data-flat.vmdk" + query, httpStatus(404));
    }

    VSphereFolderPutPrepSpec spec() const
    {
        VSphereFolderPutPrepSpec spec;
        spec.targetVm = m_vm;
        spec.fileName = "data";
        spec.virtualSizeBytes = 1024 * 1024;
        return spec;
    }
};

TEST_F(FolderPutKeepAliveTest, ResultFromClientKeepsTheClientSessionLoggedIn)
{
    scriptPrepare();

    auto opened = VSphereClient::open(m_params);
    ASSERT_TRUE(opened) << opened.error().toString();
    std::unique_ptr<VSphereClient> client = std::move(opened).value();
    {
        auto prep = client->prepareFolderPutImport(spec());
        ASSERT_TRUE(prep) << prep.error().toString();
        EXPECT_EQ(prep.value().descriptorDatastorePath, "[ds1] web01/data.vmdk");
        EXPECT_EQ(m_connectionsOpened, 1);
        EXPECT_EQ(count("Login"), 1);

        client.reset();
        EXPECT_EQ(count("Logout"), 0);
    } // the result, and its keepAlive, go here
    EXPECT_EQ(count("Logout"), 1);
}

TEST_F(FolderPutKeepAliveTest, ResultFromFreeFunctionKeepsItsOwnSessionLoggedIn)
{
    scriptPrepare();

    {
        auto prep = prepareFolderPutImport(m_params, spec());
        ASSERT_TRUE(prep) << prep.error().toString();
        EXPECT_EQ(count("Login"), 1);
        EXPECT_EQ(count("Logout"), 0);
    } // the result, and its keepAlive, go here
    EXPECT_EQ(count("Logout"), 1);
}

} // namespace
} // namespace VSphereSoap
