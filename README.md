# vsphere-soap-lib

Export and import of virtual disk images from and to VMware vSphere, for any C++20 tool that needs it.
It talks to vCenter or a standalone ESXi host over the vSphere Web Services (SOAP) API for the control plane and
plain HTTPS for the data, so there is no VMware SDK (VDDK) to ship, license or load.

It is self-contained: it has its own `Result`/`Error`, HTTPS client and log sink.
It forces none of them on a tool that has its own: the disk classes have no base class, logging is a sink the tool
installs, and an optional `IDisk` lives in a separate target.
The behaviour behind each path, including what live testing against real vCenter and ESXi hosts found, is in
[docs/design.md](docs/design.md).

## What it does

| Direction | Classes and functions | Header | How |
|---|---|---|---|
| Export a disk | `VmwareSoapDisk` | `vsphere-soap/VmwareSoapDisk.hpp` | Ranged HTTPS GETs on the disk's flat extent in the datastore; with Changed Block Tracking, unallocated regions are answered by `isZero()` without being read. The VM has to be powered off. |
| Import as a new VM | `VSphereImportSession`, `VSphereDatastoreBackend` | `vsphere-soap/VSphereImportSession.hpp`, `vsphere-soap/VSphereDatastoreBackend.hpp` | `ImportVApp` creates the VM and an HttpNfcLease; the image is streamed as a compressed streamOptimized VMDK, with no local staging file. |
| Import onto an existing VM | `attachImportedDisk`, `eagerZeroAllDisks` | `vsphere-soap/VSphereDiskImport.hpp` | The disk imported into a disposable placeholder VM is moved onto the target VM and attached. |
| Import, fallback transport | `prepareFolderPutImport`, `VSphereFolderPutBackend`, `finishFolderPutImport` | `vsphere-soap/VSphereFolderPut*.hpp` | A monolithicFlat VMDK PUT straight into the target VM's folder, for hosts that refuse the lease upload. |
| Placement by name | `resolvePlacement` | `vsphere-soap/VSpherePlacement.hpp` | Datastore, cluster or host, resource pool and VM folder names to the ids an import takes. |
| VM queries and actions | `listVmDisks`, `listVmNetworkAdapters`, `getVmNotes`, `getVmPowerState`, `powerOnVm`, `powerOffVm`, `destroyVm`, `checkDatastoreWriteAccess` | `vsphere-soap/VSphereVmNetworkInfo.hpp` | One-shot calls, each on its own session. |
| Raw vSphere calls | `SoapClient`, `VimSession`, `VimInventory`, `HttpsRestClient` | `vsphere-soap/vim/*.hpp` | The SOAP layer the rest is built on, for calls the high-level API doesn't cover. Namespace `VSphereSoap::Vim`; exposes pugixml types. |

