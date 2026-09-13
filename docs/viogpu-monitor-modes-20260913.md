# Complete monitor-mode recommendations and preserve failed-add status

When Windows already has the driver's preferred mode in its monitor modeset,
`AddSingleMonitorMode` previously returned success after releasing the duplicate
temporary object. Missing alternatives were never considered. In later loop
iterations, a failed `pfnAddMode` was also replaced by the status of
`pfnReleaseModeInfo`, hiding real add errors behind successful cleanup.

The callback now continues after a successfully released duplicate, preserves
the initiating add failure, and propagates a failed release when duplicate
cleanup is the only failure. Successful additions remain owned by Windows;
only failed additions are released. Existing ordering, color information and
current-mode preference are retained. This change does not choose a new active
display mode or force the preferred refresh rate.

Microsoft contracts consulted:

- [RecommendMonitorModes](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_recommendmonitormodes)
  requires an OS callback error to be returned rather than hidden by fallback.
- [MonitorSourceModeSet AddMode](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_monitorsourcemodeset_addmode)
  distinguishes an identical existing mode from invalid frequency, invalid mode
  and memory failures.
- [Mode preference](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmdt/ne-d3dkmdt-_d3dkmdt_mode_preference)
  describes monitor preference separately from the current active mode.

## Validation and scope

The actual callback and signal builder pass95 checks with controlled OS peers;
the unmodified875edab baseline fails five. Both semantic negatives detect the
intended early-return and error-masking regressions. The full miniport static
contract passes. The parent signed workflow remains version58476; two inherited
contract literals were aligned from58473. Final package identity/signing is
owned by the integrating parent.

This descendant also integrates frozen first-timeout publicationf67b71e and
versioned mode restoration helperbaa779c. Their actual production fixtures pass
1370 and50 checks, respectively, including both semantic negatives in each
suite. The original unversioned mode helper, queue implementation, timeout
values, reset semantics, image and firmware are unchanged.

Current crosvm ec25e475 emits a legacy3040x1904@60 recovery timing and preferred
DisplayID3040x1904@165. Its EDID producer and installed-equivalent KMD875edab
parser were freshly tested together, yielding1160680000Hz and1450850/8793Hz
(165.000568634Hz). The earlier71.83Hz high-clock overflow is already repaired.
The installed executable identifierd4a141fb is a SHA256 prefix, not a git ID.

Windows previously enumerated165 but selected60. A historical165Hz apply
returned-1 after about5s, with synchronous queue poison and DWM loss. The
monitor recommendation defect is independently established; it is not yet
proven to cause that timeout or to be the sole reason for active60Hz.

## Parent-owned target test

After integration, signing and the separate ordinary D3D Present validation,
read EnumDisplaySettings CURRENT and REGISTRY, advertised modes and the active
QueryDisplayConfig rational. Capture the first-timeout registry fields from
the active device key before and after the test.

Use `.install_scripts/viogpu-test-display-mode-v2.ps1` without `-Apply` for
CDS_TEST only. The later bounded temporary test uses `-Apply` with default
3040x1904@165 and five-second hold; do not use `-Keep`. The helper observes actual
state even after an unsuccessful API return and attempts one restoration when
changed or unknown. Both initiating and restoration failures remain failures.
An individual Windows modeset call still has no hard wall-clock bound.

Measure KMT vblank cadence and actual delivered frames separately. Local/CI
success does not establish active165Hz, physical165fps or complete GPU runtime
acceptance. No target operations occurred in this worker lane.
