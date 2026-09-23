#include "wddmddi.h"

#if !DBG
#include "driver_entry.tmh"
#endif

static BOOLEAN g_VioGpuWddmRenderOnlyRegistration = TRUE;
/* dxgkrnl reports SupportSetTimingsFromVidPn=0 for this adapter even with the
 * DDI registered, WDDM 2.3 reported and the whole WDDM 2.2 connection set in
 * place (measured on target 2026-09-16 via KMTQAITYPE_ADAPTERTYPE). The DDI
 * documentation says SetTimingsFromVidPn *replaces* DxgkDdiCommitVidPn, so the
 * remaining discriminator is that this driver still offers CommitVidPn. This
 * switch withholds it. Off by default: a driver that gets neither path would
 * have no way to set a mode at all, so it is opt-in per device through the
 * service Parameters key and recoverable with a reboot.
 *
 * FALSIFIED on target 2026-09-16: with the switch on, the adapter does not
 * start -- `configManagerErrorCode=43`, `GetDisplayConfigBufferSizes` answers
 * ERROR_NOT_SUPPORTED(50), and `NativeTimingPathCalls` is still 0, so
 * DXGKDDI_SETTIMINGSFROMVIDPN is not called even in CommitVidPn's absence.
 * dxgkrnl requires DxgkDdiCommitVidPn from this driver and does not accept
 * SetTimingsFromVidPn as a replacement for it here. Keep the switch off; it is
 * retained only so the experiment is not repeated. */
static BOOLEAN g_VioGpuWddmConnectorTimingModel = FALSE;

/* Multi-plane overlay reachability probe. Windows can only take a fullscreen
 * app's buffer straight to the scanout -- independent flip -- through MPO, and
 * this driver reports no overlay planes, so every frame is composited by DWM
 * instead. Measured 2026-09-16: DWM's own work is ~13 ms per frame and making
 * the app fullscreen changed nothing (40.5 delivered against 88 rendered),
 * because there is no independent-flip path to take.
 *
 * This now arms the whole MPO set -- caps, CheckMultiPlaneOverlaySupport3 and
 * SetVidPnSourceAddressWithMultiPlaneOverlay3 -- because dxgkrnl refuses to
 * finish adapter start when the caps are advertised without the other two
 * (CM_PROB_FAILED_POST_START, measured). Check accepts exactly one unrotated,
 * unscaled, opaque full-target plane and refuses everything else, so the only
 * traffic the flip path can ever see is the one case it implements. It is
 * **one-shot**: the value is cleared as it is read, so a boot
 * that goes wrong cannot repeat, which is the trap the guest-blob scanout
 * experiment fell into. */
static BOOLEAN g_VioGpuWddmOverlayProbe = FALSE;
/* Whether the table handed to DxgkInitialize carries
 * DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3. Read from the built
 * table, not re-derived from the probe, so DriverCaps can never describe a
 * registration that did not happen. dxgkrnl 10.0.26100 treats a display
 * adapter reporting DXGKDDI_WDDMv2_3 as an MPO3 driver: on 2.3 it leaves
 * legacy display-state synchronization, and when DWM destroys the scanned-out
 * primary DxgkDestroyAllocationInternal calls ADAPTER_DISPLAY::DisableMPOPlanes
 * starting at plane 0, which calls this slot with no NULL check. With the slot
 * empty that is a jump to 0 -- the 0x3B/STATUS_BREAKPOINT bugcheck in dwm.exe
 * every time the Android display attached (four identical minidumps on
 * 58505, 2026-09-17). */
static BOOLEAN g_VioGpuWddmMpo3Registration = FALSE;

BOOLEAN VioGpuWddmIsOverlayProbeRegistration()
{
    return g_VioGpuWddmOverlayProbe;
}

BOOLEAN VioGpuWddmIsMpo3Registration()
{
    return g_VioGpuWddmMpo3Registration;
}

BOOLEAN VioGpuWddmIsRenderOnlyRegistration()
{
    return g_VioGpuWddmRenderOnlyRegistration;
}

#pragma code_seg(push)
#pragma code_seg("INIT")

static_assert(DXGKDDI_INTERFACE_VERSION == DXGKDDI_INTERFACE_VERSION_WDDM2_0 || DXGKDDI_INTERFACE_VERSION == DXGKDDI_INTERFACE_VERSION_WDDM2_3,
              "viogpuwddm requires WDDM 2.0 physical-engine declarations");

