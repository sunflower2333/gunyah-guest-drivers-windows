# WDDM 2.0 physical engine and Task Manager accounting

This candidate implements the WDDM 2.0 contracts needed to expose the existing
VIOGPU scheduler and system-memory allocations to Windows VidSch/VidMm. It has
not been installed on the device. Task Manager visibility, changing utilization
and memory counters, and stable Full Display+Render are still runtime gates.

## Source identity and isolation

- Independent shallow clone:
  `/home/sunf/droidvm-repos/reference/codes/viogpu-wddm2-20260911`.
- Branch: `viogpu-wddm2-taskmanager-20260911`.
- Base KMD: `7648b72f0a0e8ec3699544508817f805de5f262c` (58373).
- Candidate KMD code: `d63390fdc8033862ca56e9b9b09de88f51b6848f`.
- Final CI/package source: `41ade6e6ad6af7ae33eb13605e5fd68761a51af6` (adds
  this report and the regression test path to CI triggers).
- Paired Mesa: `4ace9df987ea3c08469488618080498af4c7300d`, unchanged.
- KMD origin: `https://github.com/sunflower2333/gunyah-guest-drivers-windows.git`.
- Both KMD and nested Mesa use separate shallow object stores, without a
  worktree, alternates, or hardlinks to the original active checkouts.
- Planning files are isolated under `.planning/wddm2-taskmanager/`.

## Why physical-mode WDDM 2.0 fits this device

The current guest accepts node 0, engine 0 and affinity 1. Graphics, compute and
transfer reach the same native-context submission path. Host Turnip/virgl owns
the real GPU address space; the guest KMD does not implement GPU page tables.

Microsoft explicitly permits physical-mode engines in WDDM 2.0. These retain
the WDDM 1.x allocation-list, patch-list and scheduling model. GpuMmu and IoMmu
require opt-in. Therefore this implementation reports exactly one 3D engine,
with guest GpuMmu, IoMmu and virtual addressing disabled. It does not invent
separate copy/compute engines, hardware queues, dedicated VRAM, or host GPU
sensor telemetry.

Task Manager uses VidSch and VidMm data and requires WDDM 2.0 or newer. Its
engine utilization is scheduler activity, not a direct sample of the physical
Adreno utilization sensor. Matched host KGSL measurements remain necessary to
assess actual hardware efficiency. This change does not by itself remove any
present, copy, queue or CPU bottleneck.

## Implemented contracts

| Contract | Result |
| --- | --- |
| Registration and capability version | Project declarations and initialization table use `DXGKDDI_INTERFACE_VERSION_WDDM2_0`; native `WDDMVersion` uses `DXGKDDI_WDDMv2`. Legacy DOD retains its existing version. |
| Node metadata | Registered global `DxgkDdiGetNodeMetadata` validates combined node/physical-adapter ordinal 0, exposes one `DXGK_ENGINE_TYPE_3D`, clears unsupported MMU flags. |
| Adapter topology | Existing public `DriverCaps.GpuEngineTopology` reports one execution node; the public metadata DDI describes that node. No system-reserved physical-adapter query handler is added. |
| Segment enumeration | `QUERYSEGMENT4` reports one CPU-visible, cache-coherent aperture backed by guest system RAM. It validates physical adapter 0, buffer sizes, readiness, descriptor count and stride. |
| Count-only query | With `NbSegment==0`, modifies only `NbSegment`; does not inspect a potentially poisoned descriptor pointer or other output members. |
| Descriptor ABI | Writes only the known descriptor extent, preserving a larger caller-provided descriptor tail and stride. |
| Allocation ABI | Uses `FlagsWddm2` and `PhysicalAdapterIndex=0`; sets `AccessedPhysically=1` so VidMm maps allocations referenced by physical allocation/patch lists. CPU-visible/cache flags retain their existing policy. |
| Paging flags | Removes old `Flags.SynchronousPaging`, whose bit is reserved in the WDDM2 flag union. Existing paging transactions, BO identity, maps, fences and reset ownership remain the production path. |
| Memory accounting | Existing allocations remain VidMm-managed guest RAM; the aperture address extent is not dedicated VRAM capacity. Windows owns shared-memory commit accounting and budgets. |

`CreateProcess`/`DestroyProcess`, `SubmitCommandVirtual` and page-table DDIs are
not added. Microsoft's official compute sample conditionally registers these
only under `COS_GPUVA_SUPPORT`, while its physical path registers
Render/Patch/SubmitCommand. Current guest resource ownership remains tied to
the existing device/context/allocation lifetimes. This avoids introducing fake
process or GPUVA objects solely to claim a newer version.

