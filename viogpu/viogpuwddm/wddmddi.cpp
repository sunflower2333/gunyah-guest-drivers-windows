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

VIOGPU_WDDM_UINT32 ToPrivateFormat(D3DDDIFORMAT format)
{
    switch (format)
    {
        case D3DDDIFMT_A8R8G8B8:
            return VIOGPU_WDDM_FORMAT_B8G8R8A8_UNORM;
        case D3DDDIFMT_X8R8G8B8:
            return VIOGPU_WDDM_FORMAT_B8G8R8X8_UNORM;
        default:
            return VIOGPU_WDDM_FORMAT_NONE;
    }
}

D3DDDIFORMAT FromPrivateFormat(VIOGPU_WDDM_UINT32 format)
{
    switch (format)
    {
        case VIOGPU_WDDM_FORMAT_B8G8R8A8_UNORM:
            return D3DDDIFMT_A8R8G8B8;
        case VIOGPU_WDDM_FORMAT_B8G8R8X8_UNORM:
            return D3DDDIFMT_X8R8G8B8;
        default:
            return D3DDDIFMT_UNKNOWN;
    }
}

VOID InitializeAbiHeader(VIOGPU_WDDM_ABI_HEADER *header, VIOGPU_WDDM_UINT32 size)
{
    RtlZeroMemory(header, size);
    header->Magic = VIOGPU_WDDM_ABI_MAGIC;
    header->Version = VIOGPU_WDDM_ABI_VERSION;
    header->Size = size;
}

