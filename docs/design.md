# vsphere-soap-lib design notes

How the library talks to vSphere, why each path is shaped the way it is, and what live testing against vCenter
and ESXi found along the way. The README covers the API from a caller's side; this is the reasoning behind it.

## 1. Two planes, no SDK

- **Control plane:** vSphere Web Services SOAP (`POST /sdk`): login, inventory, snapshots, Changed Block
  Tracking, `HttpNfcLease` imports, VM reconfiguration and datastore file operations.
- **Data plane:** plain HTTPS: ranged `GET /folder/...` to read a disk's flat extent, a streamed `POST` to an
  `HttpNfcLease` device URL to upload one, and a streamed `PUT /folder/...` for the fallback transport.

The SOAP client is hand-written against the few dozen VIM operations the library needs rather than generated from
the WSDL. Requests are built as pugixml DOM trees, so every caller-supplied string is escaped by pugixml; the one
string-built XML is the OVF descriptor for `CreateImportSpec`, which escapes each value itself. Responses are read
by element local name, ignoring whichever namespace prefix the server chose. Every request uses the unversioned
`urn:vim25` namespace.

## 2. Sessions

`VimSession::open()` calls `RetrieveServiceContent` on the fixed `ServiceInstance` object, then `Login`.
The first `Set-Cookie` of the login answer becomes the session cookie for every later SOAP call and for the
datastore `/folder/` requests, which accept the same cookie.

A `NotAuthenticated` fault makes `VimSession::call()` log in again and retry once; any other fault, or a second
`NotAuthenticated`, is returned as is. The destructor logs out on a best-effort basis.
That logout invalidates the cookie at once, which is why the folder-put import keeps its session alive in an
opaque handle until the flat extent upload is done (§7).

TLS is Boost.Beast over OpenSSL. `pinnedThumbprint`, a SHA-1 fingerprint of the host's leaf certificate, replaces
chain verification rather than adding to it, the usual way to trust a self-signed ESXi or vCenter certificate.
One HTTPS connection is kept alive and reused per client, since rapid new connections from one address are what
firewalls and vCenter's reverse proxy throttle; a reused connection that the server closed is reopened once.

## 3. Errors

Nothing throws across the API. `ErrorCode` stays coarse, for callers to branch on:

| Code | Raised for |
|---|---|
| `IoError` | Transport failures, SOAP faults (the message carries the fault type), failed or timed-out tasks, unexpected HTTP statuses |
| `InvalidFormat` | Unparseable or unexpectedly shaped responses, a CBT query that stops advancing |
| `InvalidArgument` | A VM, disk, datastore or controller that matches nothing (or too much), a powered-on VM where a powered-off one is required |
| `NotImplemented` | Reading from an upload backend or writing to the export disk; CBT that is not eligible |
| `Cancelled` | An upload backend destroyed before a successful `flush()` |

Tasks are polled with a capped exponential backoff (250 ms up to 5 s) for at most 10 minutes; a lease has 5
minutes to become ready.

## 4. Export

`VmwareSoapDisk::open()` resolves the VM (by moref, BIOS UUID or inventory path), requires it to be powered off,
picks the disk by device key or datastore path, and derives the flat extent's name from the disk backing. When the
derived `-flat.vmdk` name returns 404, the extent line of the small descriptor file is read instead of trusting
the naming convention. RDM disks have no datastore file to read and are refused.

A ranged probe checks that the server honours `Range`. A server that answers 200 instead of 206 fails `open()`
loudly rather than sending the whole disk on every read. `readAt()` retries a transport failure or a 5xx up to
five times with a linear 200 ms backoff; a short body or any other status fails at once.

**Changed Block Tracking.** With `useCbt`, `open()` checks eligibility first (change tracking on, hardware version
`vmx-07` or later, no RDM, not an independent disk) so the caller gets a reason instead of a fault. It then takes a
baseline snapshot and asks `QueryChangedDiskAreas` with `changeId="*"` for the allocated-block map, paging until
the whole capacity is covered. `isZero()` then answers from that map without reading. None of this can fail
`open()`: any CBT failure is recorded in `cbtUnavailableReason()` and every block is read instead. Live, a VM
whose disk had been restored by another tool answered the query with a `FileFault`, and the export carried on.

