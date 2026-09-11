# Native D3D12 descriptor checkpoint

The pinned vkd3d-proton engine now translates native WDK descriptor heap,
buffer UAV, simple descriptor copy, heap binding and compute root table
callbacks into its embedded backend. A copied staging descriptor can feed a
compute table at nonzero heap and range offsets. Validation rejects foreign
devices, invisible heaps, out-of-range or misaligned handles and stale bindings
after command-list reset. DDI shader-visible bit 2 maps explicitly to API bit 1.

Engine source: `09146e1dbe5b75c09c25eb53867ae2dedb3c52c6`.
Standalone native CI: `sunflower2333/vkd3d-proton/actions/runs/34596053967`,
all four jobs passed. Linux CPU Vulkan executes independent root-UAV and
descriptor-table workloads with 1024-word readback each. Actual WDK callback
fixtures execute on x86/x64; ARM64 compiles and packages the same fixtures.
This evidence does not establish target GPU or Windows runtime acceptance.

The package also includes `vkd3d-umd-gpu-probe`, which calls the production
backend with an explicit Windows adapter LUID and matching Vulkan vendor and
device IDs. It independently requires that identity and the Turnip driver ID;
the CPU test-device entrypoint is absent. Both compute/readback paths execute
on the selected device when invoked. Missing identity returns2 before loading
Vulkan. Target execution remains separate from these completed CI checks and
does not establish native runtime DDI or presentation support.

The parent workflow repeats backend and three-architecture bridge validation,
builds the paired ARM64 KMD/Mesa package, signs the candidate DLLs and tools,
and records the engine and parent identities. Existing Mesa desktop
registration is retained. This checkpoint does not register a D3D12 UMD.

Remaining native runtime work includes SRV/CBV/sampler/texture descriptors,
ranged copies and graphics, runtime allocation/GPUVA/residency, monitored
fences, OpenAdapter12 integration, presentation and reset recovery. These
remain prerequisites for claiming application-compatible native Direct3D12.
