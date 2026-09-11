# Native D3D12 descriptor checkpoint

The pinned vkd3d-proton engine now translates native WDK descriptor heap,
buffer UAV/CBV/SRV, simple descriptor copy, heap binding and compute root table/CBV/SRV
callbacks into its embedded backend. A copied staging descriptor can feed a
compute table at nonzero heap and range offsets. Validation rejects foreign
devices, invisible heaps, out-of-range or misaligned handles and stale bindings
after command-list reset. DDI shader-visible bit 2 maps explicitly to API bit 1.

Engine source: `b2c510d2dd58e93cb3d53cec9a484187230b52d9`.
Standalone native CI: `sunflower2333/vkd3d-proton/actions/runs/34603845851`,
Windows architecture jobs in progress; the CPU Vulkan job passed. Linux CPU
Vulkan locally passes two UAV, four CBV and eight SRV workloads with
1024-word readback each (14336 words total). CBVs verify buffer/heap offsets,
copied and null table descriptors, root rebinding after reset and rejected zero root addresses.
Root CBVs require a live owned buffer; they cannot assume descriptor-table
null-read behavior. Actual WDK callback
fixtures execute on x86/x64; ARM64 compiles and packages the same fixtures.
This evidence does not establish target GPU or Windows runtime acceptance.

SRVs cover raw, structured and R32 typed buffer views, bounded ranges, copied
and null table descriptors, and owned root addresses. Unknown non-null native
resource handles are rejected before dereference. Non-default buffer component
mappings return E_NOTIMPL because the embedded engine currently implements
swizzles only for textures. This checkpoint also clears auxiliary buffer-range
metadata when a live descriptor is replaced by null, including copied null
descriptors; shader GetDimensions and readback verify that the old buffer size
cannot survive slot reuse.

The package also includes `vkd3d-umd-gpu-probe`, which calls the production
backend with an explicit Windows adapter LUID and matching Vulkan vendor and
device IDs. It independently requires that identity and the Turnip driver ID;
the CPU test-device entrypoint is absent. All compute/readback paths execute
on the selected device when invoked. Missing identity returns2 before loading
Vulkan. The previous09146e1 checkpoint passed all four standalone CI jobs,
all seven paired jobs at parent674154c, and two real ARM64 Turnip compute
readbacks in1005ms. The84d6bba CBV checkpoint also passed the ARM64 target in806ms
with6144 correct GPU readbacks. The new SRV target test remains pending. Neither backend
checkpoint establishes native runtime DDI or presentation support.

The parent workflow repeats backend and three-architecture bridge validation,
builds the paired ARM64 KMD/Mesa package, signs the candidate DLLs and tools,
and records the engine and parent identities. Existing Mesa desktop
registration is retained. This checkpoint does not register a D3D12 UMD.

Remaining native runtime work includes complete SRV mappings, sampler/texture descriptors,
ranged copies and graphics, runtime allocation/GPUVA/residency, monitored
fences, OpenAdapter12 integration, presentation and reset recovery. These
remain prerequisites for claiming application-compatible native Direct3D12.
