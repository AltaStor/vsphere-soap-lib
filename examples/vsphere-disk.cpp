// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Example tool: export a VM disk to a raw image, or import a raw image as a new VM

// The whole flow a consumer of this library needs, in one file:
//
//   export   VmwareSoapDisk reads a powered-off VM's disk; regions the allocated-block map (CBT)
//            reports as unallocated are skipped and left as holes in the sparse output file.
//   import   resolvePlacement() turns names into ids, VSphereImportSession creates the VM and its
//            HttpNfcLease, VSphereDatastoreBackend streams the image as a compressed streamOptimized
//            VMDK, complete() finalizes.
//
// The password is taken from VSPHERE_PASSWORD rather than the command line, so it stays out of `ps`
// output.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vsphere-soap/Log.hpp"
#include "vsphere-soap/VSphereDatastoreBackend.hpp"
#include "vsphere-soap/VSphereDiskImport.hpp"
#include "vsphere-soap/VSphereImportSession.hpp"
#include "vsphere-soap/VSpherePlacement.hpp"
#include "vsphere-soap/VSphereVmNetworkInfo.hpp"
#include "vsphere-soap/VmwareSoapDisk.hpp"

namespace {

using namespace VSphereSoap;

constexpr std::size_t kChunkBytes = 4 * 1024 * 1024;

void usage()
{
    std::cerr <<
        "usage:\n"
        "  vsphere-disk export --host H --user U --vm VM [--disk-key K] --out FILE [--no-cbt] [TLS]\n"
        "  vsphere-disk import --host H --user U --in FILE --vm-name NAME --datastore DS\n"
        "                      [--compute CLUSTER|HOST] [--pool P] [--folder F]\n"
        "                      [--provisioning thin|thick|eagerZeroedThick] [TLS]\n"
        "\n"
        "  VM      an inventory path (\"DC/vm/web-01\"), moref:vm-42 or uuid:<bios uuid>\n"
        "  TLS     --insecure, or --thumbprint AB:CD:... to pin the host's certificate\n"
        "  common  --port N (443), --verbose\n"
        "\n"
        "  The password is read from VSPHERE_PASSWORD.\n";
}

// "--name value" pairs plus bare flags; anything else is a usage error.
struct Args {
    std::map<std::string, std::string> values;

