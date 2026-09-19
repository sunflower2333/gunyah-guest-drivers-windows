# Present color conversion regression

Run `python viogpu/tests/present-formats/run.py` with a host C++ compiler.
The fixture extracts production `ValidatePresentGeometry`, `CopyPresentRow` and
the actual rectangle loop in `ExecutePresentTransaction`. It exercises all nine
pairs of the already-supported BGRA/BGRX/RGBA formats, nonopaque source alpha,
two disjoint clipped rectangles, unequal unaligned pitches, untouched padding,
and invalid geometry/backing. It neither loads nor submits to a GPU.

Row-boundary cases additionally cover all nine format pairs at widths
1/2/3/7/16/17/31/32/33/3040 and all eight byte alignments, with an independent
byte oracle, odd-pixel tails, unchanged source bytes and destination guards.
The two-pixel integer conversion retains the existing raw-copy fast path and
does not use kernel SIMD state. Hardware speedup still requires runtime timing;
the Raw/Convert copy counters distinguish which branch the workload uses.

`--negative-control-raw-copy` substitutes the old raw row copy in the real loop
and must detect wrong RGB channels and undefined X bytes used as alpha.
`--baseline` optionally reads installed commit875edab6 from local git objects
and demonstrates the same five unsupported conversions at the geometry gate;
the shallow CI checkout does not require that historical object.

Microsoft's `DxgkDdiPresent` contract explicitly allows asynchronous primary
format changes and places conversion in the miniport. `DXGI_DDI_ARG_PRESENT`
permits the destination to remain unknown until kernel mode. Therefore UMD
`BltDXGI` conversion does not remove the need for KMD format conversion.

- https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_present
- https://learn.microsoft.com/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_arg_present
- https://learn.microsoft.com/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_base_functions

The copied RGB/sRGB bytes are preserved, with channel ordering changed as needed.
BGRX to a format with alpha inserts opaque255. Same layout and BGRA to BGRX
retain the memcpy fast path. Existing raw source/destination copy-probe hashes
can differ for a color conversion, so interpret those diagnostics with formats.

These tests establish a source format defect and repair. They do not establish
that an actual `STATUS_GRAPHICS_PRESENT_OCCLUDED` originated in this geometry
gate, which previously returned `STATUS_INVALID_PARAMETER`. That diagnosis
requires a fresh KMD entry/rejection counter and format snapshot for the actual
failing Present; Windows runtime acceptance remains separate.
