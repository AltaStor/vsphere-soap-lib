// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// vSphere Web Services SOAP client

#pragma once

// Thin adapter that speaks vSphere Web Services SOAP over an IHttpTransport: builds one envelope per
// call (via pugixml, not string concatenation), POSTs it with the right Content-Type/SOAPAction, parses
// the response, and turns a soap:Fault into a Result error instead of handing the caller a "successful"
// response that actually failed.
// Hand-written against the VIM operations this library needs: no gSOAP, no WSDL codegen.

#include <string>
#include <utility>

#include <pugixml.hpp>

#include "vsphere-soap/Result.hpp"
#include "vsphere-soap/vim/IHttpTransport.hpp"

namespace VSphereSoap::Vim {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;
using VSphereSoap::Result;

/// Extra detail about a `soap:Fault` response, populated by `SoapClient::call` only when the failure was
/// a fault (not a transport error) and the caller passed a non-null `faultInfo` out-param.
/// `typeName` is the VIM fault's concrete type (the local name of `detail`'s first child, e.g.
/// "NotAuthenticated", "InvalidLogin", "FileNotFound"); this is how `VimSession` recognizes
/// a session-expiry fault worth retrying, since `Result<T>`'s `ErrorCode` is too coarse-grained to carry
/// it.
///
struct SoapFaultInfo {
    std::string typeName;
    std::string faultString;
};

class SoapClient {
public:
    /// API version used until the server's own is known.
    /// Every ESXi and vCenter from 6.7 on accepts it, and it has every ServiceContent field a session reads.
    static constexpr const char* kBaselineApiVersion = "6.7";

    /// `transport` must outlive this object.
    /// `path` is the vSphere SDK endpoint, "/sdk" for both ESXi and vCenter.
    SoapClient(IHttpTransport& transport, std::string host, std::string path = "/sdk");

    /// Posts one SOAP envelope wrapping a deep copy of `requestNode` (the already-namespaced VIM request
    /// element, e.g. `<RetrieveServiceContent xmlns="urn:vim25">...`) inside `<soapenv:Body>`.
    /// The `SOAPAction` header is "urn:vim25/" plus `apiVersion()`, which is how the server picks the API
    /// version it answers in.
    /// Attaches the session cookie captured from a prior response's `Set-Cookie`, if any.
    ///
    /// On success, returns the full parsed response document; use `soapBodyContent(doc)` to reach
    /// the actual response element inside `<soapenv:Body>` regardless of the envelope's namespace
    /// prefix.
    ///
    /// Fails with `ErrorCode::IoError` for transport failure or a `soap:Fault` response (`faultInfo`, if
    /// non-null, is populated only for the fault case); `ErrorCode::InvalidFormat` if the response isn't
    /// parseable XML or lacks a `Body` element.
    ///
    Result<pugi::xml_document> call(const pugi::xml_node& requestNode, SoapFaultInfo* faultInfo = nullptr);

    /// The vim25 API version sent with every call, `kBaselineApiVersion` until set.
    /// A session sets it to the server's `about.apiVersion` once connected.
    const std::string& apiVersion() const noexcept { return m_apiVersion; }
    void setApiVersion(std::string version) { m_apiVersion = std::move(version); }

    /// The "name=value" Cookie header value captured from the last response's Set-Cookie, if any: empty
    /// until the first `call()` response carries one.
    /// Datastore HTTP GETs (VmwareSoapDisk) need the same session cookie the SOAP calls use, so this is
    /// exposed rather than duplicated.
    ///
    const std::string& sessionCookie() const noexcept { return m_sessionCookie; }

    /// The host this client connects to: what an `HttpNfcLeaseInfo.deviceUrl`'s `*` placeholder must be
    /// substituted with before it's usable (vCenter hands back device URLs addressed to the ESXi host
    /// actually holding the datastore, using `*` for "whichever host you used to reach me" when that's
    /// ambiguous).
    ///
    const std::string& host() const noexcept { return m_host; }

private:
    IHttpTransport& m_transport;
    std::string m_host;
    std::string m_path;
    std::string m_apiVersion = kBaselineApiVersion;
    /// "name=value" Cookie header value; empty until captured from Set-Cookie.
    std::string m_sessionCookie;
};

/// Finds `<soapenv:Body>`'s first child by local name (ignoring whichever namespace prefix the server
/// used): the actual response element `call` callers want.
/// Returns a null node if `doc` doesn't have the expected envelope/body shape.
pugi::xml_node soapBodyContent(const pugi::xml_document& doc);

} // namespace VSphereSoap::Vim
