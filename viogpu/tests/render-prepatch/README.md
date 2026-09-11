# Render prepatch and final residency

Run `python viogpu/tests/render-prepatch/run.py`. The runner extracts the
production `ApplyRenderPrepatches` and `ValidateNativeRenderBindings` routines,
with Windows locking and packet storage represented by platform seams. The
independent native-context-wire and private-ABI tests validate the wire layout.

The regression reproduces a nonzero last-known VidMm placement hint after
eviction, restores residency at another aperture offset, and validates the
unchanged native resource ID/IOVA at dispatch. It also checks the no-Patch path,
zero prepatch hints, multiple references, invalid identity, and invalid final
bindings. Linux runs with ASan/UBSan; both Windows driver CI workflows use MSVC.

`--prepatch-revision 6c8b4170` selects the pre-repair translation routine as a
negative control. Four eviction/relocation assertions fail in that revision.
