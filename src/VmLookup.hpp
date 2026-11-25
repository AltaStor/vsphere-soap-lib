// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Public-to-internal VmLocator conversion and VM lookup

#pragma once

#include <optional>
#include <string_view>

#include "vsphere-soap/Result.hpp"
#include "vsphere-soap/VmwareSoapDisk.hpp" // public VmLocator
#include "vsphere-soap/vim/VimInventory.hpp"
#include "vsphere-soap/vim/VimTypes.hpp"

namespace VSphereSoap::Detail {

// The public VmLocator is spelled ::VSphereSoap::VmLocator below: Detail also sees Vim::VmLocator
// through the using-directive in DiskAttach.hpp/Placement.hpp.

/// The `Vim::VmLocator` for the first member of `vm` that is set (moref, then uuid, then inventory
/// path); `std::nullopt` when none is.
std::optional<Vim::VmLocator> toVimLocator(const ::VSphereSoap::VmLocator& vm);

/// `ErrorCode::InvalidArgument` "<what> has none of moref/uuid/inventoryPath set" when `vm` has no member
/// set; an ok `Error` otherwise.
/// Lets a caller reject an empty locator before it connects.
Error checkVmLocator(const ::VSphereSoap::VmLocator& vm, std::string_view what = "VmLocator");

/// `inventory.findVm` for the public locator type; fails as `checkVmLocator` does for an empty one.
Result<Vim::MoRef> findVm(Vim::VimInventory& inventory, const ::VSphereSoap::VmLocator& vm,
                          std::string_view what = "VmLocator");

} // namespace VSphereSoap::Detail
