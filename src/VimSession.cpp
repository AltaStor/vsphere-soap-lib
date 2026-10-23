// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// RAII vSphere Web Services login session

#include "VimSession.hpp"

#include "SoapLog.hpp"
#include "Error.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

namespace {

// The ServiceInstance is a fixed, well-known MoRef: every VIM client (govc, pyvmomi, ...) starts here;
// it's not discovered, it's a constant of the API.
const MoRef kServiceInstance{"ServiceInstance", "ServiceInstance"};

} // namespace

Result<std::unique_ptr<VimSession>> VimSession::open(SoapClient& soap, std::string username, std::string password)
{
    logger()->debug("retrieving ServiceContent for {}", username);
    const pugi::xml_document request = makeVimRequest("RetrieveServiceContent", kServiceInstance);
    auto callResult = soap.call("urn:vim25/RetrieveServiceContent", request.document_element());
    if (!callResult) {
        return std::move(callResult).error();
    }

    const pugi::xml_node responseElement = soapBodyContent(callResult.value());
    const pugi::xml_node serviceContent = returnval(responseElement);
    if (!serviceContent) {
        return Error(ErrorCode::InvalidFormat, "RetrieveServiceContent response missing <returnval>");
    }

    MoRef rootFolder = parseMoRef(serviceContent.child("rootFolder"));
    MoRef propertyCollector = parseMoRef(serviceContent.child("propertyCollector"));
    MoRef searchIndex = parseMoRef(serviceContent.child("searchIndex"));
    MoRef sessionManager = parseMoRef(serviceContent.child("sessionManager"));
    MoRef ovfManager = parseMoRef(serviceContent.child("ovfManager"));
    MoRef fileManager = parseMoRef(serviceContent.child("fileManager"));
    MoRef virtualDiskManager = parseMoRef(serviceContent.child("virtualDiskManager"));
    MoRef viewManager = parseMoRef(serviceContent.child("viewManager"));
    if (rootFolder.empty() || propertyCollector.empty() || searchIndex.empty() || sessionManager.empty()) {
        return Error(ErrorCode::InvalidFormat,
                     "RetrieveServiceContent response missing rootFolder/propertyCollector/searchIndex/"
                     "sessionManager");
    }

    // Not std::make_unique: VimSession's constructor is private (open() is the only entry point).
    std::unique_ptr<VimSession> session(new VimSession(soap, std::move(username), std::move(password),
                                                          std::move(propertyCollector), std::move(rootFolder),
                                                          std::move(searchIndex), std::move(sessionManager),
                                                          std::move(ovfManager), std::move(fileManager),
                                                          std::move(virtualDiskManager), std::move(viewManager)));

    Result<void> loginResult = session->login();
    if (!loginResult) {
        return std::move(loginResult).error();
    }

    return session;
}

VimSession::VimSession(SoapClient& soap, std::string username, std::string password, MoRef propertyCollector,
                        MoRef rootFolder, MoRef searchIndex, MoRef sessionManager, MoRef ovfManager,
                        MoRef fileManager, MoRef virtualDiskManager, MoRef viewManager)
    : m_soap(soap), m_username(std::move(username)), m_password(std::move(password)),
      m_propertyCollector(std::move(propertyCollector)), m_rootFolder(std::move(rootFolder)),
      m_searchIndex(std::move(searchIndex)), m_sessionManager(std::move(sessionManager)),
      m_ovfManager(std::move(ovfManager)), m_fileManager(std::move(fileManager)),
      m_virtualDiskManager(std::move(virtualDiskManager)), m_viewManager(std::move(viewManager)) {}

VimSession::~VimSession()
{
    if (m_loggedIn) {
        logoutBestEffort();
    }
}

Result<void> VimSession::login()
{
    logger()->debug("logging in as {}", m_username);
    pugi::xml_document request = makeVimRequest("Login", m_sessionManager);
    pugi::xml_node op = request.document_element();
    op.append_child("userName").text() = m_username.c_str();
    op.append_child("password").text() = m_password.c_str();

    auto callResult = m_soap.call("urn:vim25/Login", op);
    if (!callResult) {
        logger()->warn("SOAP login as {} failed: {}", m_username, callResult.error().toString());
        return std::move(callResult).error();
    }
    m_loggedIn = true;
    logger()->debug("logged in as {}", m_username);
    return {};
}

void VimSession::logoutBestEffort() noexcept
{
    try {
        logger()->debug("logging out ({})", m_username);
        const pugi::xml_document request = makeVimRequest("Logout", m_sessionManager);
        // best-effort: result intentionally unchecked
        (void)m_soap.call("urn:vim25/Logout", request.document_element());
    } catch (...) {
        // Never propagate out of a destructor; a failed Logout just leaves the session to expire
        // server-side on its own.
    }
}

Result<pugi::xml_document> VimSession::call(const std::string& soapAction, const pugi::xml_node& requestNode)
{
    SoapFaultInfo faultInfo;
    auto result = m_soap.call(soapAction, requestNode, &faultInfo);
    if (result) {
        return result;
    }
    if (faultInfo.typeName != "NotAuthenticated") {
        return std::move(result).error();
    }

    logger()->info("session not authenticated for {}, re-logging in and retrying", soapAction);
    Result<void> reloginResult = login();
    if (!reloginResult) {
        return std::move(reloginResult).error();
    }
    return m_soap.call(soapAction, requestNode);
}

} // namespace VSphereSoap::Detail
