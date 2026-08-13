# DroidVM pre-shared pool provider

`droidvmpool.sys` binds every `ACPI\DRVM0001` pool device emitted by DroidVM
firmware. Each WDF device evaluates its own ACPI `_UID`, maps its single `_CRS`
memory resource, and publishes a kernel-only query interface containing the
pool name, GPA, kernel VA, and size.

The provider does not allocate, free, or clear pool storage. Pool ownership is
defined by the name contract: `drm2kgsl_host` is host-owned, while `gpu_guest`
is guest-owned. A client must enumerate all provider interfaces and select the
exact name it owns rather than choosing the first `DRVM0001` instance.
