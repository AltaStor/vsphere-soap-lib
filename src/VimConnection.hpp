// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// One logged-in VIM connection, and the handle every ConnectParams-based operation runs on

#pragma once

#include <functional>
#include <memory>

#include "vsphere-soap/Result.hpp"
#include "vsphere-soap/VmwareSoapDisk.hpp" // ConnectParams
#include "vsphere-soap/vim/IHttpTransport.hpp"
#include "vsphere-soap/vim/SoapClient.hpp"
#include "vsphere-soap/vim/VimSession.hpp"

namespace VSphereSoap::Detail {

/// The transport, SoapClient and VimSession of one login, owned together.
/// VimSession only references its SoapClient, which only references its transport, so the three have to
/// live and die as a unit. Members are declared in that order so the session is destroyed first and its
/// best-effort Logout still has a SoapClient and transport to go through (the order
/// `VmwareSoapDisk::Impl` uses for the same reason).
///
struct VimConnection {
    std::unique_ptr<Vim::IHttpTransport> transport;
    std::unique_ptr<Vim::SoapClient> soap;
    std::unique_ptr<Vim::VimSession> session;
};

/// Builds the transport a new connection talks through.
using TransportFactory = std::function<std::unique_ptr<Vim::IHttpTransport>(const ConnectParams&)>;

/// Test seam: makes every connection opened afterwards use `factory` instead of a real
/// `HttpTransportAdapter`. An empty function restores the default. Not thread-safe; tests only.
void setTransportFactoryForTesting(TransportFactory factory);

/// What every ConnectParams-based operation body takes.
/// Logs in on the first `get()`, not on construction, so a body can reject bad arguments before it
/// touches the network, the order the one-shot free functions have always had. `VSphereClient::open()`
/// calls `get()` right away; the free functions build one of these per call and let the body call it.
///
/// Holds the connection through a `shared_ptr` so a folder-PUT `keepAlive` handle can keep the same login
/// alive after the owner of this object is gone (see `share()`).
/// Not thread-safe.
///
class ClientConnection {
public:
    explicit ClientConnection(ConnectParams params);

    const ConnectParams& params() const noexcept { return m_params; }

    /// The logged-in connection, opening it first if needed.
    /// Fails with the transport's or `VimSession::open()`'s error (a later call tries again).
    Result<VimConnection*> get();

    /// Shared ownership of the open connection; null before a successful `get()`.
    /// Whoever holds the last reference runs the Logout.
    std::shared_ptr<VimConnection> share() const noexcept { return m_connection; }

private:
    ConnectParams m_params;
    std::shared_ptr<VimConnection> m_connection;
};

} // namespace VSphereSoap::Detail