/* Registration and DriverCaps must describe the same selected runtime mode. */
static BOOLEAN VioGpuWddmReadRenderOnly(_In_ UNICODE_STRING *registryPath)
{
    PAGED_CODE();

    BOOLEAN renderOnly = TRUE;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, registryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    HANDLE serviceKey = NULL;
    NTSTATUS status = ZwOpenKey(&serviceKey, KEY_READ, &attributes);
    if (!NT_SUCCESS(status))
    {
        return TRUE;
    }

    UNICODE_STRING parametersName;
    RtlInitUnicodeString(&parametersName, L"Parameters");
    InitializeObjectAttributes(&attributes,
                               &parametersName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               serviceKey,
                               NULL);

    HANDLE parametersKey = NULL;
    status = ZwOpenKey(&parametersKey, KEY_QUERY_VALUE, &attributes);
    ZwClose(serviceKey);
    if (!NT_SUCCESS(status))
    {
        return TRUE;
    }

    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, L"RenderOnly");
    union {
        KEY_VALUE_PARTIAL_INFORMATION Info;
        UCHAR Bytes[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    } valueInfo = {};
    ULONG resultLength = 0;
    status = ZwQueryValueKey(parametersKey,
                             &valueName,
                             KeyValuePartialInformation,
                             valueInfo.Bytes,
                             sizeof(valueInfo.Bytes),
                             &resultLength);
    if (NT_SUCCESS(status) && valueInfo.Info.Type == REG_DWORD && valueInfo.Info.DataLength == sizeof(ULONG))
    {
        ULONG value = 0;
        RtlCopyMemory(&value, valueInfo.Info.Data, sizeof(value));
        renderOnly = value != 0;
    }

    ZwClose(parametersKey);
    return renderOnly;
}

static BOOLEAN VioGpuWddmReadOneShotFlag(_In_ UNICODE_STRING *registryPath, _In_ PCWSTR flagName)
{
    PAGED_CODE();

    BOOLEAN armed = FALSE;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, registryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    HANDLE serviceKey = NULL;
    if (!NT_SUCCESS(ZwOpenKey(&serviceKey, KEY_READ, &attributes)))
    {
        return FALSE;
    }

    UNICODE_STRING parametersName;
    RtlInitUnicodeString(&parametersName, L"Parameters");
    InitializeObjectAttributes(&attributes,
                               &parametersName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               serviceKey,
                               NULL);

    HANDLE parametersKey = NULL;
    NTSTATUS status = ZwOpenKey(&parametersKey, KEY_QUERY_VALUE | KEY_SET_VALUE, &attributes);
    ZwClose(serviceKey);
    if (!NT_SUCCESS(status))
    {
        return FALSE;
    }

    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, flagName);
    union {
        KEY_VALUE_PARTIAL_INFORMATION Info;
        UCHAR Bytes[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    } valueInfo = {};
    ULONG resultLength = 0;
    status = ZwQueryValueKey(parametersKey,
                             &valueName,
                             KeyValuePartialInformation,
                             valueInfo.Bytes,
                             sizeof(valueInfo.Bytes),
                             &resultLength);
    if (NT_SUCCESS(status) && valueInfo.Info.Type == REG_DWORD && valueInfo.Info.DataLength == sizeof(ULONG))
    {
        ULONG value = 0;
        RtlCopyMemory(&value, valueInfo.Info.Data, sizeof(value));
        armed = value != 0;
    }
    if (armed)
    {
        /* Disarm before the probe can take effect: one bad boot, never a loop. */
        ULONG cleared = 0;
        (VOID)ZwSetValueKey(parametersKey, &valueName, 0, REG_DWORD, &cleared, sizeof(cleared));
    }

    ZwClose(parametersKey);
    return armed;
}

static BOOLEAN VioGpuWddmReadOverlayProbe(_In_ UNICODE_STRING *registryPath)
{
    PAGED_CODE();
    return VioGpuWddmReadOneShotFlag(registryPath, L"MultiPlaneOverlayProbe");
}

