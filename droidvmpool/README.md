# DroidVM pre-shared pool provider

`droidvmpool.sys` binds every `ACPI\DRVM0001` pool device emitted by DroidVM
firmware. Each WDF device evaluates its own ACPI `_UID`, maps its single `_CRS`
memory resource, and publishes a kernel-only query interface containing the
pool name, GPA, and size.

The provider does not allocate, free, or clear pool storage. Pool ownership is
defined by the name contract: `drm2kgsl_host` is host-owned, while `gpu_guest`
is guest-owned. A client must enumerate all provider interfaces and select the
exact name it owns rather than choosing the first `DRVM0001` instance.

The query IOCTL is discovery only. The current direct-call interface is version
2: the ACPI resource must be cacheable read/write without conflicting
write-combined or prefetchable flags, and the pool is treated as Normal-WB,
shareable memory across the Windows CPU, crosvm, and Gunyah stage 2. Cross-stack
kernel clients obtain the versioned direct-call interface and must acquire a
mapping lease around every kernel-VA access. The caller owns the surrounding
APC-disabled region, does not wait or call pageable code, and releases the
lease on its acquiring thread. `ReleaseHardware` first blocks new leases,
waits for all active leases to finish, and only then unmaps the provider-owned
VA. Keeping a file or direct-interface reference open is not itself a mapping
lease.

The `drm2kgsl_host` pool is only the CPU-to-host-renderer control/response
transport. KGSL cache maintenance for guest-owned BOs belongs to the separate
`gpu_guest` data-pool contract and is not inferred from this provider version.
