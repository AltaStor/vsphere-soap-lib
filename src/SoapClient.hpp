// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere Web Services SOAP client

#pragma once

// Thin adapter that speaks vSphere Web Services SOAP over an IHttpTransport: builds one envelope per
// call (via pugixml, not string concatenation), POSTs it with the right Content-Type/SOAPAction, parses
// the response, and turns a soap:Fault into a Result error instead of handing the caller a "successful"
// response that actually failed.
// Hand-written against the VIM operations this library needs: no gSOAP, no WSDL codegen.

#include <string>

#include <pugixml.hpp>

#include "Result.hpp"
#include "IHttpTransport.hpp"

namespace VSphereSoap::Detail {

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
    /// `transport` must outlive this object.
    /// `path` is the vSphere SDK endpoint, "/sdk" for both ESXi and vCenter.
    SoapClient(IHttpTransport& transport, std::string host, std::string path = "/sdk");

    /// Posts one SOAP envelope wrapping a deep copy of `requestNode` (the already-namespaced VIM request
    /// element, e.g. `<RetrieveServiceContent xmlns="urn:vim25">...`) inside `<soapenv:Body>`, with
    /// `soapAction` as the `SOAPAction` header.
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
    Result<pugi::xml_document> call(const std::string& soapAction, const pugi::xml_node& requestNode,
                                                     SoapFaultInfo* faultInfo = nullptr);

    /// The "name=value" Cookie header value captured from the last response's Set-Cookie, if any: empty
    /// until the first `call()` response carries one.
    /// Datastore HTTP GETs (VmwareSoapDisk) need the same session cookie the SOAP calls use, so this is
    /// exposed rather than duplicated.
    ///
    [[nodiscard]] const std::string& sessionCookie() const noexcept { return m_sessionCookie; }

    /// The host this client connects to: what an `HttpNfcLeaseInfo.deviceUrl`'s `*` placeholder must be
    /// substituted with before it's usable (vCenter hands back device URLs addressed to the ESXi host
    /// actually holding the datastore, using `*` for "whichever host you used to reach me" when that's
    /// ambiguous).
    ///
    [[nodiscard]] const std::string& host() const noexcept { return m_host; }

private:
    IHttpTransport& m_transport;
    std::string m_host;
    std::string m_path;
    /// "name=value" Cookie header value; empty until captured from Set-Cookie.
    std::string m_sessionCookie;
};

/// Finds `<soapenv:Body>`'s first child by local name (ignoring whichever namespace prefix the server
/// used): the actual response element `call` callers want.
/// Returns a null node if `doc` doesn't have the expected envelope/body shape.
[[nodiscard]] pugi::xml_node soapBodyContent(const pugi::xml_document& doc);

} // namespace VSphereSoap::Detail
