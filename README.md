# vsphere-soap-lib

Export and import of virtual disk images from and to VMware vSphere, for any C++20 tool that needs it.
It talks to vCenter or a standalone ESXi host over the vSphere Web Services (SOAP) API for the control plane and
plain HTTPS for the data, so there is no VMware SDK (VDDK) to ship, license or load.

It is self-contained: it has its own `Result`/`Error`, HTTPS client and log sink.
The behaviour behind each path, including what live testing against real vCenter and ESXi hosts found, is in
[docs/design.md](docs/design.md).

## What it does

| Direction | Classes and functions | Header | How |
|---|---|---|---|
| Export a disk | `VmwareSoapDisk` | `VmwareSoapDisk.hpp` | Ranged HTTPS GETs on the disk's flat extent in the datastore; with Changed Block Tracking, unallocated regions are answered by `isZero()` without being read. The VM has to be powered off. |
| Import as a new VM | `VSphereImportSession`, `VSphereDatastoreBackend` | `VSphereImportSession.hpp`, `VSphereDatastoreBackend.hpp` | `ImportVApp` creates the VM and an HttpNfcLease; the image is streamed as a compressed streamOptimized VMDK, with no local staging file. |
| Import onto an existing VM | `attachImportedDisk`, `eagerZeroAllDisks` | `VSphereDiskImport.hpp` | The disk imported into a disposable placeholder VM is moved onto the target VM and attached. |
| Import, fallback transport | `prepareFolderPutImport`, `VSphereFolderPutBackend`, `finishFolderPutImport` | `VSphereFolderPut*.hpp` | A monolithicFlat VMDK PUT straight into the target VM's folder, for hosts that refuse the lease upload. |
| Placement by name | `resolvePlacement` | `VSpherePlacement.hpp` | Datastore, cluster or host, resource pool and VM folder names to the ids an import takes. |
| VM queries and actions | `listVmDisks`, `listVmNetworkAdapters`, `getVmNotes`, `getVmPowerState`, `powerOnVm`, `powerOffVm`, `destroyVm`, `checkDatastoreWriteAccess` | `VSphereVmNetworkInfo.hpp` | One-shot calls, each on its own session. |

Every fallible call returns `VSphereSoap::Result<T>`; nothing throws across the API.
`VmwareSoapDisk` and both upload backends implement `VSphereSoap::IDisk`, a byte-offset `readAt`/`writeAt`
interface, so one copy loop drives either direction. A tool with its own disk interface (`IDiskReader`/
`IDiskWriter` of abr-disk-lib) wraps these in a few lines.

### Export

```cpp
#include "VmwareSoapDisk.hpp"

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
#include "VSphereDatastoreBackend.hpp"
#include "VSphereImportSession.hpp"
#include "VSpherePlacement.hpp"

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

Headers are included by their bare names (`#include "VmwareSoapDisk.hpp"`) in both cases.
An install puts them in `<prefix>/include/vsphere-soap/`, and the package points the include path there, so
generic names such as `Error.hpp` and `Result.hpp` stay out of the shared include directory.
A tool with headers of its own by those names has to keep its include path ahead of this library's.

It is a static library; OpenSSL, zlib and pugixml come along in its link interface.

## Layout

```
include/               VmwareSoapDisk.hpp            export; ConnectParams, VmLocator, DiskSelector
                       VSphereImportSession.hpp      import: the VM and its HttpNfcLease
                       VSphereDatastoreBackend.hpp   import: the streamOptimized upload, descriptor helpers
                       VSphereDiskImport.hpp         import onto an existing VM
                       VSphereFolderPut*.hpp         the folder-put fallback transport
                       VSpherePlacement.hpp          names to placement ids
                       VSphereVmNetworkInfo.hpp      one-shot VM queries and actions
                       IDisk.hpp, Result.hpp, Error.hpp, Log.hpp
src/                   SoapClient, VimSession, VimInventory, VimTypes     the SOAP client and VIM session
                       SnapshotScope, CbtQuery, TaskWait                  snapshots, CBT, task polling
                       HttpNfcLeaseSession, DiskAttach                    lease lifecycle, disk move and attach
                       StreamOptimizedVmdkEncoder, GrainAssembler         the VMDK stream
                       HttpsRestClient, HttpTransportAdapter, IHttpTransport   HTTPS over Boost.Beast
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
- **Not built yet:** Windows (MinGW or MSVC) and macOS. The code has no platform-specific parts, and the build
  links Winsock and crypt32 on Windows, but neither has been compiled.