BOOLEAN IsCurrentAbiHeader(const VIOGPU_WDDM_ABI_HEADER *header, VIOGPU_WDDM_UINT32 size)
{
    return header != NULL && header->Magic == VIOGPU_WDDM_ABI_MAGIC && header->Version == VIOGPU_WDDM_ABI_VERSION &&
           header->Size == size && header->Reserved == 0;
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

NTSTATUS ValidateAllocationPrivate(const VIOGPU_WDDM_ALLOCATION_INFO *privateData, SIZE_T *alignedSize)
{
    const VIOGPU_WDDM_UINT32 validFlags = VIOGPU_WDDM_ALLOCATION_PRIMARY | VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;
    D3DDDIFORMAT format = privateData == NULL ? D3DDDIFMT_UNKNOWN : FromPrivateFormat(privateData->Format);

    if (privateData == NULL || alignedSize == NULL || !IsCurrentAbiHeader(&privateData->Header, sizeof(*privateData)) ||
        (privateData->Flags & ~validFlags) != 0 || privateData->Size == 0 ||
        privateData->Size > (ULONGLONG)(MAXULONG_PTR - (PAGE_SIZE - 1)) || privateData->Alignment != PAGE_SIZE ||
        privateData->Reserved != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    BOOLEAN hasSurfaceLayout = privateData->Width != 0 || privateData->Height != 0 || privateData->Pitch != 0 ||
                               privateData->Format != VIOGPU_WDDM_FORMAT_NONE;
    if (hasSurfaceLayout)
    {
        if (privateData->Width == 0 || privateData->Height == 0 || privateData->Pitch == 0 ||
            !IsSupportedSurfaceFormat(format) || privateData->Width > (MAXUINT / 4) ||
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
    else if (privateData->RefreshRateNumerator != 0 || privateData->RefreshRateDenominator != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((privateData->Flags & VIOGPU_WDDM_ALLOCATION_PRIMARY) != 0 &&
        (!hasSurfaceLayout || (privateData->Flags & VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE) != 0 ||
         privateData->RefreshRateNumerator == 0 || privateData->RefreshRateDenominator == 0))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if ((privateData->Flags & VIOGPU_WDDM_ALLOCATION_PRIMARY) == 0 &&
        (privateData->RefreshRateNumerator != 0 || privateData->RefreshRateDenominator != 0))
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
    allocationInfo->Flags.CpuVisible = (allocation->Flags & VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE) != 0;
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

    PHYSICAL_ADDRESS segmentPhysicalAddress = {};
    SIZE_T segmentSize = 0;
    if (!adapter->QueryVidMmSegment(&segmentPhysicalAddress, &segmentSize) || segmentPhysicalAddress.QuadPart < 0 ||
        ((ULONG64)segmentPhysicalAddress.QuadPart & (PAGE_SIZE - 1)) != 0 || segmentSize < PAGE_SIZE ||
        (segmentSize & (PAGE_SIZE - 1)) != 0 ||
        (ULONG64)segmentPhysicalAddress.QuadPart > MAXULONGLONG - (segmentSize - 1))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    DXGK_QUERYSEGMENTOUT *segmentInfo = static_cast<DXGK_QUERYSEGMENTOUT *>(queryAdapterInfo->pOutputData);
    segmentInfo->NbSegment = 1;
    segmentInfo->PagingBufferSegmentId = 0;
    segmentInfo->PagingBufferSize = PAGE_SIZE;
    segmentInfo->PagingBufferPrivateDataSize = sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE);

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

NTSTATUS QueryUmdPrivateInfo(VioGpuDod *adapter, const DXGKARG_QUERYADAPTERINFO *queryAdapterInfo)
{
    if (adapter == NULL || queryAdapterInfo->pInputData != NULL || queryAdapterInfo->InputDataSize != 0 ||
        queryAdapterInfo->pOutputData == NULL || queryAdapterInfo->OutputDataSize != sizeof(VIOGPU_WDDM_ADAPTER_INFO))
    {
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    }

    GPU_CAPSET_DRM capset = {};
    ULONGLONG resetGeneration = 0;
    if (!adapter->QueryNativeContextReadiness(&capset, NULL, NULL, &resetGeneration) || resetGeneration == 0)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_WDDM_ADAPTER_INFO *adapterInfo = static_cast<VIOGPU_WDDM_ADAPTER_INFO *>(queryAdapterInfo->pOutputData);
    InitializeAbiHeader(&adapterInfo->Header, sizeof(*adapterInfo));
    adapterInfo->Capabilities = VIOGPU_WDDM_CAPABILITIES_NONE;
    adapterInfo->ResetGeneration = resetGeneration;
    adapterInfo->MsmMajorVersion = capset.version_major;
    adapterInfo->MsmMinorVersion = capset.version_minor;
    adapterInfo->MsmPatchVersion = capset.version_patchlevel;
    adapterInfo->GpuId = capset.msm.gpu_id;
    adapterInfo->ChipId = capset.msm.chip_id;
    adapterInfo->GmemSize = capset.msm.gmem_size;
    adapterInfo->PriorityCount = capset.msm.priorities;
    adapterInfo->GmemBase = capset.msm.gmem_base;
    adapterInfo->HighestBankBit = capset.msm.highest_bank_bit;
    adapterInfo->HasCachedCoherentMemory = capset.msm.has_cached_coherent;
    adapterInfo->UbwcSwizzle = capset.msm.ubwc_swizzle;
    adapterInfo->MacrotileMode = capset.msm.macrotile_mode;
    adapterInfo->UcheTrapBase = capset.msm.uche_trap_base;
    adapterInfo->HasRayTracing = capset.msm.has_raytracing;
    adapterInfo->MaxFrequency = capset.msm.max_freq;
    return STATUS_SUCCESS;
}

#pragma code_seg(push)
#pragma code_seg("PAGE")

NTSTATUS QueryContextInfo(VioGpuDod *adapter, const DXGKARG_ESCAPE *escape)
{
    PAGED_CODE();

    if (adapter == NULL || escape == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL || escape->hDevice == NULL ||
        escape->hContext == NULL || escape->Flags.Value != 0 || escape->pPrivateDriverData == NULL ||
        escape->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_CONTEXT_INFO))
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_CONTEXT_INFO request = {};
    __try
    {
        RtlCopyMemory(&request, escape->pPrivateDriverData, sizeof(request));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return STATUS_INVALID_USER_BUFFER;
    }

    if (!IsCurrentAbiHeader(&request.Header, sizeof(request)) || request.Opcode != VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO)
    {
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    }
    if (request.Flags != VIOGPU_WDDM_ESCAPE_FLAGS_NONE || request.ExpectedResetGeneration == 0 ||
        request.VaStart != 0 || request.VaSize != 0 || request.ResetGeneration != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_CONTEXT *context = reinterpret_cast<VIOGPU_WDDM_CONTEXT *>(escape->hContext);
    if (!ExAcquireRundownProtection(&context->Operations))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    NTSTATUS status = STATUS_SUCCESS;
    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
    BOOLEAN snapshotAcquired = FALSE;
    VIOGPU_WDDM_DEVICE *device = reinterpret_cast<VIOGPU_WDDM_DEVICE *>(escape->hDevice);
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE || context->Device != device ||
        device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE || device->Adapter != adapter)
    {
        status = STATUS_INVALID_HANDLE;
    }
    else if (!adapter->IsDriverActive())
    {
        status = STATUS_DEVICE_NOT_READY;
    }
    else if (!VioGpuAdapter::AcquireNativeContextSnapshot(&context->NativeContext, &snapshot))
    {
        status = STATUS_DEVICE_NOT_READY;
    }
    else
    {
        snapshotAcquired = TRUE;
        ULONGLONG vaEnd = snapshot.VaStart + snapshot.VaSize;
        if (snapshot.ResetGeneration != request.ExpectedResetGeneration || snapshot.VaStart == 0 ||
            snapshot.VaSize == 0 || (snapshot.VaStart & (PAGE_SIZE - 1)) != 0 ||
            (snapshot.VaSize & (PAGE_SIZE - 1)) != 0 || vaEnd < snapshot.VaStart)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
    }

    if (NT_SUCCESS(status))
    {
        VIOGPU_WDDM_CONTEXT_INFO response = {};
        InitializeAbiHeader(&response.Header, sizeof(response));
        response.Opcode = VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO;
        response.Flags = VIOGPU_WDDM_ESCAPE_FLAGS_NONE;
        response.ExpectedResetGeneration = request.ExpectedResetGeneration;
        response.VaStart = snapshot.VaStart;
        response.VaSize = snapshot.VaSize;
        response.ResetGeneration = snapshot.ResetGeneration;
        __try
        {
            RtlCopyMemory(escape->pPrivateDriverData, &response, sizeof(response));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            status = STATUS_INVALID_USER_BUFFER;
        }
    }

    if (snapshotAcquired)
    {
        VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
    }
    ExReleaseRundownProtection(&context->Operations);
    return status;
}

#pragma code_seg(pop)

NTSTATUS ValidateCommandHeader(const VIOGPU_WDDM_RENDER_COMMAND *header,
                               UINT commandLength,
                               VIOGPU_WDDM_DEVICE *device,
                               const DXGK_ALLOCATIONLIST *allocationList,
                               UINT allocationListSize,
                               const D3DDDI_PATCHLOCATIONLIST *patchList,
                               UINT patchListSize,
                               ULONGLONG resetGeneration)
{
    const VIOGPU_WDDM_UINT32 validReferenceFlags = VIOGPU_WDDM_REFERENCE_READ | VIOGPU_WDDM_REFERENCE_WRITE;
    ULONGLONG referencesSize = (ULONGLONG)header->AllocationReferenceCount * sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE);
    ULONGLONG referencesEnd = (ULONGLONG)header->AllocationReferencesOffset + referencesSize;
    ULONGLONG commandEnd = (ULONGLONG)header->CommandStreamOffset + header->CommandStreamSize;

    if (!IsCurrentAbiHeader(&header->Header, commandLength) || header->Opcode != VIOGPU_WDDM_RENDER_NATIVE_SUBMIT ||
        header->Flags != VIOGPU_WDDM_RENDER_FLAGS_NONE || header->ExpectedResetGeneration != resetGeneration ||
        resetGeneration == 0 || header->AllocationReferenceCount == 0 ||
        header->AllocationReferenceCount != patchListSize ||
        (header->AllocationReferenceCount != 0 && (allocationList == NULL || patchList == NULL)) ||
        header->AllocationReferencesOffset != sizeof(*header) || referencesEnd > commandLength ||
        header->CommandStreamOffset != referencesEnd || header->CommandStreamSize < sizeof(ULONGLONG) ||
        commandEnd != commandLength)
    {
        return STATUS_ILLEGAL_INSTRUCTION;
    }

    for (UINT index = 0; index < ARRAYSIZE(header->Reserved); ++index)
    {
        if (header->Reserved[index] != 0)
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    const VIOGPU_WDDM_ALLOCATION_REFERENCE *references = reinterpret_cast<const VIOGPU_WDDM_ALLOCATION_REFERENCE *>(
                                                                                                        reinterpret_cast<const BYTE *>(header) +
                                                                                                        header->AllocationReferencesOffset);
    for (UINT index = 0; index < header->AllocationReferenceCount; ++index)
    {
        const VIOGPU_WDDM_ALLOCATION_REFERENCE *reference = &references[index];
        const D3DDDI_PATCHLOCATIONLIST *patch = &patchList[index];
        if (reference->AllocationIndex >= allocationListSize || reference->AllocationIndex != patch->AllocationIndex ||
            reference->Flags == 0 || (reference->Flags & ~validReferenceFlags) != 0 || reference->Length == 0 ||
            reference->Reserved != 0 || reference->AllocationOffset + reference->Length < reference->AllocationOffset ||
            patch->PatchOffset < header->CommandStreamOffset ||
            reference->AllocationOffset != patch->AllocationOffset ||
            reference->PatchOffset != patch->PatchOffset - header->CommandStreamOffset ||
            reference->PatchOffset > header->CommandStreamSize - sizeof(ULONGLONG) || patch->Reserved != 0 ||
            patch->DriverId != 0 || patch->SplitOffset != 0)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const DXGK_ALLOCATIONLIST *allocationEntry = &allocationList[reference->AllocationIndex];
        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationEntry->hDeviceSpecificAllocation);
        if (deviceAllocation == NULL || deviceAllocation->Signature != VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE ||
            deviceAllocation->Device != device || deviceAllocation->Allocation == NULL ||
            deviceAllocation->Allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
            reference->AllocationOffset > deviceAllocation->Allocation->PrivateData.Size ||
            reference->Length > deviceAllocation->Allocation->PrivateData.Size - reference->AllocationOffset ||
            allocationEntry->Reserved != 0 ||
            ((reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0) != (allocationEntry->WriteOperation != 0) ||
            (deviceAllocation->ReadOnly && (reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0))
        {
            return STATUS_INVALID_HANDLE;
        }

        for (UINT previousIndex = 0; previousIndex < index; ++previousIndex)
        {
            const D3DDDI_PATCHLOCATIONLIST *previousPatch = &patchList[previousIndex];
            if (patch->PatchOffset < previousPatch->PatchOffset + sizeof(ULONGLONG) &&
                previousPatch->PatchOffset < patch->PatchOffset + sizeof(ULONGLONG))
            {
                return STATUS_INVALID_PARAMETER;
            }
        }
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
    if (pQueryAdapterInfo->Type == DXGKQAITYPE_UMDRIVERPRIVATE)
    {
        return QueryUmdPrivateInfo(adapter, pQueryAdapterInfo);
    }
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

#pragma code_seg(push)
#pragma code_seg("PAGE")

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmEscape(CONST HANDLE hAdapter, CONST DXGKARG_ESCAPE *escape)
{
    PAGED_CODE();

    if (hAdapter == NULL || escape == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (escape->hContext != NULL || escape->PrivateDriverDataSize == sizeof(VIOGPU_WDDM_CONTEXT_INFO))
    {
        return QueryContextInfo(reinterpret_cast<VioGpuDod *>(hAdapter), escape);
    }
    return VioGpuDodEscape(hAdapter, escape);
}

#pragma code_seg(pop)

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
    data->AllocationPrivateDriverDataSize = sizeof(VIOGPU_WDDM_ALLOCATION_INFO);

    if (data->pAllocationPrivateDriverData == NULL)
    {
        return STATUS_SUCCESS;
    }

    if (allocationPrivateDriverDataSize < sizeof(VIOGPU_WDDM_ALLOCATION_INFO))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    VIOGPU_WDDM_ALLOCATION_INFO *privateData = static_cast<VIOGPU_WDDM_ALLOCATION_INFO *>(data->pAllocationPrivateDriverData);
    InitializeAbiHeader(&privateData->Header, sizeof(*privateData));
    privateData->Alignment = PAGE_SIZE;

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

                privateData->Flags = VIOGPU_WDDM_ALLOCATION_PRIMARY;
                privateData->Width = surface->Width;
                privateData->Height = surface->Height;
                privateData->Format = ToPrivateFormat(surface->Format);
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
                privateData->Flags = VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;
                privateData->Width = surface->Width;
                privateData->Height = surface->Height;
                privateData->Format = ToPrivateFormat(surface->Format);
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
                privateData->Flags = VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;
                privateData->Width = surface->Width;
                privateData->Height = surface->Height;
                privateData->Format = VIOGPU_WDDM_FORMAT_B8G8R8X8_UNORM;
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
        createAllocation->pPrivateDriverData != NULL || createAllocation->PrivateDriverDataSize != 0)
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
            allocationInfo->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_ALLOCATION_INFO))
        {
            status = STATUS_GRAPHICS_DRIVER_MISMATCH;
            break;
        }

        VIOGPU_WDDM_ALLOCATION_INFO privateData = {};
        RtlCopyMemory(&privateData, allocationInfo->pPrivateDriverData, sizeof(privateData));
        SIZE_T alignedSize = 0;
        status = ValidateAllocationPrivate(&privateData, &alignedSize);
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
        allocation->PrivateData = privateData;
        allocation->BackingSize = alignedSize;
        allocation->Pitch = privateData.Pitch;
        allocation->Width = privateData.Width;
        allocation->Height = privateData.Height;
        allocation->Format = FromPrivateFormat(privateData.Format);
        allocation->Flags = privateData.Flags;
        allocation->RefreshRateNumerator = privateData.RefreshRateNumerator;
        allocation->RefreshRateDenominator = privateData.RefreshRateDenominator;
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
        for (UINT previousIndex = 0; previousIndex < index; ++previousIndex)
        {
            if (destroyAllocation->pAllocationList[previousIndex] == destroyAllocation->pAllocationList[index])
            {
                return STATUS_INVALID_PARAMETER;
            }
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
        openAllocation->SubresourceIndex != 0 || (openAllocation->Flags.Value & ~3U) != 0 ||
        openAllocation->pPrivateDriverData != NULL || openAllocation->PrivateDriverSize != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    DXGKRNL_INTERFACE *dxgkInterface = device->Adapter->GetDxgkInterface();
    UINT openedCount = 0;
    NTSTATUS status = STATUS_SUCCESS;
    for (; openedCount < openAllocation->NumAllocations; ++openedCount)
    {
        DXGK_OPENALLOCATIONINFO *openInfo = &openAllocation->pOpenAllocation[openedCount];
        if (openInfo->pPrivateDriverData == NULL ||
            openInfo->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_ALLOCATION_INFO))
        {
            status = STATUS_GRAPHICS_DRIVER_MISMATCH;
            break;
        }

        VIOGPU_WDDM_ALLOCATION_INFO privateData = {};
        RtlCopyMemory(&privateData, openInfo->pPrivateDriverData, sizeof(privateData));
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
        if (RtlCompareMemory(&privateData, &allocation->PrivateData, sizeof(privateData)) != sizeof(privateData))
        {
            status = STATUS_GRAPHICS_DRIVER_MISMATCH;
            break;
        }

        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = new (NonPagedPoolNx) VIOGPU_WDDM_OPEN_ALLOCATION;
        if (deviceAllocation == NULL)
        {
            status = STATUS_NO_MEMORY;
            break;
        }
        if (!ReferenceDevice(device))
        {
            delete deviceAllocation;
            status = STATUS_DEVICE_NOT_READY;
            break;
        }

        deviceAllocation->Signature = VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE;
        deviceAllocation->Allocation = allocation;
        deviceAllocation->Device = device;
        deviceAllocation->ReadOnly = openAllocation->Flags.ReadOnly;
        openInfo->hDeviceSpecificAllocation = deviceAllocation;
    }

    if (!NT_SUCCESS(status))
    {
        for (UINT index = 0; index < openedCount; ++index)
        {
            VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(openAllocation->pOpenAllocation[index].hDeviceSpecificAllocation);
            deviceAllocation->Signature = 0;
            DereferenceDevice(deviceAllocation->Device);
            delete deviceAllocation;
            openAllocation->pOpenAllocation[index].hDeviceSpecificAllocation = NULL;
        }
        return status;
    }

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
        if (deviceAllocation == NULL || deviceAllocation->Signature != VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE ||
            deviceAllocation->Device != device)
        {
            return STATUS_INVALID_HANDLE;
        }
        for (UINT previousIndex = 0; previousIndex < index; ++previousIndex)
        {
            if (closeAllocation->pOpenHandleList[previousIndex] == closeAllocation->pOpenHandleList[index])
            {
                return STATUS_INVALID_PARAMETER;
            }
        }
    }

    for (UINT index = 0; index < closeAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(closeAllocation->pOpenHandleList[index]);
        deviceAllocation->Signature = 0;
        DereferenceDevice(deviceAllocation->Device);
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
    if (device == NULL || createContext == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        createContext->NodeOrdinal != 0 || createContext->EngineAffinity != 1 || createContext->Flags.Value != 0 ||
        createContext->pPrivateDriverData == NULL ||
        createContext->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_CONTEXT_CREATE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_CONTEXT_CREATE privateData = {};
    __try
    {
        RtlCopyMemory(&privateData, createContext->pPrivateDriverData, sizeof(privateData));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return STATUS_INVALID_USER_BUFFER;
    }
    if (!IsCurrentAbiHeader(&privateData.Header, sizeof(privateData)) || privateData.ExpectedResetGeneration == 0 ||
        privateData.Flags != VIOGPU_WDDM_CONTEXT_FLAGS_NONE || privateData.Reserved != 0)
    {
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
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

    NTSTATUS status = device->Adapter->CreateNativeContext(&context->NativeContext,
                                                           privateData.ExpectedResetGeneration);
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
    createContext->ContextInfo.DmaBufferPrivateDataSize = sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE);
    createContext->ContextInfo.AllocationListSize = VIOGPU_WDDM_ALLOCATION_LIST_SIZE;
    createContext->ContextInfo.PatchLocationListSize = VIOGPU_WDDM_PATCH_LIST_SIZE;
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
        render->CommandLength < sizeof(VIOGPU_WDDM_RENDER_COMMAND) ||
        render->CommandLength > VIOGPU_WDDM_DMA_BUFFER_SIZE || render->pDmaBuffer == NULL ||
        render->pDmaBufferPrivateData == NULL ||
        render->DmaBufferPrivateDataSize < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) || render->MultipassOffset != 0 ||
        render->AllocationListSize == 0 || render->AllocationListSize > VIOGPU_WDDM_ALLOCATION_LIST_SIZE ||
        render->pAllocationList == NULL || render->PatchLocationListInSize == 0 ||
        render->PatchLocationListInSize > VIOGPU_WDDM_PATCH_LIST_SIZE ||
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

    NTSTATUS status = STATUS_SUCCESS;
    BYTE *commandSnapshot = NULL;
    D3DDDI_PATCHLOCATIONLIST *patchSnapshot = NULL;
    SIZE_T patchBytes = (SIZE_T)render->PatchLocationListInSize * sizeof(*patchSnapshot);

    if (render->DmaSize < render->CommandLength || render->PatchLocationListOutSize < render->PatchLocationListInSize)
    {
        status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    if (NT_SUCCESS(status))
    {
        commandSnapshot = new (NonPagedPoolNx) BYTE[render->CommandLength];
        patchSnapshot = new (NonPagedPoolNx) D3DDDI_PATCHLOCATIONLIST[render->PatchLocationListInSize];
        if (commandSnapshot == NULL || patchSnapshot == NULL)
        {
            status = STATUS_NO_MEMORY;
        }
    }
    if (NT_SUCCESS(status))
    {
        __try
        {
            ProbeForRead(const_cast<PVOID>(render->pCommand), render->CommandLength, 1);
            RtlCopyMemory(commandSnapshot, render->pCommand, render->CommandLength);
            ProbeForRead(render->pPatchLocationListIn, patchBytes, __alignof(D3DDDI_PATCHLOCATIONLIST));
            RtlCopyMemory(patchSnapshot, render->pPatchLocationListIn, patchBytes);

            const VIOGPU_WDDM_RENDER_COMMAND *command = reinterpret_cast<const VIOGPU_WDDM_RENDER_COMMAND *>(commandSnapshot);
            status = ValidateCommandHeader(command,
                                           render->CommandLength,
                                           context->Device,
                                           render->pAllocationList,
                                           render->AllocationListSize,
                                           patchSnapshot,
                                           render->PatchLocationListInSize,
                                           snapshot.ResetGeneration);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            status = STATUS_INVALID_USER_BUFFER;
        }
    }

    if (NT_SUCCESS(status) &&
        !snapshot.Adapter->IsNativeContextGenerationCurrent(snapshot.Generation, snapshot.ResetGeneration))
    {
        status = STATUS_DEVICE_NOT_READY;
    }

    if (NT_SUCCESS(status))
    {
        PVOID dmaBuffer = render->pDmaBuffer;
        D3DDDI_PATCHLOCATIONLIST *patchOutput = render->pPatchLocationListOut;
        RtlCopyMemory(dmaBuffer, commandSnapshot, render->CommandLength);
        RtlCopyMemory(patchOutput, patchSnapshot, patchBytes);

        VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = static_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(render->pDmaBufferPrivateData);
        RtlZeroMemory(privateData, sizeof(*privateData));
        privateData->Signature = VIOGPU_WDDM_DMA_SIGNATURE;
        privateData->DmaBuffer = dmaBuffer;
        privateData->DmaBufferSize = render->DmaSize;
        privateData->CommandLength = render->CommandLength;
        privateData->ContextId = snapshot.ContextId;
        privateData->Generation = snapshot.Generation;
        privateData->ResetGeneration = snapshot.ResetGeneration;

        render->pDmaBuffer = static_cast<BYTE *>(dmaBuffer) + render->CommandLength;
        render->pPatchLocationListOut = patchOutput + render->PatchLocationListInSize;
        render->MultipassOffset = render->CommandLength;
    }

    delete[] patchSnapshot;
    delete[] commandSnapshot;
    VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
    ExReleaseRundownProtection(&context->Operations);
    return status;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmPatch(CONST HANDLE hAdapter, CONST DXGKARG_PATCH *patchArguments)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(patchArguments);
    return STATUS_NOT_SUPPORTED;
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
