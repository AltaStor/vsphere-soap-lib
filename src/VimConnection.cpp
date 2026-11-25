// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// One logged-in VIM connection, and the handle every ConnectParams-based operation runs on

#include "VimConnection.hpp"

#include <utility>

#include "SoapLog.hpp"
#include "vsphere-soap/vim/HttpTransportAdapter.hpp"

namespace VSphereSoap::Detail {

namespace {

TransportFactory& transportFactoryOverride()
{
    static TransportFactory factory;
    return factory;
}

std::unique_ptr<Vim::IHttpTransport> makeTransport(const ConnectParams& params)
{
    if (const TransportFactory& factory = transportFactoryOverride()) {
        return factory(params);
    }
    return std::make_unique<Vim::HttpTransportAdapter>(params.host, params.port, params.verifyTlsCertificate,
                                                       params.pinnedThumbprint);
}

} // namespace

void setTransportFactoryForTesting(TransportFactory factory) { transportFactoryOverride() = std::move(factory); }

ClientConnection::ClientConnection(ConnectParams params) : m_params(std::move(params)) {}

Result<VimConnection*> ClientConnection::get()
{
    if (m_connection) {
        return m_connection.get();
    }

    auto connection = std::make_shared<VimConnection>();
    connection->transport = makeTransport(m_params);
    connection->soap = std::make_unique<Vim::SoapClient>(*connection->transport, m_params.host);
    auto session = Vim::VimSession::open(*connection->soap, m_params.username, m_params.password);
    if (!session) {
        logger()->warn("VIM session open against {}:{} failed: {}", m_params.host, m_params.port,
                       session.error().toString());
        return std::move(session).error();
    }
    connection->session = std::move(session).value();
    m_connection = std::move(connection);
    return m_connection.get();
}

} // namespace VSphereSoap::Detail
