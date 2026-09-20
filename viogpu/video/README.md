# VioGPU video: explicit MediaCodec transport (experimental)

2026-09-20: an application-local synchronous Media Foundation decoder now wraps
this transport; see [MFT.md](MFT.md) for its exact contract and acceptance.
It does not register a system decoder or implement DXVA. The historical notes
below describe the original explicit transport delivery.

Base: `perf/droidvm-gpu-20260914`, commit
`5ab27d4055887f14e97195079ef51716b7b707fb`.
Development branch: `work/vpu-video-umd-perf-20260915`.
PR #3 was closed without merging at the owner's request. Development and
verification continue directly on this independent branch; no open PR is
required. Branch pushes run video, full-WDDM, performance and repository
format checks. CI success is not a hardware acceptance result.

The initially-created `work/vpu-video-perf-20260915` received concurrent
commits containing a different transport ABI. Those commits were not
force-overwritten. This UMD-integrated candidate stays on its own branch
with the same perf base. Do not blindly merge or install the two same-service
implementations together.

## What this change implements

`viogpud3d.dll` now builds and exports `VioGpuVideoOpen`, `Control`, `Allocate`,
`Queue`, `Dequeue`, `Copy`, `Stream` and `Close`. The existing OpenAdapter exports
and rendering capability policy are not changed. `video_api.h` is the explicit
client API; `codec_probe.cpp` demonstrates a full H.264 encode/decode queue
loop, including buffer return, format negotiation and drain.

`viogpuvideo.sys` is the VioGPU package's KMDF media companion. It binds to the
modern virtio-media PCI function (`PCI\VEN_1AF4&DEV_1070`), **not** the GPU
function. The upstream GPU and media queues are different devices. Encoding
and decoding use separate media functions, each with one exclusive client in
this initial implementation. The driver supports the upstream fixed-size
codec control subset, driver-owned USERPTR queues, event delivery, capture
copyout, streaming, and bounded teardown.

This is an explicit video backend, **not a registered Media Foundation MFT,
DXVA implementation, D3D11 Video DDI, or D3D12 Video interface**. The exported
API must be called by an adapted client. Installing it does not make arbitrary
players/browsers discover hardware decoding automatically. No video-engine,
codec-profile, D3D-aware or zero-copy capability is advertised speculatively.

## Memory and host requirements

There is no dependency on `rdmapool.sys`, its service, headers or libraries.
Input and output buffers are driver-owned ordinary contiguous guest RAM.
USERPTR packets carry a kernel-derived GPA, never a user-supplied pointer or
WDF DMA logical address. Buffer starts are 64 KiB aligned for the host mapping
path. Failure to allocate ordinary RAM is an explicit failure, not a hidden
fallback to a restricted pool. Maximums are 32 buffers per queue, 32 MiB per
buffer and 512 MiB total per media function, including alignment overhead.