static BOOLEAN VioGpuWddmRegistersMpo3(_In_ CONST DRIVER_INITIALIZATION_DATA *initialData)
{
    PAGED_CODE();
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_3)
    return initialData->DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3 != NULL;
#else
    UNREFERENCED_PARAMETER(initialData);
    return FALSE;
#endif
}

static BOOLEAN VioGpuWddmReadConnectorTimingModel(_In_ UNICODE_STRING *registryPath)
{
    PAGED_CODE();

    BOOLEAN connectorModel = FALSE;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, registryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    HANDLE serviceKey = NULL;
    NTSTATUS status = ZwOpenKey(&serviceKey, KEY_READ, &attributes);
    if (!NT_SUCCESS(status))
    {
        return FALSE;
    }

    UNICODE_STRING parametersName;
    RtlInitUnicodeString(&parametersName, L"Parameters");
    InitializeObjectAttributes(&attributes,
                               &parametersName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               serviceKey,
                               NULL);

    HANDLE parametersKey = NULL;
    status = ZwOpenKey(&parametersKey, KEY_QUERY_VALUE, &attributes);
    ZwClose(serviceKey);
    if (!NT_SUCCESS(status))
    {
        return FALSE;
    }

    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, L"ConnectorTimingModel");
    union {
        KEY_VALUE_PARTIAL_INFORMATION Info;
        UCHAR Bytes[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    } valueInfo = {};
    ULONG resultLength = 0;
    status = ZwQueryValueKey(parametersKey,
                             &valueName,
                             KeyValuePartialInformation,
                             valueInfo.Bytes,
                             sizeof(valueInfo.Bytes),
                             &resultLength);
    if (NT_SUCCESS(status) && valueInfo.Info.Type == REG_DWORD && valueInfo.Info.DataLength == sizeof(ULONG))
    {
        ULONG value = 0;
        RtlCopyMemory(&value, valueInfo.Info.Data, sizeof(value));
        connectorModel = value != 0;
    }

    ZwClose(parametersKey);
    return connectorModel;
}

VOID VioGpuWddmBuildInitializationData(_Out_ DRIVER_INITIALIZATION_DATA *initialData, _In_ BOOLEAN renderOnly)
{
    RtlZeroMemory(initialData, sizeof(*initialData));
    /* WDDM 2.0 permits physical-mode engines with allocation/patch lists.
     * Register the matching table; host-owned GPU page tables are not exposed
     * as a guest GpuMmu/IoMmu. VidSch retains the real submission timeline. */
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_3)
    initialData->Version = DXGKDDI_INTERFACE_VERSION_WDDM2_3;
#else
    initialData->Version = DXGKDDI_INTERFACE_VERSION_WDDM2_0;
