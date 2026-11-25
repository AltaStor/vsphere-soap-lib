// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// RAII vSphere Web Services login session

#include "vsphere-soap/vim/VimSession.hpp"

#include <string_view>

#include "SoapLog.hpp"
#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Vim {

using Detail::logger;

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

namespace {

// The ServiceInstance is a fixed, well-known MoRef: every VIM client (govc, pyvmomi, ...) starts here;
// it's not discovered, it's a constant of the API.
const MoRef kServiceInstance{"ServiceInstance", "ServiceInstance"};

// "8.0.3.0" or "6.7": digits separated by single dots, which is also what makes it safe to put in a header.
bool isApiVersion(std::string_view version)
{
    bool expectDigit = true;
    for (const char c : version) {
        if (c == '.' && !expectDigit) {
            expectDigit = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return false;
        }
        expectDigit = false;
    }
    return !expectDigit;
}

} // namespace

Result<std::unique_ptr<VimSession>> VimSession::open(SoapClient& soap, std::string username, std::string password)
{
    logger()->debug("retrieving ServiceContent for {}", username);
    const pugi::xml_document request = makeVimRequest("RetrieveServiceContent", kServiceInstance);
    auto callResult = soap.call(request.document_element());
    if (!callResult) {
        return std::move(callResult).error();
    }

    const pugi::xml_node responseElement = soapBodyContent(callResult.value());
    const pugi::xml_node serviceContent = returnval(responseElement);
    if (!serviceContent) {
        return Error(ErrorCode::InvalidFormat, "RetrieveServiceContent response missing <returnval>");
    }

    // The server's newest version is always one it accepts.
    // Without it the client stays on the baseline, which only costs fields newer than that.
    const std::string apiVersion = serviceContent.child("about").child("apiVersion").text().as_string();
    if (isApiVersion(apiVersion)) {
        soap.setApiVersion(apiVersion);
        logger()->debug("using vim25 API version {}", apiVersion);
    } else {
        logger()->warn("server reports no usable API version ('{}'), staying on {}", apiVersion, soap.apiVersion());
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

    Error loginResult = session->login();
    if (!loginResult.ok()) {
        return loginResult;
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

Error VimSession::login()
{
    logger()->debug("logging in as {}", m_username);
    pugi::xml_document request = makeVimRequest("Login", m_sessionManager);
    pugi::xml_node op = request.document_element();
    op.append_child("userName").text() = m_username.c_str();
    op.append_child("password").text() = m_password.c_str();

    auto callResult = m_soap.call(op);
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
        (void)m_soap.call(request.document_element());
    } catch (...) {
        // Never propagate out of a destructor; a failed Logout just leaves the session to expire
        // server-side on its own.
    }
}

Result<pugi::xml_document> VimSession::call(const pugi::xml_node& requestNode)
{
    SoapFaultInfo faultInfo;
    auto result = m_soap.call(requestNode, &faultInfo);
    if (result) {
        return result;
    }
    if (faultInfo.typeName != "NotAuthenticated") {
        return std::move(result).error();
    }

    logger()->info("session not authenticated for {}, re-logging in and retrying", requestNode.name());
    Error reloginResult = login();
    if (!reloginResult.ok()) {
        return reloginResult;
    }
    return m_soap.call(requestNode);
}

} // namespace VSphereSoap::Vim