    [[nodiscard]] std::string get(const std::string& name) const
    {
        const auto it = values.find(name);
        return it == values.end() ? std::string() : it->second;
    }
    [[nodiscard]] bool has(const std::string& name) const { return values.count(name) != 0; }
};

bool parseArgs(int argc, char** argv, Args& args)
{
    static const std::vector<std::string> flags = {"--insecure", "--no-cbt", "--verbose"};
    for (int i = 2; i < argc; ++i) {
        const std::string name = argv[i];
        if (name.rfind("--", 0) != 0) {
            return false;
        }
        if (std::find(flags.begin(), flags.end(), name) != flags.end()) {
            args.values[name] = "1";
        } else if (i + 1 < argc) {
            args.values[name] = argv[++i];
        } else {
            return false;
        }
    }
    return true;
}

int fail(const std::string& message)
{
    std::cerr << "Error: " << message << "\n";
    return EXIT_FAILURE;
}

ConnectParams connectParams(const Args& args)
{
    ConnectParams params;
    params.host = args.get("--host");
    if (args.has("--port")) {
        params.port = static_cast<std::uint16_t>(std::stoul(args.get("--port")));
    }
    params.username = args.get("--user");
    if (const char* password = std::getenv("VSPHERE_PASSWORD")) {
        params.password = password;
    }
    params.verifyTlsCertificate = !args.has("--insecure");
    params.pinnedThumbprint = args.get("--thumbprint");
    params.useCbt = !args.has("--no-cbt");
    params.cbtSnapshotName = "vsphere-disk-example-cbt";
    return params;
}

VmLocator vmLocator(const std::string& vm)
{
    if (vm.rfind("moref:", 0) == 0) {
        return VmLocator::byMoref(vm.substr(6));
    }
    if (vm.rfind("uuid:", 0) == 0) {
        return VmLocator::byUuid(vm.substr(5));
    }
    return VmLocator::byInventoryPath(vm);
}

bool allZero(const std::vector<std::byte>& buffer, std::size_t length)
{
    return std::all_of(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(length),
                       [](std::byte b) { return b == std::byte{0}; });
}

void progress(const char* verb, std::uint64_t done, std::uint64_t total)
{
    std::fprintf(stderr, "\r%s %llu / %llu MiB", verb, static_cast<unsigned long long>(done >> 20),
                 static_cast<unsigned long long>(total >> 20));
}

int runExport(const Args& args)
{
    if (args.get("--host").empty() || args.get("--vm").empty() || args.get("--out").empty()) {
        usage();
        return EXIT_FAILURE;
    }
    const ConnectParams params = connectParams(args);
    const VmLocator vm = vmLocator(args.get("--vm"));

    // Without a key the VM's first disk is exported.
    std::string diskKey = args.get("--disk-key");
    if (diskKey.empty()) {
        auto disks = listVmDisks(params, vm);
        if (!disks) {
            return fail(disks.error().toString());
        }
        if (disks.value().empty()) {
            return fail("the VM has no disks");
        }
        diskKey = disks.value().front().key;
    }

    auto opened = VmwareSoapDisk::open(params, vm, DiskSelector::byKey(diskKey));
    if (!opened) {
        return fail(opened.error().toString());
    }
    auto& disk = *opened.value();
    if (!disk.cbtActive() && !disk.cbtUnavailableReason().empty()) {
        std::cerr << "Warning: reading every block, CBT is unavailable: " << disk.cbtUnavailableReason() << "\n";
    }

    const std::string outPath = args.get("--out");
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail("cannot create " + outPath);
    }

    const std::uint64_t size = disk.virtualSize();
    std::vector<std::byte> buffer(kChunkBytes);
    for (std::uint64_t offset = 0; offset < size;) {
        const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, size - offset));

        auto zero = disk.isZero(offset, length);
        if (!zero) {
            return fail(zero.error().toString());
        }
        if (!zero.value()) {
            auto read = disk.readAt(offset, std::span<std::byte>(buffer.data(), length));
            if (!read) {
                return fail(read.error().toString());
            }
            // A chunk that turns out to be all zero is left as a hole too.
            if (!allZero(buffer, length)) {
                out.seekp(static_cast<std::streamoff>(offset));
                out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(length));
                if (!out) {
                    return fail("write to " + outPath + " failed");
                }
            }
        }
        offset += length;
        progress("exported", offset, size);
    }
    out.close();
    std::filesystem::resize_file(outPath, size);
    std::cerr << "\nexported disk " << diskKey << " (" << size << " bytes) to " << outPath << "\n";
    return EXIT_SUCCESS;
}