Every fallible call returns `VSphereSoap::Result<T>`, or a bare `VSphereSoap::Error` when there is no value;
nothing throws across the API.
`VmwareSoapDisk` and both upload backends share one byte-offset method set (`readAt`, `writeAt`, `virtualSize`,
`sectorSize`, `flush`) without a common base class, so a tool adapts them to its own disk interface, see
[Using it with a tool's own types](#using-it-with-a-tools-own-types).

### Export

```cpp
#include <vsphere-soap/VmwareSoapDisk.hpp>

VSphereSoap::ConnectParams params{.host = "vcenter.example.com", .username = user, .password = password};
params.pinnedThumbprint = "AB:CD:...";            // or verifyTlsCertificate = false
params.cbtSnapshotName = "my-tool-cbt-baseline";   // a name of the tool's own, see below

auto disk = VSphereSoap::VmwareSoapDisk::open(params, VSphereSoap::VmLocator::byInventoryPath("DC/vm/web-01"),
                                              VSphereSoap::DiskSelector::byKey("2000"));
if (!disk) {
    std::cerr << "Error: " << disk.error().toString() << "\n";
    return 1;
}
for (std::uint64_t offset = 0; offset < disk.value()->virtualSize(); offset += chunk) {
    if (!disk.value()->isZero(offset, chunk).value()) {
        disk.value()->readAt(offset, buffer);       // and write it wherever it goes
    }
}
```

### Import as a new VM

```cpp
#include <vsphere-soap/VSphereDatastoreBackend.hpp>
#include <vsphere-soap/VSphereImportSession.hpp>
#include <vsphere-soap/VSpherePlacement.hpp>

const std::uint64_t capacity = VSphereSoap::roundUpToGrain(imageSize);
auto placement = VSphereSoap::resolvePlacement(params, {.datastoreName = "ds-01", .computeName = "Cluster"});

VSphereSoap::VSphereImportVmSpec spec;
spec.vmName = "web-01-restored";
spec.memoryBytes = 2ull << 30;
spec.disks.push_back({"disk0", capacity});
spec.resourcePoolId = placement.value().resourcePoolId;
spec.folderId = placement.value().folderId;
spec.datastoreId = placement.value().datastoreId;

auto session = VSphereSoap::VSphereImportSession::begin(params, spec);
session.value()->startKeepAlive([&] { return percentDone.load(); });

auto target = VSphereSoap::VSphereDatastoreBackend::open({
    .deviceUrl = session.value()->deviceUrls().front().url,
    .verifyTlsCertificate = params.verifyTlsCertificate,
    .pinnedThumbprint = params.pinnedThumbprint,
    .virtualSizeBytes = capacity,
    .vmdkDescriptor = VSphereSoap::makeStreamOptimizedDescriptor(capacity, "disk0.vmdk"),
    .compressGrains = true,
});
// writeAt() each non-zero chunk in ascending order, keepAliveIfIdle() over skipped ones, then:
target.value()->flush();
session.value()->stopKeepAlive();
session.value()->complete();
```

[examples/vsphere-disk.cpp](examples/vsphere-disk.cpp) is both directions in full, with the error handling and
cleanup left out above: a failed upload calls `destroyPlaceholderVm()`, so no half-written VM is left behind.

```bash
export VSPHERE_PASSWORD=...
vsphere-disk export --host vc01 --user administrator@vsphere.local --vm DC/vm/web-01 --out web-01.raw
vsphere-disk import --host vc01 --user administrator@vsphere.local --in web-01.raw --vm-name web-01-copy \
                    --datastore ds-01 --compute Cluster --provisioning thin
```

## Things that will bite you

**Export reads only powered-off VMs.** `open()` checks and refuses otherwise; there is no guest-consistency story
for a disk that is changing under the reader.

**Give every tool its own `ConnectParams::cbtSnapshotName`.** With CBT, `open()` takes a baseline snapshot and,
before that, deletes any leftover snapshot of the same name that a killed run never removed. Two tools sharing a
name delete each other's baseline; a tool that renames it can no longer clean up what its own earlier runs left.

**`compressGrains` has to be `true` for a real upload.** ESXi accepts an uncompressed streamOptimized upload in
full and then rejects it (HTTP 500 "Not a supported disk format"). The uncompressed mode exists for tests only,
and other tools such as qemu-img misread what it produces.

**`writeAt()` calls on an upload go in ascending, non-overlapping order**, and a skipped gap has to cover whole
grains (64 KiB). A copy loop with a chunk size that is a multiple of 64 KiB satisfies both.
`keepAliveIfIdle()` is needed over long zero stretches: ESXi drops an HttpNfcLease upload that has sent nothing
for about a minute.

**HttpNfcLease ignores `eagerZeroedThick`.** Call `eagerZeroAllDisks()` after `complete()` when that is what was
asked for.

**Objects are not thread-safe**, though the upload backends and the lease keep-alive run threads of their own.
Confine each object's calls to one thread.

**Logging goes through `VSphereSoap::Log`.** By default, warnings and errors go to stderr as `Warning: [soap] ...`
and `Error: [http] ...` lines. `Log::setSink()` routes every line into the tool's own logging, `Log::setLevel()`
or `VSPHERE_SOAP_LOG_LEVEL` (trace|debug|info|warn|error|off) sets how much. Nothing is written to stdout.

## Building

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Needs a C++20 compiler with `<format>` (GCC 13+, Clang 17+, MSVC 2022), CMake 3.21+, OpenSSL and zlib.
Boost (Asio and Beast, header-only here), pugixml and GoogleTest are taken from the system when present and
fetched otherwise. On Debian/Ubuntu everything comes from packages:

```bash
apt install libssl-dev zlib1g-dev libboost-dev libpugixml-dev libgtest-dev qemu-utils
```

| Parameter | Values | Default | |
|---|---|---|---|
| `VSPHERE_SOAP_BUILD_TESTS` | `ON`, `OFF` | `ON` standalone, `OFF` from a tool | The unit tests. |
| `VSPHERE_SOAP_BUILD_EXAMPLES` | `ON`, `OFF` | `ON` standalone, `OFF` from a tool | `vsphere-disk`. |
| `VSPHERE_SOAP_BUILD_DISK` | `ON`, `OFF` | `ON` standalone, `OFF` from a tool | The optional `vsphere-soap-disk` target with `IDisk` and `DiskAdapter`. |
| `VSPHERE_SOAP_FETCH_DEPS` | `ON`, `OFF` | `ON` | Fetch Boost, pugixml and GoogleTest when missing; `OFF` makes a missing one an error. |
| `VSPHERE_SOAP_INSTALL` | `ON`, `OFF` | `ON` standalone | Install rules and the `VSphereSoap` CMake package. Skipped when pugixml was fetched, since the package would then name a dependency its consumer cannot find. |

## Using it from a tool

As a subdirectory, which builds it with the tool's own toolchain and settings:

```cmake
add_subdirectory("${VSPHERE_SOAP_LIB_DIR}" vsphere_soap_lib_build)
target_link_libraries(my-tool PRIVATE VSphereSoap::vsphere-soap)
```

Or installed, with `cmake --install build --prefix /opt/vsphere-soap`, and then:

```cmake
find_package(VSphereSoap 1 REQUIRED)
target_link_libraries(my-tool PRIVATE VSphereSoap::vsphere-soap)
```

Headers are always included with their directory, `#include <vsphere-soap/VmwareSoapDisk.hpp>`, so the library's
`Error.hpp` or `Result.hpp` never meets a tool's header of the same name.

| Target | Contents |
|---|---|
| `VSphereSoap::vsphere-soap` | The core: export, import, VM queries, the `vim/` SOAP layer, `Result`/`Error` and the log sink. |
| `VSphereSoap::vsphere-soap-disk` | Optional: `VSphereSoap::Disk::IDisk` and `DiskAdapter`, which turns any core disk class into an `IDisk`. |

Both are static libraries; OpenSSL, zlib and pugixml come along in the core's link interface.

## Using it with a tool's own types

A tool that already has a disk interface, an error type and a logger links only the core and adds a thin layer
of its own.

**Disks.** The core disk classes have no base class, so one template adapts all three to the tool's interface,
converting the library's `Error` where the tool's differs:

```cpp
template <class T>
class SoapDisk final : public Tool::IDisk {
public:
    explicit SoapDisk(std::unique_ptr<T> disk) : m_disk(std::move(disk)) {}

    Tool::Result<std::uint64_t> readAt(std::uint64_t offset, std::span<std::byte> out) override
    {
        return toTool(m_disk->readAt(offset, out));
    }
    Tool::Result<void> flush() override
    {
        const VSphereSoap::Error error = m_disk->flush();
        return error.ok() ? Tool::Result<void>() : Tool::Result<void>(toTool(error));
    }
    // writeAt, virtualSize, sectorSize and isZero the same way
    T& disk() { return *m_disk; } // keepAliveIfIdle(), cbtActive()

private:
    std::unique_ptr<T> m_disk;
};
```

`include/vsphere-soap/disk/DiskAdapter.hpp` is the same template against the library's own `IDisk`.
A default-constructed `VSphereSoap::Error` means success (`ok()`); `code()` and `message()` carry the rest.

**Logging.** Messages arrive on two channels, "soap" for vSphere calls and "http" for the transport.
Routing them into spdlog:

```cpp
VSphereSoap::Log::setLevel(VSphereSoap::Log::Level::Debug);
VSphereSoap::Log::setSink([](VSphereSoap::Log::Level level, std::string_view channel, std::string_view message) {
    static const auto levels = std::array{spdlog::level::trace, spdlog::level::debug, spdlog::level::info,
                                          spdlog::level::warn, spdlog::level::err, spdlog::level::off};
    spdlog::get(std::string(channel))->log(levels[static_cast<int>(level)], "{}", message);
});
```

The sink is called from the upload threads too, so it has to be thread-safe.

**Raw calls.** `vsphere-soap/vim/` gives direct access to the SOAP session for what the high-level API doesn't
cover. `HttpTransportAdapter` is the library's HTTPS client behind `IHttpTransport`, which a tool may also
implement over a transport of its own.

```cpp
VSphereSoap::Vim::HttpTransportAdapter transport(host, 443, verifyTls, thumbprint);
VSphereSoap::Vim::SoapClient soap(transport, host);
auto session = VSphereSoap::Vim::VimSession::open(soap, user, password);
VSphereSoap::Vim::VimInventory inventory(*session.value());
auto vms = inventory.listObjects("VirtualMachine", {"name", "runtime.powerState"});
```

## Layout

```
include/vsphere-soap/  VmwareSoapDisk.hpp            export; ConnectParams, VmLocator, DiskSelector
                       VSphereImportSession.hpp      import: the VM and its HttpNfcLease
                       VSphereDatastoreBackend.hpp   import: the streamOptimized upload, descriptor helpers
                       VSphereDiskImport.hpp         import onto an existing VM
                       VSphereFolderPut*.hpp         the folder-put fallback transport
                       VSpherePlacement.hpp          names to placement ids
                       VSphereVmNetworkInfo.hpp      one-shot VM queries and actions
                       Result.hpp, Error.hpp, Log.hpp
  vim/                 SoapClient, VimSession, VimInventory, VimTypes     the SOAP client and VIM session
                       HttpsRestClient, HttpTransportAdapter, IHttpTransport   HTTPS over Boost.Beast
  disk/                IDisk.hpp, DiskAdapter.hpp    the optional vsphere-soap-disk target
src/                   SnapshotScope, CbtQuery, TaskWait                  snapshots, CBT, task polling
                       HttpNfcLeaseSession, DiskAttach                    lease lifecycle, disk move and attach
                       StreamOptimizedVmdkEncoder, GrainAssembler         the VMDK stream
tests/                 unit tests against a scripted fake transport; qemu-img interop of the encoder
examples/              vsphere-disk
docs/design.md         how each path works and what live testing found
```

## What has been tested

- **Unit tests:** SOAP envelopes and faults, session and inventory handling, CBT paging, the VMDK encoder and
  grain framing, disk attach and import spec building, placement lookup, log routing, all against a scripted
  fake transport. The encoder's compressed output passes `qemu-img check` and converts back byte for byte.
- **Live, against a vCenter test cluster:** `vsphere-disk export` of a 10 GiB thin disk from a powered-off VM
  (CBT was unavailable on that disk, so the baseline snapshot was taken, dropped again, and every block read);
  `vsphere-disk import` of a 1 GiB image as a new thin VM, placed by datastore and cluster name, read back
  through `VmwareSoapDisk` and matching byte for byte.
- **Built as a subdirectory** of a separate consumer project, with the tool's log sink receiving the messages.
- **Installed and used through `find_package`** by a consumer with its own `Error.hpp`, `Result.hpp` and
  `IDisk.hpp` ahead in its include path, adapting the disk classes to its own interface.
  Through `DiskAdapter`, a 10 GiB disk of a powered-off VM was opened with CBT, read and checked with `isZero()`
  on a live vCenter, the VM found by name through the `vim/` layer.
- **Not built yet:** Windows (MinGW or MSVC) and macOS. The code has no platform-specific parts, and the build
  links Winsock and crypt32 on Windows, but neither has been compiled.