#endif

    initialData->DxgkDdiAddDevice = VioGpuDodAddDevice;
    initialData->DxgkDdiStartDevice = VioGpuDodStartDevice;
    initialData->DxgkDdiStopDevice = VioGpuDodStopDevice;
    initialData->DxgkDdiResetDevice = VioGpuDodResetDevice;
    initialData->DxgkDdiRemoveDevice = VioGpuDodRemoveDevice;
    initialData->DxgkDdiDispatchIoRequest = VioGpuDodDispatchIoRequest;
    initialData->DxgkDdiInterruptRoutine = VioGpuDodInterruptRoutine;
    initialData->DxgkDdiDpcRoutine = VioGpuDodDpcRoutine;
    initialData->DxgkDdiQueryChildRelations = VioGpuDodQueryChildRelations;
    initialData->DxgkDdiQueryChildStatus = VioGpuDodQueryChildStatus;
    initialData->DxgkDdiQueryDeviceDescriptor = VioGpuDodQueryDeviceDescriptor;
    initialData->DxgkDdiSetPowerState = VioGpuDodSetPowerState;
    initialData->DxgkDdiNotifyAcpiEvent = VioGpuWddmNotifyAcpiEvent;
    initialData->DxgkDdiUnload = VioGpuDodUnload;
    initialData->DxgkDdiQueryInterface = VioGpuDodQueryInterface;
    initialData->DxgkDdiControlEtwLogging = VioGpuWddmControlEtwLogging;

    initialData->DxgkDdiQueryAdapterInfo = VioGpuWddmQueryAdapterInfo;
    initialData->DxgkDdiCreateDevice = VioGpuWddmCreateDevice;
    initialData->DxgkDdiDestroyDevice = VioGpuWddmDestroyDevice;
    initialData->DxgkDdiCreateAllocation = VioGpuWddmCreateAllocation;
    initialData->DxgkDdiDestroyAllocation = VioGpuWddmDestroyAllocation;
    initialData->DxgkDdiDescribeAllocation = VioGpuWddmDescribeAllocation;
    initialData->DxgkDdiGetStandardAllocationDriverData = VioGpuWddmGetStandardAllocationDriverData;
    initialData->DxgkDdiOpenAllocation = VioGpuWddmOpenAllocation;
    initialData->DxgkDdiCloseAllocation = VioGpuWddmCloseAllocation;
    initialData->DxgkDdiCreateContext = VioGpuWddmCreateContext;
    initialData->DxgkDdiDestroyContext = VioGpuWddmDestroyContext;
    initialData->DxgkDdiBuildPagingBuffer = VioGpuWddmBuildPagingBuffer;
    initialData->DxgkDdiRender = VioGpuWddmRender;
    initialData->DxgkDdiRenderKm = VioGpuWddmRenderKm;
    initialData->DxgkDdiPresent = VioGpuWddmPresent;
    initialData->DxgkDdiPatch = VioGpuWddmPatch;
    initialData->DxgkDdiSubmitCommand = VioGpuWddmSubmitCommand;
    initialData->DxgkDdiPreemptCommand = VioGpuWddmPreemptCommand;
    initialData->DxgkDdiQueryCurrentFence = VioGpuWddmQueryCurrentFence;
    initialData->DxgkDdiResetFromTimeout = VioGpuWddmResetFromTimeout;
    initialData->DxgkDdiRestartFromTimeout = VioGpuWddmRestartFromTimeout;
    initialData->DxgkDdiCollectDbgInfo = VioGpuWddmCollectDbgInfo;
    initialData->DxgkDdiControlInterrupt = VioGpuWddmControlInterrupt;

    initialData->DxgkDdiEscape = VioGpuWddmEscape;

    /* SchedulingCaps advertises MultiEngineAware in both modes, which obliges
     * the miniport to supply these entry points in both modes.  They are
     * engine/scheduler DDIs, not display ones. DxgkDdiCancelCommand remains
     * assigned, but CancelCommandAware is still 0: this physical-mode engine
     * does not advertise the optional command cancellation contract. */
    initialData->DxgkDdiCancelCommand = VioGpuWddmCancelCommand;
    initialData->DxgkDdiQueryDependentEngineGroup = VioGpuWddmQueryDependentEngineGroup;
    initialData->DxgkDdiQueryEngineStatus = VioGpuWddmQueryEngineStatus;
    initialData->DxgkDdiResetEngine = VioGpuWddmResetEngine;
    initialData->DxgkDdiGetNodeMetadata = VioGpuWddmGetNodeMetadata;
    /* Required by WDDM 2.0 adapter start in both modes: "Driver is compiled
     * against DXGKDDI_INTERFACE_VERSION_WDDM2_0_M2_2_1 or greater, but does not
     * fill in the pfnCalibrateGpuClock or pfnSetStablePowerState DDI". */
    initialData->DxgkDdiCalibrateGpuClock = VioGpuWddmCalibrateGpuClock;
    initialData->DxgkDdiSetStablePowerState = VioGpuWddmSetStablePowerState;

    if (!renderOnly)
    {
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_3)
        initialData->DxgkDdiSetTargetAdjustedColorimetry = VioGpuWddmSetTargetAdjustedColorimetry;
        initialData->DxgkDdiSetTargetGamma = VioGpuWddmSetTargetGamma;
        initialData->DxgkDdiSetTimingsFromVidPn = VioGpuWddmSetTimingsFromVidPn;
        initialData->DxgkDdiUpdateMonitorLinkInfo = VioGpuWddmUpdateMonitorLinkInfo;
        /* The MPO set is all-or-nothing. VIOGPU_ADVANCED_COLOR_MPO3 was never
         * defined anywhere, so these two were dead code with no function bodies
         * at all, while the caps below were gated separately -- arming the probe
         * therefore advertised MaxOverlays=1 with the check and flip entry
         * points absent, and dxgkrnl refused to finish adapter start
         * (CM_PROB_FAILED_POST_START, measured 2026-09-17). Register the whole
         * set on the one gate so the caps can never be advertised alone. */
        if (g_VioGpuWddmOverlayProbe)
        {
            initialData->DxgkDdiGetMultiPlaneOverlayCaps = VioGpuWddmGetMultiPlaneOverlayCaps;
            initialData->DxgkDdiCheckMultiPlaneOverlaySupport3 = VioGpuWddmCheckMultiPlaneOverlaySupport3;
            initialData->DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3 = VioGpuWddmSetVidPnSourceAddressMpo3;
        }
