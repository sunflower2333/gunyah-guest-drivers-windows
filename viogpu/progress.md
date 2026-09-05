
## RenderOnly Has Two Sources: 2026-09-04 (final)

This corrects the previous entry, which blamed the guest reboot.

`RenderOnly` is read from **two** places:

- `HKLM\SYSTEM\CurrentControlSet\Services\viogpuwddm\Parameters\RenderOnly`, read
  by `VioGpuWddmReadRenderOnly` in `DriverEntry`, which selects the registered
  DDI table.
- The **device driver key** (`...Control\Class\{4d36e968-...}\<nnnn>`), read in
  `VioGpuDod::StartDevice` via
  `SetRenderOnly(VioGpuWddmIsRenderOnlyRegistration() || !NT_SUCCESS(...) || !!value)`.
  This one gates `*pNumberOfViews` and `*pNumberOfChildren`.

The INF writes `RenderOnly=1` into the device key on **every** install. The
install helper only cleared the service copy, so after each install the adapter
kept publishing zero children: no monitor, no mode, `Availability=8`, while
`NativeStartStage=4095` / `NativeStartStatus=0` correctly reported StartDevice
succeeding. Reinstalling an older package reproduced it exactly, because that
install rewrites the device key too - which is why the 58212 "rollback" also
showed no display and made this look like a reboot-path defect.

Clearing both copies and rebooting restored the display immediately:
`VC 100.6.101.58212 1280x1024 avail=3`, `MONCOUNT 1`. Any future install must
clear `RenderOnly` in every `{4d36e968-...}\<nnnn>` subkey whose `DriverDesc`
matches, not just in `Services\viogpuwddm\Parameters`.

### 58219 status

Installed with `RenderOnly` cleared in every key. The display registers:
`VC 100.6.101.58219 1280x1024 avail=3`, `MONCOUNT 1`.

The present fix could **not** be validated. The guest sits at the logon screen -
`console 1 Conn`, `LogonUI` running, no explorer, `AutoAdminLogon=1` with
`DefaultUserName=USER` but the logon never completes - so no interactive session
exists to paint anything, and sshd flaps (repeated
"OpenSSH SSH Server service terminated unexpectedly"). The framebuffer sampled
five times over 100 s with pointer input driven over VNC is frozen on one
sha (`806019c66d91d7c0`), and that sha is the same one seen before the 58219
install, so it is stale content retained in the crosvm resource, not a new frame.

A reading taken during this window showed `NativePresentStatus=0xC000000D`
against the earlier `0xC00000BB`, which looks like the fix landing. It must not
be reported as such: the driver's own diagnostic protocol writes
`NativePresentReason` last as a commit barrier, and it reads 0 with an even
`NativePresentDiagnosticEpoch=32`, which means no present diagnostic is
committed for this boot. The status value therefore cannot be attributed to a
present on this boot.

### Next

1. Get an interactive session on the console - fix or bypass the stalled
   autologon - so something paints and continuous presents can be measured.
2. `NativePresentSourceResource2DState=0` while the destination reads `3` is the
   standing lead for why a present is refused: the blt reads the source from CPU
   memory, so a source-side 2D host backing requirement would be wrong.

## Present Refusal Localised: 2026-09-04 (close)

Measurement conditions were confounded twice and both are now controlled:

- The guest IP moved from 192.168.1.146 to **192.168.1.147**, which made every
  port read as closed and looked like a dead guest. The socat forwarder must be
  repointed, not restarted.
- Windows display power-save was blanking the screen, which produced several
  "frozen framebuffer" readings. `powercfg /change monitor-timeout-ac 0` (and
  -dc) removes it.

With those controlled, on driver **100.6.101.58219**, display registered
(`1280x1024`, `avail=3`, `MONCOUNT 1`), desktop session live
(`EXPLORER 3844:1`):

    vncwatch 10 samples / 6 s
      t=  0s sha=c036cbb7553a909f   (blank - display was powered off)
      t=  6s sha=44a1faa4141fe8c5   (wake / transitional)
      t= 42s sha=806019c66d91d7c0   (desktop)
    DISTINCT_FRAMES 3 of 10 -> UPDATING

So frames do reach the Host, and the earlier "stale content" reading was wrong:
content changes across display state transitions. What never reaches the Host is
ordinary window painting - a full-screen painter confirmed running in session 1
(`flash_session1=1`) never appears, and the framebuffer holds the desktop frame
across 12 samples.

### Which branch refuses

`NativePresentContextType = 2` = `VioGpuWddmContextGdi`, so DWM's blt arrives on
a GDI context. Walking `VioGpuWddmPresent`'s classification with the recorded
fields:

    SourceResourceId          = 2      ( != 0, < NATIVE_RESOURCE_ID_START )
    SourceHostState           = 0      ( HostNone )
    SourceFlags               = 2      ( CPU visible -> IsGdiSourceAllocation )
    SourceAllocationListValue = 2      ( read, segment 1 -> prepatched )
    DestinationAllocationListValue = 3 ( write, segment 1 -> prepatched )
    SourcePlacementState      = 47     ( same as destination, placement fine )
    SourceResource2DState     = 0      <-- not BackingAttached
    DestinationResource2DState= 3      ( BackingAttached )

`HasGdiPresentIdentity` is satisfied, so `gdiSource` is TRUE and the first branch
does not fire. `HasLiveGdiPresentIdentity` additionally requires
`Resource2DState == VioGpu2DResourceBackingAttached`, which is 0, so
`gdiSourcePrepatchLive` is FALSE and the refusal is

    reason = VioGpuWddmPresentDiagnosticGdiSourcePlacement   (2)