int runImport(const Args& args)
{
    if (args.get("--host").empty() || args.get("--in").empty() || args.get("--vm-name").empty() ||
        args.get("--datastore").empty()) {
        usage();
        return EXIT_FAILURE;
    }
    const ConnectParams params = connectParams(args);

    const std::string inPath = args.get("--in");
    std::ifstream in(inPath, std::ios::binary);
    if (!in) {
        return fail("cannot open " + inPath);
    }
    const std::uint64_t sourceSize = std::filesystem::file_size(inPath);
    if (sourceSize == 0) {
        return fail(inPath + " is empty");
    }
    const std::uint64_t capacity = roundUpToGrain(sourceSize);

    auto placement = resolvePlacement(params, {.datastoreName = args.get("--datastore"),
                                               .computeName = args.get("--compute"),
                                               .resourcePoolName = args.get("--pool"),
                                               .folderName = args.get("--folder")});
    if (!placement) {
        return fail(placement.error().toString());
    }

    VSphereImportVmSpec spec;
    spec.vmName = args.get("--vm-name");
    spec.memoryBytes = 1024ull * 1024 * 1024;
    spec.cpuCount = 1;
    spec.disks.push_back({"disk0", capacity});
    spec.resourcePoolId = placement.value().resourcePoolId;
    spec.folderId = placement.value().folderId;
    spec.datastoreId = placement.value().datastoreId;
    spec.diskProvisioning = args.has("--provisioning") ? args.get("--provisioning") : "thin";

    auto begun = VSphereImportSession::begin(params, spec);
    if (!begun) {
        return fail(begun.error().toString());
    }
    auto& session = *begun.value();
    std::cerr << "created VM " << session.vmId() << ", uploading " << sourceSize << " bytes\n";

    std::atomic<int> percent{0};
    session.startKeepAlive([&percent] { return percent.load(); });

    // A failed upload leaves nothing worth keeping: the VM and its partial disk are deleted together.
    auto abandon = [&session](const std::string& message) {
        session.stopKeepAlive();
        if (auto destroyed = session.destroyPlaceholderVm(); !destroyed.ok()) {
            std::cerr << "Warning: VM " << session.vmId() << " could not be removed: " << destroyed.toString() << "\n";
        }
        return fail(message);
    };

    VSphereDatastoreBackendParams backendParams;
    backendParams.deviceUrl = session.deviceUrls().front().url;
    backendParams.verifyTlsCertificate = params.verifyTlsCertificate;
    backendParams.pinnedThumbprint = params.pinnedThumbprint;
    backendParams.virtualSizeBytes = capacity;
    backendParams.vmdkDescriptor = makeStreamOptimizedDescriptor(capacity, spec.vmName + ".vmdk");
    backendParams.compressGrains = true;

    auto backend = VSphereDatastoreBackend::open(backendParams);
    if (!backend) {
        return abandon(backend.error().toString());
    }
    auto& target = *backend.value();

    std::vector<std::byte> buffer(kChunkBytes);
    for (std::uint64_t offset = 0; offset < sourceSize;) {
        const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, sourceSize - offset));
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(length));
        if (!in) {
            return abandon("read from " + inPath + " failed");
        }
        // Whole zero chunks are skipped; the keep-alive stops ESXi dropping a connection gone quiet over them.
        if (allZero(buffer, length)) {
            if (auto kept = target.keepAliveIfIdle(offset + length); !kept.ok()) {
                return abandon(kept.toString());
            }
        } else if (auto written = target.writeAt(offset, std::span<const std::byte>(buffer.data(), length)); !written) {
            return abandon(written.error().toString());
        }
        offset += length;
        percent = static_cast<int>(offset * 100 / sourceSize);
        progress("imported", offset, sourceSize);
    }
    std::cerr << "\n";

    if (auto flushed = target.flush(); !flushed.ok()) {
        return abandon(flushed.toString());
    }
    session.stopKeepAlive();
    if (auto completed = session.complete(); !completed.ok()) {
        return fail("the disk was uploaded but the lease could not be completed: " + completed.toString());
    }
    // HttpNfcLease does not honor eagerZeroedThick by itself.
    if (spec.diskProvisioning == "eagerZeroedThick") {
        if (auto zeroed = eagerZeroAllDisks(params, VmLocator::byMoref(session.vmId())); !zeroed.ok()) {
            return fail(zeroed.toString());
        }
    }
    std::cerr << "imported " << inPath << " as VM '" << spec.vmName << "' (" << session.vmId() << ")\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv)
{
    Args args;
    if (argc < 2 || !parseArgs(argc, argv, args)) {
        usage();
        return EXIT_FAILURE;
    }
    if (args.has("--verbose")) {
        VSphereSoap::Log::setLevel(VSphereSoap::Log::Level::Info);
    }

    const std::string command = argv[1];
    if (command == "export") {
        return runExport(args);
    }
    if (command == "import") {
        return runImport(args);
    }
    usage();
    return EXIT_FAILURE;
}
