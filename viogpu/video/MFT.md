# Application-local Media Foundation decoder

`viogpuvideo_mft.dll` adds a synchronous `IMFTransform` frontend to the existing
virtio-media companion. Call `VioGpuCreateVideoDecoder(index, &transform)` after
`MFStartup`. The index is the enumerated media interface, not a GPU adapter.
The DLL uses the real companion API; it contains no software decoder or
automatic fallback to a system transform.

The module is intentionally application-local. It is not a registered system
MFT, DXVA/D3D11 Video DDI, asynchronous hardware MFT, or D3D texture frontend.
Creating it does not make Edge or arbitrary players use the VPU. These broader
integration steps remain unimplemented and require their own validation.

## Supported contract

- One stream, progressive H.264 Annex-B access units to tightly packed NV12
  `IMFSample` buffers. Each input sample must contain one complete access unit
  and a timestamp. Optional `MF_MT_MPEG_SEQUENCE_HEADER` must also be Annex-B.
  Length-prefixed AVC and interlaced types are rejected.
- The requested media function must enumerate H.264 on its OUTPUT queue.
  `S_FMT` codec substitution fails. Actual decoder readiness is checked when
  streaming begins, not by the factory's ability to allocate a COM object.
- SOURCE_CHANGE is translated to `MF_E_TRANSFORM_STREAM_CHANGE`. The caller
  must obtain and set the new output type. The negotiated coded width, height,
  NV12 plane count, stride and allocation bounds are checked. CAPTURE uses the
  host's current `MIN_BUFFERS_FOR_CAPTURE` plus one, within the 32-buffer limit.
- Input storage remains busy until OUTPUT DQBUF. A returned CAPTURE stays owned
  by the transform until its copy into an MF sample succeeds; only then is it
  requeued. Padded host rows are copied into the advertised tight NV12 layout.
- Dynamic resolution drains the old CAPTURE generation through LAST before
  stopping/releasing/reallocating that queue. OUTPUT remains alive.
- `COMMAND_DRAIN` sends `DECODER_CMD_STOP`, then waits for CAPTURE LAST and all
  OUTPUT returns. An event timeout never becomes successful end-of-stream.
  No-progress waits fail after 15 seconds in `ProcessOutput`. The acceptance
  client also enforces a 30-second progress deadline.
- After a completed drain, a new discontinuity input sample opens a fresh
  session. FLUSH and END_STREAMING close/revoke the current generation before
  accepting new input. Failed close is reported. No device reset is hidden.
- PTS travels through the transport in integer microseconds; lower 100-ns
  digits are truncated. Frame duration uses the input frame-rate attribute
  when provided. First/discontinuous output is marked discontinuous.

No protected content, HDR/P010, GPU zero-copy, asynchronous MFT, x64/x86
package, or per-function multi-client support is claimed. Buffer contents
travel through CPU copies, while decode itself is performed by the host codec.
No source behavior here proves which hardware codec the host selected.

## Backend and memory prerequisites

Reviewed references are the isolated 2026-09-20 VPU merge:

- crosvm `28f60f9a1022a822bc4491827862dfb2dc37cf3d`, containing upstream
  `f972aa0a` and Android codec backend;
- virtio-media `02eb1757bdfff29838e7a248556a086ec03c7b0f`, containing upstream
  `a4caa0db`, with USERPTR on both decoder queues;
- v4l2r `28ec73eb9f5d80d41761cd1894e183963438ca4d`.

The current Windows companion allocates ordinary guest RAM. Use only a host
configuration where those GPAs are accessible. This does not implement
protected-VM media-pool discovery or allocation. The host still requires its
configured media_host pool. The old 4-GiB-BAR deployment is not a substitute
for the latest pool-backed backend. Reconcile host GPU/HDR changes before
deploying a new crosvm; this branch never changes a VM or deployment script.

## Build and acceptance

