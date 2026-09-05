#include "wddmddi.h"

#include "../common/baseobj.h"
#include "../viogpudo/viogpudo.h"

VOID ReleaseApertureMapping(_Inout_ VIOGPU_WDDM_ALLOCATION *allocation);
NTSTATUS AllocateApertureBackingEntries(_In_ const VIOGPU_WDDM_ALLOCATION *allocation,
                                        _Outptr_result_buffer_(*entryCount) GPU_MEM_ENTRY **entries,
                                        _Out_ PUINT entryCount);

namespace
{
const ULONG VIOGPU_WDDM_RESOURCE_SIGNATURE = 'rWGV';
const ULONG VIOGPU_WDDM_ALLOCATION_SIGNATURE = 'aWGV';
const ULONG VIOGPU_WDDM_DEVICE_SIGNATURE = 'dWGV';
const ULONG VIOGPU_WDDM_CONTEXT_SIGNATURE = 'cWGV';
const ULONG VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE = 'oWGV';
const ULONG VIOGPU_WDDM_DMA_SIGNATURE = 'mWGV';
const ULONG VIOGPU_WDDM_PAGING_DMA_SIGNATURE = 'pWGV';
const ULONG VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE = 'tWGV';
const ULONG VIOGPU_WDDM_SUBMISSION_SIGNATURE = 'sWGV';
const ULONG VIOGPU_WDDM_PRESENT_DMA_SIGNATURE = 'dWGP';
const ULONG VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE = 'tWGP';
const ULONG VIOGPU_WDDM_DEBUG_SIGNATURE = 'gWGV';

const UINT VIOGPU_WDDM_SEGMENT_ID = 1;
const UINT VIOGPU_WDDM_DMA_BUFFER_SIZE = 64 * 1024;
const UINT VIOGPU_WDDM_ALLOCATION_LIST_SIZE = VioGpuWddmSubmissionAllocationLimit;
const ULONGLONG VIOGPU_WDDM_APERTURE_SIZE = static_cast<ULONGLONG>(MAXULONG) + 1;
const UINT VIOGPU_WDDM_PATCH_LIST_SIZE = VioGpuWddmSubmissionAllocationLimit;
const UINT VIOGPU_WDDM_PRESENT_RECTS_PER_PASS = 256;
const LONG VIOGPU_WDDM_DEVICE_CLOSING = static_cast<LONG>(0x80000000UL);
const LONG VIOGPU_WDDM_DEVICE_REFERENCE_MASK = 0x7FFFFFFF;
const ULONGLONG VIOGPU_WDDM_CONTEXT_DESTROY_STALL_TIMEOUT_100NS = 10ULL * 1000 * 1000;
const ULONG VIOGPU_WDDM_DEFERRED_CONTEXT_DESTROY_MAX_ATTEMPTS = 64;
const LONGLONG VIOGPU_WDDM_DEFERRED_CONTEXT_DESTROY_RETRY_DELAY_100NS = 10LL * 1000 * 10;

enum VIOGPU_WDDM_APERTURE_PAGE_STATE : UCHAR
{
    VioGpuWddmAperturePageUnknown = 0,
    VioGpuWddmAperturePageMapped,
    VioGpuWddmAperturePageDummy,
};

const UINT VIOGPU_WDDM_MSM_SUBMIT_CMD_BUF = 0x0001;
const UINT VIOGPU_WDDM_MSM_SUBMIT_CMD_IB_TARGET_BUF = 0x0002;
const UINT VIOGPU_WDDM_MSM_SUBMIT_BO_READ = 0x0001;
const UINT VIOGPU_WDDM_MSM_SUBMIT_BO_WRITE = 0x0002;
const UINT VIOGPU_WDDM_MSM_SUBMIT_BO_DUMP = 0x0004;
const UINT VIOGPU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT = 0x0008;
const UINT VIOGPU_WDDM_MSM_SUBMIT_NO_IMPLICIT = 0x80000000;

#pragma pack(push, 1)
struct VIOGPU_WDDM_MSM_SUBMIT_BO
{
    UINT Flags;
    UINT Handle;
    ULONGLONG Presumed;
};

struct VIOGPU_WDDM_MSM_SUBMIT_CMD
{
    UINT Type;
    UINT SubmitIndex;
    UINT SubmitOffset;
    UINT Size;
    UINT Padding;
    UINT RelocationCount;
    ULONGLONG Iova;
};
#pragma pack(pop)

static_assert(sizeof(VIOGPU_WDDM_MSM_SUBMIT_BO) == 16, "unexpected MSM submit BO wire size");
static_assert(sizeof(VIOGPU_WDDM_MSM_SUBMIT_CMD) == 32, "unexpected MSM submit command wire size");
static_assert(sizeof(VIOGPU_WDDM_RENDER_COMMAND) + VioGpuWddmSubmissionAllocationLimit * sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE) + sizeof(MSM_CCMD_GEM_SUBMIT_REQ) + VioGpuWddmSubmissionAllocationLimit * sizeof(VIOGPU_WDDM_MSM_SUBMIT_BO) +
                                                                                                                                                                                                                      256U * sizeof(VIOGPU_WDDM_MSM_SUBMIT_CMD) <=
                                                                                                                  VIOGPU_WDDM_DMA_BUFFER_SIZE,
              "maximum Native Context submit no longer fits the DMA buffer");

VOID NativeSubmissionComplete(_In_opt_ PVOID callbackContext);
VOID NativeSubmissionCancelled(_In_opt_ PVOID callbackContext);
VOID NativeSubmissionQueueFailed(_In_opt_ PVOID callbackContext);
VOID NativeRenderDispatchWorker(_In_ PVOID callbackContext);
VOID NativeRenderDispatchCancelled(_In_ PVOID callbackContext);
VOID NativePresentWorker(_In_ PVOID callbackContext);
VOID NativePresentDispatchCancelled(_In_ PVOID callbackContext);

void DereferenceDevice(VIOGPU_WDDM_DEVICE *device);

LONG ReadResourceAllocationCount(_In_ const VIOGPU_WDDM_RESOURCE *resource)
{
    return resource == NULL ? -1
                            : InterlockedCompareExchange(const_cast<volatile LONG *>(&resource->AllocationCount), 0, 0);
}

BOOLEAN IsOwnedAllocation(_In_ const VIOGPU_WDDM_ALLOCATION *allocation, _In_ const VioGpuDod *adapter)
{
    if (allocation == NULL || adapter == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
        allocation->Adapter != adapter)
    {
        return FALSE;
    }

    const VIOGPU_WDDM_RESOURCE *resource = allocation->Resource;
    return resource == NULL || (resource->Signature == VIOGPU_WDDM_RESOURCE_SIGNATURE && resource->Adapter == adapter &&
                                ReadResourceAllocationCount(resource) > 0);
}

BOOLEAN ValidatePagingDmaPacket(_In_ const VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData,
                                _In_ const VIOGPU_WDDM_PAGING_DMA_PACKET *packet)
{
    const VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = privateData == NULL ? NULL
                                                                          : reinterpret_cast<const VIOGPU_WDDM_PAGING_PRIVATE *>(privateData);
    const VIOGPU_WDDM_PAGING_TRANSACTION *transaction = pagingPrivate == NULL ? NULL : &pagingPrivate->Transaction;
    LONG transactionState = transaction == NULL ? VioGpuWddmPagingTransactionInvalid
                                                : InterlockedCompareExchange(const_cast<volatile LONG *>(&transaction->State),
                                                                             0,
                                                                             0);
    LONG referenceHeld = transaction == NULL ? 0
                                             : InterlockedCompareExchange(const_cast<volatile LONG *>(&transaction->ReferenceHeld),
                                                                          0,
                                                                          0);
    if (privateData == NULL || packet == NULL || privateData->Signature != VIOGPU_WDDM_DMA_SIGNATURE ||
        privateData->Version != VioGpuWddmDmaPrivateVersion || privateData->Kind != VioGpuWddmDmaKindPaging ||
        privateData->DmaBuffer == NULL || privateData->DmaBufferSize < sizeof(*packet) ||
        privateData->CommandLength != sizeof(*packet) || privateData->Packet != privateData->DmaBuffer ||
        privateData->Packet != packet || privateData->PacketLength != sizeof(*packet) || privateData->Reserved != 0 ||
        privateData->Submission != pagingPrivate || transaction == NULL ||
        transaction->Signature != VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE ||
        (transactionState != VioGpuWddmPagingTransactionBuilt &&
         transactionState != VioGpuWddmPagingTransactionQueued &&
         transactionState != VioGpuWddmPagingTransactionExecuting &&
         transactionState != VioGpuWddmPagingTransactionFinished &&
         transactionState != VioGpuWddmPagingTransactionCancelled) ||
        ((transactionState == VioGpuWddmPagingTransactionBuilt ||
          transactionState == VioGpuWddmPagingTransactionQueued ||
          transactionState == VioGpuWddmPagingTransactionExecuting) &&
         referenceHeld != 1) ||
        transaction->Adapter == NULL || transaction->Allocation == NULL ||
        transaction->Operation != packet->Operation || transaction->Flags != packet->Flags ||
        transaction->ResourceId != packet->ResourceId || transaction->ContextId != packet->ContextId ||
        transaction->ContextGeneration != packet->ContextGeneration ||
        transaction->ResetGeneration != packet->ResetGeneration ||
        ((packet->Flags & (VioGpuWddmPagingFlagPageIn | VioGpuWddmPagingFlagPageOut)) != 0 &&
         !transaction->TransferDataComplete) ||
        transaction->PlacementOffset != packet->PlacementOffset ||
        transaction->TransferOffset != packet->TransferOffset || transaction->TransferSize != packet->TransferSize ||
        packet->Signature != VIOGPU_WDDM_PAGING_DMA_SIGNATURE || packet->Version != VioGpuWddmDmaPrivateVersion ||
        packet->Size != sizeof(*packet) || packet->Flags != privateData->Flags ||
        packet->ContextId != privateData->ContextId || packet->ContextGeneration != privateData->Generation ||
        packet->ResetGeneration != privateData->ResetGeneration || packet->Reserved != 0 || packet->ResourceId == 0 ||
        packet->ResourceId == MAXUINT || packet->TransferOffset > MAXUINT || packet->TransferSize > MAXULONG ||
        (packet->PlacementOffset & (PAGE_SIZE - 1)) != 0)
    {
        return FALSE;
    }

    const UINT allowedFlags = VioGpuWddmPagingFlagPageIn | VioGpuWddmPagingFlagPageOut | VioGpuWddmPagingFlagFill |
                              VioGpuWddmPagingFlagDiscard | VioGpuWddmPagingFlagTransferStart |
                              VioGpuWddmPagingFlagTransferEnd | VioGpuWddmPagingFlagAllocationIdle |
                              VioGpuWddmPagingFlagSoftwareCompleted;
    const UINT operationFlags = packet->Flags & (VioGpuWddmPagingFlagPageIn | VioGpuWddmPagingFlagPageOut |
                                                 VioGpuWddmPagingFlagFill | VioGpuWddmPagingFlagDiscard);
    const UINT transferFlags = packet->Flags & (VioGpuWddmPagingFlagTransferStart | VioGpuWddmPagingFlagTransferEnd);
    if ((packet->Flags & ~allowedFlags) != 0 || (packet->Flags & VioGpuWddmPagingFlagSoftwareCompleted) == 0)
    {
        return FALSE;
    }

    BOOLEAN hasContext = packet->ContextId != 0;
    if (hasContext ? packet->ContextGeneration <= 0 || packet->ResetGeneration == 0
                   : packet->ContextGeneration != 0 || packet->ResetGeneration != 0)
    {
        return FALSE;
    }
    if (hasContext ? packet->ResourceId < VIOGPU_NATIVE_RESOURCE_ID_START
                   : packet->ResourceId >= VIOGPU_NATIVE_RESOURCE_ID_START)
    {
        return FALSE;
    }

    if (packet->Operation == DXGK_OPERATION_TRANSFER)
    {
        BOOLEAN pageIn = operationFlags == VioGpuWddmPagingFlagPageIn;
        BOOLEAN pageOut = operationFlags == VioGpuWddmPagingFlagPageOut;
        if ((!pageIn && !pageOut) || packet->TransferSize == 0 ||
            (pageOut && (packet->Flags & VioGpuWddmPagingFlagAllocationIdle) == 0) ||
            packet->TransferOffset > MAXULONGLONG - packet->TransferSize ||
            packet->PlacementOffset > MAXULONGLONG - packet->TransferOffset)
        {
            return FALSE;
        }
        ULONGLONG placementTransferOffset = packet->PlacementOffset + packet->TransferOffset;
        return packet->TransferSize - 1 <= MAXULONGLONG - placementTransferOffset;
    }

    if (packet->Operation == DXGK_OPERATION_FILL)
    {
        return operationFlags == VioGpuWddmPagingFlagFill && transferFlags == 0 &&
               (packet->Flags & VioGpuWddmPagingFlagAllocationIdle) == 0 && packet->TransferOffset == 0 &&
               packet->TransferSize != 0 && packet->TransferSize - 1 <= MAXULONGLONG - packet->PlacementOffset;
    }

    if (packet->Operation == DXGK_OPERATION_DISCARD_CONTENT)
    {
        return operationFlags == VioGpuWddmPagingFlagDiscard && transferFlags == 0 && packet->TransferOffset == 0 &&
               packet->TransferSize == 0;
    }

    return FALSE;
}

BOOLEAN ValidatePresentDmaPacket(_In_ const VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData,
                                 _In_ const VIOGPU_WDDM_PRESENT_DMA_PACKET *packet,
                                 _In_ const VIOGPU_WDDM_PRESENT_TRANSACTION *transaction)
{
    LONG state = transaction == NULL ? VioGpuWddmPresentInvalid
                                     : InterlockedCompareExchange(const_cast<volatile LONG *>(&transaction->State),
                                                                  0,
                                                                  0);
    if (privateData == NULL || packet == NULL || transaction == NULL ||
        privateData->Signature != VIOGPU_WDDM_DMA_SIGNATURE || privateData->Version != VioGpuWddmDmaPrivateVersion ||
        privateData->Kind != VioGpuWddmDmaKindPresent || privateData->DmaBuffer == NULL ||
        privateData->DmaBufferSize < sizeof(*packet) || privateData->CommandLength != sizeof(*packet) ||
        privateData->Flags != 1U || privateData->Packet != privateData->DmaBuffer || privateData->Packet != packet ||
        privateData->PacketLength != sizeof(*packet) || privateData->Reserved != 0 ||
        privateData->Submission != transaction || transaction->Signature != VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE ||
        transaction->ReferenceCount <= 0 || transaction->Context == NULL || transaction->Adapter == NULL ||
        transaction->Source == NULL || transaction->Destination == NULL ||
        transaction->Source == transaction->Destination || transaction->DmaBuffer != privateData->DmaBuffer ||
        transaction->PrivateData != privateData || transaction->RectCount == 0 ||
        transaction->DestinationSubRects == NULL ||
        (state != VioGpuWddmPresentBuilt && state != VioGpuWddmPresentPatched && state != VioGpuWddmPresentQueued &&
         state != VioGpuWddmPresentExecuting) ||
        packet->Signature != VIOGPU_WDDM_PRESENT_DMA_SIGNATURE || packet->Version != VioGpuWddmDmaPrivateVersion ||
        packet->Size != sizeof(*packet) || packet->Flags != 1U ||
        packet->SourceResourceId != transaction->Source->ResourceId ||
        packet->DestinationResourceId != transaction->Destination->ResourceId ||
        packet->RectCount != transaction->RectCount || packet->Reserved != 0 ||
        packet->SourcePlacementOffset != transaction->SourcePlacementOffset ||
        packet->DestinationPlacementOffset != transaction->DestinationPlacementOffset ||
        packet->DestinationResetGeneration != transaction->DestinationResetGeneration)
    {
        return FALSE;
    }
    return TRUE;
}

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

NTSTATUS UnregisterNativeAllocationRange(VIOGPU_WDDM_ALLOCATION *allocation);

NTSTATUS ValidateAllocationPrivate(const VIOGPU_WDDM_ALLOCATION_INFO *privateData, SIZE_T *alignedSize)
{
    const VIOGPU_WDDM_UINT32 validFlags = VIOGPU_WDDM_ALLOCATION_PRIMARY | VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE |
                                          VIOGPU_WDDM_ALLOCATION_NATIVE | VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY;
    D3DDDIFORMAT format = privateData == NULL ? D3DDDIFMT_UNKNOWN : FromPrivateFormat(privateData->Format);

    if (privateData == NULL || alignedSize == NULL || !IsCurrentAbiHeader(&privateData->Header, sizeof(*privateData)) ||
        (privateData->Flags & ~validFlags) != 0 || privateData->Size == 0 ||
        privateData->Size > (ULONGLONG)(MAXULONG_PTR - (PAGE_SIZE - 1)) || privateData->Alignment != PAGE_SIZE ||
        privateData->ContextId != 0 && (privateData->Flags & VIOGPU_WDDM_ALLOCATION_NATIVE) == 0)
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

    SIZE_T localAlignedSize = ((SIZE_T)privateData->Size + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
    BOOLEAN nativeAllocation = (privateData->Flags & VIOGPU_WDDM_ALLOCATION_NATIVE) != 0;
    if (nativeAllocation)
    {
        if ((privateData->Flags & VIOGPU_WDDM_ALLOCATION_PRIMARY) != 0 || privateData->RequestedIova == 0 ||
            (privateData->RequestedIova & (PAGE_SIZE - 1)) != 0 || privateData->ExpectedResetGeneration == 0 ||
            privateData->ContextId == 0 || localAlignedSize > MAXULONG ||
            privateData->RequestedIova > MAXULONGLONG - ((ULONGLONG)localAlignedSize - 1))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }
    else if (privateData->RequestedIova != 0 || privateData->ExpectedResetGeneration != 0 ||
             privateData->ContextId != 0 || (privateData->Flags & VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *alignedSize = localAlignedSize;
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
    BOOLEAN cpuVisible = (allocation->Flags & VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE) != 0;
    allocationInfo->Flags.CpuVisible = cpuVisible;
    allocationInfo->Flags.Cached = cpuVisible;
    allocationInfo->Flags.SynchronousPaging = TRUE;
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
            if (allocation->NativeContext != NULL)
            {
                NTSTATUS rangeStatus = UnregisterNativeAllocationRange(allocation);
                NT_ASSERT(NT_SUCCESS(rangeStatus));
                UNREFERENCED_PARAMETER(rangeStatus);
                BOOLEAN dereferenced = VioGpuAdapter::DereferenceNativeContextAllocation(allocation->NativeContext);
                NT_ASSERT(dereferenced);
                UNREFERENCED_PARAMETER(dereferenced);
                allocation->NativeContext = NULL;
            }
            if ((allocation->Flags & VIOGPU_WDDM_ALLOCATION_NATIVE) == 0 && allocation->ResourceId != 0)
            {
                BOOLEAN released = allocation->Adapter != NULL &&
                                   allocation->Adapter->Release2DResourceId(allocation->ResourceId);
                NT_ASSERT(released);
                UNREFERENCED_PARAMETER(released);
                allocation->ResourceId = 0;
            }
            if (allocation->Resource != NULL)
            {
                LONG remaining = InterlockedDecrement(&allocation->Resource->AllocationCount);
                NT_ASSERT(remaining >= 0);
                UNREFERENCED_PARAMETER(remaining);
            }
            allocation->Signature = 0;
            delete allocation;
            allocationInfo[index].hAllocation = NULL;
        }
    }
}

BOOLEAN IsNativeAllocation(const VIOGPU_WDDM_ALLOCATION *allocation)
{
    return allocation != NULL && (allocation->Flags & VIOGPU_WDDM_ALLOCATION_NATIVE) != 0;
}

BOOLEAN IsStandardAllocation(const VIOGPU_WDDM_ALLOCATION *allocation)
{
    return allocation != NULL && !IsNativeAllocation(allocation);
}

BOOLEAN IsStandardPrimaryAllocation(const VIOGPU_WDDM_ALLOCATION *allocation)
{
    return IsStandardAllocation(allocation) && (allocation->Flags & VIOGPU_WDDM_ALLOCATION_PRIMARY) != 0;
}

BOOLEAN IsGdiSourceAllocation(const VIOGPU_WDDM_ALLOCATION *allocation)
{
    return IsStandardAllocation(allocation) && !IsStandardPrimaryAllocation(allocation) &&
           (allocation->Flags & VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE) != 0;
}

VOID RecordNativeAllocationDestroyState(_In_ VioGpuDod *adapter,
                                        _In_ DWORD stage,
                                        _In_ NTSTATUS status,
                                        _In_ DWORD detail,
                                        _In_ VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (adapter == NULL || allocation == NULL)
    {
        return;
    }

    VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration = allocation->NativeContext;
    VIOGPU_WDDM_ALLOCATION_RANGE *range = NULL;
    BOOLEAN rangePresent = FALSE;
    BOOLEAN rangeLinked = FALSE;
    DWORD registrationState = 0;
    DWORD registrationReferences = 0;
    DWORD rangeCount = 0;
    ULONGLONG rangeIova = 0;
    SIZE_T rangeLength = 0;
    if (registration != NULL)
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&registration->BindingLock, &oldIrql);
        registration = allocation->NativeContext;
        range = allocation->ContextRange;
        if (registration != NULL)
        {
            registrationState = static_cast<DWORD>(InterlockedCompareExchange(&registration->State,
                                                                                VioGpuNativeContextDead,
                                                                                VioGpuNativeContextDead));
            registrationReferences = registration->AllocationReferences;
        }
        if (range != NULL)
        {
            rangePresent = TRUE;
            rangeLinked = range->Linked;
            rangeIova = range->Iova;
            rangeLength = range->Length;
        }
        for (PLIST_ENTRY entry = registration->AllocationRanges.Flink; entry != &registration->AllocationRanges;
             entry = entry->Flink)
        {
            ++rangeCount;
        }
        KeReleaseSpinLock(&registration->BindingLock, oldIrql);
    }

    adapter->RecordNativeAllocationDestroyDiagnostic(stage,
                                                      status,
                                                      detail,
                                                      allocation->NativeContext != NULL,
                                                      rangePresent,
                                                      rangeLinked,
                                                      registrationState,
                                                      registrationReferences,
                                                      allocation->Destroying,
                                                      allocation->HostState,
                                                      allocation->ContextId,
                                                      allocation->ResourceId,
                                                      rangeCount,
                                                      allocation->PrivateData.RequestedIova,
                                                      rangeIova,
                                                      rangeLength);
}

BOOLEAN HasLiveNativePresentIdentity(_In_ const VIOGPU_WDDM_ALLOCATION *allocation,
                                     _In_ const VIOGPU_WDDM_CONTEXT *context,
                                     _In_ const VioGpuDod *adapter)
{
    return allocation != NULL && context != NULL && adapter != NULL &&
           allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE && allocation->Adapter == adapter &&
           context->Type == VioGpuWddmContextNative && IsNativeAllocation(allocation) &&
           allocation->NativeContext == &context->NativeContext &&
           allocation->HostState == VioGpuWddmAllocationHostLive &&
           allocation->ResourceId >= VIOGPU_NATIVE_RESOURCE_ID_START && allocation->BlobId == allocation->ResourceId &&
           allocation->ContextId != 0 && allocation->ContextGeneration > 0 && allocation->ContextResetGeneration != 0 &&
           allocation->BoundContextId == allocation->ContextId &&
           allocation->BoundGeneration == allocation->ContextGeneration &&
           allocation->BoundResetGeneration == allocation->ContextResetGeneration;
}

NTSTATUS RegisterNativeAllocationRange(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (!IsNativeAllocation(allocation) || allocation->NativeContext == NULL || allocation->ContextRange != NULL ||
        allocation->PrivateData.RequestedIova == 0 || allocation->BackingSize == 0 ||
        allocation->PrivateData.RequestedIova > MAXULONGLONG - (allocation->BackingSize - 1))
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_ALLOCATION_RANGE *range = new (NonPagedPoolNx) VIOGPU_WDDM_ALLOCATION_RANGE;
    if (range == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    RtlZeroMemory(range, sizeof(*range));
    InitializeListHead(&range->Link);
    range->Registration = allocation->NativeContext;
    range->Iova = allocation->PrivateData.RequestedIova;
    range->Length = allocation->BackingSize;
    range->ResourceId = allocation->ResourceId;
    range->ContextId = allocation->ContextId;

    VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration = allocation->NativeContext;
    KIRQL oldIrql;
    DWORD rejectionReason = 0;
    DWORD rangeCount = 0;
    ULONGLONG collisionIova = 0;
    DWORD collisionLength = 0;
    UINT collisionResourceId = 0;
    UINT collisionContextId = 0;
    DWORD registrationState = 0;
    DWORD registrationReferences = 0;
    KeAcquireSpinLock(&registration->BindingLock, &oldIrql);
    registrationState = static_cast<DWORD>(InterlockedCompareExchange(&registration->State,
                                                                        VioGpuNativeContextDead,
                                                                        VioGpuNativeContextDead));
    registrationReferences = registration->AllocationReferences;
    BOOLEAN valid = registration->Adapter != NULL && registration->Adapter->GetVioGpu() == allocation->Adapter &&
                    registration->Owner != NULL && registration->Registered &&
                    registration->Generation == allocation->ContextGeneration &&
                    registration->ResetGeneration == allocation->ContextResetGeneration &&
                    registration->ContextId == allocation->ContextId && registrationState == VioGpuNativeContextLive;
    if (!valid)
    {
        rejectionReason = 1;
    }
    if (valid)
    {
        ULONGLONG rangeEnd = range->Iova + (ULONGLONG)range->Length - 1;
        for (PLIST_ENTRY entry = registration->AllocationRanges.Flink; entry != &registration->AllocationRanges;
             entry = entry->Flink)
        {
            ++rangeCount;
            VIOGPU_WDDM_ALLOCATION_RANGE *existing = CONTAINING_RECORD(entry, VIOGPU_WDDM_ALLOCATION_RANGE, Link);
            if (existing->Registration != registration || !existing->Linked || existing->Iova == 0 ||
                existing->Length == 0 || existing->Iova > MAXULONGLONG - ((ULONGLONG)existing->Length - 1))
            {
                rejectionReason = 2;
                valid = FALSE;
                break;
            }
            ULONGLONG existingEnd = existing->Iova + (ULONGLONG)existing->Length - 1;
            if (range->Iova <= existingEnd && existing->Iova <= rangeEnd)
            {
                rejectionReason = 3;
                collisionIova = existing->Iova;
                collisionLength = existing->Length > MAXULONG ? MAXULONG : static_cast<DWORD>(existing->Length);
                collisionResourceId = existing->ResourceId;
                collisionContextId = existing->ContextId;
                valid = FALSE;
                break;
            }
        }
    }
    if (valid)
    {
        InsertTailList(&registration->AllocationRanges, &range->Link);
        range->Linked = TRUE;
        allocation->ContextRange = range;
    }
    KeReleaseSpinLock(&registration->BindingLock, oldIrql);
    if (!valid)
    {
        if (allocation->Adapter != NULL)
        {
            allocation->Adapter->RecordNativeAllocationRangeDiagnostic(STATUS_DEVICE_BUSY,
                                                                        rejectionReason,
                                                                        rangeCount,
                                                                        range->Iova,
                                                                        collisionIova,
                                                                        collisionLength,
                                                                        collisionResourceId,
                                                                        collisionContextId,
                                                                        registrationState,
                                                                        registrationReferences,
                                                                        allocation->Destroying,
                                                                        allocation->HostState);
        }
        delete range;
        return STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS UnregisterNativeAllocationRange(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL || allocation->NativeContext == NULL)
    {
        return STATUS_SUCCESS;
    }

    VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration = allocation->NativeContext;
    KIRQL oldIrql;
    KeAcquireSpinLock(&registration->BindingLock, &oldIrql);
    VIOGPU_WDDM_ALLOCATION_RANGE *range = allocation->ContextRange;
    BOOLEAN valid = allocation->NativeContext == registration && range != NULL && range->Registration == registration;
    if (!valid)
    {
        KeReleaseSpinLock(&registration->BindingLock, oldIrql);
        return range == NULL ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    }
    BOOLEAN linked = range->Linked;
    if (linked)
    {
        RemoveEntryList(&range->Link);
        range->Linked = FALSE;
        allocation->ContextRange = NULL;
    }
    KeReleaseSpinLock(&registration->BindingLock, oldIrql);
    if (!linked)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    delete range;
    return STATUS_SUCCESS;
}

BOOLEAN ValidateNativeAllocationRange(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (!IsNativeAllocation(allocation) || allocation->NativeContext == NULL)
    {
        return FALSE;
    }
    VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration = allocation->NativeContext;
    KIRQL oldIrql;
    KeAcquireSpinLock(&registration->BindingLock, &oldIrql);
    VIOGPU_WDDM_ALLOCATION_RANGE *range = allocation->ContextRange;
    BOOLEAN valid = allocation->NativeContext == registration && range != NULL && range->Linked &&
                    range->Registration == registration && range->Iova == allocation->PrivateData.RequestedIova &&
                    range->Length == allocation->BackingSize;
    if (valid)
    {
        valid = FALSE;
        for (PLIST_ENTRY entry = registration->AllocationRanges.Flink; entry != &registration->AllocationRanges;
             entry = entry->Flink)
        {
            if (entry == &range->Link)
            {
                valid = TRUE;
                break;
            }
        }
    }
    KeReleaseSpinLock(&registration->BindingLock, oldIrql);
    return valid;
}

BOOLEAN ValidateNativeAllocationDestroyState(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (!IsNativeAllocation(allocation))
    {
        return TRUE;
    }
    if (allocation->NativeContext != NULL && allocation->ContextRange != NULL)
    {
        return ValidateNativeAllocationRange(allocation);
    }
    return allocation->Destroying && allocation->NativeContext == NULL && allocation->ContextRange == NULL &&
           allocation->HostState == VioGpuWddmAllocationHostNone;
}

NTSTATUS FinalizeDeferredNativeContextDestroy(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    VIOGPU_WDDM_CONTEXT *context = allocation->DeferredContext;
    if (context == NULL)
    {
        return STATUS_SUCCESS;
    }
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE || context->Type != VioGpuWddmContextNative ||
        context->Device != NULL || context->DeferredAdapter == NULL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (InterlockedCompareExchange(&context->DestroyState,
                                   VioGpuWddmContextDestroyFinalizing,
                                   VioGpuWddmContextDestroyDeferred) != VioGpuWddmContextDestroyDeferred)
    {
        return STATUS_DEVICE_BUSY;
    }

    BOOLEAN released = FALSE;
    NTSTATUS status = STATUS_DEVICE_BUSY;
    for (ULONG attempt = 0; attempt < VIOGPU_WDDM_DEFERRED_CONTEXT_DESTROY_MAX_ATTEMPTS; ++attempt)
    {
        status = context->DeferredAdapter->DestroyNativeContext(&context->NativeContext, &released);
        if (!released && VioGpuAdapter::IsNativeContextReleased(&context->NativeContext))
        {
            released = TRUE;
        }
        if (released || status != STATUS_DEVICE_BUSY ||
            attempt + 1 == VIOGPU_WDDM_DEFERRED_CONTEXT_DESTROY_MAX_ATTEMPTS)
        {
            break;
        }

        LARGE_INTEGER retryDelay;
        retryDelay.QuadPart = -VIOGPU_WDDM_DEFERRED_CONTEXT_DESTROY_RETRY_DELAY_100NS;
        status = KeDelayExecutionThread(KernelMode, FALSE, &retryDelay);
        if (status != STATUS_SUCCESS)
        {
            break;
        }
    }
    if (!released)
    {
        InterlockedExchange(&context->DestroyState, VioGpuWddmContextDestroyDeferred);
        return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;
    }

    allocation->DeferredContext = NULL;
    context->DeferredAdapter = NULL;
    context->Signature = 0;
    delete context;
    return STATUS_SUCCESS;
}

NTSTATUS DetachAllocationNativeContext(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    VioGpuDod *adapter = allocation->Adapter;
    RecordNativeAllocationDestroyState(adapter,
                                       VioGpuNativeAllocationDestroyEntered,
                                       STATUS_PENDING,
                                       0,
                                       allocation);
    VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration = allocation->NativeContext;
    VIOGPU_WDDM_ALLOCATION_RANGE *range = allocation->ContextRange;
    if (registration == NULL || range == NULL)
    {
        NTSTATUS status = registration == NULL && range == NULL ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
        if (status == STATUS_SUCCESS)
        {
            status = FinalizeDeferredNativeContextDestroy(allocation);
        }
        RecordNativeAllocationDestroyState(adapter,
                                           VioGpuNativeAllocationDestroyDetach,
                                           status,
                                           1,
                                           allocation);
        if (status == STATUS_SUCCESS)
        {
            RecordNativeAllocationDestroyState(adapter,
                                               VioGpuNativeAllocationDestroyComplete,
                                               status,
                                               0,
                                               allocation);
        }
        return status;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&registration->BindingLock, &oldIrql);
    BOOLEAN valid = allocation->Destroying && allocation->HostState == VioGpuWddmAllocationHostNone &&
                    allocation->NativeContext == registration && allocation->ContextRange == range &&
                    range->Registration == registration && range->Linked && registration->AllocationReferences != 0;
    VIOGPU_WDDM_CONTEXT *deferredContext = NULL;
    if (valid)
    {
        valid = FALSE;
        for (PLIST_ENTRY entry = registration->AllocationRanges.Flink; entry != &registration->AllocationRanges;
             entry = entry->Flink)
        {
            if (entry == &range->Link)
            {
                valid = TRUE;
                break;
            }
        }
    }
    if (valid && registration->AllocationClosing && registration->AllocationReferences == 1)
    {
        BOOLEAN onlyRange = registration->AllocationRanges.Flink == &range->Link &&
                            registration->AllocationRanges.Blink == &range->Link;
        deferredContext = CONTAINING_RECORD(registration, VIOGPU_WDDM_CONTEXT, NativeContext);
        valid = onlyRange && deferredContext->Signature == VIOGPU_WDDM_CONTEXT_SIGNATURE &&
                deferredContext->Type == VioGpuWddmContextNative && deferredContext->Device == NULL &&
                deferredContext->DeferredAdapter != NULL &&
                InterlockedCompareExchange(&deferredContext->DestroyState,
                                           VioGpuWddmContextDestroyDeferred,
                                           VioGpuWddmContextDestroyDeferred) == VioGpuWddmContextDestroyDeferred;
    }
    if (valid)
    {
        RemoveEntryList(&range->Link);
        range->Linked = FALSE;
        allocation->ContextRange = NULL;
        --registration->AllocationReferences;
        allocation->NativeContext = NULL;
        allocation->DeferredContext = deferredContext;
    }
    KeReleaseSpinLock(&registration->BindingLock, oldIrql);
    NTSTATUS status = valid ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    RecordNativeAllocationDestroyState(adapter,
                                       VioGpuNativeAllocationDestroyDetach,
                                       status,
                                       2,
                                       allocation);
    if (!valid)
    {
        return status;
    }
    delete range;
    status = FinalizeDeferredNativeContextDestroy(allocation);
    if (status != STATUS_SUCCESS)
    {
        RecordNativeAllocationDestroyState(adapter, VioGpuNativeAllocationDestroyDetach, status, 3, allocation);
        return status;
    }
    RecordNativeAllocationDestroyState(adapter,
                                       VioGpuNativeAllocationDestroyComplete,
                                       STATUS_SUCCESS,
                                       0,
                                       allocation);
    return status;
}

BOOLEAN AcquireAllocationNativeContextSnapshot(VIOGPU_WDDM_ALLOCATION *allocation,
                                               VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot)
{
    if (!IsNativeAllocation(allocation) || snapshot == NULL || allocation->NativeContext == NULL ||
        allocation->ContextGeneration <= 0 || allocation->ContextResetGeneration == 0 || allocation->ContextId == 0 ||
        allocation->BackingSize == 0 || allocation->PrivateData.RequestedIova == 0)
    {
        return FALSE;
    }

    if (!VioGpuAdapter::AcquireNativeContextSnapshot(allocation->NativeContext, snapshot))
    {
        return FALSE;
    }

    BOOLEAN matches = snapshot->Registration == allocation->NativeContext &&
                      snapshot->Generation == allocation->ContextGeneration &&
                      snapshot->ResetGeneration == allocation->ContextResetGeneration &&
                      snapshot->ContextId == allocation->ContextId &&
                      snapshot->ResetGeneration == allocation->PrivateData.ExpectedResetGeneration &&
                      snapshot->VaStart != 0 && snapshot->VaSize != 0 &&
                      allocation->PrivateData.RequestedIova >= snapshot->VaStart &&
                      (ULONGLONG)allocation->BackingSize <= snapshot->VaSize &&
                      snapshot->VaStart <= MAXULONGLONG - (snapshot->VaSize - 1) &&
                      allocation->PrivateData.RequestedIova <= snapshot->VaStart + snapshot->VaSize - (ULONGLONG)allocation->BackingSize;
    if (!matches)
    {
        VioGpuAdapter::ReleaseNativeContextSnapshot(snapshot);
    }
    return matches;
}

// The ordinary wrapper below rejects Destroying after this wait.  DestroyAllocation
// alone may retain the mutex to retry fail-closed Host ownership teardown.
NTSTATUS AcquireAllocationLifecycleForDestroy(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    return KeWaitForSingleObject(&allocation->LifecycleMutex, Executive, KernelMode, FALSE, &timeout);
}

NTSTATUS AcquireAllocationLifecycle(VIOGPU_WDDM_ALLOCATION *allocation)
{
    NTSTATUS status = AcquireAllocationLifecycleForDestroy(allocation);
    if (status != STATUS_SUCCESS)
    {
        return status;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&allocation->SubmissionLock, &oldIrql);
    BOOLEAN destroying = allocation->Destroying;
    KeReleaseSpinLock(&allocation->SubmissionLock, oldIrql);
    if (destroying)
    {
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }
    return STATUS_SUCCESS;
}

/* Present owns two allocation lifetimes at once. Every caller must acquire
 * them in address order so concurrent source/destination pairs cannot form a
 * lock cycle. A failed second acquisition intentionally leaves the first
 * mutex held for the caller's common unwind path. */
NTSTATUS AcquirePresentAllocationLifecycles(VIOGPU_WDDM_ALLOCATION *source,
                                            VIOGPU_WDDM_ALLOCATION *destination,
                                            BOOLEAN *sourceLocked,
                                            BOOLEAN *destinationLocked)
{
    if (source == NULL || destination == NULL || source == destination || sourceLocked == NULL ||
        destinationLocked == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *sourceLocked = FALSE;
    *destinationLocked = FALSE;
    BOOLEAN sourceFirst = reinterpret_cast<ULONG_PTR>(source) < reinterpret_cast<ULONG_PTR>(destination);
    VIOGPU_WDDM_ALLOCATION *first = sourceFirst ? source : destination;
    VIOGPU_WDDM_ALLOCATION *second = sourceFirst ? destination : source;
    NTSTATUS status = AcquireAllocationLifecycle(first);
    if (status != STATUS_SUCCESS)
    {
        if (NT_SUCCESS(status))
        {
            status = STATUS_GRAPHICS_ALLOCATION_BUSY;
        }
        return status;
    }
    if (sourceFirst)
    {
        *sourceLocked = TRUE;
    }
    else
    {
        *destinationLocked = TRUE;
    }

    status = AcquireAllocationLifecycle(second);
    if (status != STATUS_SUCCESS)
    {
        if (NT_SUCCESS(status))
        {
            status = STATUS_GRAPHICS_ALLOCATION_BUSY;
        }
        return status;
    }
    if (sourceFirst)
    {
        *destinationLocked = TRUE;
    }
    else
    {
        *sourceLocked = TRUE;
    }
    return STATUS_SUCCESS;
}

// These internal references close the callback/destruction race.  They remain
// synchronous until SubmitCommand has a real completion record to retain them.
NTSTATUS AcquireContextSubmissionReference(VIOGPU_WDDM_CONTEXT *context)
{
    if (context == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    BOOLEAN valid = context->Signature == VIOGPU_WDDM_CONTEXT_SIGNATURE && !context->SubmissionClosing &&
                    context->SubmissionReferences < MAXLONG;
    if (valid)
    {
        ++context->SubmissionReferences;
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    return valid ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

BOOLEAN ReleaseContextSubmissionReference(VIOGPU_WDDM_CONTEXT *context)
{
    if (context == NULL)
    {
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    BOOLEAN released = context->SubmissionReferences != 0;
    if (released)
    {
        --context->SubmissionReferences;
        if (context->SubmissionClosing)
        {
            // Releasing the spin lock must be this owner's final context access.
            KeSetEvent(&context->SubmissionProgressEvent, IO_NO_INCREMENT, FALSE);
        }
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    return released;
}

BOOLEAN RecordContextUmdFence(VIOGPU_WDDM_CONTEXT *context, UINT fenceId)
{
    if (context == NULL || fenceId == 0)
    {
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    UINT submitted = static_cast<UINT>(context->SubmittedUmdFence);
    BOOLEAN valid = context->Signature == VIOGPU_WDDM_CONTEXT_SIGNATURE && !context->SubmissionClosing &&
                    context->UmdFenceCount < VioGpuWddmContextFenceTrackerCapacity &&
                    (submitted == 0 || static_cast<LONG>(fenceId - submitted) > 0);
    for (UINT offset = 0; valid && offset < context->UmdFenceCount; ++offset)
    {
        UINT index = (context->UmdFenceHead + offset) % VioGpuWddmContextFenceTrackerCapacity;
        if (context->UmdFences[index].State != VioGpuWddmContextFenceFree &&
            context->UmdFences[index].FenceId == fenceId)
        {
            valid = FALSE;
        }
    }
    if (valid)
    {
        UINT tail = (context->UmdFenceHead + context->UmdFenceCount) % VioGpuWddmContextFenceTrackerCapacity;
        context->UmdFences[tail].FenceId = fenceId;
        context->UmdFences[tail].State = VioGpuWddmContextFencePending;
        ++context->UmdFenceCount;
        InterlockedExchange(&context->SubmittedUmdFence, static_cast<LONG>(fenceId));
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    return valid;
}

BOOLEAN RetireContextUmdFence(VIOGPU_WDDM_CONTEXT *context, UINT fenceId)
{
    if (context == NULL || fenceId == 0)
    {
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    BOOLEAN valid = context->Signature == VIOGPU_WDDM_CONTEXT_SIGNATURE && context->UmdFenceCount != 0;
    VIOGPU_WDDM_CONTEXT_FENCE_ENTRY *match = NULL;
    if (valid)
    {
        for (UINT offset = 0; offset < context->UmdFenceCount; ++offset)
        {
            UINT index = (context->UmdFenceHead + offset) % VioGpuWddmContextFenceTrackerCapacity;
            if (context->UmdFences[index].State == VioGpuWddmContextFencePending &&
                context->UmdFences[index].FenceId == fenceId)
            {
                match = &context->UmdFences[index];
                break;
            }
        }
    }
    if (match != NULL)
    {
        match->State = VioGpuWddmContextFenceRetired;
        UINT completed = 0;
        while (context->UmdFenceCount != 0 &&
               context->UmdFences[context->UmdFenceHead].State == VioGpuWddmContextFenceRetired)
        {
            VIOGPU_WDDM_CONTEXT_FENCE_ENTRY *head = &context->UmdFences[context->UmdFenceHead];
            completed = head->FenceId;
            head->FenceId = 0;
            head->State = VioGpuWddmContextFenceFree;
            context->UmdFenceHead = (context->UmdFenceHead + 1) % VioGpuWddmContextFenceTrackerCapacity;
            --context->UmdFenceCount;
        }
        if (completed != 0)
        {
            InterlockedExchange(&context->CompletedUmdFence, static_cast<LONG>(completed));
        }
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    return match != NULL;
}

/* A permanent queue failure begins adapter reset.  Any UMD fences which were
 * published before the queue rejected the command must be removed from this
 * context, otherwise a later GET_COMPLETED_FENCE could expose a completion
 * endpoint for work that never entered the Host queue. */
VOID InvalidateContextUmdFenceTracker(VIOGPU_WDDM_CONTEXT *context)
{
    if (context == NULL)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    if (context->Signature == VIOGPU_WDDM_CONTEXT_SIGNATURE)
    {
        UINT completed = static_cast<UINT>(context->CompletedUmdFence);
        context->UmdFenceHead = 0;
        context->UmdFenceCount = 0;
        RtlZeroMemory(context->UmdFences, sizeof(context->UmdFences));
        InterlockedExchange(&context->SubmittedUmdFence, static_cast<LONG>(completed));
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
}

UINT QueryContextCompletedUmdFence(VIOGPU_WDDM_CONTEXT *context)
{
    if (context == NULL)
    {
        return 0;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    UINT completed = context->Signature == VIOGPU_WDDM_CONTEXT_SIGNATURE ? static_cast<UINT>(context->CompletedUmdFence)
                                                                         : 0;
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    return completed;
}

NTSTATUS BeginContextSubmissionRundown(VIOGPU_WDDM_CONTEXT *context)
{
    if (context == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    NTSTATUS status = STATUS_SUCCESS;
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE)
    {
        status = STATUS_INVALID_HANDLE;
    }
    else
    {
        context->SubmissionClosing = TRUE;
        KeClearEvent(&context->SubmissionProgressEvent);
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    return status;
}

BOOLEAN WaitForContextSubmissionProgress(_In_ VIOGPU_WDDM_CONTEXT *context, _Inout_ ULONGLONG *stallDeadline)
{
    if (context == NULL || stallDeadline == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }

    ULONGLONG now = KeQueryInterruptTime();
    LARGE_INTEGER delay;
    delay.QuadPart = now >= *stallDeadline ? 0 : -static_cast<LONGLONG>(*stallDeadline - now);
    NTSTATUS status = KeWaitForSingleObject(&context->SubmissionProgressEvent, Executive, KernelMode, FALSE, &delay);
    if (status != STATUS_SUCCESS)
    {
        return FALSE;
    }
    *stallDeadline = KeQueryInterruptTime() + VIOGPU_WDDM_CONTEXT_DESTROY_STALL_TIMEOUT_100NS;
    return TRUE;
}

NTSTATUS AcquireAllocationSubmissionReference(VIOGPU_WDDM_ALLOCATION *allocation, VioGpuDod *adapter)
{
    if (allocation == NULL || adapter == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&allocation->SubmissionLock, &oldIrql);
    BOOLEAN valid = allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE && allocation->Adapter == adapter &&
                    !allocation->Destroying && allocation->SubmissionReferences < MAXLONG;
    if (valid)
    {
        ++allocation->SubmissionReferences;
    }
    KeReleaseSpinLock(&allocation->SubmissionLock, oldIrql);
    return valid ? STATUS_SUCCESS : STATUS_GRAPHICS_ALLOCATION_BUSY;
}

BOOLEAN ReleaseAllocationSubmissionReference(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL)
    {
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&allocation->SubmissionLock, &oldIrql);
    BOOLEAN released = allocation->SubmissionReferences != 0;
    if (released)
    {
        --allocation->SubmissionReferences;
    }
    KeReleaseSpinLock(&allocation->SubmissionLock, oldIrql);
    return released;
}

NTSTATUS ReferenceAllocationOpen(VIOGPU_WDDM_ALLOCATION *allocation, VioGpuDod *adapter)
{
    if (allocation == NULL || adapter == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&allocation->SubmissionLock, &oldIrql);
    BOOLEAN valid = allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE && allocation->Adapter == adapter &&
                    !allocation->Destroying && allocation->OpenReferences < MAXLONG;
    if (valid)
    {
        ++allocation->OpenReferences;
    }
    KeReleaseSpinLock(&allocation->SubmissionLock, oldIrql);
    return valid ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

BOOLEAN ReleaseAllocationOpen(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL)
    {
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&allocation->SubmissionLock, &oldIrql);
    BOOLEAN released = allocation->OpenReferences != 0;
    if (released)
    {
        --allocation->OpenReferences;
    }
    KeReleaseSpinLock(&allocation->SubmissionLock, oldIrql);
    return released;
}

NTSTATUS BeginAllocationDestroy(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&allocation->SubmissionLock, &oldIrql);
    NTSTATUS status = STATUS_SUCCESS;
    if (allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE)
    {
        status = STATUS_INVALID_HANDLE;
    }
    else
    {
        allocation->Destroying = TRUE;
        if (allocation->SubmissionReferences != 0 || allocation->OpenReferences != 0)
        {
            status = STATUS_GRAPHICS_ALLOCATION_BUSY;
        }
    }
    KeReleaseSpinLock(&allocation->SubmissionLock, oldIrql);
    return status;
}

NTSTATUS AcquireRenderAllocationReferences(const VIOGPU_WDDM_RENDER_COMMAND *header,
                                           VIOGPU_WDDM_DEVICE *device,
                                           const DXGK_ALLOCATIONLIST *allocationList,
                                           UINT allocationListSize,
                                           UINT *acquiredCount)
{
    if (header == NULL || device == NULL || allocationList == NULL || acquiredCount == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *acquiredCount = 0;

    const VIOGPU_WDDM_ALLOCATION_REFERENCE *references = reinterpret_cast<const VIOGPU_WDDM_ALLOCATION_REFERENCE *>(
                                                                                                        reinterpret_cast<const BYTE *>(header) +
                                                                                                        header->AllocationReferencesOffset);
    for (UINT index = 0; index < header->AllocationReferenceCount; ++index)
    {
        const VIOGPU_WDDM_ALLOCATION_REFERENCE *reference = &references[index];
        if (reference->AllocationIndex >= allocationListSize)
        {
            break;
        }
        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationList[reference->AllocationIndex].hDeviceSpecificAllocation);
        VIOGPU_WDDM_ALLOCATION *allocation = deviceAllocation == NULL || deviceAllocation->Signature != VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE || deviceAllocation->Device != device || !IsOwnedAllocation(deviceAllocation->Allocation, device->Adapter)
                                                                                                                                                 ? NULL
                                                                                                                                                 : deviceAllocation->Allocation;
        NTSTATUS status = AcquireAllocationLifecycle(allocation);
        if (status == STATUS_SUCCESS)
        {
            status = AcquireAllocationSubmissionReference(allocation, device->Adapter);
            KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        }
        else if (NT_SUCCESS(status))
        {
            status = STATUS_GRAPHICS_ALLOCATION_BUSY;
        }
        if (status != STATUS_SUCCESS)
        {
            for (UINT rollback = 0; rollback < *acquiredCount; ++rollback)
            {
                const VIOGPU_WDDM_ALLOCATION_REFERENCE *rollbackReference = &references[rollback];
                VIOGPU_WDDM_OPEN_ALLOCATION *rollbackOpen = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationList[rollbackReference->AllocationIndex].hDeviceSpecificAllocation);
                if (rollbackOpen != NULL)
                {
                    ReleaseAllocationSubmissionReference(rollbackOpen->Allocation);
                }
            }
            *acquiredCount = 0;
            return status;
        }
        ++(*acquiredCount);
    }
    if (*acquiredCount == header->AllocationReferenceCount)
    {
        return STATUS_SUCCESS;
    }

    for (UINT rollback = 0; rollback < *acquiredCount; ++rollback)
    {
        const VIOGPU_WDDM_ALLOCATION_REFERENCE *rollbackReference = &references[rollback];
        if (rollbackReference->AllocationIndex >= allocationListSize)
        {
            continue;
        }
        VIOGPU_WDDM_OPEN_ALLOCATION *rollbackOpen = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationList[rollbackReference->AllocationIndex].hDeviceSpecificAllocation);
        if (rollbackOpen != NULL)
        {
            ReleaseAllocationSubmissionReference(rollbackOpen->Allocation);
        }
    }
    *acquiredCount = 0;
    return STATUS_INVALID_HANDLE;
}

VOID FreeRenderSubmissionStorage(_Inout_ VIOGPU_WDDM_SUBMISSION *submission)
{
    if (submission == NULL)
    {
        return;
    }

    delete[] submission->References;
    submission->References = NULL;
}

NTSTATUS PublishPreparedSubmission(VIOGPU_WDDM_SUBMISSION *submission,
                                   VIOGPU_WDDM_CONTEXT *context,
                                   const VIOGPU_WDDM_RENDER_COMMAND *header,
                                   VIOGPU_WDDM_DEVICE *device,
                                   const DXGK_ALLOCATIONLIST *allocationList,
                                   UINT allocationListSize,
                                   UINT allocationCount,
                                   PVOID dmaBuffer,
                                   UINT dmaBufferSize,
                                   PVOID dmaPrivateData,
                                   UINT dmaPrivateDataSize,
                                   UINT commandLength,
                                   PGPU_VBUFFER virtioBuffer,
                                   BOOLEAN fullyPrepatched,
                                   const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot)
{
    if (submission == NULL || context == NULL || header == NULL || device == NULL || allocationList == NULL ||
        snapshot == NULL || snapshot->Adapter == NULL || allocationCount == 0 ||
        allocationCount > VioGpuWddmSubmissionAllocationLimit || allocationCount != header->AllocationReferenceCount ||
        dmaBuffer == NULL || dmaBufferSize < commandLength || dmaPrivateData == NULL ||
        dmaPrivateDataSize < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) || commandLength == 0 ||
        header->CommandStreamSize < sizeof(MSM_CCMD_GEM_SUBMIT_REQ) || header->CommandStreamOffset >= commandLength ||
        header->CommandStreamSize > commandLength - header->CommandStreamOffset || virtioBuffer == NULL ||
        snapshot->ContextId == 0 || snapshot->Generation <= 0 || snapshot->ResetGeneration == 0 ||
        !snapshot->Adapter->IsNativeContextGenerationCurrent(snapshot->Generation, snapshot->ResetGeneration))
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(submission, sizeof(*submission));
    submission->Signature = VIOGPU_WDDM_SUBMISSION_SIGNATURE;
    submission->ReferenceCount = 1;
    submission->State = VioGpuWddmSubmissionPrepared;
    submission->CancelRequested = 0;
    submission->WorkReferenceHeld = 0;
    InitializeListHead(&submission->ContextEntry.Link);
    submission->ContextEntry.Kind = VioGpuWddmContextSubmissionRender;
    submission->ContextEntry.Owner = submission;
    submission->ContextEntry.Context = context;
    InitializeListHead(&submission->Work.Link);
    submission->Work.Routine = NativeRenderDispatchWorker;
    submission->Work.CancelRoutine = NativeRenderDispatchCancelled;
    submission->Work.Context = submission;
    submission->Work.CancelRequested = &submission->CancelRequested;
    submission->Context = context;
    submission->DmaBuffer = dmaBuffer;
    submission->DmaBufferSize = dmaBufferSize;
    submission->DmaPrivateData = dmaPrivateData;
    submission->DmaPrivateDataSize = dmaPrivateDataSize;
    submission->CommandLength = commandLength;
    submission->ContextId = snapshot->ContextId;
    submission->Generation = snapshot->Generation;
    submission->ResetGeneration = snapshot->ResetGeneration;
    submission->FenceId = 0;
    const MSM_CCMD_GEM_SUBMIT_REQ *submitRequest = reinterpret_cast<const MSM_CCMD_GEM_SUBMIT_REQ *>(static_cast<const BYTE *>(dmaBuffer) +
                                                                                                     header->CommandStreamOffset);
    submission->UmdFenceId = submitRequest->fence;
    if (submission->UmdFenceId == 0)
    {
        submission->Signature = 0;
        return STATUS_INVALID_PARAMETER;
    }
    submission->VirtioBuffer = virtioBuffer;
    submission->Adapter = device->Adapter;
    submission->CommandStream = static_cast<BYTE *>(dmaBuffer) + header->CommandStreamOffset;
    submission->CommandStreamSize = header->CommandStreamSize;
    submission->CommandStreamOffset = header->CommandStreamOffset;
    submission->PatchApplied = FALSE;
    submission->FullyPrepatched = fullyPrepatched;
    submission->AllocationCount = 0;
    VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = static_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(dmaPrivateData);
    if (privateData->Submission != NULL)
    {
        submission->Signature = 0;
        return STATUS_INVALID_DEVICE_STATE;
    }

    submission->References = new (NonPagedPoolNx) VIOGPU_WDDM_SUBMISSION_REFERENCE[allocationCount];
    if (submission->References == NULL)
    {
        FreeRenderSubmissionStorage(submission);
        submission->Signature = 0;
        return STATUS_NO_MEMORY;
    }
    RtlZeroMemory(submission->References, (SIZE_T)allocationCount * sizeof(*submission->References));

    const VIOGPU_WDDM_ALLOCATION_REFERENCE *references = reinterpret_cast<const VIOGPU_WDDM_ALLOCATION_REFERENCE *>(
                                                                                                        reinterpret_cast<const BYTE *>(header) +
                                                                                                        header->AllocationReferencesOffset);
    for (UINT index = 0; index < allocationCount; ++index)
    {
        const VIOGPU_WDDM_ALLOCATION_REFERENCE *reference = &references[index];
        if (reference->AllocationIndex >= allocationListSize)
        {
            FreeRenderSubmissionStorage(submission);
            submission->Signature = 0;
            return STATUS_INVALID_HANDLE;
        }
        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationList[reference->AllocationIndex].hDeviceSpecificAllocation);
        if (deviceAllocation == NULL || deviceAllocation->Signature != VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE ||
            deviceAllocation->Device != device || deviceAllocation->Allocation == NULL ||
            deviceAllocation->Allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
            deviceAllocation->Allocation->Adapter != device->Adapter)
        {
            FreeRenderSubmissionStorage(submission);
            submission->Signature = 0;
            return STATUS_INVALID_HANDLE;
        }
        submission->References[index].Allocation = deviceAllocation->Allocation;
        submission->References[index].AllocationIndex = reference->AllocationIndex;
        submission->References[index].Flags = reference->Flags;
        submission->References[index].AllocationOffset = reference->AllocationOffset;
        submission->References[index].Length = reference->Length;
        submission->References[index].PatchOffset = reference->PatchOffset;
        submission->References[index].Reserved = 0;
        ++submission->AllocationCount;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    BOOLEAN valid = context->Signature == VIOGPU_WDDM_CONTEXT_SIGNATURE && context->Device == device &&
                    !context->SubmissionClosing && context->SubmissionReferences > 0 &&
                    context->SubmissionReferences < MAXLONG;
    if (valid)
    {
        privateData->Submission = submission;
        KeMemoryBarrier();
        virtioBuffer->complete_cb = NativeSubmissionComplete;
        virtioBuffer->complete_ctx = submission;
        virtioBuffer->cancel_cb = NativeSubmissionCancelled;
        virtioBuffer->cancel_ctx = submission;
        virtioBuffer->queue_error_cb = NativeSubmissionQueueFailed;
        virtioBuffer->queue_error_ctx = submission;
        virtioBuffer->auto_release = false;
        if (VioGpuArmVbufferTerminalCallbacks(virtioBuffer))
        {
            InsertTailList(&context->PendingSubmissions, &submission->ContextEntry.Link);
        }
        else
        {
            VioGpuDetachVbufferTerminalCallbacks(virtioBuffer);
            privateData->Submission = NULL;
            valid = FALSE;
        }
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    if (!valid)
    {
        FreeRenderSubmissionStorage(submission);
        submission->Signature = 0;
        submission->AllocationCount = 0;
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

BOOLEAN ReferenceRenderSubmission(_Inout_ VIOGPU_WDDM_SUBMISSION *submission)
{
    if (submission == NULL || submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE)
    {
        return FALSE;
    }

    LONG references = InterlockedCompareExchange(&submission->ReferenceCount, 0, 0);
    while (references > 0 && references < MAXLONG)
    {
        LONG observed = InterlockedCompareExchange(&submission->ReferenceCount, references + 1, references);
        if (observed == references)
        {
            return TRUE;
        }
        references = observed;
    }
    return FALSE;
}

VOID DereferenceRenderSubmission(_Inout_ VIOGPU_WDDM_SUBMISSION *submission)
{
    if (submission == NULL)
    {
        return;
    }

    LONG references = InterlockedDecrement(&submission->ReferenceCount);
    NT_ASSERT(references >= 0);
    if (references == 0)
    {
        NT_ASSERT(submission->Signature == VIOGPU_WDDM_SUBMISSION_SIGNATURE);
        NT_ASSERT(InterlockedCompareExchange(&submission->State, 0, 0) == VioGpuWddmSubmissionQuarantined);
        NT_ASSERT(submission->VirtioBuffer == NULL);

        for (UINT index = 0; index < submission->AllocationCount; ++index)
        {
            BOOLEAN released = ReleaseAllocationSubmissionReference(submission->References[index].Allocation);
            NT_ASSERT(released);
            UNREFERENCED_PARAMETER(released);
            submission->References[index].Allocation = NULL;
        }
        submission->AllocationCount = 0;
        FreeRenderSubmissionStorage(submission);

        VIOGPU_WDDM_CONTEXT *context = submission->Context;
        BOOLEAN contextReleased = ReleaseContextSubmissionReference(context);
        NT_ASSERT(contextReleased);
        UNREFERENCED_PARAMETER(contextReleased);

        submission->ContextEntry.Owner = NULL;
        submission->ContextEntry.Context = NULL;
        submission->Context = NULL;
        submission->Adapter = NULL;
        submission->DmaBuffer = NULL;
        submission->DmaPrivateData = NULL;
        submission->CommandStream = NULL;
        submission->Signature = 0;
        delete submission;
    }
}

BOOLEAN AcquireRenderWorkReference(_Inout_ VIOGPU_WDDM_SUBMISSION *submission)
{
    if (!ReferenceRenderSubmission(submission))
    {
        return FALSE;
    }
    if (InterlockedCompareExchange(&submission->WorkReferenceHeld, 1, 0) == 0)
    {
        return TRUE;
    }
    DereferenceRenderSubmission(submission);
    return FALSE;
}

VOID ReleaseRenderWorkReference(_Inout_ VIOGPU_WDDM_SUBMISSION *submission)
{
    if (submission != NULL && InterlockedCompareExchange(&submission->WorkReferenceHeld, 0, 1) == 1)
    {
        DereferenceRenderSubmission(submission);
    }
}

BOOLEAN QuarantineSubmission(VIOGPU_WDDM_SUBMISSION *submission,
                             LONG expectedState,
                             BOOLEAN releaseBuffer,
                             BOOLEAN terminalCallbackOwned = FALSE)
{
    if (submission == NULL || submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE || submission->Context == NULL)
    {
        return FALSE;
    }

    VIOGPU_WDDM_CONTEXT *context = submission->Context;
    PGPU_VBUFFER virtioBuffer = NULL;
    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    BOOLEAN owned = InterlockedCompareExchange(&submission->State, expectedState, expectedState) == expectedState;
    BOOLEAN linked = owned && submission->ContextEntry.Link.Flink != &submission->ContextEntry.Link &&
                     submission->ContextEntry.Link.Blink != &submission->ContextEntry.Link &&
                     submission->ContextEntry.Kind == VioGpuWddmContextSubmissionRender &&
                     submission->ContextEntry.Owner == submission && submission->ContextEntry.Context == context;
    virtioBuffer = linked ? submission->VirtioBuffer : NULL;
    BOOLEAN terminalOwned = virtioBuffer == NULL || terminalCallbackOwned;
    if (linked && !terminalOwned)
    {
        terminalOwned = VioGpuClaimVbufferTerminalCallbacks(virtioBuffer) == VioGpuVbufferTerminalClaimWon;
    }
    if (linked && terminalOwned)
    {
        RemoveEntryList(&submission->ContextEntry.Link);
        InitializeListHead(&submission->ContextEntry.Link);
        InterlockedExchange(&submission->State, VioGpuWddmSubmissionQuarantined);
        submission->VirtioBuffer = NULL;
        if (virtioBuffer != NULL)
        {
            VioGpuDetachVbufferTerminalCallbacks(virtioBuffer);
        }
    }
    else if (linked)
    {
        linked = FALSE;
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    if (!linked)
    {
        return FALSE;
    }

    if (submission->DmaPrivateData != NULL && submission->DmaPrivateDataSize >= sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE))
    {
        VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = static_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(submission->DmaPrivateData);
        if (privateData->Submission == submission)
        {
            privateData->Submission = NULL;
        }
    }
    VioGpuDod *adapter = submission->Adapter;
    if (releaseBuffer && virtioBuffer != NULL)
    {
        if (adapter != NULL)
        {
            adapter->ReleaseNativeSubmitBuffer(virtioBuffer);
        }
        else
        {
            VioGpuCompleteVbufferTerminalCallbacks(virtioBuffer);
        }
    }
    submission->ContextEntry.Owner = NULL;
    submission->ContextEntry.Context = NULL;
    DereferenceRenderSubmission(submission);
    return TRUE;
}

VOID ReleasePreparedSubmission(VIOGPU_WDDM_SUBMISSION *submission)
{
    QuarantineSubmission(submission, VioGpuWddmSubmissionPrepared, TRUE);
}

BOOLEAN QuarantineTerminalSubmission(VIOGPU_WDDM_SUBMISSION *submission, BOOLEAN releaseBuffer)
{
    if (submission == NULL)
    {
        return FALSE;
    }
    LONG state = InterlockedCompareExchange(&submission->State, 0, 0);
    return state >= VioGpuWddmSubmissionPrepared && state <= VioGpuWddmSubmissionHostIssued &&
           QuarantineSubmission(submission, state, releaseBuffer, TRUE);
}

VOID NativeSubmissionComplete(_In_opt_ PVOID callbackContext)
{
    VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(callbackContext);
    if (submission == NULL || submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE ||
        !ReferenceRenderSubmission(submission))
    {
        return;
    }

    VioGpuDod *adapter = submission->Adapter;
    VIOGPU_WDDM_CONTEXT *context = submission->Context;
    PGPU_VBUFFER buffer = submission->VirtioBuffer;
    BOOLEAN workReferenceHeld = InterlockedCompareExchange(&submission->WorkReferenceHeld, 0, 0) == 1;
    if (adapter == NULL || context == NULL || buffer == NULL || !workReferenceHeld)
    {
        if (adapter != NULL)
        {
            if (context != NULL && buffer != NULL)
            {
                QuarantineTerminalSubmission(submission, TRUE);
            }
            else if (buffer != NULL)
            {
                adapter->ReleaseNativeSubmitBuffer(buffer);
            }
            adapter->RequestHardwareResetAtAnyIrql();
            if (workReferenceHeld)
            {
                adapter->CompleteNativePassiveWork(&submission->Work);
                ReleaseRenderWorkReference(submission);
            }
        }
        DereferenceRenderSubmission(submission);
        return;
    }
    UINT fenceId = static_cast<UINT>(submission->FenceId);
    UINT nodeOrdinal = context->NodeOrdinal;
    UINT engineOrdinal = 0;
    const UINT expectedFlags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    PGPU_CMD_SUBMIT_3D command = reinterpret_cast<PGPU_CMD_SUBMIT_3D>(buffer->buf);
    PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(buffer->resp_buf);

    BOOLEAN valid = InterlockedCompareExchange(&submission->State,
                                               VioGpuWddmSubmissionHostIssued,
                                               VioGpuWddmSubmissionHostIssued) == VioGpuWddmSubmissionHostIssued &&
                    submission->FenceId != 0 && submission->FenceId <= MAXUINT && nodeOrdinal == 0 &&
                    context->EngineAffinity == 1 && buffer->size == sizeof(GPU_CMD_SUBMIT_3D) &&
                    buffer->data_buf != NULL && buffer->data_size == submission->CommandStreamSize &&
                    buffer->resp_size == sizeof(GPU_CTRL_HDR) && buffer->response_size == sizeof(GPU_CTRL_HDR) &&
                    command != NULL && command->hdr.type == VIRTIO_GPU_CMD_SUBMIT_3D &&
                    command->hdr.flags == expectedFlags && command->hdr.fence_id == submission->FenceId &&
                    command->hdr.ctx_id == submission->ContextId && command->hdr.ring_idx == 1 &&
                    command->hdr.padding[0] == 0 && command->hdr.padding[1] == 0 && command->hdr.padding[2] == 0 &&
                    command->size == submission->CommandStreamSize && command->num_in_fences == 0 && response != NULL &&
                    response->type == VIRTIO_GPU_RESP_OK_NODATA && response->flags == expectedFlags &&
                    response->fence_id == submission->FenceId && response->ctx_id == submission->ContextId &&
                    response->ring_idx == 1 && response->padding[0] == 0 && response->padding[1] == 0 &&
                    response->padding[2] == 0 &&
                    adapter->IsNativeContextGenerationCurrent(submission->Generation, submission->ResetGeneration);

    if (valid)
    {
        valid = RetireContextUmdFence(context, submission->UmdFenceId);
    }

    if (!QuarantineTerminalSubmission(submission, TRUE))
    {
        adapter->RequestHardwareResetAtAnyIrql();
        adapter->ReleaseNativeSubmitBuffer(buffer);
        adapter->CompleteNativePassiveWork(&submission->Work);
        ReleaseRenderWorkReference(submission);
        DereferenceRenderSubmission(submission);
        return;
    }

    if (valid)
    {
        adapter->NotifyNativeSubmissionCompletion(fenceId, nodeOrdinal, engineOrdinal, FALSE);
    }
    else
    {
        adapter->NotifyNativeSubmissionFault(fenceId,
                                             STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                             nodeOrdinal,
                                             engineOrdinal,
                                             FALSE);
    }
    adapter->CompleteNativePassiveWork(&submission->Work);
    ReleaseRenderWorkReference(submission);
    DereferenceRenderSubmission(submission);
}

VOID NativeSubmissionCancelled(_In_opt_ PVOID callbackContext)
{
    VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(callbackContext);
    if (submission == NULL || submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE ||
        submission->Adapter == NULL || submission->Context == NULL)
    {
        return;
    }
    if (!ReferenceRenderSubmission(submission))
    {
        return;
    }
    BOOLEAN workReferenceHeld = InterlockedCompareExchange(&submission->WorkReferenceHeld, 0, 0) == 1;

    /* Reset/close owns the GPU_VBUFFER in this path.  Release only the WDDM
     * context/allocation record, and never fabricate scheduler completion. */
    VioGpuDod *adapter = submission->Adapter;
    InvalidateContextUmdFenceTracker(submission->Context);
    if (!QuarantineTerminalSubmission(submission, FALSE))
    {
        adapter->RequestHardwareResetAtAnyIrql();
    }
    if (workReferenceHeld)
    {
        adapter->CompleteNativePassiveWork(&submission->Work);
        ReleaseRenderWorkReference(submission);
    }
    DereferenceRenderSubmission(submission);
}

VOID NativeSubmissionQueueFailed(_In_opt_ PVOID callbackContext)
{
    VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(callbackContext);
    if (submission == NULL || submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE ||
        !ReferenceRenderSubmission(submission))
    {
        return;
    }

    VioGpuDod *adapter = submission->Adapter;
    VIOGPU_WDDM_CONTEXT *context = submission->Context;
    BOOLEAN workReferenceHeld = InterlockedCompareExchange(&submission->WorkReferenceHeld, 0, 0) == 1;
    if (adapter == NULL || context == NULL || submission->FenceId == 0 || submission->FenceId > MAXUINT ||
        !workReferenceHeld)
    {
        if (adapter != NULL)
        {
            if (context != NULL)
            {
                QuarantineTerminalSubmission(submission, FALSE);
            }
            adapter->RequestHardwareResetAtAnyIrql();
            if (workReferenceHeld)
            {
                adapter->CompleteNativePassiveWork(&submission->Work);
                ReleaseRenderWorkReference(submission);
            }
        }
        DereferenceRenderSubmission(submission);
        return;
    }
    UINT fenceId = static_cast<UINT>(submission->FenceId);
    UINT nodeOrdinal = context->NodeOrdinal;
    UINT engineOrdinal = 0;
    InvalidateContextUmdFenceTracker(submission->Context);
    if (QuarantineTerminalSubmission(submission, FALSE))
    {
        adapter->NotifyNativeSubmissionFault(fenceId,
                                             STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                             nodeOrdinal,
                                             engineOrdinal,
                                             FALSE);
    }
    else
    {
        adapter->RequestHardwareResetAtAnyIrql();
    }
    adapter->CompleteNativePassiveWork(&submission->Work);
    ReleaseRenderWorkReference(submission);
    DereferenceRenderSubmission(submission);
}

_Use_decl_annotations_ VOID NativeRenderDispatchWorker(PVOID callbackContext)
{
    VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(callbackContext);
    if (submission == NULL || submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE ||
        submission->Adapter == NULL || submission->VirtioBuffer == NULL || submission->FenceId == 0 ||
        submission->FenceId > MAXUINT || InterlockedCompareExchange(&submission->WorkReferenceHeld, 0, 0) != 1 ||
        InterlockedCompareExchange(&submission->CancelRequested, 0, 0) != 0 ||
        InterlockedCompareExchange(&submission->State,
                                   VioGpuWddmSubmissionHostIssued,
                                   VioGpuWddmSubmissionEngineQueued) != VioGpuWddmSubmissionEngineQueued)
    {
        if (submission != NULL && submission->Adapter != NULL)
        {
            VioGpuDod *adapter = submission->Adapter;
            QuarantineSubmission(submission, VioGpuWddmSubmissionEngineQueued, TRUE);
            adapter->RequestHardwareResetAtAnyIrql();
            adapter->CompleteNativePassiveWork(&submission->Work);
            ReleaseRenderWorkReference(submission);
        }
        return;
    }

    VioGpuDod *adapter = submission->Adapter;
    UINT fenceId = static_cast<UINT>(submission->FenceId);
    UINT nodeOrdinal = submission->Context->NodeOrdinal;
    BOOLEAN operationAcquired = adapter->AcquireNativeSubmissionOperation();
    int queueResult = operationAcquired ? adapter->QueueNativeSubmit(submission->VirtioBuffer, fenceId) : -1;
    if (operationAcquired)
    {
        adapter->ReleaseNativeSubmissionOperation();
    }
    if (queueResult >= 0)
    {
        return;
    }

    InvalidateContextUmdFenceTracker(submission->Context);
    QuarantineSubmission(submission, VioGpuWddmSubmissionHostIssued, TRUE);
    adapter->NotifyNativeSubmissionFault(fenceId, STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE, nodeOrdinal, 0, TRUE);
    adapter->CompleteNativePassiveWork(&submission->Work);
    ReleaseRenderWorkReference(submission);
}

_Use_decl_annotations_ VOID NativeRenderDispatchCancelled(PVOID callbackContext)
{
    VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(callbackContext);
    if (submission == NULL || submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE)
    {
        return;
    }
    InterlockedExchange(&submission->CancelRequested, 1);
    InvalidateContextUmdFenceTracker(submission->Context);
    QuarantineSubmission(submission, VioGpuWddmSubmissionEngineQueued, TRUE);
    ReleaseRenderWorkReference(submission);
}

NTSTATUS ResolveSubmissionPrivateData(PVOID privateDataBase,
                                      UINT privateDataSize,
                                      UINT submissionStart,
                                      UINT submissionEnd,
                                      VioGpuDod *adapter,
                                      HANDLE runtimeContext,
                                      VIOGPU_WDDM_SUBMISSION **submissionOut)
{
    if (submissionOut == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *submissionOut = NULL;
    if (privateDataBase == NULL || adapter == NULL || runtimeContext == NULL || submissionStart > privateDataSize ||
        submissionEnd < submissionStart || submissionEnd > privateDataSize ||
        submissionEnd - submissionStart != sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = reinterpret_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(static_cast<BYTE *>(privateDataBase) +
                                                                                               submissionStart);
    if (privateData->Signature != VIOGPU_WDDM_DMA_SIGNATURE || privateData->Version != VioGpuWddmDmaPrivateVersion ||
        privateData->Kind != VioGpuWddmDmaKindRender || privateData->DmaBuffer == NULL ||
        privateData->DmaBufferSize < privateData->CommandLength || privateData->CommandLength == 0 ||
        privateData->ContextId == 0 || privateData->Generation <= 0 || privateData->ResetGeneration == 0 ||
        privateData->Flags != 0 || privateData->Packet != privateData->DmaBuffer ||
        privateData->PacketLength != privateData->CommandLength || privateData->Reserved != 0 ||
        privateData->Submission == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_CONTEXT *context = reinterpret_cast<VIOGPU_WDDM_CONTEXT *>(runtimeContext);
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE)
    {
        return STATUS_INVALID_HANDLE;
    }

    VIOGPU_WDDM_SUBMISSION *submission = NULL;
    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    for (PLIST_ENTRY link = context->PendingSubmissions.Flink;
         !context->SubmissionClosing && link != &context->PendingSubmissions;
         link = link->Flink)
    {
        VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY *entry = CONTAINING_RECORD(link,
                                                                        VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY,
                                                                        Link);
        if (entry->Kind == VioGpuWddmContextSubmissionRender && entry->Context == context &&
            entry->Owner == privateData->Submission)
        {
            VIOGPU_WDDM_SUBMISSION *candidate = static_cast<VIOGPU_WDDM_SUBMISSION *>(entry->Owner);
            if (candidate != NULL && candidate->ContextEntry.Owner == candidate &&
                candidate->ContextEntry.Context == context && ReferenceRenderSubmission(candidate))
            {
                submission = candidate;
            }
            break;
        }
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    if (submission == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    if (submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE || submission->Context == NULL ||
        submission->Context != context || submission->Context->Type != VioGpuWddmContextNative ||
        submission->Adapter != adapter || submission->DmaBuffer != privateData->DmaBuffer ||
        submission->DmaBufferSize != privateData->DmaBufferSize || submission->DmaPrivateData != privateData ||
        submission->DmaPrivateDataSize != sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) ||
        submission->CommandLength != privateData->CommandLength || submission->ContextId != privateData->ContextId ||
        submission->Generation != privateData->Generation ||
        submission->ResetGeneration != privateData->ResetGeneration || submission->CommandStream == NULL ||
        submission->CommandStreamSize == 0 || submission->CommandStreamOffset >= submission->CommandLength ||
        submission->CommandStreamSize > submission->CommandLength - submission->CommandStreamOffset ||
        submission->AllocationCount == 0 || submission->AllocationCount > VioGpuWddmSubmissionAllocationLimit ||
        submission->References == NULL || submission->VirtioBuffer == NULL ||
        !adapter->IsNativeContextGenerationCurrent(submission->Generation, submission->ResetGeneration))
    {
        DereferenceRenderSubmission(submission);
        return STATUS_DEVICE_NOT_READY;
    }

    *submissionOut = submission;
    return STATUS_SUCCESS;
}

VOID ReleaseRenderAllocationReferences(const VIOGPU_WDDM_RENDER_COMMAND *header,
                                       const DXGK_ALLOCATIONLIST *allocationList,
                                       UINT allocationListSize,
                                       UINT acquiredCount)
{
    if (header == NULL || allocationList == NULL || acquiredCount == 0)
    {
        return;
    }

    const VIOGPU_WDDM_ALLOCATION_REFERENCE *references = reinterpret_cast<const VIOGPU_WDDM_ALLOCATION_REFERENCE *>(
                                                                                                        reinterpret_cast<const BYTE *>(header) +
                                                                                                        header->AllocationReferencesOffset);
    UINT count = acquiredCount < header->AllocationReferenceCount ? acquiredCount : header->AllocationReferenceCount;
    for (UINT index = 0; index < count; ++index)
    {
        const VIOGPU_WDDM_ALLOCATION_REFERENCE *reference = &references[index];
        if (reference->AllocationIndex >= allocationListSize)
        {
            continue;
        }
        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationList[reference->AllocationIndex].hDeviceSpecificAllocation);
        if (deviceAllocation != NULL)
        {
            ReleaseAllocationSubmissionReference(deviceAllocation->Allocation);
        }
    }
}

VOID ClearAllocationHostBinding(VIOGPU_WDDM_ALLOCATION *allocation)
{
    allocation->HostState = VioGpuWddmAllocationHostNone;
    allocation->BoundGeneration = 0;
    allocation->BoundResetGeneration = 0;
    allocation->BoundContextId = 0;
}

BOOLEAN AllocationResetRetired(VIOGPU_WDDM_ALLOCATION *allocation)
{
    return allocation != NULL && allocation->NativeContext != NULL && allocation->ContextResetGeneration != 0 &&
           VioGpuAdapter::IsNativeContextAllocationBindingRetired(allocation->NativeContext) &&
           allocation->Adapter != NULL &&
           allocation->Adapter->IsNativeContextResetRetired(allocation->ContextResetGeneration);
}

NTSTATUS ReleaseAllocationHostOwnership(VIOGPU_WDDM_ALLOCATION *allocation,
                                        VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                                        BOOLEAN snapshotAcquired)
{
    if (allocation == NULL || !IsNativeAllocation(allocation) ||
        allocation->ResourceId < VIOGPU_NATIVE_RESOURCE_ID_START || allocation->ResourceId == MAXUINT ||
        allocation->BlobId != allocation->ResourceId)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (allocation->HostState == VioGpuWddmAllocationHostNone)
    {
        return STATUS_SUCCESS;
    }
    if (AllocationResetRetired(allocation))
    {
        ClearAllocationHostBinding(allocation);
        return STATUS_SUCCESS;
    }
    if (!snapshotAcquired || snapshot == NULL || snapshot->Adapter == NULL ||
        allocation->BoundGeneration != snapshot->Generation ||
        allocation->BoundResetGeneration != snapshot->ResetGeneration ||
        allocation->BoundContextId != snapshot->ContextId || allocation->HostState == VioGpuWddmAllocationHostUnknown)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    BOOLEAN released = FALSE;
    VIOGPU_HOST_CONTEXT_RESULT result = snapshot->Adapter->DestroyNativeGuestAllocation(snapshot,
                                                                                        allocation->ResourceId,
                                                                                        &released);
    if (released)
    {
        ClearAllocationHostBinding(allocation);
        return STATUS_SUCCESS;
    }
    if (result == VioGpuHostContextUnknown)
    {
        allocation->HostState = VioGpuWddmAllocationHostUnknown;
    }
    return STATUS_DEVICE_NOT_READY;
}

NTSTATUS ValidateNativePlacement(VIOGPU_WDDM_ALLOCATION *allocation, LARGE_INTEGER segmentAddress, ULONGLONG *offset)
{
    if (allocation == NULL || offset == NULL || allocation->BackingSize == 0 || segmentAddress.QuadPart < 0 ||
        ((ULONGLONG)segmentAddress.QuadPart & (PAGE_SIZE - 1)) != 0 ||
        (ULONGLONG)segmentAddress.QuadPart > MAXULONGLONG - ((ULONGLONG)allocation->BackingSize - 1))
    {
        return STATUS_INVALID_PARAMETER;
    }
    *offset = (ULONGLONG)segmentAddress.QuadPart;
    return STATUS_SUCCESS;
}

NTSTATUS ResolveTransferMdlAddress(PMDL mdl, UINT mdlOffset, SIZE_T transferSize, PVOID *address)
{
    if (mdl == NULL || address == NULL || transferSize == 0 || MmGetMdlByteOffset(mdl) != 0 ||
        (mdl->MdlFlags & MDL_PAGES_LOCKED) == 0 || mdlOffset > (MAXULONG_PTR >> PAGE_SHIFT))
    {
        return STATUS_INVALID_PARAMETER;
    }
    *address = NULL;

    SIZE_T byteOffset = (SIZE_T)mdlOffset << PAGE_SHIFT;
    SIZE_T byteCount = MmGetMdlByteCount(mdl);
    if (byteOffset > byteCount || transferSize > byteCount - byteOffset)
    {
        return STATUS_INVALID_PARAMETER;
    }

    PVOID baseAddress = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
    if (baseAddress == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *address = static_cast<PUCHAR>(baseAddress) + byteOffset;
    return STATUS_SUCCESS;
}

NTSTATUS CopyAperturePlacement(VIOGPU_WDDM_ALLOCATION *allocation,
                               SIZE_T allocationOffset,
                               SIZE_T transferSize,
                               PVOID systemAddress,
                               BOOLEAN toSegment)
{
    if (allocation == NULL || allocation->ApertureAddress == NULL || systemAddress == NULL || transferSize == 0 ||
        allocationOffset > allocation->BackingSize || transferSize > allocation->BackingSize - allocationOffset)
    {
        return STATUS_INVALID_PARAMETER;
    }

    PVOID apertureAddress = static_cast<PUCHAR>(allocation->ApertureAddress) + allocationOffset;
    if (toSegment)
    {
        RtlCopyMemory(apertureAddress, systemAddress, transferSize);
        KeMemoryBarrier();
    }
    else
    {
        KeMemoryBarrier();
        RtlCopyMemory(systemAddress, apertureAddress, transferSize);
    }
    return STATUS_SUCCESS;
}

NTSTATUS FillAperturePlacement(VIOGPU_WDDM_ALLOCATION *allocation, SIZE_T fillSize, UINT pattern)
{
    if (allocation == NULL || allocation->ApertureAddress == NULL || fillSize == 0 ||
        fillSize > allocation->BackingSize || (fillSize & (sizeof(ULONG) - 1)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    for (SIZE_T offset = 0; offset < fillSize; offset += sizeof(pattern))
    {
        RtlCopyMemory(static_cast<PUCHAR>(allocation->ApertureAddress) + offset, &pattern, sizeof(pattern));
    }
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}

BOOLEAN ResolveStandard2DFormat(D3DDDIFORMAT format, _Out_ UINT *virtioFormat)
{
    if (virtioFormat == NULL)
    {
        return FALSE;
    }
    switch (format)
    {
        case D3DDDIFMT_A8R8G8B8:
            *virtioFormat = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
            return TRUE;
        case D3DDDIFMT_X8R8G8B8:
            *virtioFormat = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
            return TRUE;
        case D3DDDIFMT_A8B8G8R8:
            *virtioFormat = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
            return TRUE;
        case D3DDDIFMT_X8B8G8R8:
            *virtioFormat = VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
            return TRUE;
        default:
            *virtioFormat = 0;
            return FALSE;
    }
}

VOID PublishNativePlacement(VIOGPU_WDDM_ALLOCATION *allocation,
                            const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                            ULONGLONG segmentOffset,
                            VIOGPU_WDDM_ALLOCATION_HOST_STATE hostState)
{
    allocation->PlacementOffset = segmentOffset;
    allocation->PlacementValid = TRUE;
    allocation->HostState = hostState;
    allocation->BoundGeneration = snapshot->Generation;
    allocation->BoundResetGeneration = snapshot->ResetGeneration;
    allocation->BoundContextId = snapshot->ContextId;
}

VOID PublishStandardPlacement(VIOGPU_WDDM_ALLOCATION *allocation, ULONGLONG segmentOffset)
{
    allocation->PlacementOffset = segmentOffset;
    allocation->PlacementValid = TRUE;
}

VOID ClearNativePlacement(VIOGPU_WDDM_ALLOCATION *allocation)
{
    allocation->PlacementOffset = 0;
    allocation->PlacementValid = FALSE;
}

BOOLEAN ReconcileStandard2DAllocationAfterReset(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (!IsStandardAllocation(allocation) || allocation->Adapter == NULL)
    {
        return FALSE;
    }

    BOOLEAN retired = FALSE;
    BOOLEAN valid = allocation->Adapter->Reconcile2DResourceAfterReset(&allocation->Resource2DState,
                                                                       &allocation->Resource2DResetGeneration,
                                                                       &retired);
    if (!valid || !retired)
    {
        return valid;
    }

    return TRUE;
}

/* Stages for RecordNative2DBackingDiagnostic.  A present that finds its source
 * unbacked reports only Resource2DState == 0, which is the same value for all
 * four ways this can fail, so name them. */
#define VIOGPU_2D_BACKING_STAGE_RECONCILE 1
#define VIOGPU_2D_BACKING_STAGE_PLACEMENT 2
#define VIOGPU_2D_BACKING_STAGE_FORMAT 3
#define VIOGPU_2D_BACKING_STAGE_ENTRIES 4
#define VIOGPU_2D_BACKING_STAGE_HOST 5

DWORD BuildPresentPlacementDiagnosticState(_In_opt_ const VIOGPU_WDDM_ALLOCATION *allocation);

VOID RecordBackingFailure(_In_ const VIOGPU_WDDM_ALLOCATION *allocation, _In_ DWORD stage, _In_ DWORD detail)
{
    if (allocation != NULL && allocation->Adapter != NULL)
    {
        allocation->Adapter->RecordNative2DBackingDiagnostic(stage,
                                                             detail,
                                                             allocation->ResourceId,
                                                             allocation->Width,
                                                             allocation->Height,
                                                             static_cast<DWORD>(allocation->BackingSize & MAXULONG));
    }
}

BOOLEAN EnsureStandard2DAllocationBacking(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (!ReconcileStandard2DAllocationAfterReset(allocation))
    {
        RecordBackingFailure(allocation,
                             VIOGPU_2D_BACKING_STAGE_RECONCILE,
                             allocation != NULL ? static_cast<DWORD>(allocation->Resource2DState) : 0);
        return FALSE;
    }
    if (allocation->Resource2DState == VioGpu2DResourceBackingAttached)
    {
        return allocation->PlacementValid && allocation->ApertureMdl != NULL && allocation->ApertureAddress != NULL &&
               allocation->ApertureMappedPageCount == allocation->AperturePageCount;
    }
    if (!allocation->PlacementValid || allocation->ApertureMdl == NULL || allocation->ApertureAddress == NULL ||
        allocation->ApertureMappedPageCount != allocation->AperturePageCount)
    {
        RecordBackingFailure(allocation,
                             VIOGPU_2D_BACKING_STAGE_PLACEMENT,
                             BuildPresentPlacementDiagnosticState(allocation));
        return FALSE;
    }

    UINT virtioFormat = 0;
    if (!ResolveStandard2DFormat(allocation->Format, &virtioFormat))
    {
        RecordBackingFailure(allocation, VIOGPU_2D_BACKING_STAGE_FORMAT, static_cast<DWORD>(allocation->Format));
        return FALSE;
    }

    GPU_MEM_ENTRY *entries = NULL;
    UINT entryCount = 0;
    NTSTATUS status = AllocateApertureBackingEntries(allocation, &entries, &entryCount);
    if (!NT_SUCCESS(status))
    {
        RecordBackingFailure(allocation, VIOGPU_2D_BACKING_STAGE_ENTRIES, static_cast<DWORD>(status));
        return FALSE;
    }

    VIOGPU_HOST_CONTEXT_RESULT result = allocation->Adapter->Create2DResourceBacking(allocation->ResourceId,
                                                                                     virtioFormat,
                                                                                     allocation->Width,
                                                                                     allocation->Height,
                                                                                     allocation->BackingSize,
                                                                                     entries,
                                                                                     entryCount,
                                                                                     &allocation->Resource2DState,
                                                                                     &allocation->Resource2DResetGeneration);
    ExFreePoolWithTag(entries, 'eSGV');
    if (result != VioGpuHostContextConfirmed || allocation->Resource2DState != VioGpu2DResourceBackingAttached)
    {
        RecordBackingFailure(allocation,
                             VIOGPU_2D_BACKING_STAGE_HOST,
                             (static_cast<DWORD>(result) << 8) | static_cast<DWORD>(allocation->Resource2DState));
        return FALSE;
    }
    return TRUE;
}

BOOLEAN ReconcileGdiSourcePlacementAfterReset(VIOGPU_WDDM_ALLOCATION *allocation)
{
    /* Once the miniport publishes no flip capability, dxgkrnl stops flipping DWM's
     * swapchain and converts it into a blt between two primaries, so the Present
     * source on a GDI context is a standard primary rather than a CPU-visible GDI
     * surface.  The GDI source class is left exactly as it is; a primary is
     * admitted alongside it. */
    if ((!IsGdiSourceAllocation(allocation) && !IsStandardPrimaryAllocation(allocation)) ||
        allocation->Adapter == NULL || !allocation->PlacementValid ||
        !EnsureStandard2DAllocationBacking(allocation))
    {
        return FALSE;
    }

    return allocation->ApertureMdl != NULL && allocation->ApertureAddress != NULL &&
           allocation->ApertureMappedPageCount == allocation->AperturePageCount &&
           allocation->Resource2DState == VioGpu2DResourceBackingAttached && allocation->Resource2DResetGeneration != 0;
}

BOOLEAN HasGdiPresentIdentity(_In_ const VIOGPU_WDDM_ALLOCATION *allocation,
                              _In_ const VIOGPU_WDDM_CONTEXT *context,
                              _In_ const VioGpuDod *adapter)
{
    return allocation != NULL && context != NULL && adapter != NULL &&
           allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE && allocation->Adapter == adapter &&
           context->Type == VioGpuWddmContextGdi &&
           (IsGdiSourceAllocation(allocation) || IsStandardPrimaryAllocation(allocation)) &&
           allocation->HostState == VioGpuWddmAllocationHostNone && allocation->BlobId == 0 &&
           allocation->ResourceId != 0 && allocation->ResourceId < VIOGPU_NATIVE_RESOURCE_ID_START &&
           allocation->ContextId == 0 && allocation->ContextGeneration == 0 && allocation->ContextResetGeneration == 0;
}

BOOLEAN HasLiveGdiPresentIdentity(_In_ const VIOGPU_WDDM_ALLOCATION *allocation,
                                  _In_ const VIOGPU_WDDM_CONTEXT *context,
                                  _In_ const VioGpuDod *adapter)
{
    return HasGdiPresentIdentity(allocation, context, adapter) &&
           allocation->Resource2DState == VioGpu2DResourceBackingAttached &&
           allocation->Resource2DResetGeneration != 0 && allocation->PlacementValid &&
           allocation->ApertureMdl != NULL && allocation->ApertureAddress != NULL &&
           allocation->ApertureMappedPageCount == allocation->AperturePageCount;
}

DWORD BuildPresentPlacementDiagnosticState(_In_opt_ const VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL)
    {
        return 0;
    }

    DWORD state = 0;
    state |= allocation->PlacementValid ? 1U << 0 : 0;
    state |= allocation->ApertureMdl != NULL ? 1U << 1 : 0;
    state |= allocation->ApertureAddress != NULL ? 1U << 2 : 0;
    state |= allocation->ApertureMappedPageCount == allocation->AperturePageCount ? 1U << 3 : 0;
    state |= allocation->Destroying ? 1U << 4 : 0;
    state |= allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE ? 1U << 5 : 0;
    return state;
}

DWORD PresentDiagnosticLowPart(_In_ ULONGLONG value)
{
    return static_cast<DWORD>(value & MAXULONG);
}

DWORD PresentDiagnosticHighPart(_In_ ULONGLONG value)
{
    return static_cast<DWORD>(value >> 32);
}

VOID InitializePresentExecutionDiagnostic(_In_opt_ const VIOGPU_WDDM_PRESENT_TRANSACTION *transaction,
                                          _In_ VIOGPU_WDDM_PRESENT_EXECUTION_STAGE stage,
                                          _In_ NTSTATUS status,
                                          _In_ DWORD detail,
                                          _Out_ VIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC *diagnostic)
{
    RtlZeroMemory(diagnostic, sizeof(*diagnostic));
    diagnostic->Stage = static_cast<DWORD>(stage);
    diagnostic->Status = static_cast<DWORD>(status);
    diagnostic->Detail = detail;
    if (transaction == NULL)
    {
        return;
    }

    diagnostic->FenceId = transaction->FenceId;
    diagnostic->TransactionState = static_cast<DWORD>(InterlockedCompareExchange(const_cast<volatile LONG *>(&transaction->State),
                                                                                 0,
                                                                                 0));
    diagnostic->ContextType = transaction->Context == NULL ? 0 : static_cast<DWORD>(transaction->Context->Type);
}

VOID BuildPresentExecutionDiagnostic(_In_ const VIOGPU_WDDM_PRESENT_TRANSACTION *transaction,
                                     _In_ VIOGPU_WDDM_PRESENT_EXECUTION_STAGE stage,
                                     _In_ NTSTATUS status,
                                     _In_ DWORD detail,
                                     _Out_ VIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC *diagnostic)
{
    InitializePresentExecutionDiagnostic(transaction, stage, status, detail, diagnostic);
    const VIOGPU_WDDM_ALLOCATION *source = transaction->Source;
    const VIOGPU_WDDM_ALLOCATION *destination = transaction->Destination;
    diagnostic->SourceResourceId = source == NULL ? 0 : source->ResourceId;
    diagnostic->DestinationResourceId = destination == NULL ? 0 : destination->ResourceId;
    diagnostic->SourcePlacementState = BuildPresentPlacementDiagnosticState(source);
    diagnostic->DestinationPlacementState = BuildPresentPlacementDiagnosticState(destination);
    diagnostic->SourceResource2DState = source == NULL ? 0 : static_cast<DWORD>(source->Resource2DState);
    diagnostic->DestinationResource2DState = destination == NULL ? 0 : static_cast<DWORD>(destination->Resource2DState);

    ULONGLONG sourcePlacementOffset = source == NULL ? 0 : source->PlacementOffset;
    ULONGLONG destinationPlacementOffset = destination == NULL ? 0 : destination->PlacementOffset;
    diagnostic->SourcePlacementOffsetLow = PresentDiagnosticLowPart(sourcePlacementOffset);
    diagnostic->SourcePlacementOffsetHigh = PresentDiagnosticHighPart(sourcePlacementOffset);
    diagnostic->DestinationPlacementOffsetLow = PresentDiagnosticLowPart(destinationPlacementOffset);
    diagnostic->DestinationPlacementOffsetHigh = PresentDiagnosticHighPart(destinationPlacementOffset);
    diagnostic->TransactionSourcePlacementOffsetLow = PresentDiagnosticLowPart(transaction->SourcePlacementOffset);
    diagnostic->TransactionSourcePlacementOffsetHigh = PresentDiagnosticHighPart(transaction->SourcePlacementOffset);
    diagnostic->TransactionDestinationPlacementOffsetLow = PresentDiagnosticLowPart(transaction->DestinationPlacementOffset);
    diagnostic->TransactionDestinationPlacementOffsetHigh = PresentDiagnosticHighPart(transaction->DestinationPlacementOffset);

    ULONGLONG sourceResetGeneration = source == NULL ? 0 : source->Resource2DResetGeneration;
    ULONGLONG destinationResetGeneration = destination == NULL ? 0 : destination->Resource2DResetGeneration;
    diagnostic->SourceResetGenerationLow = PresentDiagnosticLowPart(sourceResetGeneration);
    diagnostic->SourceResetGenerationHigh = PresentDiagnosticHighPart(sourceResetGeneration);
    diagnostic->DestinationResetGenerationLow = PresentDiagnosticLowPart(destinationResetGeneration);
    diagnostic->DestinationResetGenerationHigh = PresentDiagnosticHighPart(destinationResetGeneration);
    diagnostic->TransactionDestinationResetGenerationLow = PresentDiagnosticLowPart(transaction->DestinationResetGeneration);
    diagnostic->TransactionDestinationResetGenerationHigh = PresentDiagnosticHighPart(transaction->DestinationResetGeneration);
}

DWORD BuildPresentAllocationListDiagnosticValue(_In_ const DXGK_ALLOCATIONLIST *entry)
{
    if (entry == NULL)
    {
        return 0;
    }
    return (entry->WriteOperation != 0 ? 1U : 0U) | ((entry->SegmentId & 0x1FU) << 1) |
           ((entry->Reserved & 0x03FFFFFFU) << 6);
}

VOID RecordPresentEntryRejection(_In_opt_ VIOGPU_WDDM_CONTEXT *context,
                                 _In_opt_ const DXGKARG_PRESENT *present,
                                 _In_ VIOGPU_WDDM_PRESENT_DIAGNOSTIC_REASON reason)
{
    if (context == NULL || present == NULL || context->Device == NULL || context->Device->Adapter == NULL)
    {
        return;
    }

    VIOGPU_NATIVE_PRESENT_DIAGNOSTIC diagnostic = {};
    diagnostic.ContextType = static_cast<DWORD>(context->Type);
    diagnostic.PresentFlags = present->Flags.Value;
    diagnostic.SubRectCount = present->SubRectCnt;
    diagnostic.MultipassOffset = present->MultipassOffset;
    context->Device->Adapter->RecordNativePresentDiagnostic(static_cast<DWORD>(reason),
                                                            STATUS_INVALID_PARAMETER,
                                                            &diagnostic);
}

VOID RecordPresentDiagnostic(_In_ VIOGPU_WDDM_CONTEXT *context,
                             _In_ const DXGKARG_PRESENT *present,
                             _In_ const VIOGPU_WDDM_ALLOCATION *source,
                             _In_ const VIOGPU_WDDM_ALLOCATION *destination,
                             _In_ VIOGPU_WDDM_PRESENT_DIAGNOSTIC_REASON reason,
                             _In_ NTSTATUS status)
{
    if (context == NULL || context->Device == NULL || context->Device->Adapter == NULL || present == NULL ||
        present->pAllocationList == NULL || source == NULL || destination == NULL ||
        reason == VioGpuWddmPresentDiagnosticNone)
    {
        return;
    }

    const DXGK_ALLOCATIONLIST *sourceEntry = &present->pAllocationList[DXGK_PRESENT_SOURCE_INDEX];
    const DXGK_ALLOCATIONLIST *destinationEntry = &present->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX];
    VIOGPU_NATIVE_PRESENT_DIAGNOSTIC diagnostic = {};
    diagnostic.ContextType = static_cast<DWORD>(context->Type);
    diagnostic.PresentFlags = present->Flags.Value;
    diagnostic.SubRectCount = present->SubRectCnt;
    diagnostic.MultipassOffset = present->MultipassOffset;
    diagnostic.SourceFlags = source->Flags;
    diagnostic.DestinationFlags = destination->Flags;
    diagnostic.SourceHostState = static_cast<DWORD>(source->HostState);
    diagnostic.DestinationHostState = static_cast<DWORD>(destination->HostState);
    diagnostic.SourceResource2DState = static_cast<DWORD>(source->Resource2DState);
    diagnostic.DestinationResource2DState = static_cast<DWORD>(destination->Resource2DState);
    diagnostic.SourcePlacementState = BuildPresentPlacementDiagnosticState(source);
    diagnostic.DestinationPlacementState = BuildPresentPlacementDiagnosticState(destination);
    diagnostic.SourceFormat = static_cast<DWORD>(source->Format);
    diagnostic.DestinationFormat = static_cast<DWORD>(destination->Format);
    diagnostic.SourceWidth = source->Width;
    diagnostic.SourceHeight = source->Height;
    diagnostic.SourcePitch = source->Pitch;
    diagnostic.DestinationWidth = destination->Width;
    diagnostic.DestinationHeight = destination->Height;
    diagnostic.DestinationPitch = destination->Pitch;
    diagnostic.SourceAllocationListValue = BuildPresentAllocationListDiagnosticValue(sourceEntry);
    diagnostic.DestinationAllocationListValue = BuildPresentAllocationListDiagnosticValue(destinationEntry);
    diagnostic.SourceResourceId = source->ResourceId;
    diagnostic.DestinationResourceId = destination->ResourceId;
    diagnostic.SourceRectLeft = static_cast<DWORD>(present->SrcRect.left);
    diagnostic.SourceRectTop = static_cast<DWORD>(present->SrcRect.top);
    diagnostic.SourceRectRight = static_cast<DWORD>(present->SrcRect.right);
    diagnostic.SourceRectBottom = static_cast<DWORD>(present->SrcRect.bottom);
    diagnostic.DestinationRectLeft = static_cast<DWORD>(present->DstRect.left);
    diagnostic.DestinationRectTop = static_cast<DWORD>(present->DstRect.top);
    diagnostic.DestinationRectRight = static_cast<DWORD>(present->DstRect.right);
    diagnostic.DestinationRectBottom = static_cast<DWORD>(present->DstRect.bottom);
    context->Device->Adapter->RecordNativePresentDiagnostic(static_cast<DWORD>(reason), status, &diagnostic);
}

BOOLEAN ValidatePresentRect(const RECT *rect, UINT width, UINT height)
{
    return rect != NULL && rect->left >= 0 && rect->top >= 0 && rect->right > rect->left && rect->bottom > rect->top &&
           static_cast<UINT>(rect->right) <= width && static_cast<UINT>(rect->bottom) <= height;
}

BOOLEAN ValidatePresentPrepatchEntry(_In_ const DXGK_ALLOCATIONLIST *entry,
                                     _In_ const VIOGPU_WDDM_ALLOCATION *allocation,
                                     _In_ BOOLEAN writeOperation,
                                     _Out_ BOOLEAN *prepatched)
{
    if (prepatched == NULL)
    {
        return FALSE;
    }
    *prepatched = FALSE;
    if (entry == NULL || allocation == NULL || entry->Reserved != 0 ||
        (entry->WriteOperation != 0) != (writeOperation != FALSE))
    {
        return FALSE;
    }
    if (entry->SegmentId == 0)
    {
        return TRUE;
    }
    if (entry->SegmentId != VIOGPU_WDDM_SEGMENT_ID || entry->PhysicalAddress.QuadPart < 0 ||
        static_cast<ULONGLONG>(entry->PhysicalAddress.QuadPart) != allocation->PlacementOffset)
    {
        return FALSE;
    }
    *prepatched = TRUE;
    return TRUE;
}

BOOLEAN ValidatePresentGeometry(_In_ const VIOGPU_WDDM_ALLOCATION *source,
                                _In_ const VIOGPU_WDDM_ALLOCATION *destination,
                                _In_ const RECT *sourceRect,
                                _In_ const RECT *destinationRect,
                                _In_reads_(rectCount) const RECT *destinationSubRects,
                                _In_ UINT rectCount)
{
    if (source == NULL || destination == NULL || sourceRect == NULL || destinationRect == NULL ||
        destinationSubRects == NULL || rectCount == 0 || rectCount > VIOGPU_WDDM_PRESENT_RECTS_PER_PASS ||
        source->Format != destination->Format || !IsSupportedSurfaceFormat(source->Format) ||
        static_cast<ULONGLONG>(source->Pitch) < static_cast<ULONGLONG>(source->Width) * 4 ||
        static_cast<ULONGLONG>(destination->Pitch) < static_cast<ULONGLONG>(destination->Width) * 4 ||
        static_cast<ULONGLONG>(source->Pitch) * source->Height > source->BackingSize ||
        static_cast<ULONGLONG>(destination->Pitch) * destination->Height > destination->BackingSize ||
        !ValidatePresentRect(sourceRect, source->Width, source->Height) ||
        !ValidatePresentRect(destinationRect, destination->Width, destination->Height) ||
        sourceRect->right - sourceRect->left != destinationRect->right - destinationRect->left ||
        sourceRect->bottom - sourceRect->top != destinationRect->bottom - destinationRect->top)
    {
        return FALSE;
    }

    for (UINT index = 0; index < rectCount; ++index)
    {
        const RECT *rect = &destinationSubRects[index];
        LONGLONG sourceLeft = static_cast<LONGLONG>(sourceRect->left) +
                              (static_cast<LONGLONG>(rect->left) - destinationRect->left);
        LONGLONG sourceTop = static_cast<LONGLONG>(sourceRect->top) +
                             (static_cast<LONGLONG>(rect->top) - destinationRect->top);
        LONGLONG sourceRight = sourceLeft + (static_cast<LONGLONG>(rect->right) - rect->left);
        LONGLONG sourceBottom = sourceTop + (static_cast<LONGLONG>(rect->bottom) - rect->top);
        if (!ValidatePresentRect(rect, destination->Width, destination->Height) || rect->left < destinationRect->left ||
            rect->top < destinationRect->top || rect->right > destinationRect->right ||
            rect->bottom > destinationRect->bottom || sourceLeft < sourceRect->left || sourceTop < sourceRect->top ||
            sourceRight > sourceRect->right || sourceBottom > sourceRect->bottom)
        {
            return FALSE;
        }
    }
    return TRUE;
}

BOOLEAN ReferencePresentTransaction(VIOGPU_WDDM_PRESENT_TRANSACTION *transaction)
{
    if (transaction == NULL || transaction->Signature != VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE)
    {
        return FALSE;
    }

    LONG references = InterlockedCompareExchange(&transaction->ReferenceCount, 0, 0);
    while (references > 0 && references < MAXLONG)
    {
        LONG observed = InterlockedCompareExchange(&transaction->ReferenceCount, references + 1, references);
        if (observed == references)
        {
            return TRUE;
        }
        references = observed;
    }
    return FALSE;
}

VOID DereferencePresentTransaction(VIOGPU_WDDM_PRESENT_TRANSACTION *transaction)
{
    if (transaction == NULL)
    {
        return;
    }

    LONG references = InterlockedDecrement(&transaction->ReferenceCount);
    NT_ASSERT(references >= 0);
    if (references != 0)
    {
        return;
    }

    NT_ASSERT(transaction->Signature == VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE);
    NT_ASSERT(transaction->Context != NULL && transaction->Adapter != NULL && transaction->Source != NULL &&
              transaction->Destination != NULL);
    VIOGPU_WDDM_CONTEXT *context = transaction->Context;
    VioGpuDod *adapter = transaction->Adapter;
    BOOLEAN sourceReleased = ReleaseAllocationSubmissionReference(transaction->Source);
    BOOLEAN destinationReleased = ReleaseAllocationSubmissionReference(transaction->Destination);
    BOOLEAN contextReleased = ReleaseContextSubmissionReference(context);
    NT_ASSERT(sourceReleased && destinationReleased && contextReleased);
    UNREFERENCED_PARAMETER(sourceReleased);
    UNREFERENCED_PARAMETER(destinationReleased);
    UNREFERENCED_PARAMETER(contextReleased);

    NT_ASSERT(InterlockedCompareExchange(&transaction->WorkReferenceHeld, 0, 0) == 0);
    delete[] transaction->DestinationSubRects;
    transaction->DestinationSubRects = NULL;
    transaction->Signature = 0;
    delete transaction;
    UNREFERENCED_PARAMETER(adapter);
}

BOOLEAN AcquirePresentWorkReference(_Inout_ VIOGPU_WDDM_PRESENT_TRANSACTION *transaction)
{
    if (!ReferencePresentTransaction(transaction))
    {
        return FALSE;
    }
    if (InterlockedCompareExchange(&transaction->WorkReferenceHeld, 1, 0) == 0)
    {
        return TRUE;
    }
    DereferencePresentTransaction(transaction);
    return FALSE;
}

VOID ReleasePresentWorkReference(_Inout_ VIOGPU_WDDM_PRESENT_TRANSACTION *transaction)
{
    if (transaction != NULL && InterlockedCompareExchange(&transaction->WorkReferenceHeld, 0, 1) == 1)
    {
        DereferencePresentTransaction(transaction);
    }
}

BOOLEAN RegisterPresentTransaction(VIOGPU_WDDM_PRESENT_TRANSACTION *transaction)
{
    if (transaction == NULL || transaction->Adapter == NULL || !ReferencePresentTransaction(transaction))
    {
        return FALSE;
    }
    if (transaction->Adapter->RegisterWddmPresentTransaction(&transaction->AdapterLink))
    {
        return TRUE;
    }
    DereferencePresentTransaction(transaction);
    return FALSE;
}

VOID UnregisterPresentTransaction(VIOGPU_WDDM_PRESENT_TRANSACTION *transaction)
{
    if (transaction != NULL && transaction->Adapter != NULL &&
        transaction->Adapter->UnregisterWddmPresentTransaction(&transaction->AdapterLink))
    {
        DereferencePresentTransaction(transaction);
    }
}

BOOLEAN ValidatePresentDmaSubmissionRange(_In_ const VIOGPU_WDDM_PRESENT_TRANSACTION *transaction,
                                          _In_ PVOID dmaBuffer,
                                          _In_ UINT dmaBufferSize,
                                          _In_ UINT submissionStart,
                                          _In_ UINT submissionEnd)
{
    return transaction != NULL && dmaBuffer != NULL && submissionStart <= submissionEnd &&
           submissionEnd <= dmaBufferSize &&
           submissionEnd - submissionStart == sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET) &&
           transaction->DmaBuffer == static_cast<BYTE *>(dmaBuffer) + submissionStart &&
           transaction->DmaBufferSize == dmaBufferSize - submissionStart;
}

BOOLEAN ValidateRenderDmaSubmissionRange(_In_ const VIOGPU_WDDM_SUBMISSION *submission,
                                         _In_ PVOID dmaBuffer,
                                         _In_ UINT dmaBufferSize,
                                         _In_ UINT submissionStart,
                                         _In_ UINT submissionEnd)
{
    return submission != NULL && dmaBuffer != NULL && submissionStart <= submissionEnd &&
           submissionEnd <= dmaBufferSize && submissionEnd - submissionStart == submission->CommandLength &&
           submission->DmaBuffer == static_cast<BYTE *>(dmaBuffer) + submissionStart &&
           submission->DmaBufferSize == dmaBufferSize - submissionStart;
}

BOOLEAN ValidatePresentSubmitDmaRange(_In_ const VIOGPU_WDDM_PRESENT_TRANSACTION *transaction,
                                      _In_ UINT dmaBufferSize,
                                      _In_ UINT submissionStart,
                                      _In_ UINT submissionEnd)
{
    return transaction != NULL && transaction->DmaBuffer != NULL && submissionStart <= submissionEnd &&
           submissionEnd <= dmaBufferSize &&
           submissionEnd - submissionStart == sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET) &&
           transaction->DmaBufferSize == dmaBufferSize - submissionStart;
}

BOOLEAN ValidateRenderSubmitDmaRange(_In_ const VIOGPU_WDDM_SUBMISSION *submission,
                                     _In_ UINT dmaBufferSize,
                                     _In_ UINT submissionStart,
                                     _In_ UINT submissionEnd)
{
    return submission != NULL && submission->DmaBuffer != NULL && submissionStart <= submissionEnd &&
           submissionEnd <= dmaBufferSize && submissionEnd - submissionStart == submission->CommandLength &&
           submission->DmaBufferSize == dmaBufferSize - submissionStart;
}

NTSTATUS ResolvePresentTransaction(PVOID privateDataBase,
                                   UINT privateDataSize,
                                   UINT submissionStart,
                                   UINT submissionEnd,
                                   VioGpuDod *adapter,
                                   HANDLE runtimeContext,
                                   LONG expectedState,
                                   VIOGPU_WDDM_PRESENT_TRANSACTION **transactionOut)
{
    if (transactionOut == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *transactionOut = NULL;
    if (privateDataBase == NULL || adapter == NULL || runtimeContext == NULL || submissionStart > privateDataSize ||
        submissionEnd < submissionStart || submissionEnd > privateDataSize ||
        submissionEnd - submissionStart != sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_CONTEXT *context = reinterpret_cast<VIOGPU_WDDM_CONTEXT *>(runtimeContext);
    VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = reinterpret_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(static_cast<BYTE *>(privateDataBase) +
                                                                                               submissionStart);
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE || privateData->Submission == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = NULL;
    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    for (PLIST_ENTRY link = context->PendingSubmissions.Flink;
         !context->SubmissionClosing && link != &context->PendingSubmissions;
         link = link->Flink)
    {
        VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY *entry = CONTAINING_RECORD(link,
                                                                        VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY,
                                                                        Link);
        if (entry->Kind == VioGpuWddmContextSubmissionPresent && entry->Context == context &&
            entry->Owner == privateData->Submission)
        {
            VIOGPU_WDDM_PRESENT_TRANSACTION *candidate = static_cast<VIOGPU_WDDM_PRESENT_TRANSACTION *>(entry->Owner);
            LONG state = candidate == NULL ? VioGpuWddmPresentInvalid
                                           : InterlockedCompareExchange(&candidate->State, 0, 0);
            if (candidate != NULL && candidate->ContextEntry.Owner == candidate &&
                candidate->ContextEntry.Context == context &&
                candidate->Signature == VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE && candidate->Context == context &&
                candidate->Adapter == adapter && (expectedState < 0 || state == expectedState) &&
                ReferencePresentTransaction(candidate))
            {
                transaction = candidate;
            }
            break;
        }
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    if (transaction == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_WDDM_PRESENT_DMA_PACKET *packet = static_cast<VIOGPU_WDDM_PRESENT_DMA_PACKET *>(privateData->Packet);
    BOOLEAN nativeIdentity = HasLiveNativePresentIdentity(transaction->Source, context, adapter) &&
                             transaction->Source->ContextId == privateData->ContextId &&
                             transaction->Source->ContextGeneration == privateData->Generation &&
                             transaction->Source->ContextResetGeneration == privateData->ResetGeneration;
    BOOLEAN gdiIdentity = HasGdiPresentIdentity(transaction->Source, context, adapter) && privateData->ContextId == 0 &&
                          privateData->Generation == 0 && privateData->ResetGeneration == 0 &&
                          transaction->Source->ContextId == 0 && transaction->Source->ContextGeneration == 0 &&
                          transaction->Source->ContextResetGeneration == 0;
    if (!ValidatePresentDmaPacket(privateData, packet, transaction) ||
        transaction->PrivateDataSize != sizeof(*privateData) || (!nativeIdentity && !gdiIdentity))
    {
        DereferencePresentTransaction(transaction);
        return STATUS_DEVICE_NOT_READY;
    }

    *transactionOut = transaction;
    return STATUS_SUCCESS;
}

BOOLEAN RetirePresentTransaction(VIOGPU_WDDM_PRESENT_TRANSACTION *transaction,
                                 LONG expectedState,
                                 VIOGPU_WDDM_PRESENT_STATE finalState)
{
    if (transaction == NULL || transaction->Signature != VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE ||
        transaction->Context == NULL || transaction->Adapter == NULL ||
        (finalState != VioGpuWddmPresentFinished && finalState != VioGpuWddmPresentCancelled))
    {
        return FALSE;
    }

    VIOGPU_WDDM_CONTEXT *context = transaction->Context;
    KIRQL oldIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
    BOOLEAN owned = InterlockedCompareExchange(&transaction->State, finalState, expectedState) == expectedState;
    BOOLEAN linked = transaction->ContextEntry.Link.Flink != &transaction->ContextEntry.Link &&
                     transaction->ContextEntry.Link.Blink != &transaction->ContextEntry.Link &&
                     transaction->ContextEntry.Kind == VioGpuWddmContextSubmissionPresent &&
                     transaction->ContextEntry.Owner == transaction && transaction->ContextEntry.Context == context;
    if (owned && linked)
    {
        RemoveEntryList(&transaction->ContextEntry.Link);
        InitializeListHead(&transaction->ContextEntry.Link);
    }
    if (owned && transaction->PrivateData != NULL && transaction->PrivateData->Submission == transaction)
    {
        transaction->PrivateData->Submission = NULL;
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    if (!owned)
    {
        return FALSE;
    }

    UnregisterPresentTransaction(transaction);
    transaction->ContextEntry.Owner = NULL;
    transaction->ContextEntry.Context = NULL;
    DereferencePresentTransaction(transaction);
    return TRUE;
}

VOID ProbePresentCopy(_In_ const VIOGPU_WDDM_PRESENT_TRANSACTION *transaction,
                      _Out_ VIOGPU_NATIVE_PRESENT_COPY_PROBE *probe)
{
    RtlZeroMemory(probe, sizeof(*probe));
    probe->SourceHash = 2166136261U;
    probe->DestinationHash = 2166136261U;
    probe->FenceId = transaction->FenceId;
    probe->SourceResourceId = transaction->Source->ResourceId;
    probe->DestinationResourceId = transaction->Destination->ResourceId;
    probe->RectCount = transaction->RectCount;
    probe->HostPresentResult = static_cast<DWORD>(VioGpuHostContextNotSubmitted);

    const PUCHAR sourceBase = static_cast<PUCHAR>(transaction->Source->ApertureAddress);
    const PUCHAR destinationBase = static_cast<PUCHAR>(transaction->Destination->ApertureAddress);
    for (UINT rectIndex = 0; rectIndex < transaction->RectCount && probe->SampleCount < 256; ++rectIndex)
    {
        const RECT *rect = &transaction->DestinationSubRects[rectIndex];
        UINT width = static_cast<UINT>(rect->right - rect->left);
        UINT height = static_cast<UINT>(rect->bottom - rect->top);
        UINT xCells = width < 4 ? width : 4;
        UINT yCells = height < 4 ? height : 4;
        for (UINT row = 0; row < yCells && probe->SampleCount < 256; ++row)
        {
            SIZE_T relativeY = (static_cast<SIZE_T>(2 * row + 1) * height) / (2 * yCells);
            SIZE_T destinationY = static_cast<SIZE_T>(rect->top) + relativeY;
            SIZE_T sourceY = static_cast<SIZE_T>(transaction->SourceRect.top) +
                             static_cast<SIZE_T>(rect->top - transaction->DestinationRect.top) + relativeY;
            for (UINT column = 0; column < xCells && probe->SampleCount < 256; ++column)
            {
                SIZE_T relativeX = (static_cast<SIZE_T>(2 * column + 1) * width) / (2 * xCells);
                SIZE_T destinationX = static_cast<SIZE_T>(rect->left) + relativeX;
                SIZE_T sourceX = static_cast<SIZE_T>(transaction->SourceRect.left) +
                                 static_cast<SIZE_T>(rect->left - transaction->DestinationRect.left) + relativeX;
                SIZE_T sourceOffset = sourceY * transaction->Source->Pitch + sourceX * 4;
                SIZE_T destinationOffset = destinationY * transaction->Destination->Pitch + destinationX * 4;
                DWORD sourcePixel = 0;
                DWORD destinationPixel = 0;
                RtlCopyMemory(&sourcePixel, sourceBase + sourceOffset, sizeof(sourcePixel));
                RtlCopyMemory(&destinationPixel, destinationBase + destinationOffset, sizeof(destinationPixel));
                if (probe->SampleCount == 0)
                {
                    probe->SourceFirstPixel = sourcePixel;
                    probe->DestinationFirstPixel = destinationPixel;
                }
                probe->SourceRgbNonzero += (sourcePixel & 0x00FFFFFFU) != 0 ? 1 : 0;
                probe->DestinationRgbNonzero += (destinationPixel & 0x00FFFFFFU) != 0 ? 1 : 0;
                probe->SourceHash = (probe->SourceHash ^ sourcePixel) * 16777619U;
                probe->DestinationHash = (probe->DestinationHash ^ destinationPixel) * 16777619U;
                ++probe->SampleCount;
            }
        }
    }
}

NTSTATUS ExecutePresentTransaction(VIOGPU_WDDM_PRESENT_TRANSACTION *transaction,
                                   VIOGPU_WDDM_PRESENT_EXECUTION_STAGE *failureStage,
                                   DWORD *failureDetail,
                                   VIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC *executionDiagnostic)
{
    if (failureStage == NULL || failureDetail == NULL || executionDiagnostic == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *failureStage = VioGpuWddmPresentExecuteInvalidTransaction;
    *failureDetail = 0;
    InitializePresentExecutionDiagnostic(NULL, *failureStage, STATUS_CANCELLED, *failureDetail, executionDiagnostic);
    if (transaction == NULL || transaction->Signature != VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE ||
        transaction->Context == NULL || transaction->Adapter == NULL || transaction->Source == NULL ||
        transaction->Destination == NULL || transaction->RectCount == 0 || transaction->DestinationSubRects == NULL)
    {
        return STATUS_CANCELLED;
    }
    if (InterlockedCompareExchange(&transaction->CancelRequested, 0, 0) != 0)
    {
        *failureStage = VioGpuWddmPresentExecuteCancelled;
        InitializePresentExecutionDiagnostic(transaction,
                                             *failureStage,
                                             STATUS_CANCELLED,
                                             *failureDetail,
                                             executionDiagnostic);
        return STATUS_CANCELLED;
    }

    VIOGPU_WDDM_ALLOCATION *source = transaction->Source;
    VIOGPU_WDDM_ALLOCATION *destination = transaction->Destination;
    if (source == destination)
    {
        *failureStage = VioGpuWddmPresentExecuteAliasedAllocations;
        InitializePresentExecutionDiagnostic(transaction,
                                             *failureStage,
                                             STATUS_DEVICE_NOT_READY,
                                             *failureDetail,
                                             executionDiagnostic);
        return STATUS_DEVICE_NOT_READY;
    }
    BOOLEAN sourceLocked = FALSE;
    BOOLEAN destinationLocked = FALSE;
    NTSTATUS status = AcquirePresentAllocationLifecycles(source, destination, &sourceLocked, &destinationLocked);
    if (status != STATUS_SUCCESS)
    {
        *failureStage = sourceLocked ? VioGpuWddmPresentExecuteDestinationLifecycle
                                     : VioGpuWddmPresentExecuteSourceLifecycle;
    }
    VIOGPU_NATIVE_PRESENT_COPY_PROBE copyProbe = {};

    VIOGPU_NATIVE_CONTEXT_SNAPSHOT sourceSnapshot = {};
    BOOLEAN sourceSnapshotAcquired = FALSE;
    BOOLEAN nativeSource = HasLiveNativePresentIdentity(source, transaction->Context, transaction->Adapter);
    BOOLEAN gdiSource = transaction->Context->Type == VioGpuWddmContextGdi &&
                        (IsGdiSourceAllocation(source) || IsStandardPrimaryAllocation(source));
    if (NT_SUCCESS(status))
    {
        sourceSnapshotAcquired = nativeSource && AcquireAllocationNativeContextSnapshot(source, &sourceSnapshot);
        if (gdiSource)
        {
            if (!ReconcileGdiSourcePlacementAfterReset(source))
            {
                status = STATUS_DEVICE_NOT_READY;
                *failureStage = VioGpuWddmPresentExecuteGdiSourceReconcile;
            }
            else
            {
                gdiSource = HasLiveGdiPresentIdentity(source, transaction->Context, transaction->Adapter);
            }
        }
        BOOLEAN sourceValid = (nativeSource && sourceSnapshotAcquired &&
                               sourceSnapshot.ContextId == source->ContextId &&
                               sourceSnapshot.Generation == source->ContextGeneration &&
                               sourceSnapshot.ResetGeneration == source->ContextResetGeneration) ||
                              gdiSource;
        if (NT_SUCCESS(status) && !sourceValid)
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteSourceIdentity;
        }
        else if (NT_SUCCESS(status) &&
                 (source->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE || source->Adapter != transaction->Adapter))
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteSourceObject;
        }
        else if (NT_SUCCESS(status) && (destination->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
                                        destination->Adapter != transaction->Adapter))
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteDestinationObject;
        }
        else if (NT_SUCCESS(status) && !IsStandardPrimaryAllocation(destination))
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteDestinationPrimary;
        }
        else if (NT_SUCCESS(status) && (!source->PlacementValid || source->ApertureAddress == NULL))
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteSourcePlacement;
        }
        else if (NT_SUCCESS(status) && (!EnsureStandard2DAllocationBacking(destination) ||
                                        destination->Resource2DState != VioGpu2DResourceBackingAttached))
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteDestinationBacking;
        }
        else if (NT_SUCCESS(status) && !destination->PlacementValid)
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteDestinationPlacement;
        }
        else if (NT_SUCCESS(status) && !ValidatePresentGeometry(source,
                                                                destination,
                                                                &transaction->SourceRect,
                                                                &transaction->DestinationRect,
                                                                transaction->DestinationSubRects,
                                                                transaction->RectCount))
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteGeometry;
        }
        else if (NT_SUCCESS(status) && source->PlacementOffset != transaction->SourcePlacementOffset)
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteSourcePlacementOffset;
        }
        else if (NT_SUCCESS(status) && destination->PlacementOffset != transaction->DestinationPlacementOffset)
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteDestinationPlacementOffset;
        }
        else if (NT_SUCCESS(status) &&
                 destination->Resource2DResetGeneration != transaction->DestinationResetGeneration)
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteDestinationResetGeneration;
        }
    }

    if (NT_SUCCESS(status))
    {
        if (source->ApertureAddress == NULL || destination->ApertureAddress == NULL)
        {
            status = STATUS_DEVICE_NOT_READY;
            *failureStage = VioGpuWddmPresentExecuteCopyAddress;
        }
        if (NT_SUCCESS(status))
        {
            PUCHAR sourceBase = static_cast<PUCHAR>(source->ApertureAddress);
            PUCHAR destinationBase = static_cast<PUCHAR>(destination->ApertureAddress);
            for (UINT index = 0; index < transaction->RectCount; ++index)
            {
                const RECT *destinationRect = &transaction->DestinationSubRects[index];
                SIZE_T sourceLeft = static_cast<SIZE_T>(transaction->SourceRect.left) +
                                    static_cast<SIZE_T>(destinationRect->left - transaction->DestinationRect.left);
                SIZE_T sourceTop = static_cast<SIZE_T>(transaction->SourceRect.top) +
                                   static_cast<SIZE_T>(destinationRect->top - transaction->DestinationRect.top);
                UINT copyWidth = static_cast<UINT>(destinationRect->right - destinationRect->left);
                UINT copyHeight = static_cast<UINT>(destinationRect->bottom - destinationRect->top);
                SIZE_T rowBytes = static_cast<SIZE_T>(copyWidth) * 4;
                for (UINT row = 0; row < copyHeight; ++row)
                {
                    SIZE_T sourceOffset = (sourceTop + row) * source->Pitch + sourceLeft * 4;
                    SIZE_T destinationOffset = static_cast<SIZE_T>(destinationRect->top + row) * destination->Pitch +
                                               static_cast<SIZE_T>(destinationRect->left) * 4;
                    RtlCopyMemory(destinationBase + destinationOffset, sourceBase + sourceOffset, rowBytes);
                }
            }
            KeMemoryBarrier();
            KeFlushIoBuffers(destination->ApertureMdl, FALSE, TRUE);
            ProbePresentCopy(transaction, &copyProbe);
        }
    }

    if (NT_SUCCESS(status) && InterlockedCompareExchange(&transaction->CancelRequested, 0, 0) != 0)
    {
        status = STATUS_CANCELLED;
        *failureStage = VioGpuWddmPresentExecuteCancelled;
    }
    if (NT_SUCCESS(status))
    {
        // Classic virglrenderer accepted partial transfers for this SG-backed primary but left its
        // scanout texture black. Publish the complete backing while retaining dirty CPU copies above.
        VIOGPU_HOST_CONTEXT_RESULT result = transaction->Adapter->Present2DResource(destination->ResourceId,
                                                                                    0,
                                                                                    destination->Width,
                                                                                    destination->Height,
                                                                                    0,
                                                                                    0,
                                                                                    &destination->Resource2DState,
                                                                                    &destination->Resource2DResetGeneration);
        copyProbe.HostPresentResult = static_cast<DWORD>(result);
        if (result == VioGpuHostContextConfirmed)
        {
            ++copyProbe.HostPresentCount;
        }
        if (result != VioGpuHostContextConfirmed ||
            InterlockedCompareExchange(&transaction->CancelRequested, 0, 0) != 0)
        {
            status = result == VioGpuHostContextConfirmed ? STATUS_CANCELLED : STATUS_DEVICE_NOT_READY;
            *failureStage = result == VioGpuHostContextConfirmed ? VioGpuWddmPresentExecuteCancelled
                                                                 : VioGpuWddmPresentExecuteHostPresent;
            *failureDetail = static_cast<DWORD>(result);
        }
    }

    if (sourceSnapshotAcquired)
    {
        VioGpuAdapter::ReleaseNativeContextSnapshot(&sourceSnapshot);
    }
    if (NT_SUCCESS(status))
    {
        *failureStage = VioGpuWddmPresentExecuteComplete;
        transaction->Adapter->RecordNativePresentCopyProbe(&copyProbe);
    }
    if (sourceLocked && destinationLocked)
    {
        BuildPresentExecutionDiagnostic(transaction, *failureStage, status, *failureDetail, executionDiagnostic);
    }
    else
    {
        InitializePresentExecutionDiagnostic(transaction, *failureStage, status, *failureDetail, executionDiagnostic);
    }
    if (destinationLocked)
    {
        KeReleaseMutex(&destination->LifecycleMutex, FALSE);
    }
    if (sourceLocked)
    {
        KeReleaseMutex(&source->LifecycleMutex, FALSE);
    }
    return status;
}

_Use_decl_annotations_ VOID NativePresentWorker(PVOID callbackContext)
{
    VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = static_cast<VIOGPU_WDDM_PRESENT_TRANSACTION *>(callbackContext);
    if (transaction == NULL || transaction->Signature != VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE ||
        transaction->Adapter == NULL || transaction->Context == NULL || transaction->FenceId == 0 ||
        InterlockedCompareExchange(&transaction->WorkReferenceHeld, 0, 0) != 1)
    {
        if (transaction != NULL && transaction->Adapter != NULL)
        {
            if (transaction->Signature == VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE)
            {
                VIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC executionDiagnostic = {};
                DWORD detail = transaction->Context == NULL ? 1U << 0 : 0;
                detail |= transaction->FenceId == 0 ? 1U << 1 : 0;
                detail |= InterlockedCompareExchange(&transaction->WorkReferenceHeld, 0, 0) == 1 ? 0 : 1U << 2;
                InitializePresentExecutionDiagnostic(transaction,
                                                     VioGpuWddmPresentExecuteInvalidTransaction,
                                                     STATUS_INVALID_PARAMETER,
                                                     detail,
                                                     &executionDiagnostic);
                BOOLEAN diagnosticClaimed = transaction->Adapter->ClaimNativePresentExecutionDiagnostic();
                if (diagnosticClaimed)
                {
                    transaction->Adapter->RecordNativePresentExecutionDiagnostic(&executionDiagnostic);
                }
                transaction->Adapter->RequestHardwareResetAtAnyIrql();
                if (diagnosticClaimed)
                {
                    transaction->Adapter->RecordNativePresentExecutionResetProvenance();
                }
            }
            else
            {
                transaction->Adapter->RequestHardwareResetAtAnyIrql();
            }
            transaction->Adapter->CompleteNativePassiveWork(&transaction->Work);
            ReleasePresentWorkReference(transaction);
        }
        return;
    }

    VioGpuDod *adapter = transaction->Adapter;
    if (InterlockedCompareExchange(&transaction->State, VioGpuWddmPresentExecuting, VioGpuWddmPresentQueued) !=
        VioGpuWddmPresentQueued)
    {
        LONG state = InterlockedCompareExchange(&transaction->State, 0, 0);
        VIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC executionDiagnostic = {};
        InitializePresentExecutionDiagnostic(transaction,
                                             VioGpuWddmPresentExecuteStateTransition,
                                             STATUS_DEVICE_NOT_READY,
                                             static_cast<DWORD>(state),
                                             &executionDiagnostic);
        BOOLEAN diagnosticClaimed = adapter->ClaimNativePresentExecutionDiagnostic();
        if (diagnosticClaimed)
        {
            adapter->RecordNativePresentExecutionDiagnostic(&executionDiagnostic);
        }
        if (state >= VioGpuWddmPresentBuilt && state <= VioGpuWddmPresentExecuting)
        {
            RetirePresentTransaction(transaction, state, VioGpuWddmPresentCancelled);
        }
        adapter->RequestHardwareResetAtAnyIrql();
        if (diagnosticClaimed)
        {
            adapter->RecordNativePresentExecutionResetProvenance();
        }
        adapter->CompleteNativePassiveWork(&transaction->Work);
        ReleasePresentWorkReference(transaction);
        return;
    }

    UINT fenceId = transaction->FenceId;
    UINT nodeOrdinal = transaction->Context->NodeOrdinal;
    UINT engineOrdinal = 0;
    BOOLEAN operationAcquired = adapter->AcquireNativeSubmissionOperation();
    VIOGPU_WDDM_PRESENT_EXECUTION_STAGE failureStage = VioGpuWddmPresentExecuteSubmissionOperation;
    DWORD failureDetail = 0;
    VIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC executionDiagnostic = {};
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (operationAcquired)
    {
        status = ExecutePresentTransaction(transaction, &failureStage, &failureDetail, &executionDiagnostic);
    }
    else
    {
        InitializePresentExecutionDiagnostic(transaction, failureStage, status, failureDetail, &executionDiagnostic);
    }
    VIOGPU_WDDM_PRESENT_STATE finalState = NT_SUCCESS(status) ? VioGpuWddmPresentFinished : VioGpuWddmPresentCancelled;
    BOOLEAN retired = RetirePresentTransaction(transaction, VioGpuWddmPresentExecuting, finalState);
    if (!retired)
    {
        executionDiagnostic.Stage = VioGpuWddmPresentExecuteTransactionRetire;
        executionDiagnostic.Status = static_cast<DWORD>(STATUS_DEVICE_NOT_READY);
        executionDiagnostic.Detail = static_cast<DWORD>(finalState);
        executionDiagnostic.TransactionState = static_cast<DWORD>(InterlockedCompareExchange(&transaction->State,
                                                                                             0,
                                                                                             0));
        BOOLEAN diagnosticClaimed = adapter->ClaimNativePresentExecutionDiagnostic();
        if (diagnosticClaimed)
        {
            adapter->RecordNativePresentExecutionDiagnostic(&executionDiagnostic);
        }
        adapter->RequestHardwareResetAtAnyIrql();
        if (diagnosticClaimed)
        {
            adapter->RecordNativePresentExecutionResetProvenance();
        }
    }
    else if (NT_SUCCESS(status))
    {
        adapter->NotifyNativeSoftwareCompletion(fenceId, nodeOrdinal, engineOrdinal);
    }
    else
    {
        BOOLEAN diagnosticClaimed = adapter->ClaimNativePresentExecutionDiagnostic();
        if (diagnosticClaimed)
        {
            adapter->RecordNativePresentExecutionDiagnostic(&executionDiagnostic);
        }
        adapter->NotifyNativeSubmissionFault(fenceId,
                                             STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                             nodeOrdinal,
                                             engineOrdinal,
                                             TRUE);
        if (diagnosticClaimed)
        {
            adapter->RecordNativePresentExecutionResetProvenance();
        }
    }
    adapter->CompleteNativePassiveWork(&transaction->Work);
    if (operationAcquired)
    {
        adapter->ReleaseNativeSubmissionOperation();
    }
    ReleasePresentWorkReference(transaction);
}

_Use_decl_annotations_ VOID NativePresentDispatchCancelled(PVOID callbackContext)
{
    VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = static_cast<VIOGPU_WDDM_PRESENT_TRANSACTION *>(callbackContext);
    if (transaction == NULL || transaction->Signature != VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE)
    {
        return;
    }
    InterlockedExchange(&transaction->CancelRequested, 1);
    RetirePresentTransaction(transaction, VioGpuWddmPresentQueued, VioGpuWddmPresentCancelled);
    ReleasePresentWorkReference(transaction);
}

NTSTATUS QuerySegment(VioGpuDod *adapter, const DXGKARG_QUERYADAPTERINFO *queryAdapterInfo)
{
    if (adapter == NULL || queryAdapterInfo->pOutputData == NULL ||
        queryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    GPU_CAPSET_DRM capset = {};
    ULONGLONG resetGeneration = 0;
    if (!adapter->QueryNativeContextReadiness(&capset, NULL, NULL, &resetGeneration) || resetGeneration == 0)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    DXGK_QUERYSEGMENTOUT *segmentInfo = static_cast<DXGK_QUERYSEGMENTOUT *>(queryAdapterInfo->pOutputData);
    segmentInfo->NbSegment = 1;
    segmentInfo->PagingBufferSegmentId = 0;
    segmentInfo->PagingBufferSize = PAGE_SIZE;
    segmentInfo->PagingBufferPrivateDataSize = sizeof(VIOGPU_WDDM_PAGING_PRIVATE);

    if (segmentInfo->pSegmentDescriptor != NULL)
    {
        DXGK_SEGMENTDESCRIPTOR *descriptor = segmentInfo->pSegmentDescriptor;
        RtlZeroMemory(descriptor, sizeof(*descriptor));
        descriptor->BaseAddress.QuadPart = 0;
        descriptor->CpuTranslatedAddress.QuadPart = 0;
        descriptor->Size = VIOGPU_WDDM_APERTURE_SIZE;
        descriptor->CommitLimit = VIOGPU_WDDM_APERTURE_SIZE;
        /* VidMm supplies ordinary guest RAM pages through MapApertureSegment.
         * The mapped pages are CPU accessible and cache coherent, which is
         * required for the shared primary's supported write segment. */
        descriptor->Flags.CpuVisible = TRUE;
        descriptor->Flags.Aperture = TRUE;
        descriptor->Flags.CacheCoherent = TRUE;
    }

    return STATUS_SUCCESS;
}

/* The Direct3D runtime asks for the segment list through the versioned
 * QUERYSEGMENT2/3 types on WDDM 1.2+.  Answering only the original
 * DXGKQAITYPE_QUERYSEGMENT left those returning STATUS_NOT_SUPPORTED, so the
 * runtime could not describe video memory and reported no feature levels at
 * all.  These describe the same single aperture segment as QuerySegment. */
template <typename SegmentOut, typename SegmentDescriptor>
static NTSTATUS QuerySegmentVersioned(VioGpuDod *adapter, const DXGKARG_QUERYADAPTERINFO *queryAdapterInfo)
{
    if (adapter == NULL || queryAdapterInfo->pOutputData == NULL ||
        queryAdapterInfo->OutputDataSize < sizeof(SegmentOut))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    GPU_CAPSET_DRM capset = {};
    ULONGLONG resetGeneration = 0;
    if (!adapter->QueryNativeContextReadiness(&capset, NULL, NULL, &resetGeneration) || resetGeneration == 0)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    SegmentOut *segmentInfo = static_cast<SegmentOut *>(queryAdapterInfo->pOutputData);
    segmentInfo->NbSegment = 1;
    segmentInfo->PagingBufferSegmentId = 0;
    segmentInfo->PagingBufferSize = PAGE_SIZE;
    segmentInfo->PagingBufferPrivateDataSize = sizeof(VIOGPU_WDDM_PAGING_PRIVATE);

    /* Dxgkrnl calls twice: first with a null descriptor pointer to learn the
     * count, then again with storage for that many descriptors. */
    if (segmentInfo->pSegmentDescriptor != NULL)
    {
        SegmentDescriptor *descriptor = segmentInfo->pSegmentDescriptor;
        RtlZeroMemory(descriptor, sizeof(*descriptor));
        descriptor->BaseAddress.QuadPart = 0;
        descriptor->CpuTranslatedAddress.QuadPart = 0;
        descriptor->Size = VIOGPU_WDDM_APERTURE_SIZE;
        descriptor->CommitLimit = VIOGPU_WDDM_APERTURE_SIZE;
        descriptor->Flags.CpuVisible = TRUE;
        descriptor->Flags.Aperture = TRUE;
        descriptor->Flags.CacheCoherent = TRUE;
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

    UINT hasRayTracing = 0;
    switch (capset.msm.has_raytracing)
    {
        case VIRTGPU_CAP_BOOL_TRUE:
            hasRayTracing = 1;
            break;
        case VIRTGPU_CAP_BOOL_UNSUPPORTED_BY_HOST:
        case VIRTGPU_CAP_BOOL_FALSE:
            break;
        default:
            return STATUS_GRAPHICS_DRIVER_MISMATCH;
    }

    VIOGPU_WDDM_ADAPTER_INFO *adapterInfo = static_cast<VIOGPU_WDDM_ADAPTER_INFO *>(queryAdapterInfo->pOutputData);
    RtlZeroMemory(adapterInfo, sizeof(*adapterInfo));
    InitializeAbiHeader(&adapterInfo->Header, sizeof(*adapterInfo));
    adapterInfo->Capabilities = VIOGPU_WDDM_CAPABILITIES_NONE;
    adapterInfo->ResetGeneration = resetGeneration;
    adapterInfo->MsmMajorVersion = capset.version_major;
    adapterInfo->MsmMinorVersion = capset.version_minor;
    adapterInfo->MsmPatchVersion = capset.version_patchlevel;
    adapterInfo->GpuId = capset.msm.gpu_id;
    adapterInfo->ChipId = capset.msm.chip_id;
    adapterInfo->GmemSize = capset.msm.gmem_size;
    // Each WDDM context owns one host submitqueue, created at priority zero.
    adapterInfo->PriorityCount = 1;
    adapterInfo->GmemBase = capset.msm.gmem_base;
    adapterInfo->HighestBankBit = capset.msm.highest_bank_bit;
    adapterInfo->HasCachedCoherentMemory = capset.msm.has_cached_coherent;
    adapterInfo->UbwcSwizzle = capset.msm.ubwc_swizzle;
    adapterInfo->MacrotileMode = capset.msm.macrotile_mode;
    adapterInfo->UcheTrapBase = capset.msm.uche_trap_base;
    adapterInfo->HasRayTracing = hasRayTracing;
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
        request.VaStart != 0 || request.VaSize != 0 || request.ResetGeneration != 0 || request.ContextId != 0 ||
        request.SubmitQueueId != 0)
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
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE || context->Type != VioGpuWddmContextNative ||
        context->Device != device || device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE || device->Adapter != adapter)
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
            (snapshot.VaSize & (PAGE_SIZE - 1)) != 0 || vaEnd < snapshot.VaStart || snapshot.SubmitQueueId == 0)
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
        response.ContextId = snapshot.ContextId;
        response.SubmitQueueId = snapshot.SubmitQueueId;
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

NTSTATUS QueryCompletedFenceInfo(VioGpuDod *adapter, const DXGKARG_ESCAPE *escape)
{
    PAGED_CODE();

    if (adapter == NULL || escape == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL || escape->hDevice == NULL ||
        escape->hContext == NULL || escape->Flags.Value != 0 || escape->pPrivateDriverData == NULL ||
        escape->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_FENCE_INFO))
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_FENCE_INFO request = {};
    __try
    {
        RtlCopyMemory(&request, escape->pPrivateDriverData, sizeof(request));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return STATUS_INVALID_USER_BUFFER;
    }

    if (!IsCurrentAbiHeader(&request.Header, sizeof(request)) ||
        request.Opcode != VIOGPU_WDDM_ESCAPE_GET_COMPLETED_FENCE)
    {
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    }
    if (request.Flags != VIOGPU_WDDM_ESCAPE_FLAGS_NONE || request.ExpectedResetGeneration == 0 ||
        request.CompletedFence != 0 || request.ResetGeneration != 0 || request.ContextId != 0 || request.Reserved != 0)
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
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE || context->Type != VioGpuWddmContextNative ||
        context->Device != device || device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE || device->Adapter != adapter)
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
        if (snapshot.ResetGeneration != request.ExpectedResetGeneration || snapshot.ContextId == 0)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
    }

    if (NT_SUCCESS(status))
    {
        VIOGPU_WDDM_FENCE_INFO response = {};
        InitializeAbiHeader(&response.Header, sizeof(response));
        response.Opcode = VIOGPU_WDDM_ESCAPE_GET_COMPLETED_FENCE;
        response.Flags = VIOGPU_WDDM_ESCAPE_FLAGS_NONE;
        response.ExpectedResetGeneration = request.ExpectedResetGeneration;
        response.CompletedFence = QueryContextCompletedUmdFence(context);
        response.ResetGeneration = snapshot.ResetGeneration;
        response.ContextId = snapshot.ContextId;
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
                               const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *nativeContext)
{
    const VIOGPU_WDDM_UINT32 validReferenceFlags = VIOGPU_WDDM_REFERENCE_READ | VIOGPU_WDDM_REFERENCE_WRITE;
    ULONGLONG resetGeneration = nativeContext == NULL ? 0 : nativeContext->ResetGeneration;
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
            (reference->PatchOffset & (sizeof(ULONG) - 1)) != 0 ||
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
            deviceAllocation->Allocation->Adapter != device->Adapter ||
            !IsNativeAllocation(deviceAllocation->Allocation) ||
            reference->AllocationOffset > deviceAllocation->Allocation->PrivateData.Size ||
            reference->Length > deviceAllocation->Allocation->PrivateData.Size - reference->AllocationOffset ||
            allocationEntry->Reserved != 0 ||
            ((reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0) != (allocationEntry->WriteOperation != 0) ||
            (deviceAllocation->ReadOnly && (reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0) ||
            ((deviceAllocation->Allocation->Flags & VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY) != 0 &&
             (reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0))
        {
            return STATUS_INVALID_HANDLE;
        }

        VIOGPU_WDDM_ALLOCATION *allocation = deviceAllocation->Allocation;
        NTSTATUS allocationStatus = AcquireAllocationLifecycle(allocation);
        if (allocationStatus != STATUS_SUCCESS)
        {
            return allocationStatus;
        }
        /* A zero SegmentId means residency is still deferred. Validate stable
         * identity here; prepatch and Patch separately validate Host binding
         * and placement before writing KMD-owned addresses. */
        BOOLEAN current = nativeContext != NULL && allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE &&
                          allocation->Adapter == device->Adapter && !allocation->Destroying &&
                          allocation->NativeContext == nativeContext->Registration &&
                          allocation->ContextGeneration == nativeContext->Generation &&
                          allocation->ContextResetGeneration == nativeContext->ResetGeneration &&
                          allocation->ContextId == nativeContext->ContextId &&
                          allocation->PrivateData.ExpectedResetGeneration == nativeContext->ResetGeneration;
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        if (!current)
        {
            return STATUS_DEVICE_NOT_READY;
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

NTSTATUS ValidateNativeSubmitPacket(const VIOGPU_WDDM_RENDER_COMMAND *header,
                                    VIOGPU_WDDM_DEVICE *device,
                                    const DXGK_ALLOCATIONLIST *allocationList,
                                    UINT allocationListSize,
                                    const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *nativeContext)
{
    if (header == NULL || device == NULL || allocationList == NULL || nativeContext == NULL ||
        nativeContext->SubmitQueueId == 0 || header->CommandStreamSize < sizeof(MSM_CCMD_GEM_SUBMIT_REQ) ||
        (header->CommandStreamSize & (sizeof(UINT) - 1)) != 0)
    {
        return STATUS_ILLEGAL_INSTRUCTION;
    }

    const BYTE *stream = reinterpret_cast<const BYTE *>(header) + header->CommandStreamOffset;
    const MSM_CCMD_GEM_SUBMIT_REQ *request = reinterpret_cast<const MSM_CCMD_GEM_SUBMIT_REQ *>(stream);
    const UINT validSubmitFlags = MSM_PIPE_3D0 | VIOGPU_WDDM_MSM_SUBMIT_NO_IMPLICIT;
    if (request->hdr.cmd != MSM_CCMD_GEM_SUBMIT || request->hdr.len != header->CommandStreamSize ||
        request->hdr.seqno == 0 || request->hdr.rsp_off != 0 || request->flags == 0 ||
        (request->flags & ~validSubmitFlags) != 0 || (request->flags & MSM_PIPE_3D0) != MSM_PIPE_3D0 ||
        request->queue_id != nativeContext->SubmitQueueId || request->fence == 0 ||
        request->nr_bos != header->AllocationReferenceCount || request->nr_bos == 0 ||
        request->nr_bos > VioGpuWddmSubmissionAllocationLimit || request->nr_cmds == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ULONGLONG bosBytes = (ULONGLONG)request->nr_bos * sizeof(VIOGPU_WDDM_MSM_SUBMIT_BO);
    ULONGLONG commandBytes = (ULONGLONG)request->nr_cmds * sizeof(VIOGPU_WDDM_MSM_SUBMIT_CMD);
    ULONGLONG expectedSize = sizeof(*request) + bosBytes + commandBytes;
    if (expectedSize != header->CommandStreamSize)
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    const VIOGPU_WDDM_ALLOCATION_REFERENCE *references = reinterpret_cast<const VIOGPU_WDDM_ALLOCATION_REFERENCE *>(
                                                                                                        reinterpret_cast<const BYTE *>(header) +
                                                                                                        header->AllocationReferencesOffset);
    const VIOGPU_WDDM_MSM_SUBMIT_BO *bos = reinterpret_cast<const VIOGPU_WDDM_MSM_SUBMIT_BO *>(request->payload);
    const VIOGPU_WDDM_MSM_SUBMIT_CMD *commands = reinterpret_cast<const VIOGPU_WDDM_MSM_SUBMIT_CMD *>(request->payload +
                                                                                                      bosBytes);
    const UINT validBoFlags = VIOGPU_WDDM_MSM_SUBMIT_BO_READ | VIOGPU_WDDM_MSM_SUBMIT_BO_WRITE |
                              VIOGPU_WDDM_MSM_SUBMIT_BO_DUMP | VIOGPU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT;

    for (UINT index = 0; index < request->nr_bos; ++index)
    {
        const VIOGPU_WDDM_ALLOCATION_REFERENCE *reference = &references[index];
        if (reference->AllocationIndex >= allocationListSize)
        {
            return STATUS_INVALID_HANDLE;
        }
        VIOGPU_WDDM_OPEN_ALLOCATION *openAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationList[reference->AllocationIndex].hDeviceSpecificAllocation);
        VIOGPU_WDDM_ALLOCATION *allocation = openAllocation == NULL ? NULL : openAllocation->Allocation;
        UINT expectedAccess = 0;
        if ((reference->Flags & VIOGPU_WDDM_REFERENCE_READ) != 0)
        {
            expectedAccess |= VIOGPU_WDDM_MSM_SUBMIT_BO_READ;
        }
        if ((reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0)
        {
            expectedAccess |= VIOGPU_WDDM_MSM_SUBMIT_BO_WRITE;
        }
        UINT expectedPatchOffset = static_cast<UINT>(sizeof(*request) +
                                                     (ULONGLONG)index * sizeof(VIOGPU_WDDM_MSM_SUBMIT_BO) +
                                                     FIELD_OFFSET(VIOGPU_WDDM_MSM_SUBMIT_BO, Presumed));
        if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
            allocation->Adapter != device->Adapter || bos[index].Handle != 0 || bos[index].Presumed != 0 ||
            bos[index].Flags == 0 || (bos[index].Flags & ~validBoFlags) != 0 ||
            (bos[index].Flags & (VIOGPU_WDDM_MSM_SUBMIT_BO_READ | VIOGPU_WDDM_MSM_SUBMIT_BO_WRITE)) != expectedAccess ||
            reference->PatchOffset != expectedPatchOffset)
        {
            return STATUS_INVALID_PARAMETER;
        }
        for (UINT previous = 0; previous < index; ++previous)
        {
            const VIOGPU_WDDM_ALLOCATION_REFERENCE *previousReference = &references[previous];
            VIOGPU_WDDM_OPEN_ALLOCATION *previousOpen = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationList[previousReference->AllocationIndex].hDeviceSpecificAllocation);
            if (previousOpen != NULL && previousOpen->Allocation == allocation)
            {
                return STATUS_INVALID_PARAMETER;
            }
        }
    }

    for (UINT index = 0; index < request->nr_cmds; ++index)
    {
        const VIOGPU_WDDM_MSM_SUBMIT_CMD *command = &commands[index];
        if ((command->Type != VIOGPU_WDDM_MSM_SUBMIT_CMD_BUF &&
             command->Type != VIOGPU_WDDM_MSM_SUBMIT_CMD_IB_TARGET_BUF) ||
            command->SubmitIndex >= request->nr_bos || command->Size == 0 ||
            (command->Size & (sizeof(UINT) - 1)) != 0 || command->Padding != 0 || command->RelocationCount != 0 ||
            command->Iova != 0)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const VIOGPU_WDDM_ALLOCATION_REFERENCE *reference = &references[command->SubmitIndex];
        VIOGPU_WDDM_OPEN_ALLOCATION *openAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationList[reference->AllocationIndex].hDeviceSpecificAllocation);
        VIOGPU_WDDM_ALLOCATION *allocation = openAllocation == NULL ? NULL : openAllocation->Allocation;
        if (allocation == NULL || command->SubmitOffset > allocation->PrivateData.Size ||
            command->Size > allocation->PrivateData.Size - command->SubmitOffset)
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS ApplyRenderPrepatches(_Inout_ VIOGPU_WDDM_RENDER_COMMAND *header,
                               _In_ VIOGPU_WDDM_DEVICE *device,
                               _In_reads_(allocationListSize) const DXGK_ALLOCATIONLIST *allocationList,
                               _In_ UINT allocationListSize,
                               _In_ const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *nativeContext,
                               _Out_ BOOLEAN *fullyPrepatched)
{
    if (header == NULL || device == NULL || device->Adapter == NULL || allocationList == NULL ||
        nativeContext == NULL || fullyPrepatched == NULL || header->AllocationReferenceCount == 0 ||
        header->AllocationReferenceCount > VioGpuWddmSubmissionAllocationLimit ||
        header->CommandStreamSize < sizeof(MSM_CCMD_GEM_SUBMIT_REQ))
    {
        return STATUS_INVALID_PARAMETER;
    }

    *fullyPrepatched = TRUE;
    VIOGPU_WDDM_ALLOCATION_REFERENCE *references = reinterpret_cast<VIOGPU_WDDM_ALLOCATION_REFERENCE *>(reinterpret_cast<BYTE *>(header) +
                                                                                                        header->AllocationReferencesOffset);
    BYTE *commandStream = reinterpret_cast<BYTE *>(header) + header->CommandStreamOffset;
    MSM_CCMD_GEM_SUBMIT_REQ *request = reinterpret_cast<MSM_CCMD_GEM_SUBMIT_REQ *>(commandStream);
    VIOGPU_WDDM_MSM_SUBMIT_BO *bos = reinterpret_cast<VIOGPU_WDDM_MSM_SUBMIT_BO *>(request->payload);
    if (request->nr_bos != header->AllocationReferenceCount)
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (UINT index = 0; index < header->AllocationReferenceCount; ++index)
    {
        VIOGPU_WDDM_ALLOCATION_REFERENCE *reference = &references[index];
        if (reference->AllocationIndex >= allocationListSize || (reference->PatchOffset & (sizeof(ULONG) - 1)) != 0 ||
            reference->PatchOffset > header->CommandStreamSize - sizeof(ULONGLONG))
        {
            return STATUS_INVALID_PARAMETER;
        }

        const DXGK_ALLOCATIONLIST *allocationEntry = &allocationList[reference->AllocationIndex];
        if (allocationEntry->SegmentId == 0)
        {
            *fullyPrepatched = FALSE;
            continue;
        }

        VIOGPU_WDDM_OPEN_ALLOCATION *openAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationEntry->hDeviceSpecificAllocation);
        VIOGPU_WDDM_ALLOCATION *allocation = openAllocation == NULL ? NULL : openAllocation->Allocation;
        NTSTATUS status = AcquireAllocationLifecycle(allocation);
        if (status != STATUS_SUCCESS)
        {
            return status;
        }

        BOOLEAN valid = openAllocation != NULL && allocation != NULL &&
                        openAllocation->Signature == VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE &&
                        openAllocation->Device == device && allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE &&
                        allocation->Adapter == device->Adapter && !allocation->Destroying &&
                        allocation->NativeContext == nativeContext->Registration &&
                        allocation->HostState == VioGpuWddmAllocationHostLive && allocation->PlacementValid &&
                        allocation->ApertureAddress != NULL &&
                        allocation->ResourceId >= VIOGPU_NATIVE_RESOURCE_ID_START &&
                        allocation->ResourceId != MAXUINT && allocation->BlobId == allocation->ResourceId &&
                        allocation->ContextId == nativeContext->ContextId &&
                        allocation->ContextGeneration == nativeContext->Generation &&
                        allocation->ContextResetGeneration == nativeContext->ResetGeneration &&
                        allocation->BoundContextId == nativeContext->ContextId &&
                        allocation->BoundGeneration == nativeContext->Generation &&
                        allocation->BoundResetGeneration == nativeContext->ResetGeneration &&
                        allocationEntry->SegmentId == VIOGPU_WDDM_SEGMENT_ID &&
                        allocationEntry->PhysicalAddress.QuadPart >= 0 &&
                        static_cast<ULONGLONG>(allocationEntry->PhysicalAddress.QuadPart) == allocation->PlacementOffset &&
                        allocationEntry->Reserved == 0 &&
                        ((reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0) == (allocationEntry->WriteOperation !=
                                                                                    0) &&
                        (!openAllocation->ReadOnly || (reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) == 0) &&
                        reference->AllocationOffset <= allocation->PrivateData.Size &&
                        reference->Length <= allocation->PrivateData.Size - reference->AllocationOffset &&
                        allocation->PrivateData.RequestedIova != 0 &&
                        allocation->PrivateData.RequestedIova <= MAXULONGLONG - reference->AllocationOffset;
        UINT resourceId = valid ? allocation->ResourceId : 0;
        ULONGLONG iova = valid ? allocation->PrivateData.RequestedIova + reference->AllocationOffset : 0;
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        if (!valid)
        {
            return STATUS_DEVICE_NOT_READY;
        }

        RtlCopyMemory(&bos[index].Handle, &resourceId, sizeof(resourceId));
        RtlCopyMemory(commandStream + reference->PatchOffset, &iova, sizeof(iova));
    }
    return STATUS_SUCCESS;
}
} // namespace

VOID VioGpuWddmDrainPresentTransactions(_In_ VioGpuDod *adapter)
{
    if (adapter == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return;
    }

    adapter->CloseWddmPresentTransactions();

    for (;;)
    {
        PLIST_ENTRY link = adapter->PopWddmPresentTransactionForReset();
        if (link == NULL)
        {
            return;
        }

        VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = CONTAINING_RECORD(link,
                                                                         VIOGPU_WDDM_PRESENT_TRANSACTION,
                                                                         AdapterLink);
        if (transaction->Signature == VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE)
        {
            InterlockedExchange(&transaction->CancelRequested, 1);
            LONG state = InterlockedCompareExchange(&transaction->State, 0, 0);
            if (state == VioGpuWddmPresentBuilt || state == VioGpuWddmPresentPatched)
            {
                RetirePresentTransaction(transaction, state, VioGpuWddmPresentCancelled);
            }
            else if (state == VioGpuWddmPresentQueued)
            {
                VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP ownership = adapter->CancelNativePassiveWork(&transaction->Work);
                if (ownership == VioGpuNativePassiveWorkRemoved)
                {
                    UINT fenceId = transaction->FenceId;
                    UINT nodeOrdinal = transaction->Context->NodeOrdinal;
                    BOOLEAN retired = RetirePresentTransaction(transaction,
                                                               VioGpuWddmPresentQueued,
                                                               VioGpuWddmPresentCancelled);
                    if (retired && !adapter->IsHardwareResetRequested())
                    {
                        adapter->NotifyNativeSubmissionFault(fenceId,
                                                             STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                                             nodeOrdinal,
                                                             0,
                                                             TRUE);
                    }
                    ReleasePresentWorkReference(transaction);
                }
            }
        }

        /* Pop transfers the adapter-registry reference to this reset drain. */
        DereferencePresentTransaction(transaction);
    }
}

VOID NativePagingBatchCancelled(_In_ PVOID callbackContext);
VOID NativePagingBatchWorker(_In_ PVOID callbackContext);

static_assert(static_cast<UINT>(DXGKQAITYPE_64BITONLYCAPS) == 47U, "unexpected DXGKQAITYPE_64BITONLYCAPS value");
static_assert(sizeof(DXGK_64_BIT_ONLY_CAPS) == sizeof(UINT), "unexpected DXGK_64_BIT_ONLY_CAPS size");

static NTSTATUS Query64BitOnlyCaps(_In_ CONST DXGKARG_QUERYADAPTERINFO *queryAdapterInfo)
{
    if (queryAdapterInfo->OutputDataSize < sizeof(DXGK_64_BIT_ONLY_CAPS))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (queryAdapterInfo->pOutputData == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    DXGK_64_BIT_ONLY_CAPS *caps = static_cast<DXGK_64_BIT_ONLY_CAPS *>(queryAdapterInfo->pOutputData);
    RtlZeroMemory(caps, sizeof(*caps));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmQueryAdapterInfo(CONST HANDLE hAdapter,
                                                                    CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    if (hAdapter == NULL || pQueryAdapterInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    if (pQueryAdapterInfo->Type == DXGKQAITYPE_UMDRIVERPRIVATE)
    {
        status = QueryUmdPrivateInfo(adapter, pQueryAdapterInfo);
    }
    else if (pQueryAdapterInfo->Type == DXGKQAITYPE_QUERYSEGMENT)
    {
        status = QuerySegment(adapter, pQueryAdapterInfo);
    }
    else if (pQueryAdapterInfo->Type == DXGKQAITYPE_QUERYSEGMENT3)
    {
        status = QuerySegmentVersioned<DXGK_QUERYSEGMENTOUT3, DXGK_SEGMENTDESCRIPTOR3>(adapter, pQueryAdapterInfo);
    }
    else if (pQueryAdapterInfo->Type == DXGKQAITYPE_64BITONLYCAPS)
    {
        status = Query64BitOnlyCaps(pQueryAdapterInfo);
    }
    else if (static_cast<UINT>(pQueryAdapterInfo->Type) == 24U || static_cast<UINT>(pQueryAdapterInfo->Type) == 25U)
    {
        /* Legacy dxgkrnl asks for node/adapter performance data during
         * ADAPTER_RENDER activation.  The Win7 Native Context target does
         * not publish performance counters, but rejecting these optional
         * probes makes AddAdapter fail before the UMD can open. */
        if (pQueryAdapterInfo->OutputDataSize != 0U && pQueryAdapterInfo->pOutputData == NULL)
        {
            status = STATUS_INVALID_PARAMETER;
        }
        else
        {
            if (pQueryAdapterInfo->OutputDataSize != 0U)
            {
                RtlZeroMemory(pQueryAdapterInfo->pOutputData, pQueryAdapterInfo->OutputDataSize);
            }
            status = STATUS_SUCCESS;
        }
    }
    else
    {
        status = VioGpuDodQueryAdapterInfo(hAdapter, pQueryAdapterInfo);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               NT_SUCCESS(status) ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "viogpu WDDM QueryAdapterInfo: type=%u input=%u output=%u status=0x%08X\n",
               pQueryAdapterInfo->Type,
               pQueryAdapterInfo->InputDataSize,
               pQueryAdapterInfo->OutputDataSize,
               status);
    if (!NT_SUCCESS(status))
    {
        adapter->RecordNativeQueryAdapterInfoDiagnostic(pQueryAdapterInfo->Type,
                                                        status,
                                                        pQueryAdapterInfo->InputDataSize,
                                                        pQueryAdapterInfo->OutputDataSize);
    }
    return status;
}

#pragma code_seg(push)
#pragma code_seg("PAGE")

_Use_decl_annotations_ NTSTATUS VioGpuWddmNotifyAcpiEvent(PVOID miniportDeviceContext,
                                                          DXGK_EVENT_TYPE eventType,
                                                          ULONG event,
                                                          PVOID argument,
                                                          PULONG acpiFlags)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(eventType);
    UNREFERENCED_PARAMETER(event);
    UNREFERENCED_PARAMETER(argument);

    if (miniportDeviceContext == NULL || acpiFlags == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* VirtIO-GPU has no ACPI hot-key or firmware notification channel.  The
     * WDDM callback is nevertheless part of the legacy registration table;
     * acknowledge a well-formed notification with no follow-up flags so
     * Dxgkrnl does not treat an optional callback as a device failure. */
    *acpiFlags = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ VOID VioGpuWddmControlEtwLogging(BOOLEAN enable, ULONG flags, UCHAR level)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(enable);
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(level);
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmSetPalette(CONST HANDLE hAdapter,
                                                              CONST DXGKARG_SETPALETTE *setPalette)
{
    PAGED_CODE();
    if (hAdapter == NULL || setPalette == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (setPalette->VidPnSourceId != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* The miniport advertises only true-color scanout formats.  There is no
     * indexed palette to program, but dxgkrnl may still issue this optional
     * callback while it walks the VidPN.  Treat it as a successful no-op so
     * the harmless query cannot abort modeset/adapter activation. */
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmGetScanLine(CONST HANDLE hAdapter, DXGKARG_GETSCANLINE *getScanLine)
{
    PAGED_CODE();
    if (hAdapter == NULL || getScanLine == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    getScanLine->InVerticalBlank = FALSE;
    getScanLine->ScanLine = 0;
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    return adapter->GetScanLine(getScanLine);
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmControlInterrupt(CONST HANDLE hAdapter,
                                                                    CONST DXGK_INTERRUPT_TYPE interruptType,
                                                                    BOOLEAN enableInterrupt)
{
    PAGED_CODE();
    if (hAdapter == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    return adapter->ControlInterrupt(interruptType, enableInterrupt);
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmRenderKm(CONST HANDLE hContext, DXGKARG_RENDER *render)
{
    PAGED_CODE();
    if (hContext == NULL || render == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
#if defined(VIOGPU_WDDM_TEST_IMPLEMENTATIONS)
    /* Compile-time experiment only.  This path is intentionally excluded
     * from the product build because a real CDD RenderKm packet needs a
     * separate command contract.  The Win8 DXGKARG_RENDER declaration has no
     * mode flag, so the test bridge relies on the opt-in build switch and
     * exercises the existing Native Render validator/queue path unchanged. */
    return VioGpuWddmRender(hContext, render);
#else
    /* PresentationCaps.SupportKernelModeCommandBuffer remains zero. CDD
     * commands are not Native Context MSM submits and must never enter the
     * Vulkan command parser. */
    return STATUS_ILLEGAL_INSTRUCTION;
#endif
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmEscape(CONST HANDLE hAdapter, CONST DXGKARG_ESCAPE *escape)
{
    PAGED_CODE();

    if (hAdapter == NULL || escape == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (escape->PrivateDriverDataSize == sizeof(VIOGPU_WDDM_FENCE_INFO))
    {
        return QueryCompletedFenceInfo(reinterpret_cast<VioGpuDod *>(hAdapter), escape);
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

        case D3DKMDT_STANDARDALLOCATION_GDISURFACE:
            {
                /* Dxgkrnl requests a GDI standard allocation while it builds a
                 * D3D device.  Refusing it returned STATUS_NOT_SUPPORTED from
                 * DxgkDdiGetStandardAllocationDriverData, which dxgkrnl reports
                 * as "Driver returned an invalid NTSTATUS code" and turns into a
                 * failed device creation, so Direct3D could never open a device
                 * on this adapter.  GDI surfaces live in the same host-shared
                 * aperture as the shadow and staging surfaces. */
                D3DKMDT_GDISURFACEDATA *surface = data->pCreateGdiSurfaceData;
                if (surface == NULL)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                D3DDDIFORMAT format = surface->Format;
                if (format == D3DDDIFMT_UNKNOWN)
                {
                    format = D3DDDIFMT_X8R8G8B8;
                }

                status = CalculateSurfaceLayout(surface->Width,
                                                surface->Height,
                                                format,
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
                privateData->Format = ToPrivateFormat(format);
                return STATUS_SUCCESS;
            }

        default:
            {
                /* Publish the unmodelled type rather than silently refusing it,
                 * and report it with a status this DDI is allowed to return. */
                VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
                if (adapter != NULL)
                {
                    adapter->RecordNativeStandardAllocationDiagnostic(
                        static_cast<ULONG>(data->StandardAllocationType));
                }
                return STATUS_INVALID_PARAMETER;
            }
    }
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmCreateAllocation(CONST HANDLE hAdapter,
                                                                    DXGKARG_CREATEALLOCATION *createAllocation)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || createAllocation == NULL || createAllocation->NumAllocations == 0 ||
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
            resource->Adapter = adapter;
            InterlockedExchange(&resource->AllocationCount, 0);
            createdResource = TRUE;
        }
        else if (resource->Signature != VIOGPU_WDDM_RESOURCE_SIGNATURE || resource->Adapter != adapter ||
                 ReadResourceAllocationCount(resource) < 0)
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
        __try
        {
            RtlCopyMemory(&privateData, allocationInfo->pPrivateDriverData, sizeof(privateData));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }
        SIZE_T alignedSize = 0;
        status = ValidateAllocationPrivate(&privateData, &alignedSize);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        UINT nativeResourceId = 0;
        UINT standardResourceId = 0;
        VIOGPU_NATIVE_CONTEXT_REGISTRATION *nativeContext = NULL;
        LONG contextGeneration = 0;
        ULONGLONG contextResetGeneration = 0;
        UINT contextId = 0;
        if ((privateData.Flags & VIOGPU_WDDM_ALLOCATION_NATIVE) != 0)
        {
            VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
            if (!adapter->AcquireNativeContextSnapshotForAllocation(privateData.RequestedIova,
                                                                    alignedSize,
                                                                    privateData.ExpectedResetGeneration,
                                                                    privateData.ContextId,
                                                                    &snapshot))
            {
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
            if (!VioGpuAdapter::ReferenceNativeContextAllocation(&snapshot, &nativeContext))
            {
                VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
            contextGeneration = snapshot.Generation;
            contextResetGeneration = snapshot.ResetGeneration;
            contextId = snapshot.ContextId;
            VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
            nativeResourceId = adapter->AllocateNativeResourceId(privateData.ExpectedResetGeneration);
            if (nativeResourceId == 0)
            {
                BOOLEAN dereferenced = VioGpuAdapter::DereferenceNativeContextAllocation(nativeContext);
                NT_ASSERT(dereferenced);
                UNREFERENCED_PARAMETER(dereferenced);
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
        }
        else
        {
            standardResourceId = adapter->Allocate2DResourceId();
            if (standardResourceId == 0)
            {
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
        }

        VIOGPU_WDDM_ALLOCATION *allocation = new (NonPagedPoolNx) VIOGPU_WDDM_ALLOCATION;
        if (allocation == NULL)
        {
            if (nativeContext != NULL)
            {
                BOOLEAN dereferenced = VioGpuAdapter::DereferenceNativeContextAllocation(nativeContext);
                NT_ASSERT(dereferenced);
                UNREFERENCED_PARAMETER(dereferenced);
            }
            if (standardResourceId != 0)
            {
                BOOLEAN released = adapter->Release2DResourceId(standardResourceId);
                NT_ASSERT(released);
                UNREFERENCED_PARAMETER(released);
            }
            status = STATUS_NO_MEMORY;
            break;
        }

        RtlZeroMemory(allocation, sizeof(*allocation));
        allocation->Signature = VIOGPU_WDDM_ALLOCATION_SIGNATURE;
        KeInitializeMutex(&allocation->LifecycleMutex, 0);
        KeInitializeSpinLock(&allocation->SubmissionLock);
        allocation->SubmissionReferences = 0;
        allocation->OpenReferences = 0;
        allocation->Destroying = FALSE;
        allocation->Adapter = adapter;
        allocation->Resource = resource;
        allocation->NativeContext = nativeContext;
        allocation->DeferredContext = NULL;
        allocation->ContextGeneration = contextGeneration;
        allocation->ContextResetGeneration = contextResetGeneration;
        allocation->ContextId = contextId;
        allocation->PrivateData = privateData;
        allocation->BackingSize = alignedSize;
        allocation->ResourceId = nativeContext != NULL ? nativeResourceId : standardResourceId;
        allocation->BlobId = nativeResourceId;
        allocation->Resource2DState = VioGpu2DResourceNone;
        allocation->Resource2DResetGeneration = 0;
        allocation->HostState = VioGpuWddmAllocationHostNone;
        allocation->Pitch = privateData.Pitch;
        allocation->Width = privateData.Width;
        allocation->Height = privateData.Height;
        allocation->Format = FromPrivateFormat(privateData.Format);
        allocation->Flags = privateData.Flags;
        allocation->RefreshRateNumerator = privateData.RefreshRateNumerator;
        allocation->RefreshRateDenominator = privateData.RefreshRateDenominator;
        if (nativeContext != NULL)
        {
            status = RegisterNativeAllocationRange(allocation);
            if (!NT_SUCCESS(status))
            {
                allocation->Signature = 0;
                allocation->Adapter = NULL;
                delete allocation;
                BOOLEAN dereferenced = VioGpuAdapter::DereferenceNativeContextAllocation(nativeContext);
                NT_ASSERT(dereferenced);
                UNREFERENCED_PARAMETER(dereferenced);
                break;
            }
        }
        if (resource != NULL)
        {
            InterlockedIncrement(&resource->AllocationCount);
        }
        InitializeAllocationInfo(allocationInfo, allocation, alignedSize);
    }

    if (!NT_SUCCESS(status))
    {
        DestroyCreatedAllocations(createAllocation->pAllocationInfo, createdCount);
        if (createdResource)
        {
            NT_ASSERT(ReadResourceAllocationCount(resource) == 0);
            resource->Signature = 0;
            resource->Adapter = NULL;
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
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || destroyAllocation == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        (destroyAllocation->NumAllocations != 0 && destroyAllocation->pAllocationList == NULL) ||
        (destroyAllocation->Flags.Value & ~1U) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (UINT index = 0; index < destroyAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(destroyAllocation->pAllocationList[index]);
        if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
            allocation->Adapter != adapter || !IsOwnedAllocation(allocation, adapter))
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
        if (resource == NULL || resource->Signature != VIOGPU_WDDM_RESOURCE_SIGNATURE || resource->Adapter != adapter ||
            ReadResourceAllocationCount(resource) < 0)
        {
            return STATUS_INVALID_HANDLE;
        }
        LONG listedResourceAllocations = 0;
        for (UINT index = 0; index < destroyAllocation->NumAllocations; ++index)
        {
            VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(destroyAllocation->pAllocationList[index]);
            if (allocation->Resource != resource)
            {
                return STATUS_INVALID_HANDLE;
            }
            ++listedResourceAllocations;
        }
        if (ReadResourceAllocationCount(resource) != listedResourceAllocations)
        {
            return STATUS_DEVICE_BUSY;
        }
    }

    for (UINT index = 0; index < destroyAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(destroyAllocation->pAllocationList[index]);
        VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
        BOOLEAN snapshotAcquired = FALSE;
        NTSTATUS status = AcquireAllocationLifecycleForDestroy(allocation);
        RecordNativeAllocationDestroyState(adapter,
                                           VioGpuNativeAllocationDestroyLifecycle,
                                           status,
                                           index,
                                           allocation);
        if (status == STATUS_SUCCESS)
        {
            if (allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE || allocation->Adapter != adapter)
            {
                status = STATUS_INVALID_HANDLE;
            }
            else
            {
                BOOLEAN destroyStateValid = ValidateNativeAllocationDestroyState(allocation);
                if (!destroyStateValid)
                {
                    status = STATUS_DEVICE_NOT_READY;
                    RecordNativeAllocationDestroyState(adapter,
                                                       VioGpuNativeAllocationDestroyBegin,
                                                       status,
                                                       index,
                                                       allocation);
                }
                else
                {
                    snapshotAcquired = AcquireAllocationNativeContextSnapshot(allocation, &snapshot);
                    status = BeginAllocationDestroy(allocation);
                    RecordNativeAllocationDestroyState(adapter,
                                                       VioGpuNativeAllocationDestroyBegin,
                                                       status,
                                                       index,
                                                       allocation);
                    if (status == STATUS_SUCCESS)
                    {
                        if (IsNativeAllocation(allocation))
                        {
                            status = ReleaseAllocationHostOwnership(allocation, &snapshot, snapshotAcquired);
                            RecordNativeAllocationDestroyState(adapter,
                                                               VioGpuNativeAllocationDestroyHost,
                                                               status,
                                                               index,
                                                               allocation);
                            if (status == STATUS_SUCCESS)
                            {
                                ClearNativePlacement(allocation);
                            }
                        }
                        else if (IsStandardAllocation(allocation) && allocation->ResourceId != 0)
                        {
                            if (!ReconcileStandard2DAllocationAfterReset(allocation))
                            {
                                status = STATUS_DEVICE_NOT_READY;
                            }
                            else if (IsStandardPrimaryAllocation(allocation))
                            {
                                BOOLEAN detached = FALSE;
                                VIOGPU_HOST_CONTEXT_RESULT result = adapter->Detach2DScanoutResource(allocation->ResourceId,
                                                                                                     &detached);
                                status = result == VioGpuHostContextConfirmed && detached ? STATUS_SUCCESS
                                                                                          : STATUS_DEVICE_NOT_READY;
                            }
                            if (status == STATUS_SUCCESS)
                            {
                                BOOLEAN released = FALSE;
                                VIOGPU_HOST_CONTEXT_RESULT result = adapter->Destroy2DResource(allocation->ResourceId,
                                                                                               &allocation->Resource2DState,
                                                                                               &allocation->Resource2DResetGeneration,
                                                                                               &released);
                                if (released)
                                {
                                    ClearNativePlacement(allocation);
                                }
                                status = result == VioGpuHostContextConfirmed && released ? STATUS_SUCCESS
                                                                                          : STATUS_DEVICE_NOT_READY;
                            }
                        }
                        if (status == STATUS_SUCCESS && !allocation->PlacementValid)
                        {
                            ReleaseApertureMapping(allocation);
                        }
                    }
                }
            }
            KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        }
        if (snapshotAcquired)
        {
            VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
        }
        if (status != STATUS_SUCCESS)
        {
            // BeginAllocationDestroy deliberately leaves every previously
            // reserved object closed on failure.  A busy or transport-unknown
            // result must not reopen the object to a new Open/Render race.
            return status;
        }
    }

    // Complete every fallible Native Context detach before deleting any
    // allocation from the callback's list.  Success leaves both owner pointers
    // null for retry; failure leaves both pointers intact.
    for (UINT index = 0; index < destroyAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(destroyAllocation->pAllocationList[index]);
        NTSTATUS status = DetachAllocationNativeContext(allocation);
        if (status != STATUS_SUCCESS)
        {
            return status;
        }
    }

    for (UINT index = 0; index < destroyAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(destroyAllocation->pAllocationList[index]);
        RecordNativeAllocationDestroyState(adapter,
                                           VioGpuNativeAllocationDestroyComplete,
                                           STATUS_SUCCESS,
                                           index,
                                           allocation);
        if (IsStandardAllocation(allocation) && allocation->ResourceId != 0)
        {
            if (!adapter->Release2DResourceId(allocation->ResourceId))
            {
                return STATUS_DEVICE_NOT_READY;
            }
            allocation->ResourceId = 0;
        }
    }

    for (UINT index = 0; index < destroyAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(destroyAllocation->pAllocationList[index]);
        if (allocation->Resource != NULL)
        {
            LONG remaining = InterlockedDecrement(&allocation->Resource->AllocationCount);
            NT_ASSERT(remaining >= 0);
            UNREFERENCED_PARAMETER(remaining);
        }
        allocation->Signature = 0;
        allocation->Adapter = NULL;
        allocation->Resource = NULL;
        delete allocation;
    }

    if (resource != NULL)
    {
        NT_ASSERT(ReadResourceAllocationCount(resource) == 0);
        resource->Signature = 0;
        resource->Adapter = NULL;
        delete resource;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmDescribeAllocation(CONST HANDLE hAdapter,
                                                                      DXGKARG_DESCRIBEALLOCATION *describeAllocation)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || describeAllocation == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(describeAllocation->hAllocation);
    if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
        allocation->Adapter != adapter)
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
        device->Adapter == NULL || openAllocation->NumAllocations == 0 || openAllocation->pOpenAllocation == NULL ||
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
        __try
        {
            RtlCopyMemory(&privateData, openInfo->pPrivateDriverData, sizeof(privateData));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }
        DXGKARGCB_GETHANDLEDATA getHandleData = {};
        getHandleData.hObject = openInfo->hAllocation;
        getHandleData.Type = DXGK_HANDLE_ALLOCATION;
        getHandleData.Flags.Value = 0;

        VIOGPU_WDDM_ALLOCATION *allocation = static_cast<VIOGPU_WDDM_ALLOCATION *>(dxgkInterface->DxgkCbGetHandleData(
                                                                                                            &getHandleData));
        if (!IsOwnedAllocation(allocation, device->Adapter))
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
        status = AcquireAllocationLifecycle(allocation);
        if (status == STATUS_SUCCESS)
        {
            status = ReferenceAllocationOpen(allocation, device->Adapter);
            KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        }
        if (status != STATUS_SUCCESS)
        {
            DereferenceDevice(device);
            delete deviceAllocation;
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
            ReleaseAllocationOpen(deviceAllocation->Allocation);
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
            deviceAllocation->Device != device || !IsOwnedAllocation(deviceAllocation->Allocation, device->Adapter))
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
        ReleaseAllocationOpen(deviceAllocation->Allocation);
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
    if (device == NULL || device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE || createContext == NULL ||
        device->Adapter == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL || createContext->NodeOrdinal != 0 ||
        createContext->EngineAffinity != 1)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Only SystemContext and GdiContext select the context class.  Every other
     * bit describes an orthogonal capability: GPU virtual addressing, hardware
     * queues, protected contexts, and bits added by later Windows releases.
     * Treating an unrecognised bit as fatal made this DDI return
     * STATUS_NOT_SUPPORTED, which is not a legal status for DxgkDdiCreateContext:
     * dxgkrnl logs "Driver returned an invalid NTSTATUS code" and fails the
     * D3D device creation that owns the context, which is why Direct3D could
     * never open a device on this adapter.  Classify on the two class bits and
     * carry the rest through untouched. */
    const ULONG contextClassFlags = createContext->Flags.Value & ~VIOGPU_WDDM_CONTEXT_CLASS_FLAG_MASK;
    const BOOLEAN systemContext = createContext->Flags.SystemContext != 0;
    const BOOLEAN gdiContext = createContext->Flags.GdiContext != 0;
    const BOOLEAN hasPrivateData = createContext->pPrivateDriverData != NULL ||
                                   createContext->PrivateDriverDataSize != 0;
    if (contextClassFlags != 0)
    {
        /* Publish the flags dxgkrnl actually used so an unmodelled capability
         * bit stays visible without a kernel debugger. */
        device->Adapter->RecordNativeCreateContextDiagnostic(createContext->Flags.Value,
                                                            createContext->NodeOrdinal,
                                                            createContext->EngineAffinity,
                                                            createContext->PrivateDriverDataSize);
    }
    VIOGPU_WDDM_CONTEXT_TYPE contextType;
    if (systemContext && gdiContext)
    {
        return STATUS_INVALID_PARAMETER;
    }
    else if (systemContext)
    {
        contextType = VioGpuWddmContextSystem;
    }
    else if (gdiContext)
    {
        contextType = VioGpuWddmContextGdi;
    }
    else
    {
        /* The UMD CreateContext callback has no GDI selector.  Dxgkrnl sends
         * standard UMD contexts with no class flags or private payload, while
         * Native Context uses the same class flags plus the required ABI
         * payload. */
        contextType = hasPrivateData ? VioGpuWddmContextNative : VioGpuWddmContextGdi;
    }

    VIOGPU_WDDM_CONTEXT_CREATE privateData = {};
    if (contextType == VioGpuWddmContextNative)
    {
        if (createContext->pPrivateDriverData == NULL ||
            createContext->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_CONTEXT_CREATE))
        {
            return STATUS_INVALID_PARAMETER;
        }
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
    }
    else if (createContext->pPrivateDriverData != NULL || createContext->PrivateDriverDataSize != 0)
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
    KeInitializeSpinLock(&context->SubmissionLock);
    context->SubmissionReferences = 0;
    context->SubmissionClosing = FALSE;
    KeInitializeEvent(&context->SubmissionProgressEvent, SynchronizationEvent, FALSE);
    context->UmdFenceHead = 0;
    context->UmdFenceCount = 0;
    RtlZeroMemory(context->UmdFences, sizeof(context->UmdFences));
    context->SubmittedUmdFence = 0;
    context->CompletedUmdFence = 0;
    context->Device = device;
    context->RuntimeContext = NULL;
    context->Type = contextType;
    context->DestroyState = VioGpuWddmContextDestroyActive;
    context->DeferredAdapter = NULL;
    context->NodeOrdinal = createContext->NodeOrdinal;
    context->EngineAffinity = createContext->EngineAffinity;
    KeInitializeSpinLock(&context->NativeContext.BindingLock);
    InitializeListHead(&context->NativeContext.AllocationRanges);
    InitializeListHead(&context->PendingSubmissions);
    context->NativeContext.State = contextType == VioGpuWddmContextNative ? VioGpuNativeContextAllocated
                                                                          : VioGpuNativeContextDead;

    NTSTATUS status = contextType == VioGpuWddmContextNative ? device->Adapter->CreateNativeContext(&context->NativeContext,
                                                                                                    privateData.ExpectedResetGeneration)
                                                             : STATUS_SUCCESS;
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
    createContext->ContextInfo.AllocationListSize = contextType == VioGpuWddmContextGdi ? 256U
                                                                                        : VIOGPU_WDDM_ALLOCATION_LIST_SIZE;
    createContext->ContextInfo.PatchLocationListSize = contextType == VioGpuWddmContextGdi ? 256U
                                                                                           : VIOGPU_WDDM_PATCH_LIST_SIZE;
    createContext->hContext = context;
    return STATUS_SUCCESS;
}

NTSTATUS DeferNativeContextDestroy(_Inout_ VIOGPU_WDDM_CONTEXT *context,
                                   _In_ VioGpuDod *adapter,
                                   _Out_ BOOLEAN *deferred)
{
    if (context == NULL || adapter == NULL || deferred == NULL || context->Type != VioGpuWddmContextNative ||
        context->Device == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *deferred = FALSE;

    VIOGPU_WDDM_DEVICE *device = context->Device;
    KIRQL oldIrql;
    KeAcquireSpinLock(&context->NativeContext.BindingLock, &oldIrql);
    ULONG references = context->NativeContext.AllocationReferences;
    LONG registrationState = InterlockedCompareExchange(&context->NativeContext.State,
                                                        VioGpuNativeContextDead,
                                                        VioGpuNativeContextDead);
    VIOGPU_NATIVE_CONTEXT_OWNER *owner = context->NativeContext.Owner;
    BOOLEAN live = context->NativeContext.Registered && context->NativeContext.Adapter != NULL &&
                   context->NativeContext.Adapter->GetVioGpu() == adapter && owner != NULL &&
                   owner->Registration == &context->NativeContext && owner->State == VioGpuNativeContextOwnerLive &&
                   context->NativeContext.Generation > 0 && context->NativeContext.ResetGeneration != 0 &&
                   context->NativeContext.ContextId != 0 && owner->Generation == context->NativeContext.Generation &&
                   owner->ResetGeneration == context->NativeContext.ResetGeneration &&
                   owner->ContextId == context->NativeContext.ContextId && registrationState == VioGpuNativeContextLive;
#if defined(VIOGPU_NATIVE_CONTEXT)
    live = live && owner->ControlResourceCreated && owner->ControlMapped && owner->ControlResourceId != 0 &&
           owner->ControlBlobSize == VIOGPU_NATIVE_CONTROL_BLOB_SIZE && owner->ControlAddress != NULL &&
           owner->SubmitQueueCreated && owner->SubmitQueueId != 0 &&
           context->NativeContext.SubmitQueueId == owner->SubmitQueueId && context->NativeContext.VaStart != 0 &&
           context->NativeContext.VaSize != 0;
#endif
    BOOLEAN retired = !context->NativeContext.Registered && context->NativeContext.Adapter == NULL && owner == NULL &&
                      context->NativeContext.Generation == 0 && context->NativeContext.ResetGeneration == 0 &&
                      context->NativeContext.ContextId == 0 && context->NativeContext.VaStart == 0 &&
                      context->NativeContext.VaSize == 0 && context->NativeContext.SubmitQueueId == 0 &&
                      registrationState == VioGpuNativeContextDead;
    BOOLEAN valid = (live || retired) &&
                    InterlockedCompareExchange(&context->DestroyState,
                                               VioGpuWddmContextDestroyActive,
                                               VioGpuWddmContextDestroyActive) == VioGpuWddmContextDestroyActive &&
                    context->DeferredAdapter == NULL && device->Adapter == adapter;
    if (!valid)
    {
        KeReleaseSpinLock(&context->NativeContext.BindingLock, oldIrql);
        return STATUS_INVALID_DEVICE_STATE;
    }

    context->NativeContext.AllocationClosing = TRUE;
    ULONG rangeCount = 0;
    BOOLEAN rangesValid = TRUE;
    for (PLIST_ENTRY entry = context->NativeContext.AllocationRanges.Flink;
         entry != &context->NativeContext.AllocationRanges;
         entry = entry->Flink)
    {
        VIOGPU_WDDM_ALLOCATION_RANGE *range = CONTAINING_RECORD(entry, VIOGPU_WDDM_ALLOCATION_RANGE, Link);
        if (range->Registration != &context->NativeContext || !range->Linked || rangeCount == MAXULONG)
        {
            rangesValid = FALSE;
            break;
        }
        ++rangeCount;
    }
    if (!rangesValid || references < rangeCount)
    {
        KeReleaseSpinLock(&context->NativeContext.BindingLock, oldIrql);
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (references > rangeCount)
    {
        KeReleaseSpinLock(&context->NativeContext.BindingLock, oldIrql);
        return STATUS_DEVICE_BUSY;
    }
    if (references == 0)
    {
        KeReleaseSpinLock(&context->NativeContext.BindingLock, oldIrql);
        return STATUS_SUCCESS;
    }

    context->DeferredAdapter = adapter;
    context->RuntimeContext = NULL;
    context->Device = NULL;
    InterlockedExchange(&context->DestroyState, VioGpuWddmContextDestroyDeferred);
    DereferenceDevice(device);
    *deferred = TRUE;
    KeReleaseSpinLock(&context->NativeContext.BindingLock, oldIrql);
    return STATUS_SUCCESS;
}

#if defined(VIOGPU_NATIVE_CONTEXT)
static VOID RecordWddmNativeContextDestroyDiagnostic(_In_ VIOGPU_WDDM_CONTEXT *context,
                                                     _In_ VIOGPU_NATIVE_CONTEXT_DESTROY_STAGE stage,
                                                     _In_ NTSTATUS status,
                                                     _In_ DWORD detail)
{
    if (context == NULL || context->Type != VioGpuWddmContextNative || context->Device == NULL ||
        context->Device->Adapter == NULL)
    {
        return;
    }

    UINT contextId = 0;
    DWORD contextState = VioGpuNativeContextDead;
    DWORD ownerState = VioGpuNativeContextOwnerCreating;
    BOOLEAN ownerRetained = FALSE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&context->NativeContext.BindingLock, &oldIrql);
    contextId = context->NativeContext.ContextId;
    contextState = static_cast<DWORD>(InterlockedCompareExchange(&context->NativeContext.State,
                                                                 VioGpuNativeContextDead,
                                                                 VioGpuNativeContextDead));
    if (context->NativeContext.Owner != NULL)
    {
        ownerState = static_cast<DWORD>(context->NativeContext.Owner->State);
        ownerRetained = TRUE;
    }
    KeReleaseSpinLock(&context->NativeContext.BindingLock, oldIrql);
    context->Device->Adapter->RecordNativeContextDestroyDiagnostic(stage,
                                                                   status,
                                                                   detail,
                                                                   VioGpuHostContextUnknown,
                                                                   contextId,
                                                                   contextState,
                                                                   ownerState,
                                                                   FALSE,
                                                                   FALSE,
                                                                   ownerRetained);
}
#endif

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmDestroyContext(CONST HANDLE hContext)
{
    VIOGPU_WDDM_CONTEXT *context = reinterpret_cast<VIOGPU_WDDM_CONTEXT *>(hContext);
    if (context == NULL || context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_HANDLE;
    }

#if defined(VIOGPU_NATIVE_CONTEXT)
    RecordWddmNativeContextDestroyDiagnostic(context, VioGpuNativeContextDestroyEntered, STATUS_PENDING, 0);
#endif

    NTSTATUS submissionStatus = BeginContextSubmissionRundown(context);
    if (!NT_SUCCESS(submissionStatus))
    {
#if defined(VIOGPU_NATIVE_CONTEXT)
        RecordWddmNativeContextDestroyDiagnostic(context, VioGpuNativeContextDestroyRundown, submissionStatus, 0);
#endif
        return submissionStatus;
    }

    if (!context->OperationsRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&context->Operations);
        ExRundownCompleted(&context->Operations);
        context->OperationsRundownCompleted = TRUE;
    }

    VioGpuDod *adapter = context->Device == NULL ? NULL : context->Device->Adapter;
    if (adapter == NULL)
    {
#if defined(VIOGPU_NATIVE_CONTEXT)
        RecordWddmNativeContextDestroyDiagnostic(context,
                                                 VioGpuNativeContextDestroyAdapter,
                                                 STATUS_DEVICE_NOT_READY,
                                                 0);
#endif
        return STATUS_DEVICE_NOT_READY;
    }

    ULONGLONG submissionStallDeadline = KeQueryInterruptTime() + VIOGPU_WDDM_CONTEXT_DESTROY_STALL_TIMEOUT_100NS;
    for (;;)
    {
        VIOGPU_WDDM_CONTEXT_SUBMISSION_KIND kind = VioGpuWddmContextSubmissionRender;
        PVOID owner = NULL;
        LONG ownerState = 0;
        BOOLEAN referenceFailed = FALSE;
        BOOLEAN asynchronous = FALSE;
        BOOLEAN malformed = FALSE;

        KIRQL submissionIrql;
        KeAcquireSpinLock(&context->SubmissionLock, &submissionIrql);
        for (PLIST_ENTRY link = context->PendingSubmissions.Flink; link != &context->PendingSubmissions;
             link = link->Flink)
        {
            VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY *entry = CONTAINING_RECORD(link,
                                                                            VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY,
                                                                            Link);
            if (entry->Context != context || entry->Owner == NULL)
            {
                malformed = TRUE;
                break;
            }
            if (entry->Kind == VioGpuWddmContextSubmissionRender)
            {
                VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(entry->Owner);
                LONG state = InterlockedCompareExchange(&submission->State, 0, 0);
                if (state == VioGpuWddmSubmissionPrepared || state == VioGpuWddmSubmissionPatched ||
                    state == VioGpuWddmSubmissionEngineQueued)
                {
                    if (!ReferenceRenderSubmission(submission))
                    {
                        referenceFailed = TRUE;
                        break;
                    }
                    owner = submission;
                    ownerState = state;
                    kind = entry->Kind;
                    break;
                }
                if (state == VioGpuWddmSubmissionPatching || state == VioGpuWddmSubmissionSubmitClaimed ||
                    state == VioGpuWddmSubmissionHostIssued)
                {
                    InterlockedExchange(&submission->CancelRequested, 1);
                    asynchronous = TRUE;
                    continue;
                }
                malformed = TRUE;
                break;
            }
            if (entry->Kind == VioGpuWddmContextSubmissionPresent)
            {
                VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = static_cast<VIOGPU_WDDM_PRESENT_TRANSACTION *>(entry->Owner);
                LONG state = InterlockedCompareExchange(&transaction->State, 0, 0);
                if (state == VioGpuWddmPresentBuilt || state == VioGpuWddmPresentPatched ||
                    state == VioGpuWddmPresentQueued)
                {
                    if (!ReferencePresentTransaction(transaction))
                    {
                        referenceFailed = TRUE;
                        break;
                    }
                    owner = transaction;
                    ownerState = state;
                    kind = entry->Kind;
                    break;
                }
                if (state == VioGpuWddmPresentExecuting)
                {
                    InterlockedExchange(&transaction->CancelRequested, 1);
                    asynchronous = TRUE;
                    continue;
                }
                malformed = TRUE;
                break;
            }
            malformed = TRUE;
            break;
        }
        BOOLEAN empty = IsListEmpty(&context->PendingSubmissions);
        LONG submissionReferences = context->SubmissionReferences;
        KeReleaseSpinLock(&context->SubmissionLock, submissionIrql);

        if (malformed || referenceFailed)
        {
            adapter->RequestHardwareResetAtAnyIrql();
#if defined(VIOGPU_NATIVE_CONTEXT)
            RecordWddmNativeContextDestroyDiagnostic(context,
                                                     VioGpuNativeContextDestroyBusy,
                                                     STATUS_GRAPHICS_ALLOCATION_BUSY,
                                                     malformed ? 1U : 2U);
#endif
            return STATUS_GRAPHICS_ALLOCATION_BUSY;
        }
        if (owner == NULL)
        {
            if (!empty || asynchronous || submissionReferences != 0)
            {
                if ((asynchronous || submissionReferences != 0) &&
                    WaitForContextSubmissionProgress(context, &submissionStallDeadline))
                {
                    continue;
                }
                BOOLEAN invariantFailure = (empty && submissionReferences != 0) || (!empty && !asynchronous);
                if (invariantFailure)
                {
                    adapter->RequestHardwareResetAtAnyIrql();
                }
#if defined(VIOGPU_NATIVE_CONTEXT)
                RecordWddmNativeContextDestroyDiagnostic(context,
                                                         VioGpuNativeContextDestroyBusy,
                                                         STATUS_GRAPHICS_ALLOCATION_BUSY,
                                                         !empty ? 3U : (asynchronous ? 4U : 5U));
#endif
                return STATUS_GRAPHICS_ALLOCATION_BUSY;
            }
            break;
        }

        BOOLEAN retired = FALSE;
        if (kind == VioGpuWddmContextSubmissionRender)
        {
            VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(owner);
            if (ownerState == VioGpuWddmSubmissionPrepared || ownerState == VioGpuWddmSubmissionPatched)
            {
                retired = QuarantineSubmission(submission, ownerState, TRUE);
            }
            else
            {
                UINT fenceId = static_cast<UINT>(submission->FenceId);
                UINT nodeOrdinal = submission->Context->NodeOrdinal;
                VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP ownership = adapter->CancelNativePassiveWork(&submission->Work);
                if (ownership == VioGpuNativePassiveWorkRemoved)
                {
                    NativeRenderDispatchCancelled(submission);
                    adapter->NotifyNativeSubmissionFault(fenceId,
                                                         STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                                         nodeOrdinal,
                                                         0,
                                                         TRUE);
                    retired = TRUE;
                }
            }
            DereferenceRenderSubmission(submission);
        }
        else
        {
            VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = static_cast<VIOGPU_WDDM_PRESENT_TRANSACTION *>(owner);
            if (ownerState == VioGpuWddmPresentBuilt || ownerState == VioGpuWddmPresentPatched)
            {
                retired = RetirePresentTransaction(transaction, ownerState, VioGpuWddmPresentCancelled);
            }
            else
            {
                UINT fenceId = transaction->FenceId;
                UINT nodeOrdinal = transaction->Context->NodeOrdinal;
                VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP ownership = adapter->CancelNativePassiveWork(&transaction->Work);
                if (ownership == VioGpuNativePassiveWorkRemoved)
                {
                    NativePresentDispatchCancelled(transaction);
                    adapter->NotifyNativeSubmissionFault(fenceId,
                                                         STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                                         nodeOrdinal,
                                                         0,
                                                         TRUE);
                    retired = TRUE;
                }
            }
            DereferencePresentTransaction(transaction);
        }
        if (!retired)
        {
            if (WaitForContextSubmissionProgress(context, &submissionStallDeadline))
            {
                continue;
            }
            // Keep completion dispatch live so a later destroy retry can observe terminal retirement.
#if defined(VIOGPU_NATIVE_CONTEXT)
            RecordWddmNativeContextDestroyDiagnostic(context,
                                                     VioGpuNativeContextDestroyBusy,
                                                     STATUS_GRAPHICS_ALLOCATION_BUSY,
                                                     6U);
#endif
            return STATUS_GRAPHICS_ALLOCATION_BUSY;
        }
    }

    if (context->Type == VioGpuWddmContextNative)
    {
        BOOLEAN deferred = FALSE;
        NTSTATUS deferStatus = DeferNativeContextDestroy(context, adapter, &deferred);
        if (deferStatus != STATUS_SUCCESS)
        {
            if (deferStatus != STATUS_DEVICE_BUSY)
            {
                adapter->RequestHardwareResetAtAnyIrql();
            }
            return deferStatus;
        }
        if (deferred)
        {
            return STATUS_SUCCESS;
        }
    }

    BOOLEAN released = context->Type != VioGpuWddmContextNative;
    NTSTATUS status = STATUS_SUCCESS;
    if (!released && context->Device != NULL && context->Device->Adapter != NULL)
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

BOOLEAN ValidateTransferMdlRange(_In_ PMDL mdl, _In_ UINT mdlOffset, _In_ SIZE_T transferSize)
{
    if (mdl == NULL || transferSize == 0 || MmGetMdlByteOffset(mdl) != 0 || mdlOffset > (MAXULONG_PTR >> PAGE_SHIFT))
    {
        return FALSE;
    }
    SIZE_T byteOffset = static_cast<SIZE_T>(mdlOffset) << PAGE_SHIFT;
    SIZE_T byteCount = MmGetMdlByteCount(mdl);
    return byteOffset <= byteCount && transferSize <= byteCount - byteOffset;
}

VOID ReleasePagingTransactionReference(_Inout_ VIOGPU_WDDM_PAGING_TRANSACTION *transaction)
{
    if (transaction != NULL && transaction->Allocation != NULL &&
        InterlockedCompareExchange(&transaction->ReferenceHeld, 0, 1) == 1)
    {
        BOOLEAN released = ReleaseAllocationSubmissionReference(transaction->Allocation);
        NT_ASSERT(released);
        UNREFERENCED_PARAMETER(released);
    }
}

BOOLEAN ValidatePagingTransactionReference(_Inout_ VIOGPU_WDDM_PAGING_TRANSACTION *transaction, _In_ VioGpuDod *adapter)
{
    if (transaction == NULL || adapter == NULL || transaction->Signature != VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE ||
        transaction->Adapter != adapter || transaction->Allocation == NULL ||
        InterlockedCompareExchange(&transaction->State, 0, 0) != VioGpuWddmPagingTransactionBuilt ||
        InterlockedCompareExchange(&transaction->ReferenceHeld, 0, 0) != 1)
    {
        return FALSE;
    }
    return TRUE;
}

BOOLEAN IsPatchOffsetForSubmission(_In_ UINT patchOffset,
                                   _In_ UINT expectedRelativeOffset,
                                   _In_ UINT submissionStartOffset)
{
    if (patchOffset == expectedRelativeOffset)
    {
        return TRUE;
    }
    return submissionStartOffset <= MAXUINT - expectedRelativeOffset &&
           patchOffset == submissionStartOffset + expectedRelativeOffset;
}

BOOLEAN FinishPagingTransaction(_Inout_ VIOGPU_WDDM_PAGING_TRANSACTION *transaction,
                                _In_ VIOGPU_WDDM_PAGING_TRANSACTION_STATE expectedState,
                                _In_ VIOGPU_WDDM_PAGING_TRANSACTION_STATE finalState)
{
    if (transaction == NULL || transaction->Signature != VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE ||
        InterlockedCompareExchange(&transaction->State, finalState, expectedState) != expectedState)
    {
        return FALSE;
    }
    return TRUE;
}

NTSTATUS ExecutePagingTransaction(_Inout_ VIOGPU_WDDM_PAGING_TRANSACTION *transaction)
{
    if (transaction == NULL || transaction->Signature != VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE ||
        InterlockedCompareExchange(&transaction->State, 0, 0) != VioGpuWddmPagingTransactionExecuting ||
        transaction->Adapter == NULL || transaction->Allocation == NULL ||
        transaction->Adapter->IsHardwareResetRequested())
    {
        return STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_WDDM_ALLOCATION *allocation = transaction->Allocation;
    NTSTATUS status = AcquireAllocationLifecycle(allocation);
    if (status != STATUS_SUCCESS)
    {
        return status;
    }

    BOOLEAN valid = allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE &&
                    allocation->Adapter == transaction->Adapter && allocation->ResourceId == transaction->ResourceId &&
                    allocation->PlacementValid && allocation->PlacementOffset == transaction->PlacementOffset &&
                    allocation->ApertureMdl != NULL && allocation->ApertureAddress != NULL;
    if (valid && transaction->ContextId == 0)
    {
        valid = IsStandardAllocation(allocation) && allocation->NativeContext == NULL && allocation->ContextId == 0 &&
                allocation->ContextGeneration == 0 && allocation->ContextResetGeneration == 0 &&
                !transaction->Adapter->IsHardwareResetRequested() && EnsureStandard2DAllocationBacking(allocation) &&
                allocation->Resource2DState == VioGpu2DResourceBackingAttached &&
                allocation->Resource2DResetGeneration != 0;
    }
    else if (valid)
    {
        valid = IsNativeAllocation(allocation) && allocation->HostState == VioGpuWddmAllocationHostLive &&
                allocation->ResourceId >= VIOGPU_NATIVE_RESOURCE_ID_START && allocation->ResourceId != MAXUINT &&
                allocation->BlobId == allocation->ResourceId && allocation->ContextId == transaction->ContextId &&
                allocation->ContextGeneration == transaction->ContextGeneration &&
                allocation->ContextResetGeneration == transaction->ResetGeneration &&
                transaction->Adapter->IsNativeContextGenerationCurrent(transaction->ContextGeneration,
                                                                       transaction->ResetGeneration);
    }

    UINT transferFlags = VioGpuWddmPagingFlagPageIn | VioGpuWddmPagingFlagPageOut;
    if (valid && (transaction->Flags & transferFlags) != 0 && !transaction->TransferDataComplete)
    {
        valid = FALSE;
    }
    if (valid && transaction->Adapter->IsHardwareResetRequested())
    {
        valid = FALSE;
    }
    KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
    return valid ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}
NTSTATUS BuildPfnEntries(_In_reads_(numberOfPages) const PFN_NUMBER *pfns,
                         _In_ SIZE_T numberOfPages,
                         _Out_writes_(entryCapacity) GPU_MEM_ENTRY *entries,
                         _In_ UINT entryCapacity,
                         _Out_ PUINT entryCount)
{
    if (pfns == NULL || entries == NULL || entryCount == NULL || numberOfPages == 0 || entryCapacity == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *entryCount = 0;

    for (SIZE_T page = 0; page < numberOfPages; ++page)
    {
        PFN_NUMBER pfn = pfns[page];
        if (static_cast<ULONGLONG>(pfn) > (MAXULONGLONG >> PAGE_SHIFT))
        {
            return STATUS_INVALID_PARAMETER;
        }
        ULONGLONG physicalAddress = static_cast<ULONGLONG>(pfn) << PAGE_SHIFT;
        /* A VirtIO SG entry describes the whole page.  The largest representable
         * PFN can still produce an address whose page end wraps UINT64; reject
         * that record before it reaches the Host blob protocol. */
        if (physicalAddress > MAXULONGLONG - (PAGE_SIZE - 1))
        {
            return STATUS_INVALID_PARAMETER;
        }
        if (*entryCount != 0)
        {
            GPU_MEM_ENTRY *previous = &entries[*entryCount - 1];
            if (previous->addr <= MAXULONGLONG - previous->length &&
                previous->addr + previous->length == physicalAddress && previous->length <= MAXULONG - PAGE_SIZE)
            {
                previous->length += PAGE_SIZE;
                continue;
            }
        }
        if (*entryCount == entryCapacity)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        entries[*entryCount].addr = physicalAddress;
        entries[*entryCount].length = PAGE_SIZE;
        entries[*entryCount].padding = 0;
        ++*entryCount;
    }
    return STATUS_SUCCESS;
}

BOOLEAN ValidateAperturePageState(_In_ const VIOGPU_WDDM_ALLOCATION *allocation, _In_ BOOLEAN requireComplete)
{
    if (allocation == NULL || allocation->AperturePfns == NULL || allocation->ApertureMappedPages == NULL ||
        allocation->AperturePageCount == 0 || allocation->ApertureMappedPageCount > allocation->AperturePageCount)
    {
        return FALSE;
    }

    SIZE_T observedMappedPages = 0;
    for (SIZE_T page = 0; page < allocation->AperturePageCount; ++page)
    {
        UCHAR pageState = allocation->ApertureMappedPages[page];
        PFN_NUMBER pfn = allocation->AperturePfns[page];
        if (pageState > VioGpuWddmAperturePageDummy || static_cast<ULONGLONG>(pfn) > (MAXULONGLONG >> PAGE_SHIFT))
        {
            return FALSE;
        }
        if (pageState == VioGpuWddmAperturePageMapped)
        {
            ++observedMappedPages;
        }
    }

    if (observedMappedPages != allocation->ApertureMappedPageCount)
    {
        return FALSE;
    }
    return !requireComplete || observedMappedPages == allocation->AperturePageCount;
}

NTSTATUS AllocateApertureBackingEntries(_In_ const VIOGPU_WDDM_ALLOCATION *allocation,
                                        _Outptr_result_buffer_(*entryCount) GPU_MEM_ENTRY **entries,
                                        _Out_ PUINT entryCount)
{
    if (allocation == NULL || entries == NULL || entryCount == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *entries = NULL;
    *entryCount = 0;
    if (allocation->AperturePfns == NULL || allocation->ApertureMappedPages == NULL ||
        allocation->AperturePageCount == 0 || allocation->ApertureMappedPageCount != allocation->AperturePageCount ||
        allocation->AperturePageCount > MAXUINT || !ValidateAperturePageState(allocation, TRUE))
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    for (SIZE_T page = 0; page < allocation->AperturePageCount; ++page)
    {
        if (allocation->ApertureMappedPages[page] != VioGpuWddmAperturePageMapped)
        {
            return STATUS_INVALID_DEVICE_STATE;
        }
    }

    UINT entryCapacity = static_cast<UINT>(allocation->AperturePageCount > VIOGPU_MAX_BACKING_ENTRIES ? VIOGPU_MAX_BACKING_ENTRIES
                                                                                                      : allocation->AperturePageCount);
    SIZE_T entriesSize = static_cast<SIZE_T>(entryCapacity) * sizeof(GPU_MEM_ENTRY);
    GPU_MEM_ENTRY *newEntries = static_cast<GPU_MEM_ENTRY *>(ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                                                         entriesSize,
                                                                                         'eSGV'));
    if (newEntries == NULL)
    {
        return STATUS_NO_MEMORY;
    }

    NTSTATUS status = BuildPfnEntries(allocation->AperturePfns,
                                      allocation->AperturePageCount,
                                      newEntries,
                                      entryCapacity,
                                      entryCount);
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(newEntries, 'eSGV');
        *entryCount = 0;
        return status;
    }
    *entries = newEntries;
    return STATUS_SUCCESS;
}

NTSTATUS EnsureAperturePageState(_Inout_ VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL || allocation->BackingSize == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    SIZE_T pageCount = BYTES_TO_PAGES(allocation->BackingSize);
    if (pageCount == 0 || pageCount > MAXULONG_PTR / sizeof(PFN_NUMBER))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (allocation->AperturePfns != NULL || allocation->ApertureMappedPages != NULL)
    {
        return allocation->AperturePfns != NULL && allocation->ApertureMappedPages != NULL && allocation->AperturePageCount == pageCount && allocation->ApertureMappedPageCount <= allocation->AperturePageCount
                                                                                                                   ? STATUS_SUCCESS
                                                                                                                   : STATUS_DEVICE_NOT_READY;
    }

    PPFN_NUMBER pfns = static_cast<PPFN_NUMBER>(ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                                            pageCount * sizeof(PFN_NUMBER),
                                                                            'fAGV'));
    if (pfns == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    PUCHAR pageStates = static_cast<PUCHAR>(ExAllocatePoolZero(NonPagedPoolNx, pageCount, 'mAGV'));
    if (pageStates == NULL)
    {
        ExFreePoolWithTag(pfns, 'fAGV');
        return STATUS_NO_MEMORY;
    }
    RtlZeroMemory(pfns, pageCount * sizeof(PFN_NUMBER));
    allocation->AperturePfns = pfns;
    allocation->ApertureMappedPages = pageStates;
    allocation->AperturePageCount = pageCount;
    allocation->ApertureMappedPageCount = 0;
    return STATUS_SUCCESS;
}

VOID ReleaseApertureCpuMapping(_Inout_ VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL)
    {
        return;
    }
    if (allocation->ApertureAddress != NULL && allocation->ApertureMdl != NULL)
    {
        MmUnmapLockedPages(allocation->ApertureAddress, allocation->ApertureMdl);
    }
    allocation->ApertureAddress = NULL;
    if (allocation->ApertureMdl != NULL)
    {
        allocation->ApertureMdl->MdlFlags &= ~MDL_PAGES_LOCKED;
        IoFreeMdl(allocation->ApertureMdl);
        allocation->ApertureMdl = NULL;
    }
}

NTSTATUS EnsureApertureCpuMapping(_Inout_ VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL || allocation->AperturePfns == NULL || allocation->ApertureMappedPages == NULL ||
        allocation->AperturePageCount == 0 || allocation->ApertureMappedPageCount != allocation->AperturePageCount ||
        allocation->BackingSize == 0 || allocation->BackingSize > MAXULONG ||
        !ValidateAperturePageState(allocation, TRUE))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (allocation->ApertureMdl != NULL || allocation->ApertureAddress != NULL)
    {
        return allocation->ApertureMdl != NULL && allocation->ApertureAddress != NULL ? STATUS_SUCCESS
                                                                                      : STATUS_DEVICE_NOT_READY;
    }

    PMDL mdl = IoAllocateMdl(NULL, static_cast<ULONG>(allocation->BackingSize), FALSE, FALSE, NULL);
    if (mdl == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    RtlCopyMemory(MmGetMdlPfnArray(mdl), allocation->AperturePfns, allocation->AperturePageCount * sizeof(PFN_NUMBER));
    mdl->MdlFlags |= MDL_PAGES_LOCKED;
    PVOID address = MmMapLockedPagesSpecifyCache(mdl,
                                                 KernelMode,
                                                 MmCached,
                                                 NULL,
                                                 FALSE,
                                                 NormalPagePriority | MdlMappingNoExecute);
    if (address == NULL)
    {
        mdl->MdlFlags &= ~MDL_PAGES_LOCKED;
        IoFreeMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    allocation->ApertureMdl = mdl;
    allocation->ApertureAddress = address;
    return STATUS_SUCCESS;
}

VOID ReleaseApertureMapping(_Inout_ VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL)
    {
        return;
    }
    ReleaseApertureCpuMapping(allocation);
    if (allocation->AperturePfns != NULL)
    {
        ExFreePoolWithTag(allocation->AperturePfns, 'fAGV');
        allocation->AperturePfns = NULL;
    }
    if (allocation->ApertureMappedPages != NULL)
    {
        ExFreePoolWithTag(allocation->ApertureMappedPages, 'mAGV');
        allocation->ApertureMappedPages = NULL;
    }
    allocation->AperturePageCount = 0;
    allocation->ApertureMappedPageCount = 0;
    allocation->ApertureBasePage = 0;
    allocation->ApertureBaseValid = FALSE;
}

NTSTATUS MapApertureAllocation(_In_ VioGpuDod *adapter,
                               _Inout_ VIOGPU_WDDM_ALLOCATION *allocation,
                               _In_ SIZE_T offsetInPages,
                               _In_ SIZE_T numberOfPages,
                               _In_ PMDL mdl,
                               _In_ UINT mdlOffset)
{
    if (adapter == NULL || allocation == NULL || mdl == NULL || numberOfPages == 0 || MmGetMdlByteCount(mdl) == 0 ||
        MmGetMdlByteOffset(mdl) != 0 || (mdl->MdlFlags & MDL_PAGES_LOCKED) == 0 ||
        offsetInPages > (MAXULONGLONG >> PAGE_SHIFT) ||
        numberOfPages - 1 > (MAXULONGLONG >> PAGE_SHIFT) - offsetInPages ||
        offsetInPages >= (VIOGPU_WDDM_APERTURE_SIZE >> PAGE_SHIFT) ||
        numberOfPages > (VIOGPU_WDDM_APERTURE_SIZE >> PAGE_SHIFT) - offsetInPages)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    NTSTATUS status = AcquireAllocationLifecycle(allocation);
    if (status != STATUS_SUCCESS)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
    BOOLEAN nativeAllocation = IsNativeAllocation(allocation);
    BOOLEAN snapshotAcquired = nativeAllocation && AcquireAllocationNativeContextSnapshot(allocation, &snapshot);
    if (nativeAllocation && !snapshotAcquired)
    {
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    SIZE_T allocationPageCount = BYTES_TO_PAGES(allocation->BackingSize);
    SIZE_T mdlPageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(mdl), MmGetMdlByteCount(mdl));
    SIZE_T allocationPage = static_cast<SIZE_T>(mdlOffset);
    BOOLEAN baseResolved = offsetInPages >= allocationPage;
    SIZE_T basePage = baseResolved ? offsetInPages - allocationPage : 0;
    SIZE_T aperturePageCount = VIOGPU_WDDM_APERTURE_SIZE >> PAGE_SHIFT;
    ULONGLONG placementOffset = static_cast<ULONGLONG>(basePage) << PAGE_SHIFT;
    BOOLEAN valid = allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE && allocation->Adapter == adapter &&
                    !allocation->Destroying && allocation->BackingSize != 0 && allocationPageCount != 0 &&
                    baseResolved && allocationPage <= allocationPageCount &&
                    numberOfPages <= allocationPageCount - allocationPage && mdlOffset <= mdlPageCount &&
                    numberOfPages <= mdlPageCount - mdlOffset && basePage < aperturePageCount &&
                    allocationPageCount <= aperturePageCount - basePage && basePage <= (MAXULONGLONG >> PAGE_SHIFT) &&
                    allocationPageCount - 1 <= (MAXULONGLONG >> PAGE_SHIFT) - basePage &&
                    (allocation->ApertureBaseValid ? allocation->ApertureBasePage == basePage
                                                   : allocation->ApertureMappedPageCount == 0);
    if (nativeAllocation)
    {
        BOOLEAN hostPlacementConsistent = (allocation->HostState == VioGpuWddmAllocationHostNone &&
                                           !allocation->PlacementValid) ||
                                          (allocation->HostState == VioGpuWddmAllocationHostLive &&
                                           allocation->PlacementValid);
        valid = valid && hostPlacementConsistent && allocation->ResourceId >= VIOGPU_NATIVE_RESOURCE_ID_START &&
                allocation->ResourceId == allocation->BlobId;
    }
    else
    {
        valid = valid && IsStandardAllocation(allocation) && allocation->HostState == VioGpuWddmAllocationHostNone &&
                allocation->ResourceId != 0 && allocation->ResourceId < VIOGPU_NATIVE_RESOURCE_ID_START;
    }

    if (valid)
    {
        status = EnsureAperturePageState(allocation);
        valid = NT_SUCCESS(status);
    }
    else
    {
        status = STATUS_DEVICE_NOT_READY;
    }

    SIZE_T observedMappedPages = 0;
    for (SIZE_T page = 0; valid && page < allocation->AperturePageCount; ++page)
    {
        UCHAR pageState = allocation->ApertureMappedPages[page];
        if (pageState > VioGpuWddmAperturePageDummy)
        {
            status = STATUS_INVALID_DEVICE_STATE;
            valid = FALSE;
        }
        else if (pageState == VioGpuWddmAperturePageMapped)
        {
            ++observedMappedPages;
        }
    }
    if (valid && observedMappedPages != allocation->ApertureMappedPageCount)
    {
        status = STATUS_INVALID_DEVICE_STATE;
        valid = FALSE;
    }
    PPFN_NUMBER mdlPfns = valid ? MmGetMdlPfnArray(mdl) : NULL;
    if (valid && mdlPfns == NULL)
    {
        status = STATUS_INVALID_PARAMETER;
        valid = FALSE;
    }
    SIZE_T pagesBecomingMapped = 0;
    for (SIZE_T page = 0; valid && page < numberOfPages; ++page)
    {
        SIZE_T allocationPageIndex = allocationPage + page;
        PFN_NUMBER pfn = mdlPfns[static_cast<SIZE_T>(mdlOffset) + page];
        UCHAR currentState = allocation->ApertureMappedPages[allocationPageIndex];
        if (static_cast<ULONGLONG>(pfn) > (MAXULONGLONG >> PAGE_SHIFT) || currentState > VioGpuWddmAperturePageDummy ||
            (currentState == VioGpuWddmAperturePageMapped && allocation->AperturePfns[allocationPageIndex] != pfn))
        {
            status = STATUS_INVALID_PARAMETER;
            valid = FALSE;
        }
        else if (currentState != VioGpuWddmAperturePageMapped)
        {
            ++pagesBecomingMapped;
        }
    }
    if (valid && pagesBecomingMapped > allocation->AperturePageCount - allocation->ApertureMappedPageCount)
    {
        status = STATUS_INVALID_DEVICE_STATE;
        valid = FALSE;
    }
    if (valid && !allocation->ApertureBaseValid)
    {
        allocation->ApertureBasePage = basePage;
        allocation->ApertureBaseValid = TRUE;
    }
    if (valid && pagesBecomingMapped != 0 && allocation->ApertureAddress != NULL)
    {
        ReleaseApertureCpuMapping(allocation);
    }
    for (SIZE_T page = 0; valid && page < numberOfPages; ++page)
    {
        SIZE_T allocationPageIndex = allocationPage + page;
        if (allocation->ApertureMappedPages[allocationPageIndex] != VioGpuWddmAperturePageMapped)
        {
            allocation->AperturePfns[allocationPageIndex] = mdlPfns[static_cast<SIZE_T>(mdlOffset) + page];
            allocation->ApertureMappedPages[allocationPageIndex] = VioGpuWddmAperturePageMapped;
        }
    }
    if (valid)
    {
        allocation->ApertureMappedPageCount += pagesBecomingMapped;
    }

    if (valid && allocation->ApertureMappedPageCount != allocation->AperturePageCount)
    {
        status = STATUS_SUCCESS;
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        if (snapshotAcquired)
        {
            VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
        }
        return STATUS_SUCCESS;
    }

    if (valid && allocation->PlacementValid)
    {
        valid = allocation->PlacementOffset == placementOffset && allocation->ApertureMdl != NULL &&
                allocation->ApertureAddress != NULL &&
                (nativeAllocation ? allocation->HostState == VioGpuWddmAllocationHostLive
                                  : allocation->Resource2DState == VioGpu2DResourceBackingAttached);
        status = valid ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        if (snapshotAcquired)
        {
            VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
        }
        return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    GPU_MEM_ENTRY *entries = NULL;
    UINT entryCount = 0;
    if (valid)
    {
        status = AllocateApertureBackingEntries(allocation, &entries, &entryCount);
        if (NT_SUCCESS(status))
        {
            status = EnsureApertureCpuMapping(allocation);
        }
    }

    if (NT_SUCCESS(status))
    {
        if (nativeAllocation)
        {
            UINT msmFlags = MSM_BO_CACHED_COHERENT;
            if ((allocation->Flags & VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY) != 0)
            {
                msmFlags |= MSM_BO_GPU_READONLY;
            }
            UINT blobFlags = VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE;
            if ((allocation->Flags & VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE) != 0)
            {
                blobFlags |= VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
            }
            BOOLEAN ownershipRetained = FALSE;
            VIOGPU_HOST_CONTEXT_RESULT result = snapshot.Adapter->CreateNativeGuestAllocation(&snapshot,
                                                                                              allocation->ResourceId,
                                                                                              allocation->BlobId,
                                                                                              allocation->PrivateData.Size,
                                                                                              allocation->BackingSize,
                                                                                              allocation->PrivateData.RequestedIova,
                                                                                              entries,
                                                                                              entryCount,
                                                                                              msmFlags,
                                                                                              blobFlags,
                                                                                              &ownershipRetained);
            if (ownershipRetained)
            {
                PublishNativePlacement(allocation,
                                       &snapshot,
                                       placementOffset,
                                       result == VioGpuHostContextConfirmed ? VioGpuWddmAllocationHostLive
                                                                            : VioGpuWddmAllocationHostUnknown);
            }
            status = result == VioGpuHostContextConfirmed && ownershipRetained ? STATUS_SUCCESS
                                                                               : STATUS_DEVICE_NOT_READY;
        }
        else
        {
            UINT virtioFormat = 0;
            if (!ResolveStandard2DFormat(allocation->Format, &virtioFormat))
            {
                status = STATUS_INVALID_PARAMETER;
            }
            else
            {
                VIOGPU_HOST_CONTEXT_RESULT result = adapter->Create2DResourceBacking(allocation->ResourceId,
                                                                                     virtioFormat,
                                                                                     allocation->Width,
                                                                                     allocation->Height,
                                                                                     allocation->BackingSize,
                                                                                     entries,
                                                                                     entryCount,
                                                                                     &allocation->Resource2DState,
                                                                                     &allocation->Resource2DResetGeneration);
                if (allocation->Resource2DState == VioGpu2DResourceBackingAttached ||
                    allocation->Resource2DState == VioGpu2DResourceUnknown)
                {
                    PublishStandardPlacement(allocation, placementOffset);
                }
                status = result == VioGpuHostContextConfirmed && allocation->Resource2DState == VioGpu2DResourceBackingAttached
                                                                                                                             ? STATUS_SUCCESS
                                                                                                                             : STATUS_DEVICE_NOT_READY;
            }
        }
    }

    if (entries != NULL)
    {
        ExFreePoolWithTag(entries, 'eSGV');
    }
    KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
    if (snapshotAcquired)
    {
        VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
    }
    return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_GRAPHICS_ALLOCATION_BUSY;
}

NTSTATUS UnmapApertureAllocation(_In_ VioGpuDod *adapter,
                                 _Inout_ VIOGPU_WDDM_ALLOCATION *allocation,
                                 _In_ SIZE_T offsetInPages,
                                 _In_ SIZE_T numberOfPages,
                                 _In_ PHYSICAL_ADDRESS dummyPage)
{
    if (adapter == NULL || allocation == NULL || numberOfPages == 0 || dummyPage.QuadPart < 0 ||
        (static_cast<ULONGLONG>(dummyPage.QuadPart) & (PAGE_SIZE - 1)) != 0 ||
        offsetInPages >= (VIOGPU_WDDM_APERTURE_SIZE >> PAGE_SHIFT) ||
        numberOfPages > (VIOGPU_WDDM_APERTURE_SIZE >> PAGE_SHIFT) - offsetInPages)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    NTSTATUS status = AcquireAllocationLifecycle(allocation);
    if (status != STATUS_SUCCESS)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
    BOOLEAN nativeAllocation = IsNativeAllocation(allocation);
    BOOLEAN snapshotAcquired = nativeAllocation && AcquireAllocationNativeContextSnapshot(allocation, &snapshot);
    /* A dead registration cannot yield a live snapshot after a confirmed
     * reset. Keep this narrow path available for VidMm bookkeeping while
     * every other snapshot failure remains busy. */
    BOOLEAN resetRetired = nativeAllocation && AllocationResetRetired(allocation);
    if (nativeAllocation && !snapshotAcquired && !resetRetired)
    {
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    SIZE_T allocationPage = allocation->ApertureBaseValid && offsetInPages >= allocation->ApertureBasePage ? offsetInPages - allocation->ApertureBasePage
                                                                                                           : MAXULONG_PTR;
    if (allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE || allocation->Adapter != adapter ||
        allocation->Destroying || !allocation->ApertureBaseValid || allocation->AperturePfns == NULL ||
        allocation->ApertureMappedPages == NULL || allocation->AperturePageCount == 0 ||
        allocation->ApertureMappedPageCount > allocation->AperturePageCount ||
        !ValidateAperturePageState(allocation, FALSE) || allocationPage > allocation->AperturePageCount ||
        numberOfPages > allocation->AperturePageCount - allocationPage ||
        (nativeAllocation && allocation->HostState != VioGpuWddmAllocationHostNone && !snapshotAcquired &&
         !resetRetired))
    {
        status = STATUS_DEVICE_NOT_READY;
    }

    SIZE_T mappedPagesToRemove = 0;
    for (SIZE_T page = 0; NT_SUCCESS(status) && page < numberOfPages; ++page)
    {
        UCHAR pageState = allocation->ApertureMappedPages[allocationPage + page];
        if (pageState > VioGpuWddmAperturePageDummy)
        {
            status = STATUS_INVALID_DEVICE_STATE;
        }
        else if (pageState == VioGpuWddmAperturePageMapped)
        {
            ++mappedPagesToRemove;
        }
    }
    if (NT_SUCCESS(status) && mappedPagesToRemove > allocation->ApertureMappedPageCount)
    {
        status = STATUS_INVALID_DEVICE_STATE;
    }

    BOOLEAN released = FALSE;
    if (NT_SUCCESS(status) && nativeAllocation)
    {
        if (allocation->HostState == VioGpuWddmAllocationHostNone)
        {
            released = TRUE;
        }
        else if (resetRetired)
        {
            /* The device reset is the ownership proof.  No old-generation
             * UNREF can be submitted after the context has been retired. */
            ClearAllocationHostBinding(allocation);
            released = TRUE;
        }
        else
        {
            VIOGPU_HOST_CONTEXT_RESULT result = snapshot.Adapter->DestroyNativeGuestAllocation(&snapshot,
                                                                                               allocation->ResourceId,
                                                                                               &released);
            status = result == VioGpuHostContextConfirmed && released ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
            if (released)
            {
                ClearAllocationHostBinding(allocation);
            }
        }
    }
    else if (NT_SUCCESS(status) && !nativeAllocation)
    {
        if (allocation->Resource2DState == VioGpu2DResourceNone)
        {
            released = TRUE;
        }
        else
        {
            if (IsStandardPrimaryAllocation(allocation))
            {
                BOOLEAN detached = FALSE;
                VIOGPU_HOST_CONTEXT_RESULT result = adapter->Detach2DScanoutResource(allocation->ResourceId, &detached);
                status = result == VioGpuHostContextConfirmed && detached ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
            }
            if (NT_SUCCESS(status))
            {
                VIOGPU_HOST_CONTEXT_RESULT result = adapter->Destroy2DResource(allocation->ResourceId,
                                                                               &allocation->Resource2DState,
                                                                               &allocation->Resource2DResetGeneration,
                                                                               &released);
                status = result == VioGpuHostContextConfirmed && released ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
            }
        }
    }

    if (released)
    {
        ClearNativePlacement(allocation);
        ReleaseApertureCpuMapping(allocation);
        PFN_NUMBER dummyPfn = static_cast<PFN_NUMBER>(static_cast<ULONGLONG>(dummyPage.QuadPart) >> PAGE_SHIFT);
        for (SIZE_T page = 0; page < numberOfPages; ++page)
        {
            SIZE_T allocationPageIndex = allocationPage + page;
            allocation->AperturePfns[allocationPageIndex] = dummyPfn;
            allocation->ApertureMappedPages[allocationPageIndex] = VioGpuWddmAperturePageDummy;
        }
        allocation->ApertureMappedPageCount -= mappedPagesToRemove;
        if (allocation->ApertureMappedPageCount == 0)
        {
            RtlZeroMemory(allocation->AperturePfns, allocation->AperturePageCount * sizeof(*allocation->AperturePfns));
            RtlZeroMemory(allocation->ApertureMappedPages, allocation->AperturePageCount);
            allocation->ApertureBasePage = 0;
            allocation->ApertureBaseValid = FALSE;
        }
    }
    KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
    if (snapshotAcquired)
    {
        VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
    }
    return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_GRAPHICS_ALLOCATION_BUSY;
}

NTSTATUS BuildSoftwarePagingTransaction(_In_ VioGpuDod *adapter,
                                        _Inout_ DXGKARG_BUILDPAGINGBUFFER *pagingBuffer,
                                        _Inout_ VIOGPU_WDDM_ALLOCATION *allocation,
                                        _In_ LARGE_INTEGER segmentAddress,
                                        _In_opt_ PMDL transferMdl,
                                        _In_ UINT mdlOffset,
                                        _In_ SIZE_T transferOffset,
                                        _In_ SIZE_T transferSize,
                                        _In_ UINT fillPattern,
                                        _In_ UINT packetFlags)
{
    ULONGLONG placementOffset = 0;
    if (adapter == NULL || pagingBuffer == NULL || allocation == NULL)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    NTSTATUS status = AcquireAllocationLifecycle(allocation);
    BOOLEAN lifecycleAcquired = status == STATUS_SUCCESS;
    BOOLEAN nativeAllocation = FALSE;
    if (status == STATUS_SUCCESS)
    {
        nativeAllocation = IsNativeAllocation(allocation);
        if (!NT_SUCCESS(ValidateNativePlacement(allocation, segmentAddress, &placementOffset)))
        {
            status = STATUS_INVALID_PARAMETER;
        }
    }
    if (status == STATUS_SUCCESS &&
        (allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE || allocation->Adapter != adapter ||
         !allocation->PlacementValid || allocation->PlacementOffset != placementOffset ||
         allocation->ApertureMdl == NULL || allocation->ApertureAddress == NULL ||
         (nativeAllocation && allocation->HostState != VioGpuWddmAllocationHostLive) ||
         (!nativeAllocation && allocation->Resource2DState != VioGpu2DResourceBackingAttached)))
    {
        status = STATUS_DEVICE_NOT_READY;
    }

    BOOLEAN transfer = (packetFlags & (VioGpuWddmPagingFlagPageIn | VioGpuWddmPagingFlagPageOut)) != 0;
    BOOLEAN transferDataComplete = !transfer;
    if (status == STATUS_SUCCESS && transfer)
    {
        PVOID systemAddress = NULL;
        status = ResolveTransferMdlAddress(transferMdl, mdlOffset, transferSize, &systemAddress);
        if (NT_SUCCESS(status) && transferOffset <= allocation->BackingSize &&
            transferSize <= allocation->BackingSize - transferOffset)
        {
            status = CopyAperturePlacement(allocation,
                                           transferOffset,
                                           transferSize,
                                           systemAddress,
                                           (packetFlags & VioGpuWddmPagingFlagPageIn) != 0);
        }
        transferDataComplete = NT_SUCCESS(status);
    }
    else if (status == STATUS_SUCCESS && (packetFlags & VioGpuWddmPagingFlagFill) != 0)
    {
        status = FillAperturePlacement(allocation, transferSize, fillPattern);
    }

    if (status == STATUS_SUCCESS)
    {
        status = AcquireAllocationSubmissionReference(allocation, adapter);
    }
    if (status == STATUS_SUCCESS)
    {
        PVOID dmaBuffer = pagingBuffer->pDmaBuffer;
        UINT dmaSize = pagingBuffer->DmaSize;
        PVOID privateBuffer = pagingBuffer->pDmaBufferPrivateData;
        UINT privateSize = pagingBuffer->DmaBufferPrivateDataSize;
        VIOGPU_WDDM_PAGING_DMA_PACKET *packet = static_cast<VIOGPU_WDDM_PAGING_DMA_PACKET *>(dmaBuffer);
        VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = static_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(privateBuffer);

        RtlZeroMemory(packet, sizeof(*packet));
        packet->Signature = VIOGPU_WDDM_PAGING_DMA_SIGNATURE;
        packet->Version = VioGpuWddmDmaPrivateVersion;
        packet->Size = static_cast<USHORT>(sizeof(*packet));
        packet->Operation = static_cast<UINT>(pagingBuffer->Operation);
        packet->Flags = packetFlags;
        packet->ResourceId = allocation->ResourceId;
        packet->ContextId = nativeAllocation ? allocation->ContextId : 0;
        packet->ContextGeneration = nativeAllocation ? allocation->ContextGeneration : 0;
        packet->Reserved = 0;
        packet->ResetGeneration = nativeAllocation ? allocation->ContextResetGeneration : 0;
        packet->PlacementOffset = placementOffset;
        packet->TransferOffset = transferOffset;
        packet->TransferSize = transferSize;

        RtlZeroMemory(pagingPrivate, sizeof(*pagingPrivate));
        VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = &pagingPrivate->Header;
        privateData->Signature = VIOGPU_WDDM_DMA_SIGNATURE;
        privateData->Version = VioGpuWddmDmaPrivateVersion;
        privateData->Kind = VioGpuWddmDmaKindPaging;
        privateData->DmaBuffer = dmaBuffer;
        privateData->DmaBufferSize = dmaSize;
        privateData->CommandLength = sizeof(*packet);
        privateData->ContextId = packet->ContextId;
        privateData->Generation = packet->ContextGeneration;
        privateData->ResetGeneration = packet->ResetGeneration;
        privateData->Flags = packet->Flags;
        privateData->Packet = packet;
        privateData->PacketLength = sizeof(*packet);
        privateData->Reserved = 0;
        privateData->Submission = pagingPrivate;

        VIOGPU_WDDM_PAGING_TRANSACTION *transaction = &pagingPrivate->Transaction;
        transaction->Signature = VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE;
        transaction->Adapter = adapter;
        transaction->Allocation = allocation;
        transaction->FillPattern = fillPattern;
        transaction->Operation = packet->Operation;
        transaction->Flags = packet->Flags;
        transaction->TransferOffset = transferOffset;
        transaction->TransferSize = transferSize;
        transaction->PlacementOffset = placementOffset;
        transaction->ResourceId = packet->ResourceId;
        transaction->ContextId = packet->ContextId;
        transaction->ContextGeneration = packet->ContextGeneration;
        transaction->ResetGeneration = packet->ResetGeneration;
        transaction->TransferDataComplete = transferDataComplete;
        InterlockedExchange(&transaction->ReferenceHeld, 1);
        InterlockedExchange(&transaction->ExecutionStarted, 0);
        InterlockedExchange(&transaction->CancelRequested, 0);
        InitializeListHead(&pagingPrivate->Work.Link);
        pagingPrivate->Work.Routine = NativePagingBatchWorker;
        pagingPrivate->Work.CancelRoutine = NativePagingBatchCancelled;
        pagingPrivate->Work.Context = pagingPrivate;
        pagingPrivate->Work.CancelRequested = &transaction->CancelRequested;
        KeMemoryBarrier();
        InterlockedExchange(&transaction->State, VioGpuWddmPagingTransactionBuilt);

        pagingBuffer->pDmaBuffer = static_cast<BYTE *>(dmaBuffer) + sizeof(*packet);
        pagingBuffer->DmaSize = dmaSize - sizeof(*packet);
        pagingBuffer->pDmaBufferPrivateData = static_cast<BYTE *>(privateBuffer) + sizeof(*pagingPrivate);
        pagingBuffer->DmaBufferPrivateDataSize = privateSize - sizeof(*pagingPrivate);
    }

    if (lifecycleAcquired)
    {
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
    }
    return status == STATUS_SUCCESS ? STATUS_SUCCESS : STATUS_GRAPHICS_ALLOCATION_BUSY;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmBuildPagingBuffer(CONST HANDLE hAdapter,
                                                                     DXGKARG_BUILDPAGINGBUFFER *pagingBuffer)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || pagingBuffer == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    if (pagingBuffer->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(pagingBuffer->MapApertureSegment.hAllocation);
        if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
            allocation->Adapter != adapter || pagingBuffer->MapApertureSegment.SegmentId != VIOGPU_WDDM_SEGMENT_ID ||
            (pagingBuffer->MapApertureSegment.Flags.Value & ~1U) != 0)
        {
            return STATUS_GRAPHICS_ALLOCATION_BUSY;
        }
        return MapApertureAllocation(adapter,
                                     allocation,
                                     pagingBuffer->MapApertureSegment.OffsetInPages,
                                     pagingBuffer->MapApertureSegment.NumberOfPages,
                                     pagingBuffer->MapApertureSegment.pMdl,
                                     pagingBuffer->MapApertureSegment.MdlOffset);
    }

    if (pagingBuffer->Operation == DXGK_OPERATION_UNMAP_APERTURE_SEGMENT)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(pagingBuffer->UnmapApertureSegment.hAllocation);
        if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
            allocation->Adapter != adapter || pagingBuffer->UnmapApertureSegment.SegmentId != VIOGPU_WDDM_SEGMENT_ID)
        {
            adapter->RequestHardwareResetAtAnyIrql();
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
        NTSTATUS status = UnmapApertureAllocation(adapter,
                                                  allocation,
                                                  pagingBuffer->UnmapApertureSegment.OffsetInPages,
                                                  pagingBuffer->UnmapApertureSegment.NumberOfPages,
                                                  pagingBuffer->UnmapApertureSegment.DummyPage);
        if (!NT_SUCCESS(status))
        {
            adapter->RequestHardwareResetAtAnyIrql();
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
        return STATUS_SUCCESS;
    }

    if (pagingBuffer->pDmaBuffer == NULL || pagingBuffer->pDmaBufferPrivateData == NULL ||
        pagingBuffer->DmaSize < sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET) ||
        pagingBuffer->DmaBufferPrivateDataSize < sizeof(VIOGPU_WDDM_PAGING_PRIVATE))
    {
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }

    VIOGPU_WDDM_ALLOCATION *allocation = NULL;
    LARGE_INTEGER segmentAddress = {};
    PMDL transferMdl = NULL;
    UINT mdlOffset = 0;
    SIZE_T transferOffset = 0;
    SIZE_T transferSize = 0;
    UINT fillPattern = 0;
    UINT packetFlags = VioGpuWddmPagingFlagSoftwareCompleted;
    switch (pagingBuffer->Operation)
    {
        case DXGK_OPERATION_TRANSFER:
            allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(pagingBuffer->Transfer.hAllocation);
            transferOffset = pagingBuffer->Transfer.TransferOffset;
            transferSize = pagingBuffer->Transfer.TransferSize;
            mdlOffset = pagingBuffer->Transfer.MdlOffset;
            if ((pagingBuffer->Transfer.Flags.Value & ~0x1CU) != 0)
            {
                return STATUS_GRAPHICS_ALLOCATION_BUSY;
            }
            if (pagingBuffer->Transfer.Source.SegmentId == 0 &&
                pagingBuffer->Transfer.Destination.SegmentId == VIOGPU_WDDM_SEGMENT_ID)
            {
                packetFlags |= VioGpuWddmPagingFlagPageIn;
                segmentAddress = pagingBuffer->Transfer.Destination.SegmentAddress;
                transferMdl = pagingBuffer->Transfer.Source.pMdl;
            }
            else if (pagingBuffer->Transfer.Source.SegmentId == VIOGPU_WDDM_SEGMENT_ID &&
                     pagingBuffer->Transfer.Destination.SegmentId == 0)
            {
                packetFlags |= VioGpuWddmPagingFlagPageOut;
                segmentAddress = pagingBuffer->Transfer.Source.SegmentAddress;
                transferMdl = pagingBuffer->Transfer.Destination.pMdl;
            }
            else
            {
                return STATUS_GRAPHICS_ALLOCATION_BUSY;
            }
            if (pagingBuffer->Transfer.Flags.TransferStart)
            {
                packetFlags |= VioGpuWddmPagingFlagTransferStart;
            }
            if (pagingBuffer->Transfer.Flags.TransferEnd)
            {
                packetFlags |= VioGpuWddmPagingFlagTransferEnd;
            }
            if (pagingBuffer->Transfer.Flags.AllocationIsIdle)
            {
                packetFlags |= VioGpuWddmPagingFlagAllocationIdle;
            }
            break;

        case DXGK_OPERATION_FILL:
            allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(pagingBuffer->Fill.hAllocation);
            if (pagingBuffer->Fill.Destination.SegmentId != VIOGPU_WDDM_SEGMENT_ID)
            {
                return STATUS_GRAPHICS_ALLOCATION_BUSY;
            }
            segmentAddress = pagingBuffer->Fill.Destination.SegmentAddress;
            transferSize = pagingBuffer->Fill.FillSize;
            fillPattern = pagingBuffer->Fill.FillPattern;
            packetFlags |= VioGpuWddmPagingFlagFill;
            break;

        case DXGK_OPERATION_DISCARD_CONTENT:
            allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(pagingBuffer->DiscardContent.hAllocation);
            if ((pagingBuffer->DiscardContent.Flags.Value & ~1U) != 0)
            {
                return STATUS_GRAPHICS_ALLOCATION_BUSY;
            }
            if (pagingBuffer->DiscardContent.SegmentId != VIOGPU_WDDM_SEGMENT_ID)
            {
                return STATUS_SUCCESS;
            }
            segmentAddress = pagingBuffer->DiscardContent.SegmentAddress;
            packetFlags |= VioGpuWddmPagingFlagDiscard;
            if (pagingBuffer->DiscardContent.Flags.AllocationIsIdle)
            {
                packetFlags |= VioGpuWddmPagingFlagAllocationIdle;
            }
            break;

        default:
            return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
        allocation->Adapter != adapter)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }
    return BuildSoftwarePagingTransaction(adapter,
                                          pagingBuffer,
                                          allocation,
                                          segmentAddress,
                                          transferMdl,
                                          mdlOffset,
                                          transferOffset,
                                          transferSize,
                                          fillPattern,
                                          packetFlags);
}

BOOLEAN ResolvePagingBatchOffset(_In_ UINT base,
                                 _In_ UINT index,
                                 _In_ SIZE_T stride,
                                 _In_ UINT limit,
                                 _Out_ UINT *offset)
{
    if (offset == NULL || stride == 0)
    {
        return FALSE;
    }
    ULONGLONG candidate = static_cast<ULONGLONG>(base) + static_cast<ULONGLONG>(index) * static_cast<ULONGLONG>(stride);
    if (candidate > static_cast<ULONGLONG>(limit) || candidate > MAXUINT)
    {
        return FALSE;
    }
    *offset = static_cast<UINT>(candidate);
    return TRUE;
}

BOOLEAN ResolvePagingBatch(_In_opt_ PVOID dmaBuffer,
                           _In_ UINT dmaBufferSize,
                           _In_ UINT dmaStart,
                           _In_ UINT dmaEnd,
                           _In_ PVOID privateBuffer,
                           _In_ UINT privateBufferSize,
                           _In_ UINT privateStart,
                           _In_ UINT privateEnd,
                           _In_ VioGpuDod *adapter,
                           _In_ VIOGPU_WDDM_PAGING_TRANSACTION_STATE expectedState,
                           _Out_ VIOGPU_WDDM_PAGING_PRIVATE **firstPrivate,
                           _Out_ UINT *recordCount)
{
    if (privateBuffer == NULL || adapter == NULL || firstPrivate == NULL || recordCount == NULL || dmaStart >= dmaEnd ||
        dmaEnd > dmaBufferSize || privateStart >= privateEnd || privateEnd > privateBufferSize)
    {
        return FALSE;
    }
    *firstPrivate = NULL;
    *recordCount = 0;

    UINT dmaLength = dmaEnd - dmaStart;
    UINT privateLength = privateEnd - privateStart;
    if ((dmaLength % sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET)) != 0 ||
        (privateLength % sizeof(VIOGPU_WDDM_PAGING_PRIVATE)) != 0)
    {
        return FALSE;
    }
    UINT count = dmaLength / sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET);
    if (count == 0 || count != privateLength / sizeof(VIOGPU_WDDM_PAGING_PRIVATE))
    {
        return FALSE;
    }
    /* Publish the shape before deep validation so callers can retire every
     * record whose KMD ownership identity remains recognizable. */
    *recordCount = count;

    for (UINT index = 0; index < count; ++index)
    {
        UINT packetOffset = 0;
        UINT recordOffset = 0;
        if (!ResolvePagingBatchOffset(dmaStart,
                                      index,
                                      sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET),
                                      dmaEnd - sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET),
                                      &packetOffset) ||
            !ResolvePagingBatchOffset(privateStart,
                                      index,
                                      sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                      privateEnd - sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                      &recordOffset))
        {
            return FALSE;
        }
        VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(privateBuffer) +
                                                                                                   recordOffset);
        VIOGPU_WDDM_KMD_DMA_PRIVATE *header = &pagingPrivate->Header;
        VIOGPU_WDDM_PAGING_DMA_PACKET *packet = dmaBuffer == NULL ? static_cast<VIOGPU_WDDM_PAGING_DMA_PACKET *>(header->Packet)
                                                                  : reinterpret_cast<VIOGPU_WDDM_PAGING_DMA_PACKET *>(static_cast<BYTE *>(dmaBuffer) +
                                                                                                                      packetOffset);
        VIOGPU_WDDM_PAGING_TRANSACTION *transaction = &pagingPrivate->Transaction;
        BOOLEAN packetValid = ValidatePagingDmaPacket(header, packet);
        LONG transactionState = InterlockedCompareExchange(&transaction->State, 0, 0);
        BOOLEAN stateMatches = expectedState == VioGpuWddmPagingTransactionAny ? transactionState != VioGpuWddmPagingTransactionInvalid
                                                                               : transactionState == expectedState;
        BOOLEAN referenceValid = (transactionState == VioGpuWddmPagingTransactionBuilt ||
                                  transactionState == VioGpuWddmPagingTransactionQueued ||
                                  transactionState == VioGpuWddmPagingTransactionExecuting)
                                                                                                                                     ? InterlockedCompareExchange(&transaction->ReferenceHeld,
                                                                                                                                                                  0,
                                                                                                                                                                  0) == 1
                                                                                                                                     : TRUE;
        BOOLEAN workLinkAvailable = pagingPrivate->Work.Link.Flink == &pagingPrivate->Work.Link &&
                                    pagingPrivate->Work.Link.Blink == &pagingPrivate->Work.Link;
        if (header->DmaBuffer != packet || header->DmaBufferSize != dmaBufferSize - packetOffset ||
            header->Submission != pagingPrivate || transaction->Adapter != adapter || !stateMatches ||
            !referenceValid || pagingPrivate->Work.Routine != NativePagingBatchWorker ||
            pagingPrivate->Work.Context != pagingPrivate ||
            (expectedState != VioGpuWddmPagingTransactionAny && !workLinkAvailable) || !packetValid)
        {
            return FALSE;
        }
        if (index == 0)
        {
            *firstPrivate = pagingPrivate;
        }
    }
    return TRUE;
}

BOOLEAN CancelPagingTransaction(_Inout_ VIOGPU_WDDM_PAGING_TRANSACTION *transaction)
{
    if (transaction == NULL || transaction->Signature != VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE)
    {
        return FALSE;
    }

    LONG state = InterlockedCompareExchange(&transaction->State, 0, 0);
    for (;;)
    {
        if (state == VioGpuWddmPagingTransactionExecuting)
        {
            InterlockedExchange(&transaction->CancelRequested, 1);
            return TRUE;
        }
        if (state != VioGpuWddmPagingTransactionBuilt && state != VioGpuWddmPagingTransactionQueued)
        {
            return state == VioGpuWddmPagingTransactionCancelled || state == VioGpuWddmPagingTransactionFinished;
        }
        LONG observed = InterlockedCompareExchange(&transaction->State, VioGpuWddmPagingTransactionCancelled, state);
        if (observed == state)
        {
            ReleasePagingTransactionReference(transaction);
            return TRUE;
        }
        state = observed;
    }
}

BOOLEAN IsRecognizedPagingOwner(_In_ const VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate, _In_ VioGpuDod *adapter)
{
    return pagingPrivate != NULL && adapter != NULL && pagingPrivate->Header.Signature == VIOGPU_WDDM_DMA_SIGNATURE &&
           pagingPrivate->Header.Version == VioGpuWddmDmaPrivateVersion &&
           pagingPrivate->Header.Kind == VioGpuWddmDmaKindPaging && pagingPrivate->Header.Submission == pagingPrivate &&
           pagingPrivate->Transaction.Signature == VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE &&
           pagingPrivate->Transaction.Adapter == adapter && pagingPrivate->Work.Routine == NativePagingBatchWorker &&
           pagingPrivate->Work.Context == pagingPrivate;
}

BOOLEAN CancelRecognizedPagingTransaction(_Inout_ VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate, _In_ VioGpuDod *adapter)
{
    return IsRecognizedPagingOwner(pagingPrivate, adapter) && CancelPagingTransaction(&pagingPrivate->Transaction);
}

_Use_decl_annotations_ VOID NativePagingBatchCancelled(PVOID callbackContext)
{
    VIOGPU_WDDM_PAGING_PRIVATE *first = static_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(callbackContext);
    if (first == NULL || first->Transaction.Adapter == NULL || first->BatchPrivateData == NULL ||
        first->BatchPrivateStart >= first->BatchPrivateEnd || first->BatchPrivateEnd > first->BatchPrivateDataSize ||
        first->BatchPrivateEnd - first->BatchPrivateStart < sizeof(VIOGPU_WDDM_PAGING_PRIVATE) ||
        ((first->BatchPrivateEnd - first->BatchPrivateStart) % sizeof(VIOGPU_WDDM_PAGING_PRIVATE)) != 0)
    {
        return;
    }

    VioGpuDod *adapter = first->Transaction.Adapter;
    PVOID privateBuffer = first->BatchPrivateData;
    UINT privateStart = first->BatchPrivateStart;
    UINT privateEnd = first->BatchPrivateEnd;
    UINT count = (privateEnd - privateStart) / sizeof(VIOGPU_WDDM_PAGING_PRIVATE);
    UINT recordLimit = privateEnd - sizeof(VIOGPU_WDDM_PAGING_PRIVATE);
    for (UINT index = 0; index < count; ++index)
    {
        UINT recordOffset = 0;
        if (!ResolvePagingBatchOffset(privateStart,
                                      index,
                                      sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                      recordLimit,
                                      &recordOffset))
        {
            break;
        }
        VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(privateBuffer) +
                                                                                                   recordOffset);
        CancelRecognizedPagingTransaction(pagingPrivate, adapter);
        ReleasePagingTransactionReference(&pagingPrivate->Transaction);
    }

    first->BatchPrivateData = NULL;
    first->BatchPrivateDataSize = 0;
    first->BatchPrivateStart = 0;
    first->BatchPrivateEnd = 0;
    first->BatchFenceId = 0;
    first->BatchNodeOrdinal = 0;
    first->BatchEngineOrdinal = 0;
}

_Use_decl_annotations_ VOID NativePagingBatchWorker(PVOID callbackContext)
{
    VIOGPU_WDDM_PAGING_PRIVATE *first = static_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(callbackContext);
    if (first == NULL)
    {
        return;
    }
    VioGpuDod *adapter = first->Transaction.Adapter;
    PVOID privateBuffer = first->BatchPrivateData;
    UINT privateBufferSize = first->BatchPrivateDataSize;
    UINT privateStart = first->BatchPrivateStart;
    UINT privateEnd = first->BatchPrivateEnd;
    UINT fenceId = first->BatchFenceId;
    UINT nodeOrdinal = first->BatchNodeOrdinal;
    UINT engineOrdinal = first->BatchEngineOrdinal;
    NTSTATUS status = STATUS_SUCCESS;
    UINT count = 0;
    BOOLEAN operationAcquired = adapter != NULL && adapter->AcquireNativeSubmissionOperation();

    BOOLEAN rangeValid = privateBuffer != NULL && privateStart < privateEnd && privateEnd <= privateBufferSize &&
                         privateEnd - privateStart >= sizeof(VIOGPU_WDDM_PAGING_PRIVATE) &&
                         ((privateEnd - privateStart) % sizeof(VIOGPU_WDDM_PAGING_PRIVATE)) == 0;
    if (rangeValid)
    {
        count = (privateEnd - privateStart) / sizeof(VIOGPU_WDDM_PAGING_PRIVATE);
    }
    if (adapter == NULL || !operationAcquired || !rangeValid ||
        static_cast<BYTE *>(privateBuffer) + privateStart != reinterpret_cast<BYTE *>(first) || fenceId == 0 ||
        nodeOrdinal != 0 || engineOrdinal != 0)
    {
        status = STATUS_INVALID_PARAMETER;
    }

    UINT recordLimit = privateEnd >= sizeof(VIOGPU_WDDM_PAGING_PRIVATE) ? privateEnd - sizeof(VIOGPU_WDDM_PAGING_PRIVATE)
                                                                        : 0;
    for (UINT index = 0; NT_SUCCESS(status) && index < count; ++index)
    {
        UINT recordOffset = 0;
        if (!ResolvePagingBatchOffset(privateStart,
                                      index,
                                      sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                      recordLimit,
                                      &recordOffset))
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(privateBuffer) +
                                                                                                   recordOffset);
        BOOLEAN packetValid = ValidatePagingDmaPacket(&pagingPrivate->Header,
                                                      static_cast<VIOGPU_WDDM_PAGING_DMA_PACKET *>(pagingPrivate->Header.Packet));
        if (pagingPrivate->Transaction.Adapter != adapter ||
            InterlockedCompareExchange(&pagingPrivate->Transaction.State, 0, 0) != VioGpuWddmPagingTransactionQueued ||
            !packetValid)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (InterlockedCompareExchange(&pagingPrivate->Transaction.State,
                                       VioGpuWddmPagingTransactionExecuting,
                                       VioGpuWddmPagingTransactionQueued) != VioGpuWddmPagingTransactionQueued)
        {
            status = STATUS_CANCELLED;
            break;
        }
        if (InterlockedCompareExchange(&pagingPrivate->Transaction.CancelRequested, 0, 0) != 0)
        {
            status = STATUS_CANCELLED;
            FinishPagingTransaction(&pagingPrivate->Transaction,
                                    VioGpuWddmPagingTransactionExecuting,
                                    VioGpuWddmPagingTransactionCancelled);
            break;
        }
        InterlockedExchange(&pagingPrivate->Transaction.ExecutionStarted, 1);
        status = ExecutePagingTransaction(&pagingPrivate->Transaction);
        BOOLEAN cancelRequested = InterlockedCompareExchange(&pagingPrivate->Transaction.CancelRequested, 0, 0) != 0;
        if (!NT_SUCCESS(status) || cancelRequested)
        {
            if (NT_SUCCESS(status))
            {
                status = STATUS_CANCELLED;
            }
            FinishPagingTransaction(&pagingPrivate->Transaction,
                                    VioGpuWddmPagingTransactionExecuting,
                                    VioGpuWddmPagingTransactionCancelled);
            break;
        }
        if (!FinishPagingTransaction(&pagingPrivate->Transaction,
                                     VioGpuWddmPagingTransactionExecuting,
                                     VioGpuWddmPagingTransactionFinished))
        {
            status = STATUS_INVALID_DEVICE_STATE;
            break;
        }
    }

    for (UINT index = 0; index < count; ++index)
    {
        UINT recordOffset = 0;
        if (!ResolvePagingBatchOffset(privateStart,
                                      index,
                                      sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                      recordLimit,
                                      &recordOffset))
        {
            status = STATUS_INVALID_DEVICE_STATE;
            continue;
        }
        VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(privateBuffer) +
                                                                                                   recordOffset);
        LONG transactionState = InterlockedCompareExchange(&pagingPrivate->Transaction.State, 0, 0);
        if (transactionState == VioGpuWddmPagingTransactionBuilt ||
            transactionState == VioGpuWddmPagingTransactionQueued)
        {
            CancelPagingTransaction(&pagingPrivate->Transaction);
        }
    }

    for (UINT index = 0; index < count; ++index)
    {
        UINT recordOffset = 0;
        if (!ResolvePagingBatchOffset(privateStart,
                                      index,
                                      sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                      recordLimit,
                                      &recordOffset))
        {
            status = STATUS_INVALID_DEVICE_STATE;
            continue;
        }
        VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(privateBuffer) +
                                                                                                   recordOffset);
        ReleasePagingTransactionReference(&pagingPrivate->Transaction);
    }

    first->BatchPrivateData = NULL;
    first->BatchPrivateDataSize = 0;
    first->BatchPrivateStart = 0;
    first->BatchPrivateEnd = 0;
    first->BatchFenceId = 0;
    first->BatchNodeOrdinal = 0;
    first->BatchEngineOrdinal = 0;
    KeMemoryBarrier();

    if (adapter != NULL)
    {
        BOOLEAN completed = adapter->CompleteNativeSystemSubmission(fenceId, nodeOrdinal, engineOrdinal);
        if (!NT_SUCCESS(status) || !completed)
        {
            adapter->RequestHardwareResetAtAnyIrql();
        }
        adapter->CompleteNativePassiveWork(&first->Work);
        if (operationAcquired)
        {
            adapter->ReleaseNativeSubmissionOperation();
        }
    }
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmRender(CONST HANDLE hContext, DXGKARG_RENDER *render)
{
    VIOGPU_WDDM_CONTEXT *context = reinterpret_cast<VIOGPU_WDDM_CONTEXT *>(hContext);
    if (context == NULL || render == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL || render->pCommand == NULL ||
        render->CommandLength < sizeof(VIOGPU_WDDM_RENDER_COMMAND) ||
        render->CommandLength > VIOGPU_WDDM_DMA_BUFFER_SIZE || render->pDmaBuffer == NULL ||
        render->pDmaBufferPrivateData == NULL ||
        render->DmaBufferPrivateDataSize < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) || render->MultipassOffset != 0 ||
        render->DmaBufferSegmentId != 0 || render->AllocationListSize == 0 ||
        render->AllocationListSize > VIOGPU_WDDM_ALLOCATION_LIST_SIZE || render->pAllocationList == NULL ||
        render->PatchLocationListInSize == 0 || render->PatchLocationListInSize > VIOGPU_WDDM_PATCH_LIST_SIZE ||
        (render->PatchLocationListInSize != 0 && render->pPatchLocationListIn == NULL) ||
        (render->PatchLocationListInSize != 0 && render->pPatchLocationListOut == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (!ExAcquireRundownProtection(&context->Operations))
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE || context->Type != VioGpuWddmContextNative)
    {
        ExReleaseRundownProtection(&context->Operations);
        return STATUS_INVALID_HANDLE;
    }
    if (context->Device == NULL || context->Device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE ||
        context->Device->Adapter == NULL)
    {
        ExReleaseRundownProtection(&context->Operations);
        return STATUS_DEVICE_NOT_READY;
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
    const VIOGPU_WDDM_RENDER_COMMAND *validatedCommand = NULL;
    VIOGPU_WDDM_SUBMISSION *submission = NULL;
    PGPU_VBUFFER virtioBuffer = NULL;
    UINT allocationSubmissionReferences = 0;
    BOOLEAN contextSubmissionReference = FALSE;
    BOOLEAN submissionPublished = FALSE;
    BOOLEAN fullyPrepatched = FALSE;
    BOOLEAN hardwareOperation = context->Device->Adapter->AcquireNativeSubmissionOperation();
    SIZE_T patchBytes = (SIZE_T)render->PatchLocationListInSize * sizeof(*patchSnapshot);

    if (!hardwareOperation)
    {
        status = STATUS_DEVICE_NOT_READY;
    }
    if (NT_SUCCESS(status) &&
        (render->DmaSize < render->CommandLength || render->PatchLocationListOutSize < render->PatchLocationListInSize))
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
                                           &snapshot);
            if (NT_SUCCESS(status))
            {
                status = ValidateNativeSubmitPacket(command,
                                                    context->Device,
                                                    render->pAllocationList,
                                                    render->AllocationListSize,
                                                    &snapshot);
            }
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
        validatedCommand = reinterpret_cast<const VIOGPU_WDDM_RENDER_COMMAND *>(commandSnapshot);
        status = AcquireContextSubmissionReference(context);
        contextSubmissionReference = NT_SUCCESS(status);
    }
    if (NT_SUCCESS(status))
    {
        __try
        {
            status = AcquireRenderAllocationReferences(validatedCommand,
                                                       context->Device,
                                                       render->pAllocationList,
                                                       render->AllocationListSize,
                                                       &allocationSubmissionReferences);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            status = STATUS_INVALID_USER_BUFFER;
        }
    }

    if (NT_SUCCESS(status))
    {
        status = ApplyRenderPrepatches(reinterpret_cast<VIOGPU_WDDM_RENDER_COMMAND *>(commandSnapshot),
                                       context->Device,
                                       render->pAllocationList,
                                       render->AllocationListSize,
                                       &snapshot,
                                       &fullyPrepatched);
    }

    if (NT_SUCCESS(status))
    {
        const BYTE *commandStream = reinterpret_cast<const BYTE *>(validatedCommand) +
                                    validatedCommand->CommandStreamOffset;
        virtioBuffer = context->Device->Adapter->PrepareNativeSubmit(snapshot.ContextId,
                                                                     commandStream,
                                                                     validatedCommand->CommandStreamSize);
        if (virtioBuffer == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (NT_SUCCESS(status))
    {
        submission = new (NonPagedPoolNx) VIOGPU_WDDM_SUBMISSION;
        if (submission == NULL)
        {
            status = STATUS_NO_MEMORY;
        }
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
        privateData->Version = VioGpuWddmDmaPrivateVersion;
        privateData->Kind = VioGpuWddmDmaKindRender;
        privateData->DmaBuffer = dmaBuffer;
        privateData->DmaBufferSize = render->DmaSize;
        privateData->CommandLength = render->CommandLength;
        privateData->ContextId = snapshot.ContextId;
        privateData->Generation = snapshot.Generation;
        privateData->ResetGeneration = snapshot.ResetGeneration;
        privateData->Packet = dmaBuffer;
        privateData->PacketLength = render->CommandLength;
        privateData->Reserved = 0;
        privateData->Submission = NULL;

        status = PublishPreparedSubmission(submission,
                                           context,
                                           validatedCommand,
                                           context->Device,
                                           render->pAllocationList,
                                           render->AllocationListSize,
                                           allocationSubmissionReferences,
                                           dmaBuffer,
                                           render->DmaSize,
                                           privateData,
                                           sizeof(*privateData),
                                           render->CommandLength,
                                           virtioBuffer,
                                           fullyPrepatched,
                                           &snapshot);
        if (NT_SUCCESS(status))
        {
            submissionPublished = TRUE;

            render->pDmaBuffer = static_cast<BYTE *>(dmaBuffer) + render->CommandLength;
            render->pDmaBufferPrivateData = static_cast<BYTE *>(render->pDmaBufferPrivateData) + sizeof(*privateData);
            render->DmaBufferPrivateDataSize -= sizeof(*privateData);
            render->pPatchLocationListOut = patchOutput + render->PatchLocationListInSize;
            render->MultipassOffset = render->CommandLength;

            /* Ownership moves to the submission record until Host retirement
             * or reset cancellation. */
            submission = NULL;
            virtioBuffer = NULL;
            allocationSubmissionReferences = 0;
            contextSubmissionReference = FALSE;
            submissionPublished = FALSE;
        }
    }

    if (submissionPublished && submission != NULL)
    {
        ReleasePreparedSubmission(submission);
        submission = NULL;
        virtioBuffer = NULL;
        allocationSubmissionReferences = 0;
        contextSubmissionReference = FALSE;
    }
    else if (submission != NULL)
    {
        submission->Signature = 0;
        delete submission;
        submission = NULL;
    }
    if (virtioBuffer != NULL)
    {
        context->Device->Adapter->ReleaseNativeSubmitBuffer(virtioBuffer);
    }
    if (allocationSubmissionReferences != 0)
    {
        ReleaseRenderAllocationReferences(validatedCommand,
                                          render->pAllocationList,
                                          render->AllocationListSize,
                                          allocationSubmissionReferences);
    }
    if (contextSubmissionReference)
    {
        ReleaseContextSubmissionReference(context);
    }
    delete[] patchSnapshot;
    delete[] commandSnapshot;
    VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
    if (hardwareOperation)
    {
        context->Device->Adapter->ReleaseNativeSubmissionOperation();
    }
    ExReleaseRundownProtection(&context->Operations);
    return status;
}

VOID RetireDmaOwner(_In_ VioGpuDod *adapter,
                    _In_opt_ PVOID dmaBuffer,
                    _In_ UINT dmaBufferSize,
                    _In_ UINT dmaStart,
                    _In_ UINT dmaEnd,
                    _In_ PVOID privateBuffer,
                    _In_ UINT privateBufferSize,
                    _In_ UINT privateStart,
                    _In_ UINT privateEnd,
                    _In_opt_ HANDLE runtimeContext,
                    _In_ LONG expectedPresentState)
{
    if (adapter == NULL || privateBuffer == NULL || dmaStart > dmaEnd || dmaEnd > dmaBufferSize ||
        privateStart > privateEnd || privateEnd > privateBufferSize ||
        privateEnd - privateStart < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE))
    {
        return;
    }

    VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = reinterpret_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(static_cast<BYTE *>(privateBuffer) +
                                                                                               privateStart);
    if (privateData->Kind == VioGpuWddmDmaKindPaging)
    {
        VIOGPU_WDDM_PAGING_PRIVATE *first = NULL;
        UINT count = 0;
        BOOLEAN resolved = ResolvePagingBatch(dmaBuffer,
                                              dmaBufferSize,
                                              dmaStart,
                                              dmaEnd,
                                              privateBuffer,
                                              privateBufferSize,
                                              privateStart,
                                              privateEnd,
                                              adapter,
                                              VioGpuWddmPagingTransactionBuilt,
                                              &first,
                                              &count);
        UNREFERENCED_PARAMETER(first);
        if (!resolved && count == 0)
        {
            adapter->RequestHardwareResetAtAnyIrql();
        }
        if (count != 0)
        {
            UINT recordLimit = privateEnd - sizeof(VIOGPU_WDDM_PAGING_PRIVATE);
            for (UINT index = 0; index < count; ++index)
            {
                UINT recordOffset = 0;
                if (!ResolvePagingBatchOffset(privateStart,
                                              index,
                                              sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                              recordLimit,
                                              &recordOffset))
                {
                    adapter->RequestHardwareResetAtAnyIrql();
                    break;
                }
                VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(privateBuffer) +
                                                                                                           recordOffset);
                BOOLEAN cancelled = CancelRecognizedPagingTransaction(pagingPrivate, adapter);
                if (!cancelled)
                {
                    adapter->RequestHardwareResetAtAnyIrql();
                }
            }
        }
        return;
    }

    if (privateEnd - privateStart != sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) || runtimeContext == NULL)
    {
        return;
    }
    if (privateData->Kind == VioGpuWddmDmaKindPresent)
    {
        VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = NULL;
        if (NT_SUCCESS(ResolvePresentTransaction(privateBuffer,
                                                 privateBufferSize,
                                                 privateStart,
                                                 privateEnd,
                                                 adapter,
                                                 runtimeContext,
                                                 expectedPresentState,
                                                 &transaction)))
        {
            LONG state = InterlockedCompareExchange(&transaction->State, 0, 0);
            BOOLEAN expected = expectedPresentState < 0 ? state == VioGpuWddmPresentBuilt || state == VioGpuWddmPresentPatched
                                                        : state == expectedPresentState;
            BOOLEAN dmaRangeValid = dmaBuffer == NULL ? ValidatePresentSubmitDmaRange(transaction,
                                                                                      dmaBufferSize,
                                                                                      dmaStart,
                                                                                      dmaEnd)
                                                      : ValidatePresentDmaSubmissionRange(transaction,
                                                                                          dmaBuffer,
                                                                                          dmaBufferSize,
                                                                                          dmaStart,
                                                                                          dmaEnd);
            if (expected && transaction->PrivateDataSize == privateEnd - privateStart && dmaRangeValid)
            {
                BOOLEAN retired = RetirePresentTransaction(transaction, state, VioGpuWddmPresentCancelled);
                if (!retired)
                {
                    adapter->RequestHardwareResetAtAnyIrql();
                }
            }
            else
            {
                adapter->RequestHardwareResetAtAnyIrql();
            }
            DereferencePresentTransaction(transaction);
        }
        else
        {
            adapter->RequestHardwareResetAtAnyIrql();
        }
        return;
    }
    if (privateData->Kind == VioGpuWddmDmaKindRender)
    {
        VIOGPU_WDDM_SUBMISSION *submission = NULL;
        if (NT_SUCCESS(ResolveSubmissionPrivateData(privateBuffer,
                                                    privateBufferSize,
                                                    privateStart,
                                                    privateEnd,
                                                    adapter,
                                                    runtimeContext,
                                                    &submission)))
        {
            LONG state = InterlockedCompareExchange(&submission->State, 0, 0);
            BOOLEAN dmaRangeValid = dmaBuffer == NULL ? ValidateRenderSubmitDmaRange(submission,
                                                                                     dmaBufferSize,
                                                                                     dmaStart,
                                                                                     dmaEnd)
                                                      : ValidateRenderDmaSubmissionRange(submission,
                                                                                         dmaBuffer,
                                                                                         dmaBufferSize,
                                                                                         dmaStart,
                                                                                         dmaEnd);
            BOOLEAN exact = dmaRangeValid && submission->DmaPrivateDataSize == privateEnd - privateStart &&
                            (state == VioGpuWddmSubmissionPrepared || state == VioGpuWddmSubmissionPatched);
            if (exact)
            {
                BOOLEAN quarantined = QuarantineSubmission(submission, state, TRUE);
                if (!quarantined)
                {
                    adapter->RequestHardwareResetAtAnyIrql();
                }
            }
            else if (state == VioGpuWddmSubmissionPatching || state == VioGpuWddmSubmissionSubmitClaimed)
            {
                InterlockedExchange(&submission->CancelRequested, 1);
                adapter->RequestHardwareResetAtAnyIrql();
            }
            else
            {
                adapter->RequestHardwareResetAtAnyIrql();
            }
            DereferenceRenderSubmission(submission);
        }
        else
        {
            adapter->RequestHardwareResetAtAnyIrql();
        }
    }
}

VOID RetirePatchDmaOwner(_In_ VioGpuDod *adapter, _In_ const DXGKARG_PATCH *patchArguments)
{
    HANDLE runtimeContext = patchArguments->Flags.Paging ? NULL : patchArguments->hContext;
    RetireDmaOwner(adapter,
                   patchArguments->pDmaBuffer,
                   patchArguments->DmaBufferSize,
                   patchArguments->DmaBufferSubmissionStartOffset,
                   patchArguments->DmaBufferSubmissionEndOffset,
                   patchArguments->pDmaBufferPrivateData,
                   patchArguments->DmaBufferPrivateDataSize,
                   patchArguments->DmaBufferPrivateDataSubmissionStartOffset,
                   patchArguments->DmaBufferPrivateDataSubmissionEndOffset,
                   runtimeContext,
                   VioGpuWddmPresentBuilt);
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmPatch(CONST HANDLE hAdapter, CONST DXGKARG_PATCH *patchArguments)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    BOOLEAN ownerRangeValid = patchArguments != NULL && patchArguments->pDmaBufferPrivateData != NULL &&
                              patchArguments->DmaBufferSubmissionStartOffset <= patchArguments->DmaBufferSubmissionEndOffset &&
                              patchArguments->DmaBufferSubmissionEndOffset <= patchArguments->DmaBufferSize &&
                              patchArguments->DmaBufferPrivateDataSubmissionStartOffset <= patchArguments->DmaBufferPrivateDataSubmissionEndOffset &&
                              patchArguments->DmaBufferPrivateDataSubmissionEndOffset <= patchArguments->DmaBufferPrivateDataSize;
    if (adapter == NULL || patchArguments == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        patchArguments->pDmaBuffer == NULL || patchArguments->pDmaBufferPrivateData == NULL ||
        patchArguments->DmaBufferSegmentId != 0 || patchArguments->EngineOrdinal != 0 ||
        patchArguments->SubmissionFenceId == 0 || patchArguments->SubmissionFenceId > MAXUINT ||
        patchArguments->DmaBufferSubmissionStartOffset > patchArguments->DmaBufferSubmissionEndOffset ||
        patchArguments->DmaBufferSubmissionEndOffset > patchArguments->DmaBufferSize ||
        patchArguments->DmaBufferPrivateDataSubmissionStartOffset > patchArguments->DmaBufferPrivateDataSubmissionEndOffset ||
        patchArguments->DmaBufferPrivateDataSubmissionEndOffset > patchArguments->DmaBufferPrivateDataSize ||
        patchArguments->PatchLocationListSubmissionStart > patchArguments->PatchLocationListSize ||
        patchArguments->PatchLocationListSubmissionLength > patchArguments->PatchLocationListSize - patchArguments->PatchLocationListSubmissionStart)
    {
        if (adapter != NULL && patchArguments != NULL && KeGetCurrentIrql() == PASSIVE_LEVEL && ownerRangeValid)
        {
            RetirePatchDmaOwner(adapter, patchArguments);
        }
        return STATUS_SUCCESS;
    }

    if (patchArguments->Flags.Value == 1)
    {
        VIOGPU_WDDM_DEVICE *device = reinterpret_cast<VIOGPU_WDDM_DEVICE *>(patchArguments->hDevice);
        BOOLEAN deviceReferenced = ReferenceDevice(device);
        BOOLEAN deviceValid = deviceReferenced && device->Adapter == adapter;
        BOOLEAN emptyDmaRange = patchArguments->DmaBufferSubmissionStartOffset ==
                                patchArguments->DmaBufferSubmissionEndOffset;
        BOOLEAN emptyPrivateRange = patchArguments->DmaBufferPrivateDataSubmissionStartOffset ==
                                    patchArguments->DmaBufferPrivateDataSubmissionEndOffset;
        BOOLEAN emptySubmission = emptyDmaRange && emptyPrivateRange;
        VIOGPU_WDDM_PAGING_PRIVATE *firstPrivate = NULL;
        UINT recordCount = 0;
        BOOLEAN exact = deviceValid && patchArguments->pAllocationList == NULL &&
                        patchArguments->AllocationListSize == 0 && patchArguments->pPatchLocationList == NULL &&
                        patchArguments->PatchLocationListSize == 0 &&
                        patchArguments->PatchLocationListSubmissionStart == 0 &&
                        patchArguments->PatchLocationListSubmissionLength == 0 &&
                        (emptySubmission ||
                         ResolvePagingBatch(patchArguments->pDmaBuffer,
                                            patchArguments->DmaBufferSize,
                                            patchArguments->DmaBufferSubmissionStartOffset,
                                            patchArguments->DmaBufferSubmissionEndOffset,
                                            patchArguments->pDmaBufferPrivateData,
                                            patchArguments->DmaBufferPrivateDataSize,
                                            patchArguments->DmaBufferPrivateDataSubmissionStartOffset,
                                            patchArguments->DmaBufferPrivateDataSubmissionEndOffset,
                                            adapter,
                                            VioGpuWddmPagingTransactionBuilt,
                                            &firstPrivate,
                                            &recordCount));
        if (deviceReferenced)
        {
            DereferenceDevice(device);
        }
        UNREFERENCED_PARAMETER(firstPrivate);
        UNREFERENCED_PARAMETER(recordCount);
        if (!exact)
        {
            RetirePatchDmaOwner(adapter, patchArguments);
        }
        return STATUS_SUCCESS;
    }

    if (patchArguments->hContext == NULL || patchArguments->pAllocationList == NULL ||
        patchArguments->pPatchLocationList == NULL || patchArguments->Flags.Value != 0)
    {
        RetirePatchDmaOwner(adapter, patchArguments);
        return STATUS_SUCCESS;
    }

    if (!adapter->AcquireNativeSubmissionOperation())
    {
        RetirePatchDmaOwner(adapter, patchArguments);
        return STATUS_SUCCESS;
    }

    UINT candidatePrivateLength = patchArguments->DmaBufferPrivateDataSubmissionEndOffset -
                                  patchArguments->DmaBufferPrivateDataSubmissionStartOffset;
    VIOGPU_WDDM_KMD_DMA_PRIVATE *candidatePrivate = candidatePrivateLength < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) ? NULL
                                                                                                                 : reinterpret_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(static_cast<BYTE *>(patchArguments->pDmaBufferPrivateData) +
                                                                                                                                                                   patchArguments->DmaBufferPrivateDataSubmissionStartOffset);
    if (candidatePrivate != NULL && candidatePrivate->Kind == VioGpuWddmDmaKindPresent)
    {
        VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = NULL;
        NTSTATUS status = ResolvePresentTransaction(patchArguments->pDmaBufferPrivateData,
                                                    patchArguments->DmaBufferPrivateDataSize,
                                                    patchArguments->DmaBufferPrivateDataSubmissionStartOffset,
                                                    patchArguments->DmaBufferPrivateDataSubmissionEndOffset,
                                                    adapter,
                                                    patchArguments->hContext,
                                                    VioGpuWddmPresentBuilt,
                                                    &transaction);
        UINT dmaLength = patchArguments->DmaBufferSubmissionEndOffset - patchArguments->DmaBufferSubmissionStartOffset;
        if (NT_SUCCESS(status) &&
            (patchArguments->Flags.Value != 0 || dmaLength != sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET) ||
             patchArguments->DmaBufferPrivateDataSubmissionEndOffset - patchArguments->DmaBufferPrivateDataSubmissionStartOffset !=
                                                                                                                 sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) ||
             patchArguments->PatchLocationListSubmissionLength != 2 ||
             transaction->SourceAllocationIndex >= patchArguments->AllocationListSize ||
             transaction->DestinationAllocationIndex >= patchArguments->AllocationListSize ||
             transaction->DmaBuffer != static_cast<BYTE *>(patchArguments->pDmaBuffer) + patchArguments->DmaBufferSubmissionStartOffset ||
             patchArguments->DmaBufferSize - patchArguments->DmaBufferSubmissionStartOffset != transaction->DmaBufferSize ||
             candidatePrivateLength != transaction->PrivateDataSize || transaction->PrivateData != candidatePrivate ||
             transaction->FenceId != 0))
        {
            status = STATUS_INVALID_PARAMETER;
        }

        VIOGPU_WDDM_ALLOCATION *source = transaction == NULL ? NULL : transaction->Source;
        VIOGPU_WDDM_ALLOCATION *destination = transaction == NULL ? NULL : transaction->Destination;
        BOOLEAN sourceLocked = FALSE;
        BOOLEAN destinationLocked = FALSE;
        if (status == STATUS_SUCCESS)
        {
            status = AcquirePresentAllocationLifecycles(source, destination, &sourceLocked, &destinationLocked);
        }
        if (status == STATUS_SUCCESS)
        {
            const D3DDDI_PATCHLOCATIONLIST *sourcePatch = &patchArguments->pPatchLocationList[patchArguments->PatchLocationListSubmissionStart];
            const D3DDDI_PATCHLOCATIONLIST *destinationPatch = sourcePatch + 1;
            const DXGK_ALLOCATIONLIST *sourceEntry = &patchArguments->pAllocationList[transaction->SourceAllocationIndex];
            const DXGK_ALLOCATIONLIST *destinationEntry = &patchArguments->pAllocationList[transaction->DestinationAllocationIndex];
            VIOGPU_WDDM_OPEN_ALLOCATION *sourceOpen = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(sourceEntry->hDeviceSpecificAllocation);
            VIOGPU_WDDM_OPEN_ALLOCATION *destinationOpen = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(destinationEntry->hDeviceSpecificAllocation);
            BOOLEAN nativeSource = HasLiveNativePresentIdentity(source, transaction->Context, adapter);
            BOOLEAN gdiSource = transaction->Context->Type == VioGpuWddmContextGdi &&
                                (IsGdiSourceAllocation(source) || IsStandardPrimaryAllocation(source));
            if (gdiSource)
            {
                gdiSource = ReconcileGdiSourcePlacementAfterReset(source) &&
                            HasLiveGdiPresentIdentity(source, transaction->Context, adapter);
            }
            BOOLEAN sourceValid = (nativeSource &&
                                   adapter->IsNativeContextGenerationCurrent(source->ContextGeneration,
                                                                             source->ContextResetGeneration)) ||
                                  gdiSource;
            BOOLEAN valid = sourceValid && sourceOpen != NULL && destinationOpen != NULL &&
                            sourceOpen->Signature == VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE &&
                            destinationOpen->Signature == VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE &&
                            sourceOpen->Device == transaction->Context->Device &&
                            destinationOpen->Device == transaction->Context->Device && !destinationOpen->ReadOnly &&
                            sourceOpen->Allocation == source && destinationOpen->Allocation == destination &&
                            source->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE &&
                            destination->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE && source->Adapter == adapter &&
                            destination->Adapter == adapter && IsStandardPrimaryAllocation(destination) &&
                            source->PlacementValid && source->ApertureAddress != NULL &&
                            EnsureStandard2DAllocationBacking(destination) &&
                            destination->Resource2DState == VioGpu2DResourceBackingAttached &&
                            destination->PlacementValid && destination->ApertureAddress != NULL &&
                            sourcePatch->AllocationIndex == transaction->SourceAllocationIndex &&
                            sourcePatch->AllocationOffset == 0 &&
                            IsPatchOffsetForSubmission(sourcePatch->PatchOffset,
                                                       FIELD_OFFSET(VIOGPU_WDDM_PRESENT_DMA_PACKET,
                                                                    SourcePlacementOffset),
                                                       patchArguments->DmaBufferSubmissionStartOffset) &&
                            sourcePatch->Reserved == 0 && sourcePatch->DriverId == 0 && sourcePatch->SplitOffset == 0 &&
                            destinationPatch->AllocationIndex == transaction->DestinationAllocationIndex &&
                            destinationPatch->AllocationOffset == 0 &&
                            IsPatchOffsetForSubmission(destinationPatch->PatchOffset,
                                                       FIELD_OFFSET(VIOGPU_WDDM_PRESENT_DMA_PACKET,
                                                                    DestinationPlacementOffset),
                                                       patchArguments->DmaBufferSubmissionStartOffset) &&
                            destinationPatch->Reserved == 0 && destinationPatch->DriverId == 0 &&
                            destinationPatch->SplitOffset == 0 && sourceEntry->SegmentId == VIOGPU_WDDM_SEGMENT_ID &&
                            sourceEntry->WriteOperation == 0 && sourceEntry->Reserved == 0 &&
                            sourceEntry->PhysicalAddress.QuadPart >= 0 &&
                            static_cast<ULONGLONG>(sourceEntry->PhysicalAddress.QuadPart) == source->PlacementOffset &&
                            destinationEntry->SegmentId == VIOGPU_WDDM_SEGMENT_ID &&
                            destinationEntry->WriteOperation != 0 && destinationEntry->Reserved == 0 &&
                            destinationEntry->PhysicalAddress.QuadPart >= 0 &&
                            static_cast<ULONGLONG>(destinationEntry->PhysicalAddress.QuadPart) == destination->PlacementOffset;
            if (valid)
            {
                transaction->SourcePlacementOffset = source->PlacementOffset;
                transaction->DestinationPlacementOffset = destination->PlacementOffset;
                transaction->DestinationResetGeneration = destination->Resource2DResetGeneration;
                transaction->FenceId = patchArguments->SubmissionFenceId;
                VIOGPU_WDDM_PRESENT_DMA_PACKET *packet = static_cast<VIOGPU_WDDM_PRESENT_DMA_PACKET *>(transaction->DmaBuffer);
                packet->SourcePlacementOffset = transaction->SourcePlacementOffset;
                packet->DestinationPlacementOffset = transaction->DestinationPlacementOffset;
                packet->DestinationResetGeneration = transaction->DestinationResetGeneration;
                KeMemoryBarrier();
                valid = InterlockedCompareExchange(&transaction->State,
                                                   VioGpuWddmPresentPatched,
                                                   VioGpuWddmPresentBuilt) == VioGpuWddmPresentBuilt;
                if (valid && InterlockedCompareExchange(&transaction->CancelRequested, 0, 0) != 0)
                {
                    status = STATUS_CANCELLED;
                }
            }
            if (!valid)
            {
                status = STATUS_DEVICE_NOT_READY;
            }
        }

        if (destinationLocked)
        {
            KeReleaseMutex(&destination->LifecycleMutex, FALSE);
        }
        if (sourceLocked)
        {
            KeReleaseMutex(&source->LifecycleMutex, FALSE);
        }
        if (status != STATUS_SUCCESS && transaction != NULL)
        {
            LONG state = InterlockedCompareExchange(&transaction->State, 0, 0);
            if (state == VioGpuWddmPresentBuilt || state == VioGpuWddmPresentPatched)
            {
                RetirePresentTransaction(transaction, state, VioGpuWddmPresentCancelled);
            }
        }
        if (transaction != NULL)
        {
            DereferencePresentTransaction(transaction);
        }
        adapter->ReleaseNativeSubmissionOperation();
        return STATUS_SUCCESS;
    }

    VIOGPU_WDDM_SUBMISSION *submission = NULL;
    NTSTATUS status = ResolveSubmissionPrivateData(patchArguments->pDmaBufferPrivateData,
                                                   patchArguments->DmaBufferPrivateDataSize,
                                                   patchArguments->DmaBufferPrivateDataSubmissionStartOffset,
                                                   patchArguments->DmaBufferPrivateDataSubmissionEndOffset,
                                                   adapter,
                                                   patchArguments->hContext,
                                                   &submission);

    if (NT_SUCCESS(status))
    {
        UINT dmaLength = patchArguments->DmaBufferSubmissionEndOffset - patchArguments->DmaBufferSubmissionStartOffset;
        UINT privateLength = patchArguments->DmaBufferPrivateDataSubmissionEndOffset -
                             patchArguments->DmaBufferPrivateDataSubmissionStartOffset;
        BOOLEAN exact = submission->Context != NULL && submission->Context->NodeOrdinal == 0 &&
                        submission->Context->EngineAffinity == 1 && submission->Adapter == adapter &&
                        submission->DmaBuffer == static_cast<BYTE *>(patchArguments->pDmaBuffer) + patchArguments->DmaBufferSubmissionStartOffset &&
                        patchArguments->DmaBufferSize - patchArguments->DmaBufferSubmissionStartOffset == submission->DmaBufferSize &&
                        dmaLength == submission->CommandLength &&
                        submission->DmaPrivateData == static_cast<BYTE *>(patchArguments->pDmaBufferPrivateData) + patchArguments->DmaBufferPrivateDataSubmissionStartOffset &&
                        privateLength == submission->DmaPrivateDataSize &&
                        privateLength == sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) &&
                        patchArguments->PatchLocationListSubmissionLength == submission->AllocationCount &&
                        submission->FenceId == 0 && !submission->PatchApplied &&
                        InterlockedCompareExchange(&submission->State,
                                                   VioGpuWddmSubmissionPrepared,
                                                   VioGpuWddmSubmissionPrepared) == VioGpuWddmSubmissionPrepared;
        if (!exact)
        {
            status = STATUS_INVALID_PARAMETER;
        }
    }

    UINT *patchedResourceIds = NULL;
    ULONGLONG *patchedIovas = NULL;
    if (NT_SUCCESS(status))
    {
        patchedResourceIds = new (NonPagedPoolNx) UINT[submission->AllocationCount];
        patchedIovas = new (NonPagedPoolNx) ULONGLONG[submission->AllocationCount];
        if (patchedResourceIds == NULL || patchedIovas == NULL)
        {
            status = STATUS_NO_MEMORY;
        }
        else
        {
            RtlZeroMemory(patchedResourceIds, (SIZE_T)submission->AllocationCount * sizeof(*patchedResourceIds));
            RtlZeroMemory(patchedIovas, (SIZE_T)submission->AllocationCount * sizeof(*patchedIovas));
        }
    }

    BOOLEAN patchClaimed = FALSE;
    if (NT_SUCCESS(status))
    {
        patchClaimed = InterlockedCompareExchange(&submission->State,
                                                  VioGpuWddmSubmissionPatching,
                                                  VioGpuWddmSubmissionPrepared) == VioGpuWddmSubmissionPrepared;
        if (!patchClaimed)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
    }

    if (NT_SUCCESS(status))
    {
        for (UINT index = 0; index < submission->AllocationCount; ++index)
        {
            const VIOGPU_WDDM_SUBMISSION_REFERENCE *reference = &submission->References[index];
            const D3DDDI_PATCHLOCATIONLIST *patch = &patchArguments->pPatchLocationList[patchArguments->PatchLocationListSubmissionStart +
                                                                                        index];
            BOOLEAN relativeOffsetValid = reference->PatchOffset <= MAXUINT - submission->CommandStreamOffset;
            UINT expectedPatchOffset = relativeOffsetValid ? submission->CommandStreamOffset + reference->PatchOffset
                                                           : 0;
            if (reference->Allocation == NULL || reference->AllocationIndex >= patchArguments->AllocationListSize ||
                patch->AllocationIndex != reference->AllocationIndex ||
                patch->AllocationOffset != reference->AllocationOffset || !relativeOffsetValid ||
                !IsPatchOffsetForSubmission(patch->PatchOffset,
                                            expectedPatchOffset,
                                            patchArguments->DmaBufferSubmissionStartOffset) ||
                patch->Reserved != 0 || patch->DriverId != 0 || patch->SplitOffset != 0)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            const DXGK_ALLOCATIONLIST *allocationEntry = &patchArguments->pAllocationList[reference->AllocationIndex];
            VIOGPU_WDDM_OPEN_ALLOCATION *openAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationEntry->hDeviceSpecificAllocation);
            VIOGPU_WDDM_ALLOCATION *allocation = reference->Allocation;
            status = AcquireAllocationLifecycle(allocation);
            if (status != STATUS_SUCCESS)
            {
                break;
            }

            BOOLEAN valid = openAllocation != NULL &&
                            openAllocation->Signature == VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE &&
                            openAllocation->Allocation == allocation &&
                            openAllocation->Device == submission->Context->Device &&
                            allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE &&
                            allocation->Adapter == adapter && !allocation->Destroying &&
                            allocation->HostState == VioGpuWddmAllocationHostLive && allocation->PlacementValid &&
                            allocation->ApertureMdl != NULL && allocation->ApertureAddress != NULL &&
                            allocation->ResourceId >= VIOGPU_NATIVE_RESOURCE_ID_START &&
                            allocation->ResourceId != MAXUINT && allocation->BlobId == allocation->ResourceId &&
                            allocation->ContextId == submission->ContextId &&
                            allocation->ContextGeneration == submission->Generation &&
                            allocation->ContextResetGeneration == submission->ResetGeneration &&
                            allocation->BoundContextId == submission->ContextId &&
                            allocation->BoundGeneration == submission->Generation &&
                            allocation->BoundResetGeneration == submission->ResetGeneration &&
                            allocationEntry->SegmentId == VIOGPU_WDDM_SEGMENT_ID &&
                            allocationEntry->PhysicalAddress.QuadPart >= 0 &&
                            static_cast<ULONGLONG>(allocationEntry->PhysicalAddress.QuadPart) == allocation->PlacementOffset &&
                            allocationEntry->Reserved == 0 &&
                            ((reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) !=
                             0) == (allocationEntry->WriteOperation != 0) &&
                            (!openAllocation->ReadOnly || (reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) == 0) &&
                            ((allocation->Flags & VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY) == 0 ||
                             (reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) == 0) &&
                            reference->AllocationOffset <= allocation->PrivateData.Size &&
                            reference->Length <= allocation->PrivateData.Size - reference->AllocationOffset &&
                            allocation->PrivateData.RequestedIova != 0 &&
                            allocation->PrivateData.RequestedIova <= MAXULONGLONG - reference->AllocationOffset;
            if (valid)
            {
                patchedResourceIds[index] = allocation->ResourceId;
                patchedIovas[index] = allocation->PrivateData.RequestedIova + reference->AllocationOffset;
            }
            KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
            if (!valid)
            {
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
        }
    }

    if (NT_SUCCESS(status))
    {
        for (UINT index = 0; index < submission->AllocationCount; ++index)
        {
            const VIOGPU_WDDM_SUBMISSION_REFERENCE *reference = &submission->References[index];
            VIOGPU_WDDM_MSM_SUBMIT_BO *submitBo = reinterpret_cast<VIOGPU_WDDM_MSM_SUBMIT_BO *>(static_cast<BYTE *>(submission->CommandStream) +
                                                                                                sizeof(MSM_CCMD_GEM_SUBMIT_REQ) +
                                                                                                (SIZE_T)index * sizeof(VIOGPU_WDDM_MSM_SUBMIT_BO));
            PVOID patchAddress = static_cast<BYTE *>(submission->CommandStream) + reference->PatchOffset;
            RtlCopyMemory(&submitBo->Handle, &patchedResourceIds[index], sizeof(patchedResourceIds[index]));
            RtlCopyMemory(patchAddress, &patchedIovas[index], sizeof(patchedIovas[index]));
        }
        if (!adapter->RefreshNativeSubmit(submission->VirtioBuffer,
                                          submission->CommandStream,
                                          submission->CommandStreamSize))
        {
            status = STATUS_DEVICE_NOT_READY;
        }
    }

    if (NT_SUCCESS(status))
    {
        submission->FenceId = patchArguments->SubmissionFenceId;
        KeMemoryBarrier();
        submission->PatchApplied = TRUE;
        if (InterlockedCompareExchange(&submission->State, VioGpuWddmSubmissionPatched, VioGpuWddmSubmissionPatching) !=
            VioGpuWddmSubmissionPatching)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
    }
    if (!NT_SUCCESS(status) && submission != NULL)
    {
        if (patchClaimed)
        {
            /* Patch owns the transient state, but reset may already be
             * reclaiming the prepared VioGpu buffer.  Keep the registry
             * reference intact and let the terminal reset callback retire it
             * after native-submit rundown has excluded this Patch call. */
            InterlockedExchange(&submission->CancelRequested, 1);
            adapter->RequestHardwareResetAtAnyIrql();
        }
        else
        {
            ReleasePreparedSubmission(submission);
        }
    }

    if (submission != NULL)
    {
        DereferenceRenderSubmission(submission);
    }
    delete[] patchedIovas;
    delete[] patchedResourceIds;
    adapter->ReleaseNativeSubmissionOperation();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmPresent(CONST HANDLE hContext, DXGKARG_PRESENT *present)
{
    VIOGPU_WDDM_CONTEXT *context = reinterpret_cast<VIOGPU_WDDM_CONTEXT *>(hContext);
    if (context == NULL || present == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL || present->pDmaBuffer == NULL ||
        present->pDmaBufferPrivateData == NULL ||
        present->DmaBufferPrivateDataSize < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) || present->pAllocationList == NULL ||
        present->pPatchLocationListOut == NULL || present->Flags.Value != 1U || !present->Flags.Blt ||
        present->DmaBufferSegmentId != 0 || (present->SubRectCnt == 0 && present->MultipassOffset != 0) ||
        (present->SubRectCnt != 0 &&
         (present->pDstSubRects == NULL || present->MultipassOffset >= present->SubRectCnt)))
    {
        /* This guard runs before the allocations are resolved, so the full
         * present diagnostic cannot be built here.  Record the argument shape
         * anyway: a rejected flag word is otherwise invisible without a kernel
         * debugger, and a present dxgkrnl issues but this driver refuses leaves
         * the desktop black with nothing at all written down. */
        RecordPresentEntryRejection(context, present, VioGpuWddmPresentDiagnosticEntryRejected);
        return STATUS_INVALID_PARAMETER;
    }
    if (present->DmaSize < sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET) || present->PatchLocationListOutSize < 2)
    {
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    if (!ExAcquireRundownProtection(&context->Operations))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    NTSTATUS status = STATUS_SUCCESS;
    VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = NULL;
    RECT *subRects = NULL;
    BOOLEAN contextReference = FALSE;
    BOOLEAN sourceReference = FALSE;
    BOOLEAN destinationReference = FALSE;
    BOOLEAN published = FALSE;
    BOOLEAN transactionOwnsReferences = FALSE;
    BOOLEAN buildReference = FALSE;
    BOOLEAN sourcePrepatched = FALSE;
    BOOLEAN destinationPrepatched = FALSE;
    VIOGPU_WDDM_PRESENT_DIAGNOSTIC_REASON lateReason = VioGpuWddmPresentDiagnosticNone;
    VIOGPU_WDDM_ALLOCATION *source = NULL;
    VIOGPU_WDDM_ALLOCATION *destination = NULL;
    UINT rectOffset = present->SubRectCnt == 0 ? 0U : present->MultipassOffset;
    UINT remainingRectCount = present->SubRectCnt == 0 ? 1U : present->SubRectCnt - rectOffset;
    UINT rectCount = remainingRectCount < VIOGPU_WDDM_PRESENT_RECTS_PER_PASS ? remainingRectCount
                                                                             : VIOGPU_WDDM_PRESENT_RECTS_PER_PASS;
    VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = static_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(present->pDmaBufferPrivateData);

    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE || context->Device == NULL ||
        context->Device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE || context->Device->Adapter == NULL ||
        (context->Type != VioGpuWddmContextNative && context->Type != VioGpuWddmContextGdi) ||
        context->NodeOrdinal != 0 || context->EngineAffinity != 1)
    {
        status = STATUS_INVALID_HANDLE;
    }

    if (NT_SUCCESS(status))
    {
        VIOGPU_WDDM_OPEN_ALLOCATION *sourceOpen = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(present->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation);
        VIOGPU_WDDM_OPEN_ALLOCATION *destinationOpen = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(present->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation);
        if (sourceOpen == NULL || destinationOpen == NULL ||
            sourceOpen->Signature != VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE ||
            destinationOpen->Signature != VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE ||
            sourceOpen->Device != context->Device || destinationOpen->Device != context->Device ||
            sourceOpen->Allocation == NULL || destinationOpen->Allocation == NULL || destinationOpen->ReadOnly ||
            sourceOpen->Allocation == destinationOpen->Allocation ||
            !IsOwnedAllocation(sourceOpen->Allocation, context->Device->Adapter) ||
            !IsOwnedAllocation(destinationOpen->Allocation, context->Device->Adapter))
        {
            status = STATUS_INVALID_HANDLE;
        }
        else
        {
            source = sourceOpen->Allocation;
            destination = destinationOpen->Allocation;
        }
    }

    if (NT_SUCCESS(status))
    {
        subRects = new (NonPagedPoolNx) RECT[rectCount];
        transaction = new (NonPagedPoolNx) VIOGPU_WDDM_PRESENT_TRANSACTION;
        if (subRects == NULL || transaction == NULL)
        {
            status = STATUS_NO_MEMORY;
        }
    }
    if (NT_SUCCESS(status))
    {
        if (present->SubRectCnt == 0)
        {
            subRects[0] = present->DstRect;
        }
        else
        {
            __try
            {
                RtlCopyMemory(subRects, present->pDstSubRects + rectOffset, sizeof(RECT) * rectCount);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                status = STATUS_INVALID_USER_BUFFER;
            }
        }
    }

    if (NT_SUCCESS(status))
    {
        status = AcquireContextSubmissionReference(context);
        contextReference = NT_SUCCESS(status);
        if (!contextReference)
        {
            lateReason = VioGpuWddmPresentDiagnosticContextReference;
        }
    }
    if (NT_SUCCESS(status))
    {
        status = AcquireAllocationSubmissionReference(source, context->Device->Adapter);
        sourceReference = NT_SUCCESS(status);
        if (!sourceReference)
        {
            lateReason = VioGpuWddmPresentDiagnosticSourceReference;
        }
    }
    if (NT_SUCCESS(status))
    {
        status = AcquireAllocationSubmissionReference(destination, context->Device->Adapter);
        destinationReference = NT_SUCCESS(status);
        if (!destinationReference)
        {
            lateReason = VioGpuWddmPresentDiagnosticDestinationReference;
        }
    }

    BOOLEAN sourceLocked = FALSE;
    BOOLEAN destinationLocked = FALSE;
    if (NT_SUCCESS(status))
    {
        status = AcquirePresentAllocationLifecycles(source, destination, &sourceLocked, &destinationLocked);
        if (status != STATUS_SUCCESS)
        {
            lateReason = sourceLocked ? VioGpuWddmPresentDiagnosticDestinationLifecycle
                                      : VioGpuWddmPresentDiagnosticSourceLifecycle;
        }
    }
    if (NT_SUCCESS(status))
    {
        BOOLEAN nativeSource = HasLiveNativePresentIdentity(source, context, context->Device->Adapter);
        BOOLEAN gdiCandidate = context->Type == VioGpuWddmContextGdi &&
                               (IsGdiSourceAllocation(source) || IsStandardPrimaryAllocation(source));
        BOOLEAN gdiSource = gdiCandidate && HasGdiPresentIdentity(source, context, context->Device->Adapter);
        BOOLEAN nativeSourceCurrent = nativeSource &&
                                      context->Device->Adapter->IsNativeContextGenerationCurrent(source->ContextGeneration,
                                                                                                 source->ContextResetGeneration);
        BOOLEAN sourcePrepatchValid = ValidatePresentPrepatchEntry(&present->pAllocationList[DXGK_PRESENT_SOURCE_INDEX],
                                                                   source,
                                                                   FALSE,
                                                                   &sourcePrepatched);
        BOOLEAN destinationPrepatchValid = ValidatePresentPrepatchEntry(&present->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX],
                                                                        destination,
                                                                        TRUE,
                                                                        &destinationPrepatched);
        BOOLEAN gdiSourcePrepatchLive = TRUE;
        if (sourcePrepatchValid && sourcePrepatched && gdiSource)
        {
            gdiSourcePrepatchLive = ReconcileGdiSourcePlacementAfterReset(source) &&
                                    HasLiveGdiPresentIdentity(source, context, context->Device->Adapter);
        }
        VIOGPU_WDDM_PRESENT_DIAGNOSTIC_REASON reason = VioGpuWddmPresentDiagnosticNone;

        if (!nativeSourceCurrent && !gdiSource)
        {
            /* Neither identity held.  The present diagnostic records the source
             * fields it already knows about, but not the ones HasGdiPresentIdentity
             * actually turns on, so publish that predicate term by term.  Bit set
             * means the term held. */
            DWORD identityTerms = 0;
            identityTerms |= source->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE ? 1U << 0 : 0;
            identityTerms |= source->Adapter == context->Device->Adapter ? 1U << 1 : 0;
            identityTerms |= context->Type == VioGpuWddmContextGdi ? 1U << 2 : 0;
            identityTerms |= IsStandardAllocation(source) ? 1U << 3 : 0;
            identityTerms |= !IsStandardPrimaryAllocation(source) ? 1U << 4 : 0;
            identityTerms |= IsGdiSourceAllocation(source) ? 1U << 5 : 0;
            identityTerms |= source->HostState == VioGpuWddmAllocationHostNone ? 1U << 6 : 0;
            identityTerms |= source->BlobId == 0 ? 1U << 7 : 0;
            identityTerms |= source->ResourceId != 0 ? 1U << 8 : 0;
            identityTerms |= source->ResourceId < VIOGPU_NATIVE_RESOURCE_ID_START ? 1U << 9 : 0;
            identityTerms |= source->ContextId == 0 ? 1U << 10 : 0;
            identityTerms |= source->ContextGeneration == 0 ? 1U << 11 : 0;
            identityTerms |= source->ContextResetGeneration == 0 ? 1U << 12 : 0;
            identityTerms |= nativeSource ? 1U << 13 : 0;
            identityTerms |= gdiCandidate ? 1U << 14 : 0;
            identityTerms |= 1U << 31;
            context->Device->Adapter->RecordNativeGdiIdentityDiagnostic(identityTerms,
                                                                        static_cast<DWORD>(source->BlobId),
                                                                        static_cast<DWORD>(source->ContextId),
                                                                        static_cast<DWORD>(source->ContextGeneration));
            if (context->Type == VioGpuWddmContextNative)
            {
                reason = VioGpuWddmPresentDiagnosticNativeSourceIdentity;
            }
            else
            {
                reason = VioGpuWddmPresentDiagnosticGdiSourceIdentity;
            }
        }
        else if (source->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE || source->Adapter != context->Device->Adapter)
        {
            reason = VioGpuWddmPresentDiagnosticSourceObject;
        }
        else if (destination->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
                 destination->Adapter != context->Device->Adapter || !IsStandardPrimaryAllocation(destination))
        {
            reason = VioGpuWddmPresentDiagnosticDestinationObject;
        }
        else if (!ValidatePresentGeometry(source,
                                          destination,
                                          &present->SrcRect,
                                          &present->DstRect,
                                          subRects,
                                          rectCount))
        {
            reason = VioGpuWddmPresentDiagnosticGeometry;
        }
        else if (!sourcePrepatchValid)
        {
            reason = VioGpuWddmPresentDiagnosticSourcePrepatch;
        }
        else if (!destinationPrepatchValid)
        {
            reason = VioGpuWddmPresentDiagnosticDestinationPrepatch;
        }
        else if (sourcePrepatched && gdiSource && !gdiSourcePrepatchLive)
        {
            reason = VioGpuWddmPresentDiagnosticGdiSourcePlacement;
        }
        else if (sourcePrepatched &&
                 (!source->PlacementValid || source->ApertureMdl == NULL || source->ApertureAddress == NULL ||
                  source->ApertureMappedPageCount != source->AperturePageCount))
        {
            reason = VioGpuWddmPresentDiagnosticSourcePlacement;
        }
        else if (destinationPrepatched && (!destination->PlacementValid || destination->ApertureMdl == NULL ||
                                           destination->ApertureAddress == NULL ||
                                           destination->ApertureMappedPageCount != destination->AperturePageCount))
        {
            reason = VioGpuWddmPresentDiagnosticDestinationPlacement;
        }
        else if (destinationPrepatched && (!EnsureStandard2DAllocationBacking(destination) ||
                                           destination->Resource2DState != VioGpu2DResourceBackingAttached ||
                                           destination->Resource2DResetGeneration == 0))
        {
            reason = VioGpuWddmPresentDiagnosticDestinationBacking;
        }

        if (reason != VioGpuWddmPresentDiagnosticNone)
        {
            /* STATUS_NOT_SUPPORTED is not a legal status for DxgkDdiPresent.  dxgkrnl
             * logs "Driver returned an invalid NTSTATUS code: 0xffffffffc00000bb" and
             * stops presenting on the adapter, so the desktop froze on whichever frame
             * had last reached the Host while the driver recorded a clean present.
             * Report the classified refusal with a status this DDI may return. */
            status = STATUS_INVALID_PARAMETER;
            RecordPresentDiagnostic(context, present, source, destination, reason, status);
        }
    }

    if (NT_SUCCESS(status))
    {
        RtlZeroMemory(transaction, sizeof(*transaction));
        transaction->Signature = VIOGPU_WDDM_PRESENT_TRANSACTION_SIGNATURE;
        transaction->ReferenceCount = 1;
        transaction->State = VioGpuWddmPresentBuilt;
        InitializeListHead(&transaction->ContextEntry.Link);
        transaction->ContextEntry.Kind = VioGpuWddmContextSubmissionPresent;
        transaction->ContextEntry.Owner = transaction;
        transaction->ContextEntry.Context = context;
        InitializeListHead(&transaction->AdapterLink);
        InitializeListHead(&transaction->Work.Link);
        transaction->Work.Routine = NativePresentWorker;
        transaction->Work.CancelRoutine = NativePresentDispatchCancelled;
        transaction->Work.Context = transaction;
        transaction->Work.CancelRequested = &transaction->CancelRequested;
        transaction->Context = context;
        transaction->Adapter = context->Device->Adapter;
        transaction->Source = source;
        transaction->Destination = destination;
        transaction->DmaBuffer = present->pDmaBuffer;
        transaction->DmaBufferSize = present->DmaSize;
        transaction->PrivateData = privateData;
        transaction->PrivateDataSize = sizeof(*privateData);
        transaction->SourceAllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
        transaction->DestinationAllocationIndex = DXGK_PRESENT_DESTINATION_INDEX;
        transaction->SourceRect = present->SrcRect;
        transaction->DestinationRect = present->DstRect;
        transaction->DestinationSubRects = subRects;
        transaction->RectCount = rectCount;
        transaction->FullyPrepatched = sourcePrepatched && destinationPrepatched;
        if (sourcePrepatched)
        {
            transaction->SourcePlacementOffset = source->PlacementOffset;
        }
        if (destinationPrepatched)
        {
            transaction->DestinationPlacementOffset = destination->PlacementOffset;
            transaction->DestinationResetGeneration = destination->Resource2DResetGeneration;
        }

        VIOGPU_WDDM_PRESENT_DMA_PACKET *packet = static_cast<VIOGPU_WDDM_PRESENT_DMA_PACKET *>(present->pDmaBuffer);
        RtlZeroMemory(packet, sizeof(*packet));
        packet->Signature = VIOGPU_WDDM_PRESENT_DMA_SIGNATURE;
        packet->Version = VioGpuWddmDmaPrivateVersion;
        packet->Size = sizeof(*packet);
        packet->Flags = present->Flags.Value;
        packet->SourceResourceId = source->ResourceId;
        packet->DestinationResourceId = destination->ResourceId;
        packet->RectCount = rectCount;
        packet->SourcePlacementOffset = transaction->SourcePlacementOffset;
        packet->DestinationPlacementOffset = transaction->DestinationPlacementOffset;
        packet->DestinationResetGeneration = transaction->DestinationResetGeneration;

        RtlZeroMemory(privateData, sizeof(*privateData));
        privateData->Signature = VIOGPU_WDDM_DMA_SIGNATURE;
        privateData->Version = VioGpuWddmDmaPrivateVersion;
        privateData->Kind = VioGpuWddmDmaKindPresent;
        privateData->DmaBuffer = present->pDmaBuffer;
        privateData->DmaBufferSize = present->DmaSize;
        privateData->CommandLength = sizeof(*packet);
        privateData->ContextId = source->ContextId;
        privateData->Generation = source->ContextGeneration;
        privateData->ResetGeneration = source->ContextResetGeneration;
        privateData->Flags = present->Flags.Value;
        privateData->Packet = packet;
        privateData->PacketLength = sizeof(*packet);
        privateData->Submission = transaction;

        transactionOwnsReferences = TRUE;
        contextReference = FALSE;
        sourceReference = FALSE;
        destinationReference = FALSE;
        subRects = NULL;

        D3DDDI_PATCHLOCATIONLIST *sourcePatch = &present->pPatchLocationListOut[0];
        D3DDDI_PATCHLOCATIONLIST *destinationPatch = &present->pPatchLocationListOut[1];
        RtlZeroMemory(sourcePatch, sizeof(*sourcePatch));
        RtlZeroMemory(destinationPatch, sizeof(*destinationPatch));
        sourcePatch->AllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
        sourcePatch->PatchOffset = FIELD_OFFSET(VIOGPU_WDDM_PRESENT_DMA_PACKET, SourcePlacementOffset);
        destinationPatch->AllocationIndex = DXGK_PRESENT_DESTINATION_INDEX;
        destinationPatch->PatchOffset = FIELD_OFFSET(VIOGPU_WDDM_PRESENT_DMA_PACKET, DestinationPlacementOffset);

        buildReference = ReferencePresentTransaction(transaction);
        BOOLEAN registered = buildReference && RegisterPresentTransaction(transaction);
        if (!buildReference)
        {
            lateReason = VioGpuWddmPresentDiagnosticTransactionReference;
        }
        else if (!registered)
        {
            lateReason = VioGpuWddmPresentDiagnosticTransactionRegistration;
        }
        BOOLEAN valid = FALSE;
        if (registered)
        {
            KIRQL oldIrql;
            KeAcquireSpinLock(&context->SubmissionLock, &oldIrql);
            valid = context->Signature == VIOGPU_WDDM_CONTEXT_SIGNATURE && !context->SubmissionClosing &&
                    context->SubmissionReferences > 0 && privateData->Submission == transaction &&
                    InterlockedCompareExchange(&transaction->State,
                                               VioGpuWddmPresentBuilt,
                                               VioGpuWddmPresentBuilt) == VioGpuWddmPresentBuilt;
            if (valid)
            {
                InsertTailList(&context->PendingSubmissions, &transaction->ContextEntry.Link);
                published = TRUE;
            }
            KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
        }
        if (!valid)
        {
            status = STATUS_DEVICE_NOT_READY;
            if (registered)
            {
                lateReason = VioGpuWddmPresentDiagnosticContextPublication;
            }
        }
    }

    if (lateReason != VioGpuWddmPresentDiagnosticNone)
    {
        RecordPresentDiagnostic(context, present, source, destination, lateReason, status);
    }

    if (destinationLocked)
    {
        KeReleaseMutex(&destination->LifecycleMutex, FALSE);
    }
    if (sourceLocked)
    {
        KeReleaseMutex(&source->LifecycleMutex, FALSE);
    }
    if (published)
    {
        present->pDmaBuffer = static_cast<BYTE *>(present->pDmaBuffer) + sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET);
        present->pDmaBufferPrivateData = static_cast<BYTE *>(present->pDmaBufferPrivateData) +
                                         sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE);
        present->DmaBufferPrivateDataSize -= sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE);
        present->pPatchLocationListOut += 2;
        present->MultipassOffset = present->SubRectCnt == 0 ? 0U : rectOffset + rectCount;
        if (present->SubRectCnt != 0 && present->MultipassOffset < present->SubRectCnt)
        {
            status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
    }
    if (transactionOwnsReferences)
    {
        if (!published)
        {
            RetirePresentTransaction(transaction, VioGpuWddmPresentBuilt, VioGpuWddmPresentCancelled);
        }
        if (buildReference)
        {
            DereferencePresentTransaction(transaction);
        }
        transaction = NULL;
    }
    if (destinationReference)
    {
        ReleaseAllocationSubmissionReference(destination);
    }
    if (sourceReference)
    {
        ReleaseAllocationSubmissionReference(source);
    }
    if (contextReference)
    {
        ReleaseContextSubmissionReference(context);
    }
    delete transaction;
    delete[] subRects;
    ExReleaseRundownProtection(&context->Operations);
    return status;
}

VOID RetireUnsubmittedDmaOwner(_In_ VioGpuDod *adapter, _In_ const DXGKARG_SUBMITCOMMAND *submitCommand)
{
    RetireDmaOwner(adapter,
                   NULL,
                   submitCommand->DmaBufferSize,
                   submitCommand->DmaBufferSubmissionStartOffset,
                   submitCommand->DmaBufferSubmissionEndOffset,
                   submitCommand->pDmaBufferPrivateData,
                   submitCommand->DmaBufferPrivateDataSize,
                   submitCommand->DmaBufferPrivateDataSubmissionStartOffset,
                   submitCommand->DmaBufferPrivateDataSubmissionEndOffset,
                   submitCommand->hContext,
                   -1);
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmSubmitCommand(CONST HANDLE hAdapter,
                                                                 CONST DXGKARG_SUBMITCOMMAND *submitCommand)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    BOOLEAN pagingSubmission = submitCommand != NULL && submitCommand->Flags.Paging != 0;
    BOOLEAN ownerRangeValid = submitCommand != NULL && submitCommand->pDmaBufferPrivateData != NULL &&
                              submitCommand->DmaBufferSubmissionStartOffset <= submitCommand->DmaBufferSubmissionEndOffset &&
                              submitCommand->DmaBufferSubmissionEndOffset <= submitCommand->DmaBufferSize &&
                              submitCommand->DmaBufferPrivateDataSubmissionStartOffset <= submitCommand->DmaBufferPrivateDataSubmissionEndOffset &&
                              submitCommand->DmaBufferPrivateDataSubmissionEndOffset <= submitCommand->DmaBufferPrivateDataSize;
    if (adapter == NULL || submitCommand == NULL || KeGetCurrentIrql() != DISPATCH_LEVEL ||
        submitCommand->pDmaBufferPrivateData == NULL || submitCommand->DmaBufferSegmentId != 0 ||
        submitCommand->EngineOrdinal != 0 || submitCommand->NodeOrdinal != 0 || submitCommand->SubmissionFenceId == 0 ||
        submitCommand->SubmissionFenceId > MAXUINT ||
        submitCommand->DmaBufferSubmissionStartOffset > submitCommand->DmaBufferSubmissionEndOffset ||
        submitCommand->DmaBufferSubmissionEndOffset > submitCommand->DmaBufferSize ||
        submitCommand->DmaBufferPrivateDataSubmissionStartOffset > submitCommand->DmaBufferPrivateDataSubmissionEndOffset ||
        submitCommand->DmaBufferPrivateDataSubmissionEndOffset > submitCommand->DmaBufferPrivateDataSize)
    {
        if (adapter != NULL && submitCommand != NULL && KeGetCurrentIrql() == DISPATCH_LEVEL && ownerRangeValid)
        {
            RetireUnsubmittedDmaOwner(adapter, submitCommand);
        }
        if (adapter != NULL && submitCommand != NULL && KeGetCurrentIrql() == DISPATCH_LEVEL && pagingSubmission)
        {
            adapter->QueueNativeSoftwareSubmissionCompletion(submitCommand->SubmissionFenceId,
                                                             submitCommand->NodeOrdinal,
                                                             submitCommand->EngineOrdinal);
            adapter->RequestHardwareResetAtAnyIrql();
            return STATUS_SUCCESS;
        }
        return STATUS_INVALID_PARAMETER;
    }

    BOOLEAN emptyPagingSubmission = submitCommand->Flags.Value == 1 &&
                                    submitCommand->DmaBufferSubmissionStartOffset == submitCommand->DmaBufferSubmissionEndOffset &&
                                    submitCommand->DmaBufferPrivateDataSubmissionStartOffset == submitCommand->DmaBufferPrivateDataSubmissionEndOffset;
    if (emptyPagingSubmission)
    {
        if (!adapter->QueueNativeSoftwareSubmissionCompletion(submitCommand->SubmissionFenceId,
                                                              submitCommand->NodeOrdinal,
                                                              submitCommand->EngineOrdinal))
        {
            adapter->RequestHardwareResetAtAnyIrql();
        }
        return STATUS_SUCCESS;
    }

    if (!adapter->AcquireNativeSubmissionOperation())
    {
        RetireUnsubmittedDmaOwner(adapter, submitCommand);
        if (pagingSubmission)
        {
            adapter->QueueNativeSoftwareSubmissionCompletion(submitCommand->SubmissionFenceId,
                                                             submitCommand->NodeOrdinal,
                                                             submitCommand->EngineOrdinal);
            adapter->RequestHardwareResetAtAnyIrql();
        }
        else
        {
            adapter->NotifyNativeSubmissionFault(submitCommand->SubmissionFenceId,
                                                 STATUS_DEVICE_NOT_READY,
                                                 submitCommand->NodeOrdinal,
                                                 submitCommand->EngineOrdinal,
                                                 TRUE);
        }
        return STATUS_SUCCESS;
    }

    UINT privateStart = submitCommand->DmaBufferPrivateDataSubmissionStartOffset;
    UINT privateEnd = submitCommand->DmaBufferPrivateDataSubmissionEndOffset;
    UINT privateLength = privateEnd - privateStart;
    VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = reinterpret_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(static_cast<BYTE *>(submitCommand->pDmaBufferPrivateData) +
                                                                                               privateStart);
    NTSTATUS status = STATUS_SUCCESS;

    if (privateLength < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) || privateData->Signature != VIOGPU_WDDM_DMA_SIGNATURE ||
        privateData->Version != VioGpuWddmDmaPrivateVersion || privateData->DmaBuffer == NULL ||
        privateData->DmaBufferSize < privateData->CommandLength || privateData->CommandLength == 0 ||
        privateData->Packet != privateData->DmaBuffer || privateData->PacketLength != privateData->CommandLength ||
        privateData->Reserved != 0)
    {
        status = STATUS_INVALID_PARAMETER;
    }

    if (pagingSubmission)
    {
        VIOGPU_WDDM_DEVICE *device = reinterpret_cast<VIOGPU_WDDM_DEVICE *>(submitCommand->hDevice);
        BOOLEAN deviceReferenced = ReferenceDevice(device);
        BOOLEAN valid = NT_SUCCESS(status) && privateData->Kind == VioGpuWddmDmaKindPaging &&
                        submitCommand->Flags.Value == 1 && deviceReferenced && device->Adapter == adapter;
        VIOGPU_WDDM_PAGING_PRIVATE *firstPrivate = NULL;
        UINT recordCount = 0;
        UINT recordLimit = privateEnd >= sizeof(VIOGPU_WDDM_PAGING_PRIVATE) ? privateEnd - sizeof(VIOGPU_WDDM_PAGING_PRIVATE)
                                                                            : 0;
        valid = valid && ResolvePagingBatch(NULL,
                                            submitCommand->DmaBufferSize,
                                            submitCommand->DmaBufferSubmissionStartOffset,
                                            submitCommand->DmaBufferSubmissionEndOffset,
                                            submitCommand->pDmaBufferPrivateData,
                                            submitCommand->DmaBufferPrivateDataSize,
                                            privateStart,
                                            privateEnd,
                                            adapter,
                                            VioGpuWddmPagingTransactionBuilt,
                                            &firstPrivate,
                                            &recordCount);
        if (valid)
        {
            for (UINT index = 0; index < recordCount; ++index)
            {
                UINT recordOffset = 0;
                if (!ResolvePagingBatchOffset(privateStart,
                                              index,
                                              sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                              recordLimit,
                                              &recordOffset))
                {
                    valid = FALSE;
                    break;
                }
                VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(submitCommand->pDmaBufferPrivateData) +
                                                                                                           recordOffset);
                if (!ValidatePagingTransactionReference(&pagingPrivate->Transaction, adapter))
                {
                    valid = FALSE;
                    break;
                }
            }
        }
        if (valid)
        {
            for (UINT index = 0; index < recordCount; ++index)
            {
                UINT recordOffset = 0;
                if (!ResolvePagingBatchOffset(privateStart,
                                              index,
                                              sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                              recordLimit,
                                              &recordOffset))
                {
                    valid = FALSE;
                    break;
                }
                VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(submitCommand->pDmaBufferPrivateData) +
                                                                                                           recordOffset);
                VIOGPU_WDDM_PAGING_DMA_PACKET *packet = static_cast<VIOGPU_WDDM_PAGING_DMA_PACKET *>(pagingPrivate->Header.Packet);
                if (packet->ContextId != 0 &&
                    !adapter->IsNativeContextGenerationCurrent(packet->ContextGeneration, packet->ResetGeneration))
                {
                    valid = FALSE;
                    break;
                }
            }
        }
        if (valid)
        {
            for (UINT index = 0; index < recordCount; ++index)
            {
                UINT recordOffset = 0;
                if (!ResolvePagingBatchOffset(privateStart,
                                              index,
                                              sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                              recordLimit,
                                              &recordOffset))
                {
                    valid = FALSE;
                    break;
                }
                VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(submitCommand->pDmaBufferPrivateData) +
                                                                                                           recordOffset);
                if (InterlockedCompareExchange(&pagingPrivate->Transaction.State,
                                               VioGpuWddmPagingTransactionQueued,
                                               VioGpuWddmPagingTransactionBuilt) != VioGpuWddmPagingTransactionBuilt)
                {
                    valid = FALSE;
                    break;
                }
            }
        }
        BOOLEAN queued = FALSE;
        if (valid)
        {
            firstPrivate->BatchPrivateData = submitCommand->pDmaBufferPrivateData;
            firstPrivate->BatchPrivateDataSize = submitCommand->DmaBufferPrivateDataSize;
            firstPrivate->BatchPrivateStart = privateStart;
            firstPrivate->BatchPrivateEnd = privateEnd;
            firstPrivate->BatchFenceId = submitCommand->SubmissionFenceId;
            firstPrivate->BatchNodeOrdinal = submitCommand->NodeOrdinal;
            firstPrivate->BatchEngineOrdinal = submitCommand->EngineOrdinal;
            KeMemoryBarrier();
            queued = adapter->QueueNativePassiveWork(&firstPrivate->Work, submitCommand->SubmissionFenceId);
            if (!queued)
            {
                valid = FALSE;
            }
        }
        if (queued)
        {
            DereferenceDevice(device);
            adapter->ReleaseNativeSubmissionOperation();
            return STATUS_SUCCESS;
        }

        for (UINT index = 0; index < recordCount; ++index)
        {
            UINT recordOffset = 0;
            if (!ResolvePagingBatchOffset(privateStart,
                                          index,
                                          sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                          recordLimit,
                                          &recordOffset))
            {
                continue;
            }
            VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(submitCommand->pDmaBufferPrivateData) +
                                                                                                       recordOffset);
            CancelRecognizedPagingTransaction(pagingPrivate, adapter);
            ReleasePagingTransactionReference(&pagingPrivate->Transaction);
        }
        if (deviceReferenced)
        {
            DereferenceDevice(device);
        }
        adapter->QueueNativeSoftwareSubmissionCompletion(submitCommand->SubmissionFenceId,
                                                         submitCommand->NodeOrdinal,
                                                         submitCommand->EngineOrdinal);
        adapter->RequestHardwareResetAtAnyIrql();
        adapter->ReleaseNativeSubmissionOperation();
        return STATUS_SUCCESS;
    }
    else if (NT_SUCCESS(status) && privateData->Kind == VioGpuWddmDmaKindPresent)
    {
        VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = NULL;
        VIOGPU_WDDM_PRESENT_SUBMIT_STAGE submitFailureStage = VioGpuWddmPresentSubmitNone;
        DWORD submitFailureDetail = 0;
        status = ResolvePresentTransaction(submitCommand->pDmaBufferPrivateData,
                                           submitCommand->DmaBufferPrivateDataSize,
                                           privateStart,
                                           privateEnd,
                                           adapter,
                                           submitCommand->hContext,
                                           -1,
                                           &transaction);
        if (!NT_SUCCESS(status))
        {
            submitFailureStage = VioGpuWddmPresentSubmitResolveTransaction;
        }
        BOOLEAN promotePrepatch = FALSE;
        if (NT_SUCCESS(status))
        {
            UINT dmaLength = submitCommand->DmaBufferSubmissionEndOffset -
                             submitCommand->DmaBufferSubmissionStartOffset;
            LONG state = InterlockedCompareExchange(&transaction->State, 0, 0);
            BOOLEAN patched = state == VioGpuWddmPresentPatched &&
                              transaction->FenceId == submitCommand->SubmissionFenceId;
            promotePrepatch = state == VioGpuWddmPresentBuilt && transaction->FullyPrepatched &&
                              transaction->FenceId == 0;
            BOOLEAN dmaRangeValid = ValidatePresentSubmitDmaRange(transaction,
                                                                  submitCommand->DmaBufferSize,
                                                                  submitCommand->DmaBufferSubmissionStartOffset,
                                                                  submitCommand->DmaBufferSubmissionEndOffset);
            submitFailureDetail = submitCommand->hContext == NULL ? 1U << 0 : 0;
            /* The Win7/WDDMv1 registration used by this miniport sends the
             * legacy Present form with no SubmitCommand flag bits. Newer
             * runtimes may add RedirectedPresent to the explicit Present bit;
             * no paging, flip, null-rendering, context-switch, or reserved bit
             * belongs to this CPU-copy Present path. */
            const UINT presentSubmitFlags = 0x6U;
            BOOLEAN presentFlagsValid = submitCommand->Flags.Value == 0U ||
                                        (submitCommand->Flags.Present != 0 &&
                                         (submitCommand->Flags.Value & ~presentSubmitFlags) == 0);
            submitFailureDetail |= !presentFlagsValid ? 1U << 1 : 0;
            submitFailureDetail |= privateLength != sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) ? 1U << 2 : 0;
            submitFailureDetail |= dmaLength != sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET) ? 1U << 3 : 0;
            submitFailureDetail |= !dmaRangeValid ? 1U << 4 : 0;
            submitFailureDetail |= transaction->DmaBuffer != privateData->DmaBuffer ? 1U << 5 : 0;
            submitFailureDetail |= submitCommand->DmaBufferSize - submitCommand->DmaBufferSubmissionStartOffset != transaction->DmaBufferSize
                                                                                                                                       ? 1U << 6
                                                                                                                                       : 0;
            submitFailureDetail |= privateLength != transaction->PrivateDataSize ? 1U << 7 : 0;
            submitFailureDetail |= transaction->PrivateData != privateData ? 1U << 8 : 0;
            submitFailureDetail |= !patched && !promotePrepatch ? 1U << 9 : 0;
            submitFailureDetail |= transaction->Context->NodeOrdinal != submitCommand->NodeOrdinal ? 1U << 10 : 0;
            submitFailureDetail |= transaction->Context->EngineAffinity != 1 ? 1U << 11 : 0;
            if (submitFailureDetail != 0)
            {
                /* Keep the low contract bits stable while retaining the exact
                 * WDDM flag word in the diagnostic-only upper half.  Encode it
                 * only after the low contract checks fail; otherwise a valid
                 * nonzero Present flag would turn its own diagnostic snapshot
                 * into a rejection. */
                submitFailureDetail |= (submitCommand->Flags.Value & 0xFFFFU) << 16;
                status = STATUS_INVALID_PARAMETER;
                submitFailureStage = VioGpuWddmPresentSubmitContract;
            }
        }

        if (NT_SUCCESS(status) && promotePrepatch)
        {
            transaction->FenceId = submitCommand->SubmissionFenceId;
            KeMemoryBarrier();
            LONG previous = InterlockedCompareExchange(&transaction->State,
                                                       VioGpuWddmPresentPatched,
                                                       VioGpuWddmPresentBuilt);
            if (previous != VioGpuWddmPresentBuilt)
            {
                status = STATUS_DEVICE_NOT_READY;
                submitFailureStage = VioGpuWddmPresentSubmitPrepatchTransition;
                submitFailureDetail = static_cast<DWORD>(previous);
            }
            else
            {
                LONG cancelRequested = InterlockedCompareExchange(&transaction->CancelRequested, 0, 0);
                if (cancelRequested != 0)
                {
                    status = STATUS_CANCELLED;
                    submitFailureStage = VioGpuWddmPresentSubmitCancelled;
                    submitFailureDetail = static_cast<DWORD>(cancelRequested);
                }
            }
        }

        BOOLEAN workReference = FALSE;
        BOOLEAN queued = FALSE;
        if (NT_SUCCESS(status))
        {
            workReference = AcquirePresentWorkReference(transaction);
            if (!workReference)
            {
                status = STATUS_DEVICE_NOT_READY;
                submitFailureStage = VioGpuWddmPresentSubmitWorkReference;
                submitFailureDetail = static_cast<DWORD>(InterlockedCompareExchange(&transaction->WorkReferenceHeld,
                                                                                    0,
                                                                                    0));
            }
        }
        if (NT_SUCCESS(status))
        {
            LONG previous = InterlockedCompareExchange(&transaction->State,
                                                       VioGpuWddmPresentQueued,
                                                       VioGpuWddmPresentPatched);
            if (previous == VioGpuWddmPresentPatched)
            {
                queued = adapter->QueueNativePassiveWork(&transaction->Work, submitCommand->SubmissionFenceId);
                if (queued)
                {
                    DereferencePresentTransaction(transaction);
                    adapter->ReleaseNativeSubmissionOperation();
                    return STATUS_SUCCESS;
                }
                submitFailureStage = VioGpuWddmPresentSubmitPassiveQueue;
                LONG workState = InterlockedCompareExchange(&transaction->Work.State, 0, 0);
                LONG cancelRequested = InterlockedCompareExchange(&transaction->CancelRequested, 0, 0);
                LONG retired = InterlockedCompareExchange(&transaction->Work.Retired, 0, 0);
                submitFailureDetail = static_cast<DWORD>(previous & 0xFF) |
                                      (static_cast<DWORD>(workState & 0xFF) << 8) |
                                      (static_cast<DWORD>(cancelRequested & 0xFF) << 16) |
                                      (static_cast<DWORD>(retired & 0xFF) << 24);
            }
            else
            {
                submitFailureStage = VioGpuWddmPresentSubmitQueueTransition;
                submitFailureDetail = static_cast<DWORD>(previous);
            }
            status = STATUS_DEVICE_NOT_READY;
        }

        if (transaction != NULL)
        {
            LONG state = InterlockedCompareExchange(&transaction->State, 0, 0);
            if (state == VioGpuWddmPresentBuilt || state == VioGpuWddmPresentPatched ||
                (workReference && state == VioGpuWddmPresentQueued))
            {
                RetirePresentTransaction(transaction, state, VioGpuWddmPresentCancelled);
            }
        }
        if (submitFailureStage == VioGpuWddmPresentSubmitNone)
        {
            submitFailureStage = VioGpuWddmPresentSubmitUnexpected;
        }
        adapter->NotifyNativeSubmissionFault(submitCommand->SubmissionFenceId,
                                             STATUS_DEVICE_NOT_READY,
                                             submitCommand->NodeOrdinal,
                                             submitCommand->EngineOrdinal,
                                             TRUE,
                                             static_cast<DWORD>(submitFailureStage),
                                             status,
                                             submitFailureDetail);
        if (transaction != NULL)
        {
            if (workReference)
            {
                ReleasePresentWorkReference(transaction);
            }
            DereferencePresentTransaction(transaction);
        }
        adapter->ReleaseNativeSubmissionOperation();
        return STATUS_SUCCESS;
    }
    else if (NT_SUCCESS(status) && privateData->Kind != VioGpuWddmDmaKindRender)
    {
        status = STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_SUBMISSION *submission = NULL;
    BOOLEAN promoteRenderPrepatch = FALSE;
    BOOLEAN submitClaimed = FALSE;
    BOOLEAN umdFenceRecorded = FALSE;
    BOOLEAN workReference = FALSE;
    BOOLEAN engineQueued = FALSE;
    if (NT_SUCCESS(status))
    {
        status = ResolveSubmissionPrivateData(submitCommand->pDmaBufferPrivateData,
                                              submitCommand->DmaBufferPrivateDataSize,
                                              privateStart,
                                              privateEnd,
                                              adapter,
                                              submitCommand->hContext,
                                              &submission);
    }
    if (NT_SUCCESS(status))
    {
        UINT dmaLength = submitCommand->DmaBufferSubmissionEndOffset - submitCommand->DmaBufferSubmissionStartOffset;
        LONG state = InterlockedCompareExchange(&submission->State, 0, 0);
        BOOLEAN patched = state == VioGpuWddmSubmissionPatched && submission->PatchApplied &&
                          submission->FenceId == submitCommand->SubmissionFenceId;
        promoteRenderPrepatch = state == VioGpuWddmSubmissionPrepared && !submission->PatchApplied &&
                                submission->FullyPrepatched && submission->FenceId == 0;
        BOOLEAN exact = submitCommand->hContext != NULL && submitCommand->Flags.Value == 0 &&
                        privateLength == sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) &&
                        ValidateRenderSubmitDmaRange(submission,
                                                     submitCommand->DmaBufferSize,
                                                     submitCommand->DmaBufferSubmissionStartOffset,
                                                     submitCommand->DmaBufferSubmissionEndOffset) &&
                        privateLength == submission->DmaPrivateDataSize && dmaLength == submission->CommandLength &&
                        submission->Context != NULL && submission->Context->NodeOrdinal == submitCommand->NodeOrdinal &&
                        submission->Context->EngineAffinity == 1 && (patched || promoteRenderPrepatch) &&
                        InterlockedCompareExchange(&submission->CancelRequested, 0, 0) == 0;
        if (!exact)
        {
            status = STATUS_INVALID_PARAMETER;
        }
    }

    if (NT_SUCCESS(status))
    {
        LONG expectedState = promoteRenderPrepatch ? VioGpuWddmSubmissionPrepared : VioGpuWddmSubmissionPatched;
        submitClaimed = InterlockedCompareExchange(&submission->State,
                                                   VioGpuWddmSubmissionSubmitClaimed,
                                                   expectedState) == expectedState;
        if (!submitClaimed)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
    }

    if (NT_SUCCESS(status) && promoteRenderPrepatch)
    {
        submission->FenceId = submitCommand->SubmissionFenceId;
        KeMemoryBarrier();
    }
    if (NT_SUCCESS(status))
    {
        umdFenceRecorded = RecordContextUmdFence(submission->Context, submission->UmdFenceId);
        workReference = umdFenceRecorded && AcquireRenderWorkReference(submission);
        if (!umdFenceRecorded || !workReference || InterlockedCompareExchange(&submission->CancelRequested, 0, 0) != 0)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
    }

    if (NT_SUCCESS(status))
    {
        engineQueued = InterlockedCompareExchange(&submission->State,
                                                  VioGpuWddmSubmissionEngineQueued,
                                                  VioGpuWddmSubmissionSubmitClaimed) ==
                       VioGpuWddmSubmissionSubmitClaimed;
        if (!engineQueued)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
    }
    if (NT_SUCCESS(status) && adapter->QueueNativePassiveWork(&submission->Work, submitCommand->SubmissionFenceId))
    {
        DereferenceRenderSubmission(submission);
        adapter->ReleaseNativeSubmissionOperation();
        return STATUS_SUCCESS;
    }

    if (umdFenceRecorded && submission != NULL && submission->Context != NULL)
    {
        InvalidateContextUmdFenceTracker(submission->Context);
    }
    if (submission != NULL)
    {
        if (engineQueued)
        {
            QuarantineSubmission(submission, VioGpuWddmSubmissionEngineQueued, TRUE);
        }
        else if (submitClaimed)
        {
            InterlockedExchange(&submission->CancelRequested, 1);
            adapter->RequestHardwareResetAtAnyIrql();
        }
        if (workReference)
        {
            ReleaseRenderWorkReference(submission);
        }
        DereferenceRenderSubmission(submission);
    }
    RetireUnsubmittedDmaOwner(adapter, submitCommand);
    adapter->NotifyNativeSubmissionFault(submitCommand->SubmissionFenceId,
                                         STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                         submitCommand->NodeOrdinal,
                                         submitCommand->EngineOrdinal,
                                         TRUE);
    adapter->ReleaseNativeSubmissionOperation();

    /* Dxgkrnl bugchecks on any SubmitCommand error.  Invalid or stale KMD
     * private data is converted into a scheduler fault/reset notification. */
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmCancelCommand(CONST HANDLE hAdapter,
                                                                 CONST DXGKARG_CANCELCOMMAND *cancelCommand)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    BOOLEAN cleaned = FALSE;
    if (adapter != NULL && cancelCommand != NULL && cancelCommand->pDmaBufferPrivateData != NULL &&
        cancelCommand->DmaBufferPrivateDataSubmissionStartOffset <= cancelCommand->DmaBufferPrivateDataSubmissionEndOffset &&
        cancelCommand->DmaBufferPrivateDataSubmissionEndOffset <= cancelCommand->DmaBufferPrivateDataSize)
    {
        UINT privateStart = cancelCommand->DmaBufferPrivateDataSubmissionStartOffset;
        UINT privateEnd = cancelCommand->DmaBufferPrivateDataSubmissionEndOffset;
        UINT privateLength = privateEnd - privateStart;
        if (cancelCommand->hContext == NULL && cancelCommand->pDmaBuffer != NULL &&
            cancelCommand->DmaBufferSubmissionStartOffset <= cancelCommand->DmaBufferSubmissionEndOffset &&
            cancelCommand->DmaBufferSubmissionEndOffset <= cancelCommand->DmaBufferSize)
        {
            VIOGPU_WDDM_PAGING_PRIVATE *firstPrivate = NULL;
            UINT recordCount = 0;
            if (ResolvePagingBatch(cancelCommand->pDmaBuffer,
                                   cancelCommand->DmaBufferSize,
                                   cancelCommand->DmaBufferSubmissionStartOffset,
                                   cancelCommand->DmaBufferSubmissionEndOffset,
                                   cancelCommand->pDmaBufferPrivateData,
                                   cancelCommand->DmaBufferPrivateDataSize,
                                   privateStart,
                                   privateEnd,
                                   adapter,
                                   VioGpuWddmPagingTransactionAny,
                                   &firstPrivate,
                                   &recordCount))
            {
                VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP ownership = adapter->CancelNativePassiveWork(&firstPrivate->Work);
                UINT recordLimit = privateEnd >= sizeof(VIOGPU_WDDM_PAGING_PRIVATE) ? privateEnd - sizeof(VIOGPU_WDDM_PAGING_PRIVATE)
                                                                                    : 0;
                for (UINT index = 0; index < recordCount; ++index)
                {
                    UINT recordOffset = 0;
                    if (!ResolvePagingBatchOffset(privateStart,
                                                  index,
                                                  sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                                  recordLimit,
                                                  &recordOffset))
                    {
                        continue;
                    }
                    VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(cancelCommand->pDmaBufferPrivateData) +
                                                                                                               recordOffset);
                    cleaned = CancelRecognizedPagingTransaction(pagingPrivate, adapter) || cleaned;
                }
                if (ownership == VioGpuNativePassiveWorkRemoved)
                {
                    adapter->CompleteNativeSystemSubmission(firstPrivate->BatchFenceId,
                                                            firstPrivate->BatchNodeOrdinal,
                                                            firstPrivate->BatchEngineOrdinal);
                    adapter->RequestHardwareResetAtAnyIrql();
                }
            }
            else if ((privateLength % sizeof(VIOGPU_WDDM_PAGING_PRIVATE)) == 0)
            {
                UINT recordCountFallback = privateLength / sizeof(VIOGPU_WDDM_PAGING_PRIVATE);
                UINT recordLimit = privateEnd >= sizeof(VIOGPU_WDDM_PAGING_PRIVATE) ? privateEnd - sizeof(VIOGPU_WDDM_PAGING_PRIVATE)
                                                                                    : 0;
                VIOGPU_WDDM_PAGING_PRIVATE *fallbackFirstPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(cancelCommand->pDmaBufferPrivateData) +
                                                                                                                  privateStart);
                VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP ownership = VioGpuNativePassiveWorkNotQueued;
                if (recordCountFallback != 0 && IsRecognizedPagingOwner(fallbackFirstPrivate, adapter))
                {
                    ownership = adapter->CancelNativePassiveWork(&fallbackFirstPrivate->Work);
                }
                for (UINT index = 0; index < recordCountFallback; ++index)
                {
                    UINT recordOffset = 0;
                    if (!ResolvePagingBatchOffset(privateStart,
                                                  index,
                                                  sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
                                                  recordLimit,
                                                  &recordOffset))
                    {
                        continue;
                    }
                    VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = reinterpret_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(static_cast<BYTE *>(cancelCommand->pDmaBufferPrivateData) +
                                                                                                               recordOffset);
                    cleaned = CancelRecognizedPagingTransaction(pagingPrivate, adapter) || cleaned;
                }
                if (ownership == VioGpuNativePassiveWorkRemoved)
                {
                    adapter->CompleteNativeSystemSubmission(fallbackFirstPrivate->BatchFenceId,
                                                            fallbackFirstPrivate->BatchNodeOrdinal,
                                                            fallbackFirstPrivate->BatchEngineOrdinal);
                    adapter->RequestHardwareResetAtAnyIrql();
                }
            }
        }
        else if (cancelCommand->hContext != NULL && privateLength == sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE))
        {
            VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = reinterpret_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(static_cast<BYTE *>(cancelCommand->pDmaBufferPrivateData) +
                                                                                                       privateStart);
            if (privateData->Kind == VioGpuWddmDmaKindRender)
            {
                VIOGPU_WDDM_SUBMISSION *submission = NULL;
                if (NT_SUCCESS(ResolveSubmissionPrivateData(cancelCommand->pDmaBufferPrivateData,
                                                            cancelCommand->DmaBufferPrivateDataSize,
                                                            privateStart,
                                                            privateEnd,
                                                            adapter,
                                                            cancelCommand->hContext,
                                                            &submission)))
                {
                    BOOLEAN exact = submission->DmaPrivateDataSize == privateLength &&
                                    ValidateRenderDmaSubmissionRange(submission,
                                                                     cancelCommand->pDmaBuffer,
                                                                     cancelCommand->DmaBufferSize,
                                                                     cancelCommand->DmaBufferSubmissionStartOffset,
                                                                     cancelCommand->DmaBufferSubmissionEndOffset);
                    if (exact)
                    {
                        LONG state = InterlockedCompareExchange(&submission->State, 0, 0);
                        if (state == VioGpuWddmSubmissionPrepared || state == VioGpuWddmSubmissionPatched)
                        {
                            cleaned = QuarantineSubmission(submission, state, TRUE);
                        }
                        else if (state == VioGpuWddmSubmissionEngineQueued)
                        {
                            InterlockedExchange(&submission->CancelRequested, 1);
                            UINT fenceId = static_cast<UINT>(submission->FenceId);
                            UINT nodeOrdinal = submission->Context->NodeOrdinal;
                            VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP ownership = adapter->CancelNativePassiveWork(&submission->Work);
                            if (ownership == VioGpuNativePassiveWorkRemoved)
                            {
                                NativeRenderDispatchCancelled(submission);
                                adapter->NotifyNativeSubmissionFault(fenceId,
                                                                     STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                                                     nodeOrdinal,
                                                                     0,
                                                                     TRUE);
                            }
                            cleaned = ownership != VioGpuNativePassiveWorkNotQueued;
                        }
                        else if (state == VioGpuWddmSubmissionPatching || state == VioGpuWddmSubmissionSubmitClaimed ||
                                 state == VioGpuWddmSubmissionHostIssued)
                        {
                            InterlockedExchange(&submission->CancelRequested, 1);
                            adapter->RequestHardwareResetAtAnyIrql();
                            cleaned = TRUE;
                        }
                    }
                    DereferenceRenderSubmission(submission);
                }
            }
            else if (privateData->Kind == VioGpuWddmDmaKindPresent)
            {
                VIOGPU_WDDM_PRESENT_TRANSACTION *transaction = NULL;
                if (NT_SUCCESS(ResolvePresentTransaction(cancelCommand->pDmaBufferPrivateData,
                                                         cancelCommand->DmaBufferPrivateDataSize,
                                                         privateStart,
                                                         privateEnd,
                                                         adapter,
                                                         cancelCommand->hContext,
                                                         -1,
                                                         &transaction)))
                {
                    if (transaction->PrivateDataSize == privateLength &&
                        ValidatePresentDmaSubmissionRange(transaction,
                                                          cancelCommand->pDmaBuffer,
                                                          cancelCommand->DmaBufferSize,
                                                          cancelCommand->DmaBufferSubmissionStartOffset,
                                                          cancelCommand->DmaBufferSubmissionEndOffset))
                    {
                        LONG state = InterlockedCompareExchange(&transaction->State, 0, 0);
                        if (state == VioGpuWddmPresentBuilt || state == VioGpuWddmPresentPatched)
                        {
                            cleaned = RetirePresentTransaction(transaction, state, VioGpuWddmPresentCancelled);
                        }
                        else if (state == VioGpuWddmPresentQueued || state == VioGpuWddmPresentExecuting)
                        {
                            InterlockedExchange(&transaction->CancelRequested, 1);
                            VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP ownership = adapter->CancelNativePassiveWork(&transaction->Work);
                            if (ownership == VioGpuNativePassiveWorkRemoved)
                            {
                                UINT fenceId = transaction->FenceId;
                                UINT nodeOrdinal = transaction->Context->NodeOrdinal;
                                cleaned = RetirePresentTransaction(transaction,
                                                                   VioGpuWddmPresentQueued,
                                                                   VioGpuWddmPresentCancelled);
                                if (cleaned)
                                {
                                    adapter->NotifyNativeSubmissionFault(fenceId,
                                                                         STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                                                         nodeOrdinal,
                                                                         0,
                                                                         TRUE);
                                }
                                ReleasePresentWorkReference(transaction);
                            }
                            else
                            {
                                cleaned = TRUE;
                            }
                        }
                    }
                    DereferencePresentTransaction(transaction);
                }
            }
        }
    }

    if (!cleaned && adapter != NULL)
    {
        /* CancelCommand must return success or Dxgkrnl bugchecks.  A malformed
         * owner is treated as a transport fault while the scheduler performs
         * its reset cleanup. */
        adapter->RequestHardwareResetAtAnyIrql();
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmPreemptCommand(CONST HANDLE hAdapter,
                                                                  CONST DXGKARG_PREEMPTCOMMAND *preemptCommand)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL)
    {
        /* Dxgkrnl bugchecks on a PreemptCommand error return. */
        return STATUS_SUCCESS;
    }

    BOOLEAN valid = preemptCommand != NULL && preemptCommand->PreemptionFenceId != 0 &&
                    preemptCommand->NodeOrdinal == 0 && preemptCommand->EngineOrdinal == 0 &&
                    preemptCommand->Flags.Value == 0;
    if (!valid)
    {
        /* A malformed request is a real transport fault, not a scheduling
         * event.  Gate the transport and let the scheduler reset the adapter. */
        adapter->CountNativePreemptReset();
        adapter->ResetDevice();
        return STATUS_SUCCESS;
    }
    if (adapter->IsHardwareResetRequested())
    {
        /* Already gated.  Resetting again would only re-latch a state that
         * nothing but the scheduler's own recovery can clear. */
        return STATUS_SUCCESS;
    }
    if (!adapter->IsNativeFenceQueueEmpty())
    {
        /* Native Context has no Host cancellation primitive, so this preemption
         * cannot abort the in-flight command - but the adapter is healthy, and
         * a preemption request is the normal way the scheduler reclaims a busy
         * engine.  Resetting the device here latched a gate that only
         * SetPowerState clears, so every later present returned
         * STATUS_DEVICE_NOT_READY and the desktop froze after its first frames
         * with the cursor plane still live.  Latch the fence instead and report
         * it once the queue drains, when nothing from the preempted packet can
         * still reach guest memory. */
        if (!adapter->DeferNativePreemption(preemptCommand->PreemptionFenceId))
        {
            adapter->CountNativePreemptReset();
            adapter->ResetDevice();
        }
        return STATUS_SUCCESS;
    }

    DXGKARGCB_NOTIFY_INTERRUPT_DATA notify = {};
    notify.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    notify.DmaPreempted.PreemptionFenceId = preemptCommand->PreemptionFenceId;
    notify.DmaPreempted.LastCompletedFenceId = adapter->QueryNativeCompletedFence();
    notify.DmaPreempted.NodeOrdinal = preemptCommand->NodeOrdinal;
    notify.DmaPreempted.EngineOrdinal = preemptCommand->EngineOrdinal;
    if (!adapter->NotifyNativeSchedulerInterrupt(&notify, TRUE))
    {
        adapter->CountNativePreemptReset();
        adapter->ResetDevice();
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmQueryCurrentFence(CONST HANDLE hAdapter,
                                                                     DXGKARG_QUERYCURRENTFENCE *currentFence)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || currentFence == NULL || currentFence->NodeOrdinal != 0 || currentFence->EngineOrdinal != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    currentFence->CurrentFence = adapter->QueryNativeCompletedFence();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmCollectDbgInfo(CONST HANDLE hAdapter,
                                                                  CONST DXGKARG_COLLECTDBGINFO *collectDbgInfo)
{
    if (collectDbgInfo == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }

    if (collectDbgInfo->pExtension != NULL)
    {
        RtlZeroMemory(collectDbgInfo->pExtension, sizeof(*collectDbgInfo->pExtension));
    }
    if (collectDbgInfo->pBuffer == NULL || collectDbgInfo->BufferSize == 0)
    {
        return STATUS_SUCCESS;
    }

    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    VIOGPU_WDDM_DEBUG_SNAPSHOT snapshot = {};
    snapshot.Signature = VIOGPU_WDDM_DEBUG_SIGNATURE;
    snapshot.Version = VioGpuWddmDebugSnapshotVersion;
    snapshot.Size = static_cast<USHORT>(sizeof(snapshot));
    snapshot.Reason = collectDbgInfo->Reason;
    snapshot.CurrentIrql = static_cast<UINT>(KeGetCurrentIrql());
    if (adapter != NULL)
    {
        snapshot.HardwareResetState = adapter->QueryHardwareResetState();
        snapshot.SubmittedFence = adapter->QueryNativeSubmittedFence();
        snapshot.CompletedFence = adapter->QueryNativeCompletedFence();
    }

    SIZE_T copySize = collectDbgInfo->BufferSize < sizeof(snapshot) ? collectDbgInfo->BufferSize : sizeof(snapshot);
    RtlCopyMemory(collectDbgInfo->pBuffer, &snapshot, copySize);
    return STATUS_SUCCESS;
}

#pragma code_seg(push)
#pragma code_seg("PAGE")

_Use_decl_annotations_ NTSTATUS APIENTRY
VioGpuWddmQueryDependentEngineGroup(CONST HANDLE hAdapter, DXGKARG_QUERYDEPENDENTENGINEGROUP *queryDependentEngineGroup)
{
    PAGED_CODE();

    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (queryDependentEngineGroup != NULL)
    {
        BOOLEAN valid = adapter != NULL && queryDependentEngineGroup->NodeOrdinal == 0 &&
                        queryDependentEngineGroup->EngineOrdinal == 0;
        queryDependentEngineGroup->DependentNodeOrdinalMask = valid ? 1ULL : 0ULL;
        if (!valid && adapter != NULL)
        {
            adapter->RequestHardwareResetAtAnyIrql();
        }
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmQueryEngineStatus(CONST HANDLE hAdapter,
                                                                     DXGKARG_QUERYENGINESTATUS *queryEngineStatus)
{
    PAGED_CODE();

    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (queryEngineStatus != NULL)
    {
        queryEngineStatus->EngineStatus.Value = 0;
        BOOLEAN valid = adapter != NULL && queryEngineStatus->NodeOrdinal == 0 && queryEngineStatus->EngineOrdinal == 0;
        if (valid)
        {
            queryEngineStatus->EngineStatus.Responsive = !adapter->IsHardwareResetRequested();
        }
        else if (adapter != NULL)
        {
            adapter->RequestHardwareResetAtAnyIrql();
        }
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmResetEngine(CONST HANDLE hAdapter, DXGKARG_RESETENGINE *resetEngine)
{
    PAGED_CODE();

    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (resetEngine == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    resetEngine->LastAbortedFenceId = adapter == NULL ? 0 : adapter->QueryNativeCompletedFence();
    if (adapter == NULL || resetEngine->NodeOrdinal != 0 || resetEngine->EngineOrdinal != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

#if defined(VIOGPU_WDDM_TEST_IMPLEMENTATIONS)
    /* Compile-time experiment only.  The Host exposes one queue, so this
     * deliberately performs the adapter-wide recovery path and is never
     * enabled in the product build as a claim of per-engine reset support. */
    adapter->RequestHardwareResetAtAnyIrql();
    return adapter->ResetFromTimeout();
#else
    /* Native Context cannot cancel/reset one Host engine independently.  The
     * WDDM 1.2 engine-reset contract permits failure here so the scheduler
     * promotes recovery to ResetFromTimeout/RestartFromTimeout. */
    adapter->RequestHardwareResetAtAnyIrql();
    return STATUS_NOT_SUPPORTED;
#endif
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmResetFromTimeout(CONST HANDLE hAdapter)
{
    PAGED_CODE();

    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    return adapter == NULL ? STATUS_SUCCESS : adapter->ResetFromTimeout();
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmRestartFromTimeout(CONST HANDLE hAdapter)
{
    PAGED_CODE();

    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    return adapter == NULL ? STATUS_SUCCESS : adapter->RestartFromTimeout();
}

#pragma code_seg(pop)

_Use_decl_annotations_ NTSTATUS APIENTRY
VioGpuWddmSetVidPnSourceAddress(CONST HANDLE hAdapter, CONST DXGKARG_SETVIDPNSOURCEADDRESS *setVidPnSourceAddress)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || setVidPnSourceAddress == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        setVidPnSourceAddress->VidPnSourceId != 0 || setVidPnSourceAddress->ContextCount != 0 ||
        setVidPnSourceAddress->Flags.Value != 1U)
    {
        return STATUS_INVALID_PARAMETER;
    }

    UINT previousResourceId = 0;
    if (setVidPnSourceAddress->hAllocation == NULL)
    {
        if (setVidPnSourceAddress->PrimarySegment != 0 || setVidPnSourceAddress->PrimaryAddress.QuadPart != 0)
        {
            return STATUS_INVALID_PARAMETER;
        }
        VIOGPU_HOST_CONTEXT_RESULT result = adapter->Set2DScanout(0, 0, 0, 0, &previousResourceId);
        if (result == VioGpuHostContextConfirmed)
        {
            adapter->SetCrtcVsyncPrimaryAddress(0);
        }
        return result == VioGpuHostContextConfirmed ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(setVidPnSourceAddress->hAllocation);
    if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
        allocation->Adapter != adapter || setVidPnSourceAddress->PrimarySegment != VIOGPU_WDDM_SEGMENT_ID ||
        setVidPnSourceAddress->PrimaryAddress.QuadPart < 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status = AcquireAllocationLifecycle(allocation);
    if (status != STATUS_SUCCESS)
    {
        return status;
    }
    if (allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE || allocation->Adapter != adapter ||
        !IsStandardPrimaryAllocation(allocation) || allocation->ResourceId == 0 ||
        allocation->ResourceId >= VIOGPU_NATIVE_RESOURCE_ID_START || allocation->BlobId != 0 ||
        !EnsureStandard2DAllocationBacking(allocation) ||
        allocation->Resource2DState != VioGpu2DResourceBackingAttached || !allocation->PlacementValid ||
        static_cast<ULONGLONG>(setVidPnSourceAddress->PrimaryAddress.QuadPart) != allocation->PlacementOffset)
    {
        status = STATUS_INVALID_PARAMETER;
    }
    else
    {
        VIOGPU_HOST_CONTEXT_RESULT result = adapter->Set2DScanout(0,
                                                                  allocation->ResourceId,
                                                                  allocation->Width,
                                                                  allocation->Height,
                                                                  &previousResourceId);
        if (result == VioGpuHostContextConfirmed)
        {
            /* The vsync report carries the primary dxgkrnl programmed here. */
            adapter->SetCrtcVsyncPrimaryAddress(
                static_cast<ULONGLONG>(setVidPnSourceAddress->PrimaryAddress.QuadPart));
        }
        status = result == VioGpuHostContextConfirmed ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    }
    KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
    return status;
}
