// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere datastore HTTP path helpers

#pragma once

// Pure, server-free helpers for the datastore HTTP access path ("/folder/<path>?dcPath=...&dsName=..."):
// URL encoding, deriving a VMDK descriptor's "-flat.vmdk" extent name, and parsing the extent file name
// out of a descriptor's text when that derived name turns out to be wrong.
// Split out of VmwareSoapDisk.cpp specifically so these are unit-testable without a live server (mirrors
// VmwareVirtualMachine::resolveDatastorePath's existing style).

#include <optional>
#include <string>
#include <string_view>

namespace VSphereSoap::Detail {

/// Percent-encodes everything except unreserved characters and '/' (which is preserved as a path
/// separator, not encoded).
std::string urlEncode(std::string_view in);

/// "myvm/myvm.vmdk" -> "myvm/myvm-flat.vmdk".
/// Returns `relativePath` unchanged if it doesn't look like a plain descriptor name (already
/// a "-flat"/"-delta" name, or doesn't end in ".vmdk" at all): the derived name is only ever a guess;
/// callers must still probe it and fall back to reading the descriptor if it's wrong.
///
std::string deriveFlatName(std::string_view relativePath);

/// Replaces the last path segment of `originalRelativePath` with `newFileName`, keeping the same
/// directory: vSphere always names an extent file relative to its descriptor's own directory.
std::string siblingPath(std::string_view originalRelativePath, std::string_view newFileName);

/// Finds the flat/raw extent's file name in a VMDK descriptor's "Extent description" section, e.g.
/// a `RW 20971520 VMFS "myvm-flat.vmdk"` line: no sparse/COW parsing, just the one quoted token this
/// backend needs.
/// Returns `std::nullopt` if no extent line is found.
///
[[nodiscard]] std::optional<std::string> parseFlatExtentFileName(std::string_view descriptor);

} // namespace VSphereSoap::Detail
