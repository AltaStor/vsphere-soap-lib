// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere datastore HTTP path helpers

#pragma once

// Helpers for the datastore HTTP access path ("/folder/<path>?dcPath=...&dsName=..."):
// URL encoding, deriving a VMDK descriptor's "-flat.vmdk" extent name, and parsing the extent file name
// out of a descriptor's text when that derived name turns out to be wrong.
// Split out of VmwareSoapDisk.cpp so they are unit-testable without a live server; the one that asks
// the server goes through the transport seam.

#include <optional>
#include <string>
#include <string_view>

#include "vsphere-soap/Result.hpp"
#include "vsphere-soap/vim/IHttpTransport.hpp"

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

/// The `createType` of a VMDK descriptor, e.g. "vmfs", "monolithicFlat" or "seSparse"; nullopt without one.
std::optional<std::string> parseDescriptorCreateType(std::string_view descriptor);

/// Whether the datastore file at `target` ("/folder/<path>?dcPath=...&dsName=...") exists, asked with a one-byte
/// ranged GET authenticated by `cookie`.
/// Fails with `ErrorCode::IoError` when the answer is neither the file nor a 404.
Result<bool> datastoreFileExists(Vim::IHttpTransport& transport, const std::string& target, const std::string& cookie);

} // namespace VSphereSoap::Detail
