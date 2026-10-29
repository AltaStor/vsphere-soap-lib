// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Disk reads through a snapshot's export lease

#pragma once

// Reads one disk of a snapshot through ExportSnapshot, which works on a running VM and on a disk with snapshot
// deltas: vSphere flattens the chain and serves the disk as a streamOptimized VMDK.
// The stream has no ranged reads, so reads have to come in ascending order; what lies before a read is skipped
// by decoding past it.
// The download runs on a thread of its own, a bounded queue of decoded grains ahead of the reader.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "vsphere-soap/Result.hpp"
#include "vsphere-soap/vim/IHttpTransport.hpp"
#include "vsphere-soap/vim/VimSession.hpp"
#include "vsphere-soap/vim/VimTypes.hpp"

namespace VSphereSoap::Detail {

using namespace VSphereSoap::Vim;

using VSphereSoap::Result;

/// The export lease's URL for one disk.
struct ExportDeviceUrl {
    std::string key; ///< e.g. "/vm-1020/ParaVirtualSCSIController0:0"
    std::string url;
    std::string sslThumbprint;
};

/// The disk URL whose key ends in `controllerSlot` ("<controller type><bus>:<unit>"), or the only disk URL there
/// is when no key matches; nullopt otherwise.
std::optional<ExportDeviceUrl> pickExportDeviceUrl(const std::vector<ExportDeviceUrl>& urls,
                                                   const std::string& controllerSlot);

/// Opens the data connection to a lease URL's host; replaced in tests.
using ExportTransportFactory = std::function<std::unique_ptr<IHttpTransport>(
    const std::string& host, std::uint16_t port, bool verifyTlsCertificate, const std::string& pinnedThumbprint)>;

class ExportStreamReader {
public:
    /// Takes an export lease on `snapshot` and starts downloading the disk at `controllerSlot`.
    /// `vcHost` replaces vCenter's `*` URL placeholder.
    /// The data connection pins the thumbprint the lease reports, or verifies the chain when it reports none and
    /// `verifyTlsCertificate` is set.
    /// `session` must outlive the reader.
    /// Fails with `ErrorCode::IoError` when the lease fails, `ErrorCode::InvalidFormat` when it has no URL for
    /// the disk.
    ///
    static Result<std::unique_ptr<ExportStreamReader>> open(VimSession& session, const std::string& vcHost,
                                                            const MoRef& snapshot, const std::string& controllerSlot,
                                                            std::uint64_t capacityBytes, bool verifyTlsCertificate,
                                                            const ExportTransportFactory& transportFactory = {});

    /// Stops the download, then completes the lease when the whole stream was read and aborts it otherwise.
    ~ExportStreamReader();

    ExportStreamReader(const ExportStreamReader&) = delete;
    ExportStreamReader& operator=(const ExportStreamReader&) = delete;

    /// Reads up to `out.size()` bytes at `offset`; short only at the end of the disk.
    /// Ranges the stream has no grains for read as zero.
    /// Fails with `ErrorCode::InvalidArgument` when `offset` lies before data already passed over, and with
    /// the download's own error when it failed.
    Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out);

private:
    struct Grain {
        std::uint64_t offset = 0;
        std::vector<std::byte> data;
    };

    ExportStreamReader(VimSession& session, MoRef lease, std::uint64_t capacityBytes);

    void download(std::unique_ptr<IHttpTransport> transport, std::string target);

    /// The next grain of the stream; nullopt at its end.
    Result<std::optional<Grain>> nextGrain();

    /// HttpNfcLeaseProgress, at most every so often, so the lease does not expire.
    void keepLeaseAlive(std::uint64_t offset);

    VimSession& m_session;
    MoRef m_lease;
    std::uint64_t m_capacityBytes = 0;

    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<Grain> m_queue;
    bool m_stop = false;          // set by the reader: the download thread gives up
    bool m_downloadDone = false;  // set by the download thread
    bool m_streamComplete = false; // the end-of-stream marker was read
    Error m_downloadError;

    std::optional<Grain> m_held;   // the grain the last read ended in
    std::uint64_t m_passedUpTo = 0; // everything below this has been dropped
    std::uint64_t m_progressOffset = 0;
    std::chrono::steady_clock::time_point m_lastProgress;

    std::thread m_thread;
};

} // namespace VSphereSoap::Detail
