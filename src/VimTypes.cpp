// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Shared vSphere VIM API types and helpers

#include "vsphere-soap/vim/VimTypes.hpp"

namespace VSphereSoap::Vim {

pugi::xml_document makeVimRequest(const char* operationName, const MoRef& thisRef)
{
    pugi::xml_document doc;
    pugi::xml_node op = doc.append_child(operationName);
    op.append_attribute("xmlns") = kVimNamespace;
    pugi::xml_node thisNode = op.append_child("_this");
    thisNode.append_attribute("type") = thisRef.type.c_str();
    thisNode.text() = thisRef.value.c_str();
    return doc;
}

MoRef parseMoRef(pugi::xml_node node)
{
    if (!node) {
        return {};
    }
    const pugi::xml_attribute typeAttr = node.attribute("type");
    if (!typeAttr) {
        return {};
    }
    return MoRef{typeAttr.as_string(), node.text().as_string()};
}

pugi::xml_node returnval(pugi::xml_node responseElement) { return responseElement.child("returnval"); }

} // namespace VSphereSoap::Vim