The initial draft inferred a `PHYSICALADAPTERCAPS` query implementation from
the public output structure. Enum-level review found that Microsoft explicitly
marks `DXGKQAITYPE_PHYSICALADAPTERCAPS` as "Reserved for system use. Do not use
in your driver." The inferred UINT adapter-index input was not a documented
contract. That handler and its corresponding mock test were removed after
review; public DriverCaps/GetNodeMetadata/QUERYSEGMENT4 remain authoritative.

The inherited numeric optional performance-data query handlers 24/25 remain
unchanged. They are not a physical GPU utilization implementation and must not
be used as evidence that host sensors or Task Manager accounting work.

## Microsoft evidence

Read both the workspace documentation and the official online documentation.
Workspace documentation roots:

- `/home/sunf/droidvm-repos/windows-driver-docs/windows-driver-docs-pr/display/`
- `/home/sunf/droidvm-repos/reference/codes/windows-driver-docs-ddi/wdk-ddi-src/content/`

| Evidence | Local source | Official online source |
| --- | --- | --- |
| Task Manager requires WDDM2 and uses VidSch/VidMm | Online article | [GPUs in the Task Manager](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/) |
| Physical versus virtual engines; MMU opt-in | `display/gpu-virtual-memory-in-wddm-2-0.md` | [GPU virtual memory in WDDM 2.0](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0) |
| One aperture; physical allocation residency | `display/gpu-segments.md` | [GPU segments](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-segments) |
| Scheduler node meaning | `display/enumerating-gpu-nodes.md` | [Enumerating GPU nodes](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/enumerating-gpu-nodes) |
| Allocation union and flags | `d3dkmddi/ns-d3dkmddi-_dxgk_allocationinfo.md`, `ns-d3dkmddi-_dxgk_allocationinfoflags_wddm2_0.md` | [DXGK_ALLOCATIONINFO](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_allocationinfo), [FlagsWddm2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_allocationinfoflags_wddm2_0) |
| Count query and descriptor stride | `d3dkmddi/ns-d3dkmddi-_dxgk_querysegmentout4.md`, `ns-d3dkmddi-_dxgk_querysegmentin4.md`, `ns-d3dkmddi-_dxgk_segmentdescriptor4.md` | [QUERYSEGMENTOUT4](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_querysegmentout4), [QUERYSEGMENTIN4](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_querysegmentin4), [SEGMENTDESCRIPTOR4](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_segmentdescriptor4) |
| System-reserved query restriction | `d3dkmddi/ne-d3dkmddi-_dxgk_queryadapterinfotype.md` | [QUERYADAPTERINFOTYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ne-d3dkmddi-_dxgk_queryadapterinfotype) |
| Mandatory node metadata DDI and flags | `d3dkmddi/nc-d3dkmddi-dxgkddi_getnodemetadata.md`, `d3dkmdt/ns-d3dkmdt-_dxgk_nodemetadata.md` | [GetNodeMetadata](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_getnodemetadata), [NODEMETADATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_nodemetadata) |