#if defined(VIOGPU_ADVANCED_COLOR_CONNECTION_DDIS)
        /* WDDM 2.2 connection model beside legacy child status. Separate
         * experiment: whether dxgkrnl requires or forbids it is a target question. */
        initialData->DxgkDdiDisplayDetectControl = VioGpuWddmDisplayDetectControl;
        initialData->DxgkDdiQueryConnectionChange = VioGpuWddmQueryConnectionChange;
#endif
#endif
        initialData->DxgkDdiSetPalette = VioGpuWddmSetPalette;
        initialData->DxgkDdiSetPointerPosition = VioGpuDodSetPointerPosition;
        initialData->DxgkDdiSetPointerShape = VioGpuDodSetPointerShape;
        initialData->DxgkDdiIsSupportedVidPn = VioGpuDodIsSupportedVidPn;
        initialData->DxgkDdiRecommendFunctionalVidPn = VioGpuDodRecommendFunctionalVidPn;
        initialData->DxgkDdiEnumVidPnCofuncModality = VioGpuDodEnumVidPnCofuncModality;
        initialData->DxgkDdiSetVidPnSourceAddress = VioGpuWddmSetVidPnSourceAddress;
        initialData->DxgkDdiSetVidPnSourceVisibility = VioGpuDodSetVidPnSourceVisibility;
        /* Withheld under the connector timing model: DXGKDDI_SETTIMINGSFROMVIDPN
         * replaces this entry point, and offering both leaves dxgkrnl on the
         * legacy path with SupportSetTimingsFromVidPn reported as 0. */
        if (!g_VioGpuWddmConnectorTimingModel)
        {
            initialData->DxgkDdiCommitVidPn = VioGpuDodCommitVidPn;
        }
        initialData->DxgkDdiUpdateActiveVidPnPresentPath = VioGpuDodUpdateActiveVidPnPresentPath;
        initialData->DxgkDdiRecommendMonitorModes = VioGpuDodRecommendMonitorModes;
        initialData->DxgkDdiGetScanLine = VioGpuWddmGetScanLine;
        initialData->DxgkDdiQueryVidPnHWCapability = VioGpuDodQueryVidPnHWCapability;
    }
}

#pragma optimize("", off)
extern "C" NTSTATUS VioGpuWddmInitializeMiniport(_In_ DRIVER_OBJECT *driverObject, _In_ UNICODE_STRING *registryPath)
{
    PAGED_CODE();

    BOOLEAN renderOnly = VioGpuWddmReadRenderOnly(registryPath);
    g_VioGpuWddmRenderOnlyRegistration = renderOnly;
    g_VioGpuWddmConnectorTimingModel = VioGpuWddmReadConnectorTimingModel(registryPath);
    g_VioGpuWddmOverlayProbe = VioGpuWddmReadOverlayProbe(registryPath);
    DRIVER_INITIALIZATION_DATA initialData;
    VioGpuWddmBuildInitializationData(&initialData, renderOnly);
    g_VioGpuWddmMpo3Registration = VioGpuWddmRegistersMpo3(&initialData);

    WPP_INIT_TRACING(driverObject, registryPath);
    NTSTATUS status = DxgkInitialize(driverObject, registryPath, &initialData);
    if (!NT_SUCCESS(status))
    {
        WPP_CLEANUP(NULL);
    }

    return status;
}
#pragma optimize("", on)

extern "C" NTSTATUS DriverEntry(_In_ DRIVER_OBJECT *driverObject, _In_ UNICODE_STRING *registryPath)
{
    PAGED_CODE();
    return VioGpuWddmInitializeMiniport(driverObject, registryPath);
}

#pragma code_seg(pop)
