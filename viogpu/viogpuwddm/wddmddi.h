#pragma once

#include "../viogpudo/driver.h"

// This target is compile-only until VirtIO fence completion and TDR recovery
// are connected to the WDDM scheduler callbacks.
#if !defined(VIOGPU_WDDM_CI_ONLY)
#error viogpuwddm is not installable yet; build it only with VIOGPU_WDDM_CI_ONLY
#endif

class VioGpuDod;

VOID VioGpuWddmBuildInitializationData(_Out_ DRIVER_INITIALIZATION_DATA *initialData);

enum VIOGPU_WDDM_ABI : UINT
{
    VioGpuWddmAllocationPrivateVersion = 1,
    VioGpuWddmCommandMagic = 0x55475056,
    VioGpuWddmCommandVersion = 1,
};

enum VIOGPU_WDDM_COMMAND_OPCODE : UINT
{
    VioGpuWddmCommandNativeSubmit = 1,
};

struct VIOGPU_WDDM_COMMAND_HEADER
{
    UINT Magic;
    UINT Version;
    UINT Size;
    UINT Opcode;
};

struct VIOGPU_WDDM_DMA_PRIVATE
{
    ULONG Signature;
    PVOID DmaBuffer;
    UINT DmaBufferSize;
    UINT CommandLength;
    UINT Flags;
};

enum VIOGPU_WDDM_ALLOCATION_FLAGS : UINT
{
    VioGpuWddmAllocationPrimary = 0x1,
    VioGpuWddmAllocationCpuVisible = 0x2,
};

struct VIOGPU_WDDM_ALLOCATION_PRIVATE
{
    UINT Version;
    UINT Flags;
    ULONGLONG Size;
    UINT Pitch;
    UINT Width;
    UINT Height;
    D3DDDIFORMAT Format;
    UINT RefreshRateNumerator;
    UINT RefreshRateDenominator;
};

struct VIOGPU_WDDM_RESOURCE
{
    ULONG Signature;
};

struct VIOGPU_WDDM_ALLOCATION
{
    ULONG Signature;
    SIZE_T Size;
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
};

struct VIOGPU_WDDM_CONTEXT
{
    ULONG Signature;
    VIOGPU_WDDM_DEVICE *Device;
    HANDLE RuntimeContext;
    UINT NodeOrdinal;
    UINT EngineAffinity;
};

struct VIOGPU_WDDM_OPEN_ALLOCATION
{
    ULONG Signature;
    VIOGPU_WDDM_ALLOCATION *Allocation;
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