Official code comparison, inspected at commit
`de4a2161991eda254013da6c18226f5ea06e4a9c`:
[CosKmdGlobal.cpp](https://github.com/microsoft/graphics-driver-samples/blob/de4a2161991eda254013da6c18226f5ea06e4a9c/compute-only-sample/coskmd/CosKmdGlobal.cpp)
and
[CosKmdAdapter.cpp](https://github.com/microsoft/graphics-driver-samples/blob/de4a2161991eda254013da6c18226f5ea06e4a9c/compute-only-sample/coskmd/CosKmdAdapter.cpp).
The sample corroborates physical/GPUVA DDI selection; it is not a runtime
validation of this VIOGPU implementation.

## Validation status

- Local production-function harness: 17 cases PASS, including poisoned count
  queries, extended descriptor stride/tail, invalid ordinals/buffers, reset
  readiness, exact node metadata, and physical CPU-visible/GPU-only
  allocation policy. The harness extracts the implementation functions;
  mock WDK types are not a substitute for actual WDK compilation.
- Local full-miniport contract checker: PASS at final `41ade6e`.
- `git diff --check`: PASS.
- ARM64 WDK workflow [34579582312](https://github.com/sunflower2333/gunyah-guest-drivers-windows/actions/runs/34579582312): PASS at final `41ade6e`, including the 17 production-function tests, inherited regressions, full-miniport and guest-driver ARM64 compilation/link checks.
- Paired Mesa/KMD signed product workflow [34579585440](https://github.com/sunflower2333/gunyah-guest-drivers-windows/actions/runs/34579585440): PASS at `41ade6e`, including Mesa, Zink recovery regression, KMD build, package validation and signing.
- Downloaded paired package: `.artifacts/paired-34579585440/`, INF version
  `100.6.101.58378`; KMD and UMD are both ARM64 PE images.
- Product artifact ID `10191403919`, GitHub archive digest
  `sha256:4baf1f2f62bb38f4e95c2efb13059bceb1a404556b636bdf502d80c40bafada8`.
- Downloaded KMD SHA256:
  `4263a59e7671639825346076804bbb40b4bbb4c34a5b7fce3995cf4e5b56bee8`.
- Downloaded paired Mesa UMD SHA256:
  `855eb2cbf8b7262d06cd853eb34c8c24981f41d8cd422409a0f30799978932d2`.
- Repository-wide clang-format CI reports inherited formatting debt. Only
  changed lines/new test code were formatted; no mass reformat was made.

Earlier CI caught and fixed host-test SAL/anonymous-struct warnings and a
callback placed inside an anonymous namespace. Those failed attempts did not
produce an accepted installable candidate.

## Runtime acceptance, pending a coordinated device window

Do not install or restart while the main thread owns its GB7 performance
window. Deployment must use the complete paired, signed package and retain
the existing Full VIOGPU Display+Render VM.

1. Record the package/source identity, loaded KMD/UMD/ICD hashes, existing
   adapter LUID and PnP status. Confirm dxdiag reports WDDM 2.0 and the real
   VIOGPU adapter while display and hardware Direct3D still initialize.
2. Enumerate the VIOGPU adapter and its one 3D node in Task Manager or KMT.
   Verify no invented dedicated VRAM or extra independent execution engines.
3. Capture matched idle, known GPU workload and post-workload samples from
   `GPU Engine`, `GPU Adapter Memory` and `GPU Process Memory`. On localized
   Windows, CIM classes avoid translated performance counter path names:
   `Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine`,
   `Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory`, and
   `Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory`.
   Match instance LUID and PID to the selected adapter/workload. Preserve
   missing-class/query errors instead of converting them to zero utilization.
4. Require engine activity to change during completed work and settle after
   it, with plausible committed/shared memory and per-process attribution.
   Verify allocation destruction releases accounting. A busy counter that
   never clears or inflated accounting after workload exit is a failure.
5. Capture matching host KGSL/CPU and guest queue/fence timings. A VidSch busy
   interval can include host queue delays and must not be equated with actual
   physical GPU saturation.
6. Require stable visible desktop, Explorer operations and hardware DirectX/
   Vulkan output; no new DWM/Explorer restart, render corruption, hang, TDR or
   bugcheck. Existing full-goal application/stress checks remain required.

Version text or a visible GPU tile alone does not satisfy this acceptance.
The source/CI candidate must not be described as a completed Task Manager or
Full VIOGPU runtime result until these checks have passed.

## Runnable acceptance tools

`viogpu/tools/wddm-accounting/` now contains a native ARM64 KMT reader and a
three-phase PowerShell collector. See its README for exact invocation, output
schema and scope. Public KMT identity/node/segment-size queries and raw Windows
GPU performance providers are the default path. An explicit diagnostic-only
switch enables the system-reserved D3DKMT_QUERYSTATISTICS API for raw node and
segment details; it never changes the KMD contract and failures remain visible.

The standalone `wddm-accounting-tool.yml` workflow compiles/packages the tool
and runs 10 delta boundary cases without rebuilding or changing the validated
41ade6e paired driver package. Native tool runtime and actual Task Manager UI
acceptance on the device still await the coordinated window.

Tool CI [34581281440](https://github.com/sunflower2333/gunyah-guest-drivers-windows/actions/runs/34581281440)
PASS at `31dd593c5bf1356be6572fece97f56734bea3c49`: real ARM64 compile/link,
PE machine, executable startup, rejected invalid PID, script parse and 10
delta cases. Downloaded to `.artifacts/accounting-34581281440/`.

- Artifact ID `10191714170`, GitHub archive SHA256
  `5dd80754967c14589407238d2dfd743a8334b52acffcc818f8074e16fbd41e76`.
- Native executable SHA256
  `c337a067b48a9f4db3ee32231768c4d912e79c8bba80d36aa8df9a274a9f5620`.
- Collector SHA256
  `3570d52fd55cc49551b9bdbfe3dc8d7bf1e8c5556605f15f1e91e6a79a5cd50a`.

The package and tool are concrete reviewable candidates, not device acceptance
results. No remote operations were performed by this implementation subtask.
