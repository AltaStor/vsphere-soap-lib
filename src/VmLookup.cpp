// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Public-to-internal VmLocator conversion and VM lookup

#include "VmLookup.hpp"

#include <string>

#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Detail {

std::optional<Vim::VmLocator> toVimLocator(const ::VSphereSoap::VmLocator& vm)
{
    if (vm.moref) return Vim::VmLocator::byMoref(*vm.moref);
    if (vm.uuid) return Vim::VmLocator::byUuid(*vm.uuid);
    if (vm.inventoryPath) return Vim::VmLocator::byInventoryPath(*vm.inventoryPath);
    return std::nullopt;
}

Error checkVmLocator(const ::VSphereSoap::VmLocator& vm, std::string_view what)
{
    if (vm.moref || vm.uuid || vm.inventoryPath) {
        return {};
    }
    return Error(ErrorCode::InvalidArgument, std::string(what) + " has none of moref/uuid/inventoryPath set");
}

Result<Vim::MoRef> findVm(Vim::VimInventory& inventory, const ::VSphereSoap::VmLocator& vm, std::string_view what)
{
    const std::optional<Vim::VmLocator> locator = toVimLocator(vm);
    if (!locator) {
        return checkVmLocator(vm, what);
    }
    return inventory.findVm(*locator);
}

} // namespace VSphereSoap::Detail
