#pragma once

#include "../viogpudo/driver.h"
#include "../viogpudo/viogpudo.h"
#include "../shared/viogpu_wddm_abi.h"

// This target is compile-only until VirtIO fence completion and TDR recovery
// are connected to the WDDM scheduler callbacks.
#if !defined(VIOGPU_WDDM_CI_ONLY)
#error viogpuwddm is not installable yet; build it only with VIOGPU_WDDM_CI_ONLY
#endif

class VioGpuDod;

VOID VioGpuWddmBuildInitializationData(_Out_ DRIVER_INITIALIZATION_DATA *initialData);

struct VIOGPU_WDDM_KMD_DMA_PRIVATE
{
    ULONG Signature;
    PVOID DmaBuffer;
    UINT DmaBufferSize;
    UINT CommandLength;
    UINT ContextId;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT Flags;
};

struct VIOGPU_WDDM_RESOURCE
{
    ULONG Signature;
};

struct VIOGPU_WDDM_ALLOCATION
{
    ULONG Signature;
    VIOGPU_WDDM_ALLOCATION_INFO PrivateData;
    SIZE_T BackingSize;
    UINT Pitch;
    UINT Width;
    UINT Height;
    D3DDDIFORMAT Format;
    UINT Flags;
    UINT RefreshRateNumerator;
    UINT RefreshRateDenominator;
};

struct VIOGPU_WDDM_DEVICE
{
    ULONG Signature;
    VioGpuDod *Adapter;
    HANDLE RuntimeDevice;
    volatile LONG ReferenceState;
};

struct VIOGPU_WDDM_CONTEXT
{
    ULONG Signature;
    EX_RUNDOWN_REF Operations;
    BOOLEAN OperationsRundownCompleted;
    VIOGPU_WDDM_DEVICE *Device;
    HANDLE RuntimeContext;
    UINT NodeOrdinal;
    UINT EngineAffinity;
    VIOGPU_NATIVE_CONTEXT_REGISTRATION NativeContext;
};

struct VIOGPU_WDDM_OPEN_ALLOCATION
{
    ULONG Signature;
    VIOGPU_WDDM_ALLOCATION *Allocation;
    VIOGPU_WDDM_DEVICE *Device;
    BOOLEAN ReadOnly;
};

DXGKDDI_QUERYADAPTERINFO VioGpuWddmQueryAdapterInfo;
DXGKDDI_CREATEALLOCATION VioGpuWddmCreateAllocation;
DXGKDDI_DESTROYALLOCATION VioGpuWddmDestroyAllocation;
DXGKDDI_DESCRIBEALLOCATION VioGpuWddmDescribeAllocation;
DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA VioGpuWddmGetStandardAllocationDriverData;
DXGKDDI_OPENALLOCATIONINFO VioGpuWddmOpenAllocation;
DXGKDDI_CLOSEALLOCATION VioGpuWddmCloseAllocation;
DXGKDDI_CREATEDEVICE VioGpuWddmCreateDevice;
DXGKDDI_DESTROYDEVICE VioGpuWddmDestroyDevice;
DXGKDDI_CREATECONTEXT VioGpuWddmCreateContext;
DXGKDDI_DESTROYCONTEXT VioGpuWddmDestroyContext;
DXGKDDI_ESCAPE VioGpuWddmEscape;
DXGKDDI_BUILDPAGINGBUFFER VioGpuWddmBuildPagingBuffer;
DXGKDDI_RENDER VioGpuWddmRender;
DXGKDDI_PRESENT VioGpuWddmPresent;
DXGKDDI_PATCH VioGpuWddmPatch;
DXGKDDI_SUBMITCOMMAND VioGpuWddmSubmitCommand;
DXGKDDI_PREEMPTCOMMAND VioGpuWddmPreemptCommand;
DXGKDDI_QUERYCURRENTFENCE VioGpuWddmQueryCurrentFence;
DXGKDDI_RESETFROMTIMEOUT VioGpuWddmResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT VioGpuWddmRestartFromTimeout;
DXGKDDI_SETVIDPNSOURCEADDRESS VioGpuWddmSetVidPnSourceAddress;