`ReconcileGdiSourcePlacementAfterReset` already calls
`EnsureStandard2DAllocationBacking(source)`, so the remaining question is why
that ensure fails for a GDI source while it succeeds for the destination. That
is the next thing to fix, and it is the last thing standing between the current
build and a live desktop.

### Diagnostic gap worth closing at the same time

`NativePresentReason` reads 0 with an even `NativePresentDiagnosticEpoch`, which
by the driver's own commit protocol means no present diagnostic is committed for
the boot - yet the other fields hold plausible values. The reason is written last
as a commit barrier, so a failure part-way through leaves exactly this state:
every field readable except the one that says which branch fired. Recording the
reason first, or recording it even when the bulk writes fail, would have made
this walk unnecessary.

## Correction: the None + non-zero-generation wedge is NOT reachable

The previous entry proposed that an allocation could reach
`Resource2DState = VioGpu2DResourceNone` with a non-zero
`Resource2DResetGeneration`, which `Reconcile2DResourceAfterReset` and the
`Create2DResourceBacking` entry guard both refuse permanently. That is wrong.
Every writer that sets the state to `None` clears the generation in the same
statement:

    wddmddi.cpp:4576-4577   allocation->Resource2DState = None;
                            allocation->Resource2DResetGeneration = 0;   (CreateAllocation)
    viogpudo.cpp:7456-7457  *resourceState = None; *resourceResetGeneration = 0;  (attach rollback)
    viogpudo.cpp:7533-7534  *resourceState = None; *resourceResetGeneration = 0;  (Destroy2DResource)
    viogpudo.cpp:7572-7573  *resourceState = None; *resourceResetGeneration = 0;  (retirement)

There is no writer that leaves the pair inconsistent, so that path is not the
cause and should not be pursued.

### What is actually ruled out now

For the refusing GDI source, with `SourceResource2DState = 0` and generation
therefore 0, `Reconcile2DResourceAfterReset` returns TRUE and
`EnsureStandard2DAllocationBacking` reaches its create path. Ruled out along the
way:

- Native Context liveness - `vulkaninfo` enumerates Turnip in the same window.
- Placement/aperture - `SourcePlacementState = 47`, every bit set, identical to
  the destination.
- Format - source and destination are both `21` (`D3DDDIFMT_A8R8G8B8`), and the
  destination resolves and attaches.
- The `None` + generation wedge, above.

What remains is one of the four failure points inside the create path:
`ResolveStandard2DFormat`, `AllocateApertureBackingEntries`,
`CreateResource2DSynchronous`, or `AttachBackingSynchronous`. Distinguishing
them needs the refusal reason recorded (the diagnostic gap noted earlier) or a
counter per failure point. Two successive static hypotheses have now been
falsified by re-reading the code; the next step should be instrumentation, not
another inference.

## RESOLVED: the Present source is itself a primary

The identity recorder fired and names the branch exactly.
`NativeGdiIdentityTerms = 0x80001FCF` on `100.6.101.58232`, display up, desktop
live:

    Signature              bit0  set
    AdapterMatches         bit1  set
    ContextIsGdi           bit2  set
    IsStandardAlloc        bit3  set
    NotPrimary             bit4  *** CLEAR ***
    IsGdiSourceAlloc       bit5  *** CLEAR ***
    HostStateNone          bit6  set
    BlobIdZero             bit7  set
    ResourceIdNonZero      bit8  set
    ResourceIdBelowNative  bit9  set
    ContextIdZero          bit10 set
    ContextGenZero         bit11 set
    ContextResetGenZero    bit12 set
    nativeSource           bit13 clear
    gdiCandidate           bit14 clear

Every term holds except one: **the Present source is itself a standard primary
allocation**. `IsGdiSourceAllocation` requires `!IsStandardPrimaryAllocation`, so
bit 5 falls, `gdiCandidate` falls, `gdiSource` is FALSE, and the
`!nativeSourceCurrent && !gdiSource` branch refuses the present with
`GdiSourceIdentity`.

The cause is a direct consequence of `fe98438f`. With no flip capability
published, dxgkrnl does not flip DWM's swapchain - it converts it into a **blt
between two primaries**, source and destination both being buffers of the flip
chain. The driver accepts a Present source that is either a live native
allocation or a GDI surface, and `IsGdiSourceAllocation` is defined to exclude
primaries. So every DWM composition present is refused for being sourced from a
primary.

That also explains `SrcState = 2, DstState = 2`: both are primaries, both get
created, neither is a GDI surface.

### This is a design decision, not a bug to patch silently

`check-contract.py` pins `IsGdiSourceAllocation`'s body character for character
and fails with "GDI Present must accept only a CPU-visible non-primary standard
allocation". Excluding primaries is deliberate. Widening that predicate would
overturn a stated invariant, so the choice belongs to the owner:

- **(a) Accept a standard primary as a Present source.** This is what dxgkrnl
  actually asks for once the driver publishes no flip capability. The execute
  path already copies CPU-side using Pitch/Width/Height, all of which a primary
  carries, so the copy itself needs no change. It needs a new source class and
  its own contract rule, kept separate from the GDI class so that invariant
  stays intact.
- **(b) Publish a flip capability the driver can actually honour**, so dxgkrnl
  flips instead of blitting. `FlipOnVSyncMmIo` was wrong because it obliges the
  miniport to program the flip from an MMIO write at device IRQL, which a
  synchronous virtqueue round trip cannot do. Whether any other flip cap fits a
  PASSIVE_LEVEL scanout path is the open question.

Either way the remaining work is one deliberate change, and the measurement that
determines it is now in hand rather than inferred.
