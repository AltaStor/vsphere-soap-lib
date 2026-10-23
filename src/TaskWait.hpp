// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere task polling helper

#pragma once

// Shared vSphere `Task` polling, extracted from SnapshotScope (its own original, still-only, caller) so
// the new disk-attach flow's several `_Task` operations (ReconfigVM_Task, MoveDatastoreFile_Task) don't
// grow a third near-identical copy of the same RetrievePropertiesEx polling loop.

#include "VimSession.hpp"
#include "VimTypes.hpp"
#include "Result.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Result;

/// Polls `Task.info.state` via the property collector with capped exponential backoff (250ms -> 5s)
/// until it leaves "queued"/"running".
/// Returns `info.result` (empty `MoRef` if the task had none, e.g.
/// `RemoveSnapshot_Task`/`ReconfigVM_Task`) on "success".
/// Fails with `ErrorCode::IoError` on "error" (message taken from `info.error.localizedMessage`) or if
/// the task hasn't finished after 10 minutes.
///
[[nodiscard]] Result<MoRef> waitForVimTask(VimSession& session, const MoRef& task);

} // namespace VSphereSoap::Detail
