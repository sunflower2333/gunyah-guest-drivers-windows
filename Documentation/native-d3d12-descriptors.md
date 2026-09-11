# Native D3D12 descriptor checkpoint

The pinned vkd3d-proton engine now translates native WDK descriptor heap,
buffer UAV, simple descriptor copy, heap binding and compute root table
callbacks into its embedded backend. A copied staging descriptor can feed a
compute table at nonzero heap and range offsets. Validation rejects foreign
devices, invisible heaps, out-of-range or misaligned handles and stale bindings
after command-list reset. DDI shader-visible bit 2 maps explicitly to API bit 1.

Engine source: `7e50d05e8031485b26dfef75ae0a21eabc58cc91`.
Standalone native CI: `sunflower2333/vkd3d-proton/actions/runs/34593634076`,
all four jobs passed. Linux CPU Vulkan executes independent root-UAV and
descriptor-table workloads with 1024-word readback each. Actual WDK callback
fixtures execute on x86/x64; ARM64 compiles and packages the same fixtures.
This evidence does not establish target GPU or Windows runtime acceptance.

The parent workflow repeats backend and three-architecture bridge validation,
builds the paired ARM64 KMD/Mesa package, signs the candidate DLLs and tools,
and records the engine and parent identities. Existing Mesa desktop
registration is retained. This checkpoint does not register a D3D12 UMD.

Remaining native runtime work includes SRV/CBV/sampler/texture descriptors,
ranged copies and graphics, runtime allocation/GPUVA/residency, monitored
fences, OpenAdapter12 integration, presentation and reset recovery. These
remain prerequisites for claiming application-compatible native Direct3D12.
