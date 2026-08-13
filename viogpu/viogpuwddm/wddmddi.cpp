#include "wddmddi.h"

#include "../common/baseobj.h"
#include "../viogpudo/viogpudo.h"

namespace
{
const ULONG VIOGPU_WDDM_RESOURCE_SIGNATURE = 'rWGV';
const ULONG VIOGPU_WDDM_ALLOCATION_SIGNATURE = 'aWGV';
const ULONG VIOGPU_WDDM_DEVICE_SIGNATURE = 'dWGV';
const ULONG VIOGPU_WDDM_CONTEXT_SIGNATURE = 'cWGV';
const ULONG VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE = 'oWGV';
const ULONG VIOGPU_WDDM_DMA_SIGNATURE = 'mWGV';

const UINT VIOGPU_WDDM_SEGMENT_ID = 1;
const UINT VIOGPU_WDDM_DMA_BUFFER_SIZE = 64 * 1024;
const UINT VIOGPU_WDDM_ALLOCATION_LIST_SIZE = 64;
const UINT VIOGPU_WDDM_PATCH_LIST_SIZE = 128;
const UINT VIOGPU_WDDM_GDI_ALLOCATION_LIST_SIZE = 256;
const UINT VIOGPU_WDDM_GDI_PATCH_LIST_SIZE = 256;
const LONG VIOGPU_WDDM_DEVICE_CLOSING = static_cast<LONG>(0x80000000UL);
const LONG VIOGPU_WDDM_DEVICE_REFERENCE_MASK = 0x7FFFFFFF;

void DereferenceDevice(VIOGPU_WDDM_DEVICE *device);

BOOLEAN ReferenceDevice(VIOGPU_WDDM_DEVICE *device)
{
    if (device == NULL)
    {
        return FALSE;
    }

    LONG state = InterlockedCompareExchange(&device->ReferenceState, 0, 0);
    while ((state & VIOGPU_WDDM_DEVICE_CLOSING) == 0 && state < VIOGPU_WDDM_DEVICE_REFERENCE_MASK)
    {
        LONG observed = InterlockedCompareExchange(&device->ReferenceState, state + 1, state);
        if (observed == state)
        {
            if (device->Signature == VIOGPU_WDDM_DEVICE_SIGNATURE)
            {
                return TRUE;
            }
            DereferenceDevice(device);
            return FALSE;
        }
        state = observed;
    }
    return FALSE;
}

void DereferenceDevice(VIOGPU_WDDM_DEVICE *device)
{
    LONG state = InterlockedDecrement(&device->ReferenceState);
    NT_ASSERT((state & VIOGPU_WDDM_DEVICE_REFERENCE_MASK) != VIOGPU_WDDM_DEVICE_REFERENCE_MASK);
    UNREFERENCED_PARAMETER(state);
}

BOOLEAN IsSupportedSurfaceFormat(D3DDDIFORMAT format)
{
    return format == D3DDDIFMT_A8R8G8B8 || format == D3DDDIFMT_X8R8G8B8;
}

NTSTATUS CalculateSurfaceLayout(UINT width, UINT height, D3DDDIFORMAT format, UINT *pitch, ULONGLONG *size)
{
    if (width == 0 || height == 0 || pitch == NULL || size == NULL || !IsSupportedSurfaceFormat(format) ||
        width > (MAXUINT / 4))
    {
        return STATUS_INVALID_PARAMETER;
    }

    UINT localPitch = width * 4;
    ULONGLONG localSize = (ULONGLONG)localPitch * height;
    if (localSize == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *pitch = localPitch;
    *size = localSize;
    return STATUS_SUCCESS;
}

NTSTATUS ValidateAllocationPrivate(const VIOGPU_WDDM_ALLOCATION_PRIVATE *privateData, SIZE_T *alignedSize)
{
    const UINT validFlags = VioGpuWddmAllocationPrimary | VioGpuWddmAllocationCpuVisible;

    if (privateData == NULL || alignedSize == NULL || privateData->Version != VioGpuWddmAllocationPrivateVersion ||
        (privateData->Flags & ~validFlags) != 0 || privateData->Size == 0 ||
        privateData->Size > (ULONGLONG)(MAXULONG_PTR - (PAGE_SIZE - 1)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    BOOLEAN hasSurfaceLayout = privateData->Width != 0 || privateData->Height != 0 || privateData->Pitch != 0 ||
                               privateData->Format != D3DDDIFMT_UNKNOWN;
    if (hasSurfaceLayout)
    {
        if (privateData->Width == 0 || privateData->Height == 0 || privateData->Pitch == 0 ||
            !IsSupportedSurfaceFormat(privateData->Format) || privateData->Width > (MAXUINT / 4) ||
            privateData->Pitch < privateData->Width * 4)
        {
            return STATUS_INVALID_PARAMETER;
        }

        ULONGLONG minimumSize = (ULONGLONG)privateData->Pitch * privateData->Height;
        if (minimumSize == 0 || privateData->Size < minimumSize)
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    if ((privateData->Flags & VioGpuWddmAllocationPrimary) != 0 &&
        (!hasSurfaceLayout || (privateData->Flags & VioGpuWddmAllocationCpuVisible) != 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    *alignedSize = ((SIZE_T)privateData->Size + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
    return STATUS_SUCCESS;
}

VOID InitializeAllocationInfo(DXGK_ALLOCATIONINFO *allocationInfo,
                              VIOGPU_WDDM_ALLOCATION *allocation,
                              SIZE_T alignedSize)
{
    allocationInfo->Alignment = PAGE_SIZE;
    allocationInfo->Size = alignedSize;
    allocationInfo->PitchAlignedSize = 0;
    allocationInfo->HintedBank.Value = 0;
    allocationInfo->PreferredSegment.Value = 0;
    allocationInfo->PreferredSegment.SegmentId0 = VIOGPU_WDDM_SEGMENT_ID;
    allocationInfo->PreferredSegment.Direction0 = 0;
    allocationInfo->SupportedReadSegmentSet = 1 << (VIOGPU_WDDM_SEGMENT_ID - 1);
    allocationInfo->SupportedWriteSegmentSet = 1 << (VIOGPU_WDDM_SEGMENT_ID - 1);
    allocationInfo->EvictionSegmentSet = 0;
    allocationInfo->MaximumRenamingListLength = 0;
    allocationInfo->Flags.Value = 0;
    allocationInfo->Flags.CpuVisible = (allocation->Flags & VioGpuWddmAllocationCpuVisible) != 0;
    allocationInfo->Flags.Cached = allocationInfo->Flags.CpuVisible;
    allocationInfo->pAllocationUsageHint = NULL;
    allocationInfo->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    allocationInfo->hAllocation = allocation;
}

VOID DestroyCreatedAllocations(DXGK_ALLOCATIONINFO *allocationInfo, UINT count)
{
    for (UINT index = 0; index < count; ++index)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(allocationInfo[index].hAllocation);
        if (allocation != NULL)
        {
            allocation->Signature = 0;
            delete allocation;
            allocationInfo[index].hAllocation = NULL;
        }
    }
}

NTSTATUS QuerySegment(VioGpuDod *adapter, const DXGKARG_QUERYADAPTERINFO *queryAdapterInfo)
{
    if (adapter == NULL || queryAdapterInfo->pOutputData == NULL ||
        queryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    PVOID segmentBase = NULL;
    PHYSICAL_ADDRESS segmentPhysicalAddress = {};
    SIZE_T segmentSize = 0;
    if (!adapter->QueryVidMmSegment(&segmentBase, &segmentPhysicalAddress, &segmentSize) || segmentBase == NULL ||
        segmentSize == 0 || (segmentSize & (PAGE_SIZE - 1)) != 0)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    DXGK_QUERYSEGMENTOUT *segmentInfo = static_cast<DXGK_QUERYSEGMENTOUT *>(queryAdapterInfo->pOutputData);
    segmentInfo->NbSegment = 1;
    segmentInfo->PagingBufferSegmentId = 0;
    segmentInfo->PagingBufferSize = PAGE_SIZE;
    segmentInfo->PagingBufferPrivateDataSize = sizeof(VIOGPU_WDDM_DMA_PRIVATE);

    if (segmentInfo->pSegmentDescriptor != NULL)
    {
        DXGK_SEGMENTDESCRIPTOR *descriptor = segmentInfo->pSegmentDescriptor;
        RtlZeroMemory(descriptor, sizeof(*descriptor));
        descriptor->BaseAddress.QuadPart = 0;
        descriptor->CpuTranslatedAddress = segmentPhysicalAddress;
        descriptor->Size = segmentSize;
        descriptor->CommitLimit = segmentSize;
        descriptor->Flags.CpuVisible = TRUE;
    }

    return STATUS_SUCCESS;
}

NTSTATUS ValidateCommandHeader(const VIOGPU_WDDM_COMMAND_HEADER *header, UINT commandLength)
{
    if (header->Magic != VioGpuWddmCommandMagic || header->Version != VioGpuWddmCommandVersion ||
        header->Size != commandLength || header->Opcode != VioGpuWddmCommandNativeSubmit)
    {
        return STATUS_ILLEGAL_INSTRUCTION;
    }

    return STATUS_SUCCESS;
}
} // namespace

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmQueryAdapterInfo(CONST HANDLE hAdapter,
                                                                    CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    if (hAdapter == NULL || pQueryAdapterInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (pQueryAdapterInfo->Type == DXGKQAITYPE_QUERYSEGMENT)
    {
        return QuerySegment(adapter, pQueryAdapterInfo);
    }

    NTSTATUS status = VioGpuDodQueryAdapterInfo(hAdapter, pQueryAdapterInfo);
    if (NT_SUCCESS(status) && pQueryAdapterInfo->Type == DXGKQAITYPE_DRIVERCAPS)
    {
        DXGK_DRIVERCAPS *driverCaps = static_cast<DXGK_DRIVERCAPS *>(pQueryAdapterInfo->pOutputData);
        driverCaps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
        driverCaps->SchedulingCaps.MultiEngineAware = 1;
        driverCaps->SchedulingCaps.PreemptionAware = 0;
        driverCaps->SchedulingCaps.CancelCommandAware = 0;
    }

    return status;
}

_Use_decl_annotations_ NTSTATUS APIENTRY
VioGpuWddmGetStandardAllocationDriverData(CONST HANDLE hAdapter, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *data)
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (data == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const UINT allocationPrivateDriverDataSize = data->AllocationPrivateDriverDataSize;
    data->ResourcePrivateDriverDataSize = 0;
    data->AllocationPrivateDriverDataSize = sizeof(VIOGPU_WDDM_ALLOCATION_PRIVATE);

    if (data->pAllocationPrivateDriverData == NULL)
    {
        return STATUS_SUCCESS;
    }

    if (allocationPrivateDriverDataSize < sizeof(VIOGPU_WDDM_ALLOCATION_PRIVATE))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    VIOGPU_WDDM_ALLOCATION_PRIVATE *privateData = static_cast<VIOGPU_WDDM_ALLOCATION_PRIVATE *>(data->pAllocationPrivateDriverData);
    RtlZeroMemory(privateData, sizeof(*privateData));
    privateData->Version = VioGpuWddmAllocationPrivateVersion;

    NTSTATUS status;
    switch (data->StandardAllocationType)
    {
        case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
            {
                D3DKMDT_SHAREDPRIMARYSURFACEDATA *surface = data->pCreateSharedPrimarySurfaceData;
                if (surface == NULL)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                status = CalculateSurfaceLayout(surface->Width,
                                                surface->Height,
                                                surface->Format,
                                                &privateData->Pitch,
                                                &privateData->Size);
                if (!NT_SUCCESS(status))
                {
                    return status;
                }

                privateData->Flags = VioGpuWddmAllocationPrimary;
                privateData->Width = surface->Width;
                privateData->Height = surface->Height;
                privateData->Format = surface->Format;
                privateData->RefreshRateNumerator = surface->RefreshRate.Numerator;
                privateData->RefreshRateDenominator = surface->RefreshRate.Denominator;
                return STATUS_SUCCESS;
            }

        case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
            {
                D3DKMDT_SHADOWSURFACEDATA *surface = data->pCreateShadowSurfaceData;
                if (surface == NULL)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                status = CalculateSurfaceLayout(surface->Width,
                                                surface->Height,
                                                surface->Format,
                                                &privateData->Pitch,
                                                &privateData->Size);
                if (!NT_SUCCESS(status))
                {
                    return status;
                }

                surface->Pitch = privateData->Pitch;
                privateData->Flags = VioGpuWddmAllocationCpuVisible;
                privateData->Width = surface->Width;
                privateData->Height = surface->Height;
                privateData->Format = surface->Format;
                return STATUS_SUCCESS;
            }

        case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
            {
                D3DKMDT_STAGINGSURFACEDATA *surface = data->pCreateStagingSurfaceData;
                if (surface == NULL)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                status = CalculateSurfaceLayout(surface->Width,
                                                surface->Height,
                                                D3DDDIFMT_X8R8G8B8,
                                                &privateData->Pitch,
                                                &privateData->Size);
                if (!NT_SUCCESS(status))
                {
                    return status;
                }

                surface->Pitch = privateData->Pitch;
                privateData->Flags = VioGpuWddmAllocationCpuVisible;
                privateData->Width = surface->Width;
                privateData->Height = surface->Height;
                privateData->Format = D3DDDIFMT_X8R8G8B8;
                return STATUS_SUCCESS;
            }

        default:
            return STATUS_NOT_SUPPORTED;
    }
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmCreateAllocation(CONST HANDLE hAdapter,
                                                                    DXGKARG_CREATEALLOCATION *createAllocation)
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (createAllocation == NULL || createAllocation->NumAllocations == 0 ||
        createAllocation->pAllocationInfo == NULL || (createAllocation->Flags.Value & ~1U) != 0 ||
        createAllocation->PrivateDriverDataSize != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_RESOURCE *resource = NULL;
    BOOLEAN createdResource = FALSE;
    if (createAllocation->Flags.Resource)
    {
        resource = reinterpret_cast<VIOGPU_WDDM_RESOURCE *>(createAllocation->hResource);
        if (resource == NULL)
        {
            resource = new (NonPagedPoolNx) VIOGPU_WDDM_RESOURCE;
            if (resource == NULL)
            {
                return STATUS_NO_MEMORY;
            }
            resource->Signature = VIOGPU_WDDM_RESOURCE_SIGNATURE;
            createdResource = TRUE;
        }
        else if (resource->Signature != VIOGPU_WDDM_RESOURCE_SIGNATURE)
        {
            return STATUS_INVALID_HANDLE;
        }
    }

    UINT createdCount = 0;
    NTSTATUS status = STATUS_SUCCESS;
    for (; createdCount < createAllocation->NumAllocations; ++createdCount)
    {
        DXGK_ALLOCATIONINFO *allocationInfo = &createAllocation->pAllocationInfo[createdCount];
        if (allocationInfo->pPrivateDriverData == NULL ||
            allocationInfo->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_ALLOCATION_PRIVATE))
        {
            status = STATUS_GRAPHICS_DRIVER_MISMATCH;
            break;
        }

        VIOGPU_WDDM_ALLOCATION_PRIVATE *privateData = static_cast<VIOGPU_WDDM_ALLOCATION_PRIVATE *>(allocationInfo->pPrivateDriverData);
        SIZE_T alignedSize = 0;
        status = ValidateAllocationPrivate(privateData, &alignedSize);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        VIOGPU_WDDM_ALLOCATION *allocation = new (NonPagedPoolNx) VIOGPU_WDDM_ALLOCATION;
        if (allocation == NULL)
        {
            status = STATUS_NO_MEMORY;
            break;
        }

        RtlZeroMemory(allocation, sizeof(*allocation));
        allocation->Signature = VIOGPU_WDDM_ALLOCATION_SIGNATURE;
        allocation->Size = alignedSize;
        allocation->Pitch = privateData->Pitch;
        allocation->Width = privateData->Width;
        allocation->Height = privateData->Height;
        allocation->Format = privateData->Format;
        allocation->Flags = privateData->Flags;
        allocation->RefreshRateNumerator = privateData->RefreshRateNumerator;
        allocation->RefreshRateDenominator = privateData->RefreshRateDenominator;
        InitializeAllocationInfo(allocationInfo, allocation, alignedSize);
    }

    if (!NT_SUCCESS(status))
    {
        DestroyCreatedAllocations(createAllocation->pAllocationInfo, createdCount);
        if (createdResource)
        {
            resource->Signature = 0;
            delete resource;
        }
        return status;
    }

    if (createdResource)
    {
        createAllocation->hResource = resource;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmDestroyAllocation(CONST HANDLE hAdapter,
                                                                     CONST DXGKARG_DESTROYALLOCATION *destroyAllocation)
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (destroyAllocation == NULL ||
        (destroyAllocation->NumAllocations != 0 && destroyAllocation->pAllocationList == NULL) ||
        (destroyAllocation->Flags.Value & ~1U) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (UINT index = 0; index < destroyAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(destroyAllocation->pAllocationList[index]);
        if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE)
        {
            return STATUS_INVALID_HANDLE;
        }
    }

    VIOGPU_WDDM_RESOURCE *resource = NULL;
    if (destroyAllocation->Flags.DestroyResource)
    {
        resource = reinterpret_cast<VIOGPU_WDDM_RESOURCE *>(destroyAllocation->hResource);
        if (resource == NULL || resource->Signature != VIOGPU_WDDM_RESOURCE_SIGNATURE)
        {
            return STATUS_INVALID_HANDLE;
        }
    }

    for (UINT index = 0; index < destroyAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(destroyAllocation->pAllocationList[index]);
        allocation->Signature = 0;
        delete allocation;
    }

    if (resource != NULL)
    {
        resource->Signature = 0;
        delete resource;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmDescribeAllocation(CONST HANDLE hAdapter,
                                                                      DXGKARG_DESCRIBEALLOCATION *describeAllocation)
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (describeAllocation == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(describeAllocation->hAllocation);
    if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE)
    {
        return STATUS_INVALID_HANDLE;
    }

    describeAllocation->Width = allocation->Width;
    describeAllocation->Height = allocation->Height;
    describeAllocation->Format = allocation->Format;
    describeAllocation->MultisampleMethod.NumSamples = 1;
    describeAllocation->MultisampleMethod.NumQualityLevels = 0;
    describeAllocation->RefreshRate.Numerator = allocation->RefreshRateNumerator;
    describeAllocation->RefreshRate.Denominator = allocation->RefreshRateDenominator;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmOpenAllocation(CONST HANDLE hDevice,
                                                                  CONST DXGKARG_OPENALLOCATION *openAllocation)
{
    VIOGPU_WDDM_DEVICE *device = reinterpret_cast<VIOGPU_WDDM_DEVICE *>(hDevice);
    if (device == NULL || device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE || openAllocation == NULL ||
        openAllocation->NumAllocations == 0 || openAllocation->pOpenAllocation == NULL ||
        openAllocation->SubresourceIndex != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    DXGKRNL_INTERFACE *dxgkInterface = device->Adapter->GetDxgkInterface();
    UINT openedCount = 0;
    NTSTATUS status = STATUS_SUCCESS;
    for (; openedCount < openAllocation->NumAllocations; ++openedCount)
    {
        DXGK_OPENALLOCATIONINFO *openInfo = &openAllocation->pOpenAllocation[openedCount];
        DXGKARGCB_GETHANDLEDATA getHandleData = {};
        getHandleData.hObject = openInfo->hAllocation;
        getHandleData.Type = DXGK_HANDLE_ALLOCATION;
        getHandleData.Flags.Value = 0;

        VIOGPU_WDDM_ALLOCATION *allocation = static_cast<VIOGPU_WDDM_ALLOCATION *>(dxgkInterface->DxgkCbGetHandleData(
                                                                                                            &getHandleData));
        if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE)
        {
            status = STATUS_INVALID_HANDLE;
            break;
        }

        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = new (NonPagedPoolNx) VIOGPU_WDDM_OPEN_ALLOCATION;
        if (deviceAllocation == NULL)
        {
            status = STATUS_NO_MEMORY;
            break;
        }

        deviceAllocation->Signature = VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE;
        deviceAllocation->Allocation = allocation;
        openInfo->hDeviceSpecificAllocation = deviceAllocation;
    }

    if (!NT_SUCCESS(status))
    {
        for (UINT index = 0; index < openedCount; ++index)
        {
            VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(openAllocation->pOpenAllocation[index].hDeviceSpecificAllocation);
            deviceAllocation->Signature = 0;
            delete deviceAllocation;
            openAllocation->pOpenAllocation[index].hDeviceSpecificAllocation = NULL;
        }
        return status;
    }

    DXGKARG_OPENALLOCATION *mutableOpenAllocation = const_cast<DXGKARG_OPENALLOCATION *>(openAllocation);
    mutableOpenAllocation->SubresourceOffset = 0;
    mutableOpenAllocation->Pitch = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(openAllocation->pOpenAllocation[0].hDeviceSpecificAllocation)
                                                                                                                                       ->Allocation
                                                                                                                                       ->Pitch;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmCloseAllocation(CONST HANDLE hDevice,
                                                                   CONST DXGKARG_CLOSEALLOCATION *closeAllocation)
{
    VIOGPU_WDDM_DEVICE *device = reinterpret_cast<VIOGPU_WDDM_DEVICE *>(hDevice);
    if (device == NULL || device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE || closeAllocation == NULL ||
        closeAllocation->NumAllocations == 0 || closeAllocation->pOpenHandleList == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (UINT index = 0; index < closeAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(closeAllocation->pOpenHandleList[index]);
        if (deviceAllocation == NULL || deviceAllocation->Signature != VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE)
        {
            return STATUS_INVALID_HANDLE;
        }
    }

    for (UINT index = 0; index < closeAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(closeAllocation->pOpenHandleList[index]);
        deviceAllocation->Signature = 0;
        delete deviceAllocation;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmCreateDevice(CONST HANDLE hAdapter,
                                                                DXGKARG_CREATEDEVICE *createDevice)
{
    if (hAdapter == NULL || createDevice == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_DEVICE *device = new (NonPagedPoolNx) VIOGPU_WDDM_DEVICE;
    if (device == NULL)
    {
        return STATUS_NO_MEMORY;
    }

    device->Signature = VIOGPU_WDDM_DEVICE_SIGNATURE;
    device->Adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    device->RuntimeDevice = createDevice->hDevice;
    device->ReferenceState = 0;
    createDevice->hDevice = device;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmDestroyDevice(CONST HANDLE hDevice)
{
    VIOGPU_WDDM_DEVICE *device = reinterpret_cast<VIOGPU_WDDM_DEVICE *>(hDevice);
    if (device == NULL || device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE)
    {
        return STATUS_INVALID_HANDLE;
    }
    LONG state = InterlockedCompareExchange(&device->ReferenceState, 0, 0);
    for (;;)
    {
        if ((state & VIOGPU_WDDM_DEVICE_CLOSING) == 0)
        {
            LONG closingState = state | VIOGPU_WDDM_DEVICE_CLOSING;
            LONG observed = InterlockedCompareExchange(&device->ReferenceState, closingState, state);
            if (observed != state)
            {
                state = observed;
                continue;
            }
            state = closingState;
        }
        if ((state & VIOGPU_WDDM_DEVICE_REFERENCE_MASK) != 0)
        {
            return STATUS_DEVICE_BUSY;
        }
        break;
    }

    device->Signature = 0;
    delete device;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmCreateContext(CONST HANDLE hDevice,
                                                                 DXGKARG_CREATECONTEXT *createContext)
{
    VIOGPU_WDDM_DEVICE *device = reinterpret_cast<VIOGPU_WDDM_DEVICE *>(hDevice);
    if (device == NULL || createContext == NULL || createContext->NodeOrdinal != 0 || createContext->EngineAffinity > 1)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!ReferenceDevice(device))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_WDDM_CONTEXT *context = new (NonPagedPoolNx) VIOGPU_WDDM_CONTEXT;
    if (context == NULL)
    {
        DereferenceDevice(device);
        return STATUS_NO_MEMORY;
    }

    RtlZeroMemory(context, sizeof(*context));
    context->Signature = VIOGPU_WDDM_CONTEXT_SIGNATURE;
    ExInitializeRundownProtection(&context->Operations);
    context->OperationsRundownCompleted = FALSE;
    context->Device = device;
    context->RuntimeContext = NULL;
    context->NodeOrdinal = createContext->NodeOrdinal;
    context->EngineAffinity = createContext->EngineAffinity;
    KeInitializeSpinLock(&context->NativeContext.BindingLock);
    context->NativeContext.State = VioGpuNativeContextAllocated;

    NTSTATUS status = device->Adapter->CreateNativeContext(&context->NativeContext);
    if (!NT_SUCCESS(status))
    {
        context->Signature = 0;
        delete context;
        DereferenceDevice(device);
        return status;
    }

    RtlZeroMemory(&createContext->ContextInfo, sizeof(createContext->ContextInfo));
    createContext->ContextInfo.DmaBufferSize = VIOGPU_WDDM_DMA_BUFFER_SIZE;
    createContext->ContextInfo.DmaBufferSegmentSet = 0;
    createContext->ContextInfo.DmaBufferPrivateDataSize = sizeof(VIOGPU_WDDM_DMA_PRIVATE);
    createContext->ContextInfo.AllocationListSize = createContext->Flags.GdiContext ? VIOGPU_WDDM_GDI_ALLOCATION_LIST_SIZE
                                                                                    : VIOGPU_WDDM_ALLOCATION_LIST_SIZE;
    createContext->ContextInfo.PatchLocationListSize = createContext->Flags.GdiContext ? VIOGPU_WDDM_GDI_PATCH_LIST_SIZE
                                                                                       : VIOGPU_WDDM_PATCH_LIST_SIZE;
    createContext->hContext = context;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmDestroyContext(CONST HANDLE hContext)
{
    VIOGPU_WDDM_CONTEXT *context = reinterpret_cast<VIOGPU_WDDM_CONTEXT *>(hContext);
    if (context == NULL || context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_HANDLE;
    }

    if (!context->OperationsRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&context->Operations);
        ExRundownCompleted(&context->Operations);
        context->OperationsRundownCompleted = TRUE;
    }

    BOOLEAN released = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    if (context->Device != NULL && context->Device->Adapter != NULL)
    {
        status = context->Device->Adapter->DestroyNativeContext(&context->NativeContext, &released);
    }
    if (!released && VioGpuAdapter::IsNativeContextReleased(&context->NativeContext))
    {
        released = TRUE;
    }
    if (!released)
    {
        return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;
    }

    VIOGPU_WDDM_DEVICE *device = context->Device;
    context->Signature = 0;
    delete context;
    DereferenceDevice(device);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmBuildPagingBuffer(CONST HANDLE hAdapter,
                                                                     DXGKARG_BUILDPAGINGBUFFER *pagingBuffer)
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (pagingBuffer == NULL || pagingBuffer->pDmaBuffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (pagingBuffer->Operation == DXGK_OPERATION_DISCARD_CONTENT)
    {
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmRender(CONST HANDLE hContext, DXGKARG_RENDER *render)
{
    VIOGPU_WDDM_CONTEXT *context = reinterpret_cast<VIOGPU_WDDM_CONTEXT *>(hContext);
    if (context == NULL || render == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL || render->pCommand == NULL ||
        render->CommandLength < sizeof(VIOGPU_WDDM_COMMAND_HEADER) || render->pDmaBuffer == NULL ||
        render->pDmaBufferPrivateData == NULL || render->DmaBufferPrivateDataSize < sizeof(VIOGPU_WDDM_DMA_PRIVATE) ||
        render->MultipassOffset != 0 ||
        (render->PatchLocationListInSize != 0 && render->pPatchLocationListIn == NULL) ||
        (render->PatchLocationListInSize != 0 && render->pPatchLocationListOut == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (!ExAcquireRundownProtection(&context->Operations))
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE)
    {
        ExReleaseRundownProtection(&context->Operations);
        return STATUS_INVALID_HANDLE;
    }
    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
    if (!VioGpuAdapter::AcquireNativeContextSnapshot(&context->NativeContext, &snapshot))
    {
        ExReleaseRundownProtection(&context->Operations);
        return STATUS_DEVICE_NOT_READY;
    }

    if (render->DmaSize < render->CommandLength || render->PatchLocationListOutSize < render->PatchLocationListInSize)
    {
        VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
        ExReleaseRundownProtection(&context->Operations);
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }

    NTSTATUS status = STATUS_SUCCESS;
    VIOGPU_WDDM_COMMAND_HEADER header = {};
    __try
    {
        ProbeForRead(const_cast<PVOID>(render->pCommand), render->CommandLength, 1);
        RtlCopyMemory(&header, render->pCommand, sizeof(header));
        status = ValidateCommandHeader(&header, render->CommandLength);
        if (!NT_SUCCESS(status))
        {
            __leave;
        }

        if (render->PatchLocationListInSize != 0)
        {
            SIZE_T patchBytes = (SIZE_T)render->PatchLocationListInSize * sizeof(D3DDDI_PATCHLOCATIONLIST);
            ProbeForRead(render->pPatchLocationListIn, patchBytes, __alignof(D3DDDI_PATCHLOCATIONLIST));
            for (UINT index = 0; index < render->PatchLocationListInSize; ++index)
            {
                const D3DDDI_PATCHLOCATIONLIST *patch = &render->pPatchLocationListIn[index];
                if (patch->AllocationIndex >= render->AllocationListSize ||
                    patch->PatchOffset > render->CommandLength - sizeof(ULONGLONG))
                {
                    status = STATUS_INVALID_USER_BUFFER;
                    __leave;
                }
            }
        }

        RtlCopyMemory(render->pDmaBuffer, render->pCommand, render->CommandLength);
        if (render->PatchLocationListInSize != 0)
        {
            RtlCopyMemory(render->pPatchLocationListOut,
                          render->pPatchLocationListIn,
                          (SIZE_T)render->PatchLocationListInSize * sizeof(D3DDDI_PATCHLOCATIONLIST));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = STATUS_INVALID_USER_BUFFER;
    }

    if (!NT_SUCCESS(status))
    {
        VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
        ExReleaseRundownProtection(&context->Operations);
        return status;
    }

    if (!snapshot.Adapter->IsNativeContextGenerationCurrent(snapshot.Generation))
    {
        VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
        ExReleaseRundownProtection(&context->Operations);
        return STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_WDDM_DMA_PRIVATE *privateData = static_cast<VIOGPU_WDDM_DMA_PRIVATE *>(render->pDmaBufferPrivateData);
    RtlZeroMemory(privateData, sizeof(*privateData));
    privateData->Signature = VIOGPU_WDDM_DMA_SIGNATURE;
    privateData->DmaBuffer = render->pDmaBuffer;
    privateData->DmaBufferSize = render->DmaSize;
    privateData->CommandLength = render->CommandLength;
    privateData->ContextId = snapshot.ContextId;
    privateData->Generation = snapshot.Generation;

    render->pDmaBuffer = static_cast<BYTE *>(render->pDmaBuffer) + render->CommandLength;
    render->pPatchLocationListOut += render->PatchLocationListInSize;
    render->MultipassOffset = render->CommandLength;
    VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
    ExReleaseRundownProtection(&context->Operations);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmPatch(CONST HANDLE hAdapter, CONST DXGKARG_PATCH *patchArguments)
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (patchArguments == NULL || patchArguments->pDmaBuffer == NULL ||
        patchArguments->DmaBufferSubmissionStartOffset > patchArguments->DmaBufferSubmissionEndOffset ||
        patchArguments->DmaBufferSubmissionEndOffset > patchArguments->DmaBufferSize ||
        patchArguments->PatchLocationListSubmissionStart > patchArguments->PatchLocationListSize ||
        patchArguments->PatchLocationListSubmissionLength > patchArguments->PatchLocationListSize - patchArguments->PatchLocationListSubmissionStart ||
        (patchArguments->PatchLocationListSubmissionLength != 0 &&
         (patchArguments->pPatchLocationList == NULL || patchArguments->pAllocationList == NULL)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (patchArguments->PatchLocationListSubmissionLength != 0 &&
        patchArguments->DmaBufferSubmissionEndOffset - patchArguments->DmaBufferSubmissionStartOffset < sizeof(ULONGLONG))
    {
        return STATUS_INVALID_PARAMETER;
    }

    const D3DDDI_PATCHLOCATIONLIST *patchList = patchArguments->PatchLocationListSubmissionLength == 0 ? NULL
                                                                                                       : patchArguments->pPatchLocationList + patchArguments->PatchLocationListSubmissionStart;
    for (UINT index = 0; index < patchArguments->PatchLocationListSubmissionLength; ++index)
    {
        const D3DDDI_PATCHLOCATIONLIST *patch = &patchList[index];
        if (patch->AllocationIndex >= patchArguments->AllocationListSize ||
            patch->PatchOffset < patchArguments->DmaBufferSubmissionStartOffset ||
            patch->PatchOffset > patchArguments->DmaBufferSubmissionEndOffset - sizeof(ULONGLONG))
        {
            return STATUS_INVALID_PARAMETER;
        }

        const DXGK_ALLOCATIONLIST *allocationEntry = &patchArguments->pAllocationList[patch->AllocationIndex];
        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationEntry->hDeviceSpecificAllocation);
        if (deviceAllocation == NULL || deviceAllocation->Signature != VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE ||
            deviceAllocation->Allocation == NULL || patch->AllocationOffset >= deviceAllocation->Allocation->Size ||
            allocationEntry->SegmentId > VIOGPU_WDDM_SEGMENT_ID)
        {
            return STATUS_INVALID_HANDLE;
        }

        ULONGLONG gpuAddress = 0;
        if (allocationEntry->SegmentId == VIOGPU_WDDM_SEGMENT_ID)
        {
            gpuAddress = allocationEntry->PhysicalAddress.QuadPart + patch->AllocationOffset;
        }
        RtlCopyMemory(static_cast<BYTE *>(patchArguments->pDmaBuffer) + patch->PatchOffset,
                      &gpuAddress,
                      sizeof(gpuAddress));
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmPresent(CONST HANDLE hContext, DXGKARG_PRESENT *present)
{
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(present);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmSubmitCommand(CONST HANDLE hAdapter,
                                                                 CONST DXGKARG_SUBMITCOMMAND *submitCommand)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(submitCommand);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmPreemptCommand(CONST HANDLE hAdapter,
                                                                  CONST DXGKARG_PREEMPTCOMMAND *preemptCommand)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(preemptCommand);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmQueryCurrentFence(CONST HANDLE hAdapter,
                                                                     DXGKARG_QUERYCURRENTFENCE *currentFence)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(currentFence);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmResetFromTimeout(CONST HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmRestartFromTimeout(CONST HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_ NTSTATUS APIENTRY
VioGpuWddmSetVidPnSourceAddress(CONST HANDLE hAdapter, CONST DXGKARG_SETVIDPNSOURCEADDRESS *setVidPnSourceAddress)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(setVidPnSourceAddress);
    return STATUS_NOT_SUPPORTED;
}
