# DXVK native UMD development

`external/dxvk` pins the independent DXVK fork. The parent workflow builds
that exact submodule, the existing Mesa UMD and KMD, then signs and catalogues
the DLLs before publishing the driver artifact. It does not publish a release.
All checkouts and submodules use depth1. The candidate version is explicitly
versioned in source; it is not calculated from truncated Git history.

The DXVK library currently exposes native D3D10 resource and restricted
SM4 VS/PS shader/state/draw DDI operations to a development harness, backed
by real DXVK Vulkan commands. A separate identity-query consumer implements
the proposed optional trailer; the current KMD does not produce it. It remains an
unregistered candidate: the installed UserModeDriverName still selects Mesa.
No capability or complete-DXVK-UMD claim follows from this packaging.

Remaining native activation requirements include a reliable runtime-adapter
LUID producer contract, general shader interfaces and all mandatory DDI coverage, kernel allocation
sharing, ownership/residency and DXGI Present/fence integration. Native D3D9,
D3D11 higher levels and runtime-to-DDI validation remain unfinished. Main
coordinates device execution; no remote files are pulled into this checkout.

The coordinated KMD identity follow-up is documented in the pinned DXVK
submodule at `doc/umd-identity-proposal.md`. It preserves the original
128-byte adapter-info v0 prefix and adds a separately versioned optional
trailer. No KMD implementation changes are included here.
