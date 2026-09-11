# Native D3D12 descriptor checkpoint

The pinned vkd3d-proton engine now translates native WDK descriptor heap,
buffer UAV/CBV, simple descriptor copy, heap binding and compute root table/CBV
callbacks into its embedded backend. A copied staging descriptor can feed a
compute table at nonzero heap and range offsets. Validation rejects foreign
devices, invisible heaps, out-of-range or misaligned handles and stale bindings
after command-list reset. DDI shader-visible bit 2 maps explicitly to API bit 1.

Engine source: `84d6bba6742c14e2e99fb6981e7962e78ba604c7`.
Standalone native CI: `sunflower2333/vkd3d-proton/actions/runs/34598879880`,
in progress. Linux CPU Vulkan locally passes two UAV and four CBV workloads
with 1024-word readback each. CBVs verify buffer/heap offsets, copied and null
table descriptors, root rebinding after reset and rejected zero root addresses.
Root CBVs require a live owned buffer; they cannot assume descriptor-table
null-read behavior. Actual WDK callback
fixtures execute on x86/x64; ARM64 compiles and packages the same fixtures.
This evidence does not establish target GPU or Windows runtime acceptance.

The package also includes `vkd3d-umd-gpu-probe`, which calls the production
backend with an explicit Windows adapter LUID and matching Vulkan vendor and
device IDs. It independently requires that identity and the Turnip driver ID;
the CPU test-device entrypoint is absent. All compute/readback paths execute
on the selected device when invoked. Missing identity returns2 before loading
Vulkan. The previous09146e1 checkpoint passed all four standalone CI jobs,
all seven paired jobs at parent674154c, and two real ARM64 Turnip compute
readbacks in1005ms. The new CBV target test remains pending. Neither backend
checkpoint establishes native runtime DDI or presentation support.

The parent workflow repeats backend and three-architecture bridge validation,
builds the paired ARM64 KMD/Mesa package, signs the candidate DLLs and tools,
and records the engine and parent identities. Existing Mesa desktop
registration is retained. This checkpoint does not register a D3D12 UMD.

Remaining native runtime work includes SRV/sampler/texture descriptors,
ranged copies and graphics, runtime allocation/GPUVA/residency, monitored
fences, OpenAdapter12 integration, presentation and reset recovery. These
remain prerequisites for claiming application-compatible native Direct3D12.