Only use this candidate with a VM whose ordinary guest RAM is host-accessible
(the perf branch's unprotected deployment). This is **not** a protected-VM
buffer-sharing implementation. The guest currently cannot independently prove
that the host has made every ordinary GPA accessible; loopback/host access
validation is an acceptance gate before codec testing.

Compatible reviewed host revisions (keep the three revisions together):

- `Droid-VM/crosvm`: `bfccd3d5a7abc7a8d2c0fd1b2ab5bee119321b75`
- `Droid-VM/virtio-media`: `6b6d2b3307ce75ed35b0ab5b4703d9d1bb830cf8`
- `Droid-VM/v4l2r`: `7eb3afa6c4ff7d795394ad09dece6099b4e6a38e`

The current Android media helper still requires its host-side `media_host`
allocation. Keep that configured. It is **not** a Windows rdmapool dependency;
Windows does not map or allocate from that pool in this implementation.
Attach `kind=decoder` and `kind=encoder` with the appropriate App UID. Do not
send media commands to the GPU queue or use the removed virtio-media-adapter.

## Ownership and error contract

A QBUF command acknowledgement is not buffer completion. RAM stays retained
until DQBUF, successful queue release/CLOSE, or a transport reset that ends
backend access. Command timeout poisons the transport and quarantines all
possibly accessed allocations until reset. Ordinary per-session close does
not restart a wedged device behind the client's back.

Event descriptors are replenished independently of userspace consumption.
The bounded local event ring fails the session explicitly on overflow; it
never reports a dropped event as success. Malformed sizes, queue identifiers,
state transitions, duplicate completions, timestamps, generation mismatches
and data ranges are rejected. A polling timeout returns `S_FALSE`, not EOS.

One client must serialize calls on a session. Do not race Close with any other
call. A successful fixed control may still contain a nonzero `LinuxErrno`;
the acceptance client checks both transport HRESULT and codec errno.

The initial frame format is one-plane NV12 transported through the multi-planar
V4L2 ABI. No P010, protected content, HDR, GPU texture sharing or zero-copy
claim is made. The fixed-format whitelist does not expose arbitrary pointer
ioctls. Codec support must be probed/negotiated with the host; the H.264 sample
is not a hardware capability declaration.

## Build and package

From a WDK-enabled ARM64 developer shell, at the repository root, pass the
installed SDK/WDK version (10.0.26100.0 is the version used in the first CI run):

```powershell
./viogpu/video/build-video.ps1 -KitVersion 10.0.26100.0
cl /nologo /std:c++17 /EHsc /W4 /WX viogpu\tests\video\codec_probe.cpp `
    viogpu\viogpuwddm\objfre_win11_arm64\arm64\viogpud3d.lib `
    /Fe:video-codec-test.exe
```

The repository's `locate-windows-kit.ps1` writes to `GITHUB_ENV` and is an
Actions-only helper; do not invoke it directly in an ordinary local shell.

The dedicated GitHub workflow runs production wire/state tests and a real
ARM64 WDK build. Its artifact is an **unsigned review candidate**, not a signed
release. Do not install it on an ordinary machine expecting signing to work.
`install-video.ps1` accepts an already appropriately signed media package and
uses pnputil. It does not change signing/security policy, rdmapool, the GPU
INF, the display driver, or the existing system UMD. For the explicit test,
place the newly built `viogpud3d.dll` next to `video-codec-test.exe`.

## Hardware acceptance

The index is the enumerated media interface, not a GPU index or fixed PCI slot.
Run the matching command against the decoder or encoder function:

```text
video-codec-test encode 1 1280 720 30 input.nv12 encoded.h264
video-codec-test decode 0 1280 720 30 input-with-aud.h264 decoded.nv12
```

Numbers 0 and 1 are examples, not guaranteed discovery order. The encoder input
must be tight 8-bit NV12 with even dimensions. The decoder input must be Annex B
H.264 with AUD NAL delimiters, and is bounded to 128 MiB in this test utility.
The utility synthesizes PTS at the specified rate: it does **not** validate
container demux, DTS/PTS reordering or a Media Foundation sample pipeline.
It fails after 30 seconds without progress instead of accepting a hung drain.

Validate encoded output using an independent decoder and frame count; compare
decoded output with a reference using appropriate codec tolerances. Nonempty
output buffer count can include separate codec headers, so it is not itself
a decoded-frame count. DRC raw output can concatenate differing frame sizes;
use logged formats to interpret it. Confirm on Android that a hardware codec
was selected and no software fallback was enabled.

Mandatory device gates: cold helper first session, short streams/tail drain,
seek via close/reopen, capture starvation, dynamic resolution, duplicate and
malformed events, low RAM/fragmentation, process termination, D0/reset and
helper failure. These are hardware acceptance requirements, not claims that
this development environment ran them.

## Validation boundary / remaining integration

Local portable tests compile the **same** helpers used by the driver under
ASan/UBSan, and compare layouts to Linux V4L2 headers. A C11 inclusion check and
MSBuild XML parse were also run. Initial PR CI passed the portable tests and
compiled the modified ARM64 UMD with zero warnings/errors. The initial media
driver build stopped at INF OS-version validation (1199); the model decoration
has been corrected to 10.0.16299 or newer, matching DIRID 13 requirements.
Consult the current CI results for the driver build; no hardware execution
has been validated by these tests.

Remaining for transparent Windows application support: real D3D11 Video DDI /
DXVA decode integration, an asynchronous encoder/decoder MFT frontend,
GPU texture/fence interoperation, richer profile/format negotiation,
multi-client sessions, and end-to-end device performance/quality validation.
No placeholder implementation has been published as a working capability.
