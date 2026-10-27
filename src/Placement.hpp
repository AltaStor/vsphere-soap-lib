// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Name-based placement lookup over an open VIM session

#pragma once

#include "VimSession.hpp"
#include "VSpherePlacement.hpp"

namespace VSphereSoap::Detail {

/// The lookups behind `VSphereSoap::resolvePlacement()`, on a session the caller already holds: one
/// ContainerView read each for datacenters, datastores and compute resources, plus resource pools and
/// folders when those are named.
Result<VSpherePlacement> resolvePlacement(VimSession& session, const VSpherePlacementSpec& spec);

} // namespace VSphereSoap::Detail
