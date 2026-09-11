# Windows primary guest backing

This candidate binds standard WDDM primary backing with RESOURCE_CREATE_BLOB
(GUEST, context zero) and SET_SCANOUT_BLOB. Scheduled Present writes already
land in the guest primary; the blob path flushes those pages without a
TRANSFER_TO_HOST_2D upload. It preserves the existing native render resources,
Mesa/GL/OpenCL package pins, and resource reset/unknown ownership rules.

Activation is DWORD `GuestBlobScanout=1` in the selected display driver class
key (opened with `IoOpenDeviceRegistryKey(..., PLUGPLAY_REGKEY_DRIVER, ...)`),
on a Full Display+Render registration. It defaults off until the matching host is
installed and the paired Windows test is performed. The host prerequisite is
crosvm9f84242 with Virglc30846b; its actual Android guest-memory/DMA-BUF alias
and fence regressions passed before this Windows integration. A generic blob
feature bit alone does not prove that older hosts implement this display path.

Only standard primaries with the host-supported BGRA/BGRX/RGBA formats select
the new backing. Other standard resources retain 2D backing. Creation copies
the SG list into the queue-owned packet and validates that it covers the full
backing. Blob layout checks reject stride, size and arithmetic violations.
Blob resources are distinct in the lifecycle state, including cleanup/reset;
unknown ownership still retains the allocation pending authoritative retirement.

The background refresh takes one locked snapshot of resource ID, dimensions
and blob kind, so it cannot send a 2D transfer to a blob because of a mixed
scanout update. The active binding changes only after confirmed SET_SCANOUT.
An initially black guest primary is still bound: subsequent guest writes and
flushes must reach it. The old published-frame heuristic is retained for the
legacy resource path only.

Local checks cover 15 layout boundaries and actual production command builders,
protocol declarations and response classification. The packet fixture checks
fixed protocol bytes, copied SG metadata, allocation/queue rejection, malformed
response poisoning and outstanding packet retention after timeout/reset races.
Transport wait results are injected; this fixture does not simulate the kernel
or establish host/guest rendering correctness. Negative controls must reject
an unsafe timeout release, permissive response acceptance and a wrong opcode.
The existing fragmented backing, Render, power and fault regressions remain.
Actual WDK compilation and paired signed
package checks must pass before installation. The version epoch reserves
58451 onward after the independently frozen58450 candidate.

Required device validation: exact paired host/KMD/UMD hashes; healthy cold boot;
VIOGPU display ownership and stable DWM/Explorer; initial black and normal
frames, desktop/taskbar/cursor, resize and refresh changes; native Present and
existing graphics/share tests; bounded application/stress and recovery tests.
Confirm host logs use guest blobs/SET_SCANOUT_BLOB and no 2D transfers for
those primary IDs. Record CPU/GPU timing and the actual rendering result.

This reduces the old host upload/readback stage. Guest-side primary copies and
the host Vulkan blit still exist. Full zero copy additionally requires shared
guest/host AHB allocation identity/layout and integrated consumer-release
ownership; neither the switch nor standalone tests establish full acceptance.
