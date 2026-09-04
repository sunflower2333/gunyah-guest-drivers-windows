#include "wddmddi.h"

#if !DBG
#include "driver_entry.tmh"
#endif

static BOOLEAN g_VioGpuWddmRenderOnlyRegistration = TRUE;

BOOLEAN VioGpuWddmIsRenderOnlyRegistration()
{
    return g_VioGpuWddmRenderOnlyRegistration;
}

#pragma code_seg(push)
#pragma code_seg("INIT")

static_assert(DXGKDDI_INTERFACE_VERSION == DXGKDDI_INTERFACE_VERSION_WIN8,
              "viogpuwddm requires Win8 declarations for its internal Native Context callbacks");

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

VOID VioGpuWddmBuildInitializationData(_Out_ DRIVER_INITIALIZATION_DATA *initialData, _In_ BOOLEAN renderOnly)
{
    RtlZeroMemory(initialData, sizeof(*initialData));
    /* DriverCaps publishes WDDM 1.2 and the render engine in both modes, so the
     * registered interface has to be WDDM 1.2 in both modes too.  Registering
     * WIN7 while claiming 1.2 hands dxgkrnl a callback table that contradicts
     * the capabilities, and the D3D runtime then refuses the adapter before it
     * ever loads a user-mode driver. */
    initialData->Version = DXGKDDI_INTERFACE_VERSION_WIN8;

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
     * engine/scheduler DDIs, not display ones.  DxgkDdiCancelCommand is kept
     * assigned although CancelCommandAware is 0: dxgkrnl only reads that slot
     * from drivers registering 0x7002 or later, so at WIN8 the assignment is
     * inert, and it stays here so raising the registered version is a one-line
     * change. */
    initialData->DxgkDdiCancelCommand = VioGpuWddmCancelCommand;
    initialData->DxgkDdiQueryDependentEngineGroup = VioGpuWddmQueryDependentEngineGroup;
    initialData->DxgkDdiQueryEngineStatus = VioGpuWddmQueryEngineStatus;
    initialData->DxgkDdiResetEngine = VioGpuWddmResetEngine;

    if (!renderOnly)
    {
        initialData->DxgkDdiSetPalette = VioGpuWddmSetPalette;
        initialData->DxgkDdiSetPointerPosition = VioGpuDodSetPointerPosition;
        initialData->DxgkDdiSetPointerShape = VioGpuDodSetPointerShape;
        initialData->DxgkDdiIsSupportedVidPn = VioGpuDodIsSupportedVidPn;
        initialData->DxgkDdiRecommendFunctionalVidPn = VioGpuDodRecommendFunctionalVidPn;
        initialData->DxgkDdiEnumVidPnCofuncModality = VioGpuDodEnumVidPnCofuncModality;
        initialData->DxgkDdiSetVidPnSourceAddress = VioGpuWddmSetVidPnSourceAddress;
        initialData->DxgkDdiSetVidPnSourceVisibility = VioGpuDodSetVidPnSourceVisibility;
        initialData->DxgkDdiCommitVidPn = VioGpuDodCommitVidPn;
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