`build-mft.ps1` uses a native ARM64 Visual Studio shell and produces a flat
review directory containing `viogpuvideo_mft.dll` and `mft-probe.exe`.
The dedicated MFT workflow builds those files and runs actual `IMFTransform`
methods against a deterministic device API fixture. The existing video CI
separately compiles the companion and integrated UMD with WDK. These review
artifacts are unsigned.

`viogpu-video-signed-ci.yml` separately builds and signs the installable ARM64
VPU package with the existing fixed DroidVM certificate. No display binary or
binding is replaced. The independent INF version is `0.2.0.1` and its sole
hardware ID is `PCI\VEN_1AF4&DEV_1070`. Its driver artifact contains exactly five
flat files: `viogpuvideo.inf`, `viogpuvideo.sys`, `viogpuvideo_mft.dll`,
`viogpuvideo-package.json` and `viogpuvideo.cat`. The catalog covers the other
four files; the authenticated manifest records source/parent and final signed
binary SHA-256 values. The SYS and DLL also have embedded signatures.

The tools artifact separately contains signed verification/installation/test
scripts and a signed probe, documentation and the public certificate. It
never contains private key material. The fixture is a third artifact. Package
CI executes the production catalog verifier and requires corruption of code
or inventory, missing DLL, extra file and nested-directory cases to fail for
their intended reasons. Signing/CI does not establish target kernel acceptance.

Linux tests compile the production decoder state machine with ASan/UBSan.
Six semantic mutations must be rejected: duplicate input return, truncated
NV12, codec substitution, early CAPTURE requeue, timeout treated as EOS, and
hidden close failure. A mocked transform test is not a hardware decode test.

The workflow also generates a 12-frame 320x240 H.264 clip and decodes it using
FFmpeg to derive an independent exact NV12 SHA-256. Use the fixture and its
manifest from the same CI run; encoder versions can produce different clips.
Never copy a reference hash from a different fixture.

Once the primary deployment owner has verified the backend, upload the three
artifacts local-to-guest and run from native elevated ARM64 PowerShell. The
existing fixed certificate must already be trusted; these scripts never
import certificates, change signing policy, reboot or change VM configuration.

```powershell
C:\DroidVMTests\vpu-tools\verify-video-package.ps1 `
    -PackageDirectory C:\DroidVMTests\vpu-driver
C:\DroidVMTests\vpu-tools\install-video-package.ps1 `
    -PackageDirectory C:\DroidVMTests\vpu-driver
```

The installer requires a present media PCI function before calling `pnputil`.
Check that the device starts successfully; a staged package does not prove
binding. The INF puts the SYS and MFT side-by-side in its Driver Store directory,
but does not register the MFT. For the explicit application-local decode test:

```powershell
C:\DroidVMTests\vpu-tools\test-mft-device.ps1 `
    -PackageDirectory C:\DroidVMTests\vpu-driver `
    -ProbeDirectory C:\DroidVMTests\vpu-tools `
    -FixtureDirectory C:\DroidVMTests\vpu-fixture -DecoderIndex 0
```

This verifies the signed package and tools, stages just the MFT and probe
together in the test-output directory for DLL resolution, then runs two
independent short sessions requiring exactly 12 frames and the reference
SHA-256 each. It does not install, configure, reboot or register anything.
Capture Android's actual codec name with `(hardware, vendor) started`, paired
with the same decoder session's `frames out` on EOS/stop, for the same interval.
Only both guest pixel evidence and matching host codec evidence can establish
hardware decoding. Keep logs/output remote. The older unsigned review artifact
can still use `PackageDirectory` alone, but that is not signed installation.

Further target acceptance remains: resolution changes, tail-drain latency,
seek/flush, malformed streams, helper loss, process exit and reset. Standard
application auto-discovery, async registration, DXVA/D3D video integration,
GPU texture/fence sharing and integration into the main display-driver build
are subsequent work.

References: Microsoft [Basic MFT Processing Model](https://learn.microsoft.com/windows/win32/medfound/basic-mft-processing-model)
and [MFT_MESSAGE_COMMAND_DRAIN](https://learn.microsoft.com/windows/win32/medfound/mft-message-command-drain).
