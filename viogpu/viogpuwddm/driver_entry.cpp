#include "wddmddi.h"

#if !DBG
#include "driver_entry.tmh"
#endif

#pragma code_seg(push)
#pragma code_seg("INIT")

static_assert(DXGKDDI_INTERFACE_VERSION == DXGKDDI_INTERFACE_VERSION_WIN8,
              "viogpuwddm must use the Win8 DDI table for Native Context callbacks");

VOID VioGpuWddmBuildInitializationData(_Out_ DRIVER_INITIALIZATION_DATA *initialData)
{
    RtlZeroMemory(initialData, sizeof(*initialData));
    initialData->Version = DXGKDDI_INTERFACE_VERSION;

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
    initialData->DxgkDdiUnload = VioGpuDodUnload;
    initialData->DxgkDdiQueryInterface = VioGpuDodQueryInterface;

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
    initialData->DxgkDdiPresent = VioGpuWddmPresent;
    initialData->DxgkDdiPatch = VioGpuWddmPatch;
    initialData->DxgkDdiSubmitCommand = VioGpuWddmSubmitCommand;
    initialData->DxgkDdiCancelCommand = VioGpuWddmCancelCommand;
    initialData->DxgkDdiPreemptCommand = VioGpuWddmPreemptCommand;
    initialData->DxgkDdiQueryCurrentFence = VioGpuWddmQueryCurrentFence;
    initialData->DxgkDdiQueryDependentEngineGroup = VioGpuWddmQueryDependentEngineGroup;
    initialData->DxgkDdiQueryEngineStatus = VioGpuWddmQueryEngineStatus;
    initialData->DxgkDdiResetEngine = VioGpuWddmResetEngine;
    initialData->DxgkDdiResetFromTimeout = VioGpuWddmResetFromTimeout;
    initialData->DxgkDdiRestartFromTimeout = VioGpuWddmRestartFromTimeout;
    initialData->DxgkDdiCollectDbgInfo = VioGpuWddmCollectDbgInfo;

    initialData->DxgkDdiSetPointerPosition = VioGpuDodSetPointerPosition;
    initialData->DxgkDdiSetPointerShape = VioGpuDodSetPointerShape;
    initialData->DxgkDdiEscape = VioGpuWddmEscape;
    initialData->DxgkDdiIsSupportedVidPn = VioGpuDodIsSupportedVidPn;
    initialData->DxgkDdiRecommendFunctionalVidPn = VioGpuDodRecommendFunctionalVidPn;
    initialData->DxgkDdiEnumVidPnCofuncModality = VioGpuDodEnumVidPnCofuncModality;
    initialData->DxgkDdiSetVidPnSourceAddress = VioGpuWddmSetVidPnSourceAddress;
    initialData->DxgkDdiSetVidPnSourceVisibility = VioGpuDodSetVidPnSourceVisibility;
    initialData->DxgkDdiCommitVidPn = VioGpuDodCommitVidPn;
    initialData->DxgkDdiUpdateActiveVidPnPresentPath = VioGpuDodUpdateActiveVidPnPresentPath;
    initialData->DxgkDdiRecommendMonitorModes = VioGpuDodRecommendMonitorModes;
    initialData->DxgkDdiQueryVidPnHWCapability = VioGpuDodQueryVidPnHWCapability;
    initialData->DxgkDdiStopDeviceAndReleasePostDisplayOwnership = VioGpuDodStopDeviceAndReleasePostDisplayOwnership;
    initialData->DxgkDdiSystemDisplayEnable = VioGpuDodSystemDisplayEnable;
    initialData->DxgkDdiSystemDisplayWrite = VioGpuDodSystemDisplayWrite;
}

#pragma optimize("", off)
extern "C" NTSTATUS VioGpuWddmInitializeMiniport(_In_ DRIVER_OBJECT *driverObject, _In_ UNICODE_STRING *registryPath)
{
    PAGED_CODE();

    DRIVER_INITIALIZATION_DATA initialData;
    VioGpuWddmBuildInitializationData(&initialData);

    VioGpuSetNamedPoolNotificationDriverObject(driverObject);
    WPP_INIT_TRACING(driverObject, registryPath);
    NTSTATUS status = DxgkInitialize(driverObject, registryPath, &initialData);
    if (!NT_SUCCESS(status))
    {
        WPP_CLEANUP(NULL);
        VioGpuClearNamedPoolNotificationDriverObject();
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
