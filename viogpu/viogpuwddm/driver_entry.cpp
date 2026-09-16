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
 * service Parameters key and recoverable with a reboot. */
static BOOLEAN g_VioGpuWddmConnectorTimingModel = FALSE;

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
#if defined(VIOGPU_ADVANCED_COLOR_MPO3)
        /* Unreachable without overlay caps (MaxOverlays 0); WDDM 2.2 MPO also
         * needs DxgkDdiGetMultiPlaneOverlayCaps. Separate experiment only. */
        initialData->DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3 = VioGpuWddmSetVidPnSourceAddressMpo3;
        initialData->DxgkDdiCheckMultiPlaneOverlaySupport3 = VioGpuWddmCheckMultiPlaneOverlaySupport3;
#endif
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
    DRIVER_INITIALIZATION_DATA initialData;
    VioGpuWddmBuildInitializationData(&initialData, renderOnly);

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