The snapshot is only a CBT reference point, never a consistency mechanism, since the VM is powered off. Its name
comes from `ConnectParams::cbtSnapshotName`. `open()` removes a leftover snapshot of that name *before* reading
the VM's disk layout, because a snapshot left by a killed run redirects the disk backing to its delta file.

## 5. Import as a new VM

`HttpNfcLease` is the mechanism ovftool and govc use: describe the VM in OVF, let vCenter create it, and upload
the disks to URLs the lease hands back.

1. `CreateImportSpec` on the `OvfManager` turns a generated OVF descriptor (CPU, memory, firmware, disks, SCSI
   controller type, network adapters, provisioning) into an import spec.
2. `ImportVApp` on the resource pool creates the VM and returns the lease, which is polled until `ready`.
3. Each disk is streamed to its device URL by `VSphereDatastoreBackend`, while a keep-alive thread reports
   `HttpNfcLeaseProgress` every 30 seconds.
4. `HttpNfcLeaseComplete` finalizes; destroying the session without it aborts the lease.

**Device URLs are correlated by position.** `deviceUrl[]` is not one entry per requested disk (a one-disk import
also returns the VM's `.nvram`), and its keys are generated by vCenter, not echoed from the descriptor. Entries are
filtered to `disk=true` and matched to the requested disks in request order, which is also the order vCenter
reports them in; each result's key is then overwritten with the caller's own, so `VSphereImportDeviceUrl::key`
still equals `VSphereImportDiskSpec::key`. The URL carries its own ticket and needs no cookie.

**The disk format is streamOptimized VMDK, the only one a device URL accepts.** `StreamOptimizedVmdkEncoder`
writes it: 512-byte sectors, 64 KiB grains, each grain deflated behind a GRAIN marker, grain tables and directory
at the end, then the footer and end-of-stream marker. Grain directory and footer pointers point one sector past
their markers, as the format and qemu-img expect; the unit tests check the output with `qemu-img check` and a
byte-exact `qemu-img convert` round trip. `GrainAssembler` turns arbitrary `writeAt()` ranges into whole grains, so
a copy loop's chunk size does not have to be a multiple of the grain size, as long as only whole grains are
skipped.

**Upload details found by live testing:**

- Compression is mandatory. ESXi accepts an uncompressed stream in full, then fails it with HTTP 500 "Not a
  supported disk format (uncompressed disk)". The uncompressed mode is kept for tests only; qemu-img silently
  misreads its output.
- POST, not PUT. ImportVApp has already created the disk with the requested provisioning; POST streams into it
  and keeps it thin, while PUT replaces the file (it needs `Overwrite: t` to be accepted at all) and the result is
  thick whatever was asked for. Confirmed on ESXi 8.0.3.
- The body is chunked, since the compressed size is unknown up front, in about 1 MiB pieces through a four-deep
  queue to one long-lived request on a background thread.
- ESXi resets a connection that receives no body bytes within a second of its headers, so the encoder's header
  and descriptor are queued before the upload thread connects.
- ESXi aborts an upload that sends nothing for about a minute, which a long zero region causes; after 20 idle
  seconds `keepAliveIfIdle()` sends the pending bytes or one explicit zero grain from a range that is already
  final.
- All-zero grains are never sent. Sending them makes ESXi write every block and turns a lazy-zeroed thick disk
  into one vCenter reports as eager-zeroed.
- `eagerZeroedThick` is not honoured by the lease; `eagerZeroAllDisks()` runs `EagerZeroVirtualDisk_Task`
  afterwards.

## 6. Import onto an existing VM

`ImportVApp` always creates a VM, so `attachImportedDisk()` uses a disposable placeholder VM, the same trick
govc's `import.vmdk` uses:

1. `ReconfigVM_Task` on the placeholder removes the disk device without a `fileOperation`, keeping the file.
2. `MoveDatastoreFile_Task` moves the single streamOptimized file into the target VM's own folder, the
   directory of `config.files.vmPathName`, not that `.vmx` path itself. Within one datastore this is a rename.
3. `UnregisterVM` drops the placeholder from the inventory. `Destroy_Task` would delete its folder.
4. `ReconfigVM_Task` on the target adds a `VirtualDisk` backed by the moved file, after adding a controller in a
   separate reconfig when one has to be created.

**Controller and unit.** An explicit type and bus selects that controller; a missing one is an error unless
`createIfMissing` asks for it to be added. Without a choice, the primary controller is the one most of the VM's
disks are already on (ties to the lowest key), or the lowest-key controller when it has no disks yet. The lowest
free unit is used, skipping unit 7 on SCSI controllers, which belongs to the controller itself. The target has to be
powered off; hot-add has not been tested.

**Failures and what they leave behind.** The principle is never to delete data that was the point of the operation:

| Failure | State left | Handling |
|---|---|---|
| Upload fails or is cancelled | Placeholder with a partial disk, lease open | The caller destroys the placeholder with `destroyPlaceholderVm()`; nothing useful was produced |
| `HttpNfcLeaseComplete` fails | Placeholder with a fully written disk | Reported, nothing deleted |
| Detach fails | Placeholder still owns the disk | Reported with the placeholder's id; safe to retry the whole attach |
| Move fails | Disk detached, still in the placeholder's folder | Reported with the file's current path |
| Unregister fails | Disk moved, empty placeholder still listed | Logged and ignored; clutter, not data risk |
| Attach fails | Disk in the target's folder, attached to nothing | Reported with the path and the chosen controller and unit, so only the last call needs a retry |

## 7. The folder-put fallback

For hosts that refuse the lease upload, `prepareFolderPutImport()` writes a monolithicFlat descriptor straight
into the target VM's folder, `VSphereFolderPutBackend` streams the raw extent with one `PUT /folder/...`, and
`finishFolderPutImport()` attaches the file. There is no placeholder to clean up, but every byte of the disk,
zeros included, goes over the wire.

- The `/folder/` endpoint has no ranged PUT (`Content-Range` gives HTTP 501), and a PUT shorter than its
  `Content-Length` silently truncates the file. The exact size is therefore declared up front, gaps are
  zero-filled in bounded pieces, and `flush()` fills any tail the caller skipped.
- Writes are pushed to the wire at once, not batched: a small disk that fit under one batch sent nothing until
  `flush()`, and the idle connection was reset.
- The PUT is authenticated with the SOAP session's cookie, so that session must outlive the upload; the
  `keepAlive` handle in `VSphereFolderPutPrepResult` holds it. Without it the PUT failed with a bare HTTP 401.
- The attached disk is reported as eager-zeroed thick, which is what a fully written flat file is.

## 8. Placement by name

`resolvePlacement()` reads datastores, compute resources (clusters and standalone hosts alike), and, when named,
resource pools and VM folders, each with one ContainerView read. The compute resource has to have the datastore
mounted; without a name the only such one is taken, and more than one is an error listing them. The default pool
is the compute resource's root pool, the default folder the root VM folder of the datastore's datacenter. The same
reads work on a standalone ESXi host, where those objects are the fixed `ha-*` ones.

## 9. Schema strictness found by live testing

vCenter validates requests against the VIM schema strictly, and each of these was a fault before it was a fix:

- An `xsi:type` attribute needs `xmlns:xsi` declared on the request, and declared once: `ImportVApp` and the
  reconfig builders each declare it on their own operation node, since a second declaration is a parser error.
- `VirtualDevice` children are an ordered sequence: `key`, `backing`, `controllerKey`, `unitNumber`, then
  `capacityInKB`. Out of order is "Unexpected element tag".
- Removing a disk still needs a complete `VirtualDisk`, including `capacityInKB` and the backing's `diskMode`.
- `sharedBus` exists only on SCSI controllers; sending it for a SATA (AHCI) controller is rejected.

The vSphere Automation REST API does not fully understand a disk attached through SOAP: it reports no capacity
for it, and `DELETE /api/vcenter/vm/{vm}` refuses the VM until that disk is removed first.

## 10. Open questions

- Whether the OVF's generic `ResourceSubType="VirtualSCSI"` yields a paravirtual controller, matching the
  `ddb.adapterType = "paravirtual"` the descriptor helper writes by default.
- `MoveDatastoreFile_Task` across datastores (a real copy) has not been tried.
- Hot-add onto a powered-on VM has not been tried.
- No minimum vCenter or ESXi version has been established beyond the `vmx-07` CBT floor.
