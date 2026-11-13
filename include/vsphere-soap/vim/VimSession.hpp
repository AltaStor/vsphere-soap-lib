// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// RAII vSphere Web Services login session

#pragma once

// RAII vSphere Web Services session: RetrieveServiceContent + Login on open(), Logout on destruction.
// Analogous role to VimSession's namesake concept in every VIM client (govc, pyvmomi's SmartConnect,
// ...): the thing every other VIM call is made through.

#include <memory>
#include <string>

#include "vsphere-soap/vim/SoapClient.hpp"
#include "vsphere-soap/vim/VimTypes.hpp"
#include "vsphere-soap/Result.hpp"

namespace VSphereSoap::Vim {

using VSphereSoap::Result;

class VimSession {
public:
    /// Connects `soap` (RetrieveServiceContent) and logs in with `username`/`password`.
    /// Switches `soap` to the server's own API version, so later calls get every field the server has.
    /// `soap` must outlive the returned session.
    /// Fails with `ErrorCode::IoError` on transport/auth failure, `ErrorCode::InvalidFormat` if
    /// RetrieveServiceContent's response is missing an expected MoRef.
    ///
    static Result<std::unique_ptr<VimSession>> open(SoapClient& soap, std::string username,
                                                                     std::string password);

    /// Logs out (best-effort, swallowed on failure; matches the destructor discipline `SnapshotScope`
    /// also follows: never throws, never surfaces a cleanup failure to the caller).
    ~VimSession();

    VimSession(const VimSession&) = delete;
    VimSession& operator=(const VimSession&) = delete;
    VimSession(VimSession&&) = delete;
    VimSession& operator=(VimSession&&) = delete;

    const MoRef& propertyCollector() const noexcept { return m_propertyCollector; }
    const MoRef& rootFolder() const noexcept { return m_rootFolder; }
    const MoRef& searchIndex() const noexcept { return m_searchIndex; }
    /// `ServiceContent.ovfManager`: present on every real ESXi/vCenter, but not treated as a hard
    /// `open()` precondition the way root folder/property collector/search index/session manager are,
    /// since only the HttpNfcLease import path needs it; empty if the server somehow omitted it, which
    /// callers of that path should treat as a loud failure rather than this class guessing at
    /// a fallback.
    ///
    const MoRef& ovfManager() const noexcept { return m_ovfManager; }

    /// `ServiceContent.fileManager`: same not-a-hard-precondition treatment as `ovfManager()` above;
    /// only the datastore-file-move step of the disk-attach flow needs it.
    const MoRef& fileManager() const noexcept { return m_fileManager; }

    /// `ServiceContent.virtualDiskManager`; same not-a-hard-precondition treatment; only
    /// the eager-zeroed-thick post-import fixup (`EagerZeroVirtualDisk_Task`, see DiskAttach.cpp) needs
    /// it, since `HttpNfcLease`'s own `diskProvisioning` isn't reliably honored by every ESXi/vCenter
    /// version for that specific provisioning type (confirmed live: a real vCenter accepted the request
    /// but the resulting disk was not actually eager-zeroed).
    ///
    const MoRef& virtualDiskManager() const noexcept { return m_virtualDiskManager; }

    /// `ServiceContent.viewManager`: same not-a-hard-precondition treatment; only
    /// `VimInventory::listObjects()` (a ContainerView over the whole inventory) needs it.
    const MoRef& viewManager() const noexcept { return m_viewManager; }

    /// Forwards to the underlying `SoapClient::host()`: see that accessor's doc comment.
    /// Exposed here so callers that only hold a `VimSession&` (e.g. `HttpNfcLeaseSession`) don't need
    /// their own `SoapClient&`.
    const std::string& soapHost() const noexcept { return m_soap.host(); }

    /// Posts `requestNode` (see `SoapClient::call`) through this session. vSphere sessions expire
    /// server-side; if the call faults with `NotAuthenticated`, this re-`Login`s once and retries
    /// the same call before giving up.
    Result<pugi::xml_document> call(const pugi::xml_node& requestNode);

private:
    VimSession(SoapClient& soap, std::string username, std::string password, MoRef propertyCollector,
               MoRef rootFolder, MoRef searchIndex, MoRef sessionManager, MoRef ovfManager, MoRef fileManager,
               MoRef virtualDiskManager, MoRef viewManager);

    Error login();
    void logoutBestEffort() noexcept;

    SoapClient& m_soap;
    std::string m_username;
    std::string m_password;
    MoRef m_propertyCollector;
    MoRef m_rootFolder;
    MoRef m_searchIndex;
    MoRef m_sessionManager;
    MoRef m_ovfManager;
    MoRef m_fileManager;
    MoRef m_virtualDiskManager;
    MoRef m_viewManager;
    bool m_loggedIn = false;
};

} // namespace VSphereSoap::Vim
