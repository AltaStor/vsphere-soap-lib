// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Shared vSphere VIM API types and helpers

#pragma once

// Small shared building blocks for the VIM (vSphere Web Services) API's request/response shape, used by
// VimSession/VimInventory/SnapshotScope/CbtQuery: kept out of SoapClient.hpp since SoapClient itself is
// VIM-agnostic (it just POSTs envelopes and detects faults).

#include <string>

#include <pugixml.hpp>

namespace VSphereSoap::Vim {

inline constexpr const char* kVimNamespace = "urn:vim25";

/// A vSphere "ManagedObjectReference": a type name plus an opaque id (e.g. {"VirtualMachine",
/// "vm-123"}), used throughout the VIM API to refer to server-side objects.
struct MoRef {
    std::string type;
    std::string value;

    bool empty() const noexcept { return value.empty(); }
};

/// Builds
/// `<operationName xmlns="urn:vim25"><_this type="thisRef.type">thisRef.value</_this></operationName>`
/// as a fresh, owning document; the shared shape of every VIM request: an operation name, the object
/// it's called on, then whatever operation-specific children the caller appends to `.document_element()`
/// itself before passing it to `SoapClient::call`/`VimSession::call`.
///
pugi::xml_document makeVimRequest(const char* operationName, const MoRef& thisRef);

/// Reads a ManagedObjectReference-shaped node (a `type` attribute plus a text value) into a `MoRef`.
/// Returns an empty `MoRef` if `node` is null or has no `type` attribute.
MoRef parseMoRef(pugi::xml_node node);

/// `soap:Body`'s response element -> its `<returnval>` child, the shape every VIM response wraps its
/// actual payload in.
pugi::xml_node returnval(pugi::xml_node responseElement);

} // namespace VSphereSoap::Vim
