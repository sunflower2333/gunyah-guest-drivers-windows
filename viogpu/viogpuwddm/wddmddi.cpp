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
const ULONG VIOGPU_WDDM_PAGING_DMA_SIGNATURE = 'pWGV';
const ULONG VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE = 'tWGV';
const ULONG VIOGPU_WDDM_SUBMISSION_SIGNATURE = 'sWGV';

const UINT VIOGPU_WDDM_SEGMENT_ID = 1;
const UINT VIOGPU_WDDM_DMA_BUFFER_SIZE = 64 * 1024;
const UINT VIOGPU_WDDM_ALLOCATION_LIST_SIZE = VioGpuWddmSubmissionAllocationLimit;
const UINT VIOGPU_WDDM_PATCH_LIST_SIZE = 128;
const LONG VIOGPU_WDDM_DEVICE_CLOSING = static_cast<LONG>(0x80000000UL);
const LONG VIOGPU_WDDM_DEVICE_REFERENCE_MASK = 0x7FFFFFFF;

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

VOID NativeSubmissionComplete(_In_opt_ PVOID callbackContext);
VOID NativeSubmissionCancelled(_In_opt_ PVOID callbackContext);
VOID NativeSubmissionQueueFailed(_In_opt_ PVOID callbackContext);

void DereferenceDevice(VIOGPU_WDDM_DEVICE *device);

LONG ReadResourceAllocationCount(_In_ const VIOGPU_WDDM_RESOURCE *resource)
{
    return resource == NULL ? -1
                            : InterlockedCompareExchange(const_cast<volatile LONG *>(&resource->AllocationCount), 0, 0);
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
        transaction->Adapter == NULL || transaction->Allocation == NULL ||
        transaction->Operation != packet->Operation || transaction->Flags != packet->Flags ||
        transaction->ResourceId != packet->ResourceId || transaction->ContextId != packet->ContextId ||
        transaction->ContextGeneration != packet->ContextGeneration ||
        transaction->ResetGeneration != packet->ResetGeneration ||
        ((packet->Flags & (VioGpuWddmPagingFlagPageIn | VioGpuWddmPagingFlagPageOut)) != 0 &&
         !transaction->TransferDataComplete) ||
        transaction->PlacementOffset != packet->PlacementOffset ||
        transaction->PoolGeneration != packet->PoolGeneration ||
        transaction->TransferOffset != packet->TransferOffset || transaction->TransferSize != packet->TransferSize ||
        packet->Signature != VIOGPU_WDDM_PAGING_DMA_SIGNATURE || packet->Version != VioGpuWddmDmaPrivateVersion ||
        packet->Size != sizeof(*packet) || packet->Flags != privateData->Flags ||
        packet->ContextId != privateData->ContextId || packet->ContextGeneration != privateData->Generation ||
        packet->ResetGeneration != privateData->ResetGeneration || packet->Reserved != 0 ||
        packet->ResourceId < VIOGPU_NATIVE_RESOURCE_ID_START || packet->ResourceId == MAXUINT ||
        packet->PoolGeneration == 0 || packet->TransferOffset > MAXUINT || packet->TransferSize > MAXULONG ||
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

    if (packet->Operation == DXGK_OPERATION_TRANSFER)
    {
        BOOLEAN pageIn = operationFlags == VioGpuWddmPagingFlagPageIn;
        BOOLEAN pageOut = operationFlags == VioGpuWddmPagingFlagPageOut;
        if ((!pageIn && !pageOut) || !hasContext || packet->TransferSize == 0 ||
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
               (packet->Flags & VioGpuWddmPagingFlagAllocationIdle) == 0 && hasContext && packet->TransferOffset == 0 &&
               packet->TransferSize != 0 && packet->TransferSize - 1 <= MAXULONGLONG - packet->PlacementOffset;
    }

    if (packet->Operation == DXGK_OPERATION_DISCARD_CONTENT)
    {
        return operationFlags == VioGpuWddmPagingFlagDiscard && transferFlags == 0 && packet->TransferOffset == 0 &&
               packet->TransferSize == 0;
    }

    return FALSE;
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
VOID ClearPagingRanges(VIOGPU_WDDM_ALLOCATION *allocation);

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
            if (allocation->Resource != NULL)
            {
                LONG remaining = InterlockedDecrement(&allocation->Resource->AllocationCount);
                NT_ASSERT(remaining >= 0);
                UNREFERENCED_PARAMETER(remaining);
            }
            ClearPagingRanges(allocation);
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

    VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration = allocation->NativeContext;
    KIRQL oldIrql;
    KeAcquireSpinLock(&registration->BindingLock, &oldIrql);
    BOOLEAN valid = registration->Adapter != NULL && registration->Adapter->GetVioGpu() == allocation->Adapter &&
                    registration->Owner != NULL && registration->Registered &&
                    registration->Generation == allocation->ContextGeneration &&
                    registration->ResetGeneration == allocation->ContextResetGeneration &&
                    registration->ContextId == allocation->ContextId &&
                    InterlockedCompareExchange(&registration->State,
                                               VioGpuNativeContextDead,
                                               VioGpuNativeContextDead) == VioGpuNativeContextLive;
    if (valid)
    {
        for (PLIST_ENTRY entry = registration->AllocationRanges.Flink; entry != &registration->AllocationRanges;
             entry = entry->Flink)
        {
            VIOGPU_WDDM_ALLOCATION_RANGE *existing = CONTAINING_RECORD(entry, VIOGPU_WDDM_ALLOCATION_RANGE, Link);
            ULONGLONG rangeEnd = range->Iova + (ULONGLONG)range->Length - 1;
            ULONGLONG existingEnd = existing->Iova + (ULONGLONG)existing->Length - 1;
            if (range->Iova <= existingEnd && existing->Iova <= rangeEnd)
            {
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

VOID ClearPagingRanges(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL)
    {
        return;
    }
    while (!IsListEmpty(&allocation->PagingRanges))
    {
        PLIST_ENTRY entry = RemoveHeadList(&allocation->PagingRanges);
        VIOGPU_WDDM_PAGING_RANGE *range = CONTAINING_RECORD(entry, VIOGPU_WDDM_PAGING_RANGE, Link);
        delete range;
    }
    allocation->PagingCoveredBytes = 0;
}

NTSTATUS AddPagingRange(VIOGPU_WDDM_ALLOCATION *allocation,
                        SIZE_T offset,
                        SIZE_T size,
                        VIOGPU_WDDM_PAGING_RANGE **rangeOut)
{
    if (allocation == NULL || rangeOut == NULL || size == 0 || offset > allocation->BackingSize ||
        size > allocation->BackingSize - offset)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *rangeOut = NULL;
    VIOGPU_WDDM_PAGING_RANGE *range = new (NonPagedPoolNx) VIOGPU_WDDM_PAGING_RANGE;
    if (range == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    range->Offset = offset;
    range->Length = size;
    InitializeListHead(&range->Link);
    for (PLIST_ENTRY entry = allocation->PagingRanges.Flink; entry != &allocation->PagingRanges; entry = entry->Flink)
    {
        VIOGPU_WDDM_PAGING_RANGE *existing = CONTAINING_RECORD(entry, VIOGPU_WDDM_PAGING_RANGE, Link);
        if (offset < existing->Offset ? existing->Offset - offset < size : offset - existing->Offset < existing->Length)
        {
            delete range;
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (allocation->PagingCoveredBytes > allocation->BackingSize - size)
    {
        delete range;
        return STATUS_INVALID_PARAMETER;
    }
    InsertTailList(&allocation->PagingRanges, &range->Link);
    allocation->PagingCoveredBytes += size;
    *rangeOut = range;
    return STATUS_SUCCESS;
}

VOID RemovePagingRange(VIOGPU_WDDM_ALLOCATION *allocation, VIOGPU_WDDM_PAGING_RANGE *range)
{
    if (allocation == NULL || range == NULL)
    {
        return;
    }
    RemoveEntryList(&range->Link);
    allocation->PagingCoveredBytes -= range->Length;
    delete range;
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

NTSTATUS AcquireAllocationLifecycle(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (allocation == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&allocation->LifecycleMutex, Executive, KernelMode, FALSE, &timeout);
    if (!NT_SUCCESS(status))
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
    UINT completed = context->Signature == VIOGPU_WDDM_CONTEXT_SIGNATURE
                         ? static_cast<UINT>(context->CompletedUmdFence)
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
        if (context->SubmissionReferences != 0)
        {
            status = STATUS_GRAPHICS_ALLOCATION_BUSY;
        }
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    return status;
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
        VIOGPU_WDDM_ALLOCATION *allocation = deviceAllocation == NULL || deviceAllocation->Signature != VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE ? NULL
                                                                                                                                              : deviceAllocation->Allocation;
        NTSTATUS status = AcquireAllocationLifecycle(allocation);
        if (NT_SUCCESS(status))
        {
            status = AcquireAllocationSubmissionReference(allocation, device->Adapter);
            KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        }
        if (!NT_SUCCESS(status))
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
                                   const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot)
{
    if (submission == NULL || context == NULL || header == NULL || device == NULL || allocationList == NULL ||
        snapshot == NULL || snapshot->Adapter == NULL || allocationCount == 0 ||
        allocationCount > VioGpuWddmSubmissionAllocationLimit || allocationCount != header->AllocationReferenceCount ||
        dmaBuffer == NULL || dmaBufferSize < commandLength || dmaPrivateData == NULL ||
        dmaPrivateDataSize < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) || commandLength == 0 ||
        header->CommandStreamSize < sizeof(MSM_CCMD_GEM_SUBMIT_REQ) ||
        header->CommandStreamOffset >= commandLength ||
        header->CommandStreamSize > commandLength - header->CommandStreamOffset || virtioBuffer == NULL ||
        snapshot->ContextId == 0 || snapshot->Generation <= 0 || snapshot->ResetGeneration == 0 ||
        !snapshot->Adapter->IsNativeContextGenerationCurrent(snapshot->Generation, snapshot->ResetGeneration))
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(submission, sizeof(*submission));
    submission->Signature = VIOGPU_WDDM_SUBMISSION_SIGNATURE;
    InitializeListHead(&submission->ContextLink);
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
    const MSM_CCMD_GEM_SUBMIT_REQ *submitRequest = reinterpret_cast<const MSM_CCMD_GEM_SUBMIT_REQ *>(
        static_cast<const BYTE *>(dmaBuffer) + header->CommandStreamOffset);
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
    submission->State = VioGpuWddmSubmissionPrepared;
    submission->AllocationCount = 0;
    VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = static_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(dmaPrivateData);
    if (privateData->Submission != NULL)
    {
        submission->Signature = 0;
        return STATUS_INVALID_DEVICE_STATE;
    }

    const VIOGPU_WDDM_ALLOCATION_REFERENCE *references = reinterpret_cast<const VIOGPU_WDDM_ALLOCATION_REFERENCE *>(
                                                                                                        reinterpret_cast<const BYTE *>(header) +
                                                                                                        header->AllocationReferencesOffset);
    for (UINT index = 0; index < allocationCount; ++index)
    {
        const VIOGPU_WDDM_ALLOCATION_REFERENCE *reference = &references[index];
        if (reference->AllocationIndex >= allocationListSize)
        {
            submission->Signature = 0;
            return STATUS_INVALID_HANDLE;
        }
        VIOGPU_WDDM_OPEN_ALLOCATION *deviceAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationList[reference->AllocationIndex].hDeviceSpecificAllocation);
        if (deviceAllocation == NULL || deviceAllocation->Signature != VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE ||
            deviceAllocation->Device != device || deviceAllocation->Allocation == NULL ||
            deviceAllocation->Allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
            deviceAllocation->Allocation->Adapter != device->Adapter)
        {
            submission->Signature = 0;
            return STATUS_INVALID_HANDLE;
        }
        submission->Allocations[index] = deviceAllocation->Allocation;
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
        InsertTailList(&context->PendingSubmissions, &submission->ContextLink);
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    if (!valid)
    {
        submission->Signature = 0;
        submission->AllocationCount = 0;
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

BOOLEAN QuarantineSubmission(VIOGPU_WDDM_SUBMISSION *submission, LONG expectedState, BOOLEAN releaseBuffer)
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
    BOOLEAN linked = owned && submission->ContextLink.Flink != &submission->ContextLink &&
                     submission->ContextLink.Blink != &submission->ContextLink;
    if (linked)
    {
        RemoveEntryList(&submission->ContextLink);
        InitializeListHead(&submission->ContextLink);
        InterlockedExchange(&submission->State, VioGpuWddmSubmissionQuarantined);
        virtioBuffer = submission->VirtioBuffer;
        submission->VirtioBuffer = NULL;
        if (virtioBuffer != NULL)
        {
            virtioBuffer->complete_cb = NULL;
            virtioBuffer->complete_ctx = NULL;
            virtioBuffer->cancel_cb = NULL;
            virtioBuffer->cancel_ctx = NULL;
            virtioBuffer->queue_error_cb = NULL;
            virtioBuffer->queue_error_ctx = NULL;
        }
    }
    KeReleaseSpinLock(&context->SubmissionLock, oldIrql);
    if (!linked)
    {
        return FALSE;
    }

    for (UINT index = 0; index < submission->AllocationCount; ++index)
    {
        ReleaseAllocationSubmissionReference(submission->Allocations[index]);
        submission->Allocations[index] = NULL;
    }
    submission->AllocationCount = 0;
    BOOLEAN released = ReleaseContextSubmissionReference(context);
    NT_ASSERT(released);
    UNREFERENCED_PARAMETER(released);
    if (submission->DmaPrivateData != NULL && submission->DmaPrivateDataSize >= sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE))
    {
        VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = static_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(submission->DmaPrivateData);
        if (privateData->Submission == submission)
        {
            privateData->Submission = NULL;
        }
    }
    VioGpuDod *adapter = submission->Adapter;
    submission->Context = NULL;
    submission->Signature = 0;
    delete submission;
    if (releaseBuffer && virtioBuffer != NULL && adapter != NULL)
    {
        adapter->ReleaseNativeSubmitBuffer(virtioBuffer);
    }
    return TRUE;
}

VOID ReleasePreparedSubmission(VIOGPU_WDDM_SUBMISSION *submission)
{
    QuarantineSubmission(submission, VioGpuWddmSubmissionPrepared, TRUE);
}

BOOLEAN ReleaseQueuedSubmission(VIOGPU_WDDM_SUBMISSION *submission, BOOLEAN releaseBuffer)
{
    return QuarantineSubmission(submission, VioGpuWddmSubmissionQueued, releaseBuffer);
}

VOID NativeSubmissionComplete(_In_opt_ PVOID callbackContext)
{
    VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(callbackContext);
    if (submission == NULL || submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE ||
        submission->Context == NULL || submission->Adapter == NULL || submission->VirtioBuffer == NULL)
    {
        return;
    }

    VioGpuDod *adapter = submission->Adapter;
    VIOGPU_WDDM_CONTEXT *context = submission->Context;
    PGPU_VBUFFER buffer = submission->VirtioBuffer;
    UINT fenceId = static_cast<UINT>(submission->FenceId);
    UINT nodeOrdinal = context->NodeOrdinal;
    UINT engineOrdinal = 0;
    const UINT expectedFlags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    PGPU_CMD_SUBMIT_3D command = reinterpret_cast<PGPU_CMD_SUBMIT_3D>(buffer->buf);
    PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(buffer->resp_buf);

    BOOLEAN valid = InterlockedCompareExchange(&submission->State,
                                               VioGpuWddmSubmissionQueued,
                                               VioGpuWddmSubmissionQueued) == VioGpuWddmSubmissionQueued &&
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

    if (!ReleaseQueuedSubmission(submission, TRUE))
    {
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
}

VOID NativeSubmissionCancelled(_In_opt_ PVOID callbackContext)
{
    VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(callbackContext);
    if (submission == NULL || submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE)
    {
        return;
    }

    /* Reset/close owns the GPU_VBUFFER in this path.  Release only the WDDM
     * context/allocation record, and never fabricate scheduler completion. */
    InvalidateContextUmdFenceTracker(submission->Context);
    if (!QuarantineSubmission(submission, VioGpuWddmSubmissionPrepared, FALSE))
    {
        QuarantineSubmission(submission, VioGpuWddmSubmissionQueued, FALSE);
    }
}

VOID NativeSubmissionQueueFailed(_In_opt_ PVOID callbackContext)
{
    VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(callbackContext);
    if (submission == NULL || submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE ||
        submission->Context == NULL || submission->Adapter == NULL || submission->FenceId == 0 ||
        submission->FenceId > MAXUINT)
    {
        return;
    }

    VioGpuDod *adapter = submission->Adapter;
    UINT fenceId = static_cast<UINT>(submission->FenceId);
    UINT nodeOrdinal = submission->Context->NodeOrdinal;
    UINT engineOrdinal = 0;
    InvalidateContextUmdFenceTracker(submission->Context);
    if (ReleaseQueuedSubmission(submission, FALSE))
    {
        adapter->NotifyNativeSubmissionFault(fenceId,
                                             STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                             nodeOrdinal,
                                             engineOrdinal,
                                             FALSE);
    }
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
        submissionEnd - submissionStart < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE))
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

    VIOGPU_WDDM_SUBMISSION *submission = static_cast<VIOGPU_WDDM_SUBMISSION *>(privateData->Submission);
    if (submission->Signature != VIOGPU_WDDM_SUBMISSION_SIGNATURE || submission->Context == NULL ||
        submission->Context != runtimeContext || submission->Adapter != adapter ||
        submission->DmaBuffer != privateData->DmaBuffer || submission->DmaBufferSize != privateData->DmaBufferSize ||
        submission->DmaPrivateData != privateData ||
        submission->DmaPrivateDataSize < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) ||
        submission->CommandLength != privateData->CommandLength || submission->ContextId != privateData->ContextId ||
        submission->Generation != privateData->Generation ||
        submission->ResetGeneration != privateData->ResetGeneration || submission->CommandStream == NULL ||
        submission->CommandStreamSize == 0 || submission->CommandStreamOffset >= submission->CommandLength ||
        submission->CommandStreamSize > submission->CommandLength - submission->CommandStreamOffset ||
        submission->AllocationCount == 0 || submission->AllocationCount > VioGpuWddmSubmissionAllocationLimit ||
        submission->VirtioBuffer == NULL ||
        !adapter->IsNativeContextGenerationCurrent(submission->Generation, submission->ResetGeneration))
    {
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
           VioGpuAdapter::IsNativeContextAllocationBindingRetired(allocation->NativeContext);
}

NTSTATUS ReleaseAllocationHostOwnership(VIOGPU_WDDM_ALLOCATION *allocation,
                                        VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                                        BOOLEAN snapshotAcquired)
{
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
    if (allocation == NULL || offset == NULL || segmentAddress.QuadPart < 0 ||
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
        mdlOffset > (MAXULONG_PTR >> PAGE_SHIFT))
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

BOOLEAN AcquireNativeGuestPoolMapping(_In_ const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                                      _Out_ VioGpuGuestPoolMapping *mapping)
{
    /* A native-context snapshot owns m_NativeContextLifecycleMutex.  Use the
     * adapter lease directly here; entering VioGpuDod's hardware rundown at
     * this point would invert the allocation/create lock order. */
    return snapshot != NULL && snapshot->Adapter != NULL && mapping != NULL &&
           snapshot->Adapter->AcquireGpuGuestPoolMapping(mapping);
}

NTSTATUS CopyNativePlacement(VIOGPU_WDDM_ALLOCATION *allocation,
                             ULONGLONG segmentOffset,
                             SIZE_T allocationOffset,
                             SIZE_T transferSize,
                             ULONGLONG expectedPoolGeneration,
                             PVOID systemAddress,
                             BOOLEAN toSegment,
                             const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                             ULONGLONG *poolGeneration)
{
    if (allocation == NULL || allocation->Adapter == NULL || systemAddress == NULL || snapshot == NULL ||
        poolGeneration == NULL || transferSize == 0 || allocationOffset > allocation->BackingSize ||
        transferSize > allocation->BackingSize - allocationOffset || segmentOffset > MAXULONG_PTR)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *poolGeneration = 0;

    VioGpuGuestPoolMapping mapping;
    KeEnterGuardedRegion();
    BOOLEAN acquired = AcquireNativeGuestPoolMapping(snapshot, &mapping);
    BOOLEAN valid = acquired && mapping.GetBaseAddress() != NULL && mapping.GetGeneration() != 0 &&
                    (expectedPoolGeneration == 0 || mapping.GetGeneration() == expectedPoolGeneration) &&
                    segmentOffset <= (ULONGLONG)mapping.GetSize() &&
                    allocation->BackingSize <= mapping.GetSize() - (SIZE_T)segmentOffset;
    if (valid)
    {
        PVOID segmentAddress = static_cast<PUCHAR>(mapping.GetBaseAddress()) + (SIZE_T)segmentOffset + allocationOffset;
        if (toSegment)
        {
            RtlCopyMemory(segmentAddress, systemAddress, transferSize);
        }
        else
        {
            RtlCopyMemory(systemAddress, segmentAddress, transferSize);
        }
        *poolGeneration = mapping.GetGeneration();
    }
    mapping.Release();
    KeLeaveGuardedRegion();
    return valid ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

NTSTATUS FillNativePlacement(VIOGPU_WDDM_ALLOCATION *allocation,
                             ULONGLONG segmentOffset,
                             SIZE_T fillSize,
                             UINT pattern,
                             ULONGLONG expectedPoolGeneration,
                             const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                             ULONGLONG *poolGeneration)
{
    if (allocation == NULL || allocation->Adapter == NULL || snapshot == NULL || poolGeneration == NULL ||
        segmentOffset > MAXULONG_PTR || fillSize == 0 || fillSize > allocation->BackingSize ||
        (fillSize & (sizeof(ULONG) - 1)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *poolGeneration = 0;

    VioGpuGuestPoolMapping mapping;
    KeEnterGuardedRegion();
    BOOLEAN acquired = AcquireNativeGuestPoolMapping(snapshot, &mapping);
    BOOLEAN valid = acquired && mapping.GetBaseAddress() != NULL && mapping.GetGeneration() != 0 &&
                    mapping.GetGeneration() == expectedPoolGeneration &&
                    segmentOffset <= (ULONGLONG)mapping.GetSize() &&
                    allocation->BackingSize <= mapping.GetSize() - (SIZE_T)segmentOffset;
    if (valid)
    {
        PVOID segmentAddress = static_cast<PUCHAR>(mapping.GetBaseAddress()) + (SIZE_T)segmentOffset;
        for (SIZE_T offset = 0; offset < fillSize; offset += sizeof(pattern))
        {
            RtlCopyMemory(static_cast<PUCHAR>(segmentAddress) + offset, &pattern, sizeof(pattern));
        }
        *poolGeneration = mapping.GetGeneration();
    }
    mapping.Release();
    KeLeaveGuardedRegion();
    return valid ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

VOID PublishNativePlacement(VIOGPU_WDDM_ALLOCATION *allocation,
                            const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                            ULONGLONG segmentOffset,
                            ULONGLONG poolGeneration,
                            VIOGPU_WDDM_ALLOCATION_HOST_STATE hostState)
{
    allocation->PlacementOffset = segmentOffset;
    allocation->PoolGeneration = poolGeneration;
    allocation->PlacementValid = TRUE;
    allocation->HostState = hostState;
    allocation->BoundGeneration = snapshot->Generation;
    allocation->BoundResetGeneration = snapshot->ResetGeneration;
    allocation->BoundContextId = snapshot->ContextId;
}

VOID ClearNativePlacement(VIOGPU_WDDM_ALLOCATION *allocation)
{
    allocation->PlacementOffset = 0;
    allocation->PoolGeneration = 0;
    allocation->PlacementValid = FALSE;
}

VOID ClearNativePagingState(VIOGPU_WDDM_ALLOCATION *allocation)
{
    ClearPagingRanges(allocation);
    allocation->PagingState = VioGpuWddmAllocationPagingIdle;
    allocation->PagingPlacementOffset = 0;
    allocation->PagingPoolGeneration = 0;
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
    segmentInfo->PagingBufferPrivateDataSize = sizeof(VIOGPU_WDDM_PAGING_PRIVATE);

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
        request.CompletedFence != 0 || request.ResetGeneration != 0 || request.ContextId != 0 ||
        request.Reserved != 0)
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
        if (!NT_SUCCESS(allocationStatus))
        {
            return allocationStatus;
        }
        BOOLEAN resident = nativeContext != NULL && allocation->Signature == VIOGPU_WDDM_ALLOCATION_SIGNATURE &&
                           allocation->Adapter == device->Adapter &&
                           allocation->HostState == VioGpuWddmAllocationHostLive && allocation->PlacementValid &&
                           allocation->PoolGeneration != 0 &&
                           allocation->ResourceId >= VIOGPU_NATIVE_RESOURCE_ID_START && allocation->BlobId != 0 &&
                           allocation->NativeContext == nativeContext->Registration &&
                           allocation->ContextGeneration == nativeContext->Generation &&
                           allocation->ContextResetGeneration == nativeContext->ResetGeneration &&
                           allocation->ContextId == nativeContext->ContextId &&
                           allocation->BoundGeneration == nativeContext->Generation &&
                           allocation->BoundResetGeneration == nativeContext->ResetGeneration &&
                           allocation->BoundContextId == nativeContext->ContextId &&
                           allocation->PrivateData.ExpectedResetGeneration == nativeContext->ResetGeneration &&
                           allocationEntry->SegmentId == VIOGPU_WDDM_SEGMENT_ID &&
                           allocationEntry->PhysicalAddress.QuadPart >= 0 &&
                           (ULONGLONG)allocationEntry->PhysicalAddress.QuadPart == allocation->PlacementOffset;
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        if (!resident)
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
} // namespace

VOID NativePagingBatchWorker(_In_ PVOID callbackContext);

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
        driverCaps->SchedulingCaps.CancelCommandAware = 1;
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
        RtlCopyMemory(&privateData, allocationInfo->pPrivateDriverData, sizeof(privateData));
        SIZE_T alignedSize = 0;
        status = ValidateAllocationPrivate(&privateData, &alignedSize);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        UINT nativeResourceId = 0;
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

        VIOGPU_WDDM_ALLOCATION *allocation = new (NonPagedPoolNx) VIOGPU_WDDM_ALLOCATION;
        if (allocation == NULL)
        {
            if (nativeContext != NULL)
            {
                BOOLEAN dereferenced = VioGpuAdapter::DereferenceNativeContextAllocation(nativeContext);
                NT_ASSERT(dereferenced);
                UNREFERENCED_PARAMETER(dereferenced);
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
        allocation->ContextGeneration = contextGeneration;
        allocation->ContextResetGeneration = contextResetGeneration;
        allocation->ContextId = contextId;
        allocation->PrivateData = privateData;
        allocation->BackingSize = alignedSize;
        InitializeListHead(&allocation->PagingRanges);
        allocation->PagingCoveredBytes = 0;
        allocation->ResourceId = nativeResourceId;
        allocation->BlobId = nativeResourceId;
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
            allocation->Adapter != adapter)
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
        if (IsNativeAllocation(allocation) && !ValidateNativeAllocationRange(allocation))
        {
            return STATUS_DEVICE_NOT_READY;
        }
    }

    for (UINT index = 0; index < destroyAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(destroyAllocation->pAllocationList[index]);
        VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
        BOOLEAN snapshotAcquired = AcquireAllocationNativeContextSnapshot(allocation, &snapshot);
        NTSTATUS status = AcquireAllocationLifecycle(allocation);
        if (NT_SUCCESS(status))
        {
            if (allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE || allocation->Adapter != adapter)
            {
                status = STATUS_INVALID_HANDLE;
            }
            else
            {
                status = BeginAllocationDestroy(allocation);
                if (NT_SUCCESS(status))
                {
                    if (IsNativeAllocation(allocation))
                    {
                        status = ReleaseAllocationHostOwnership(allocation, &snapshot, snapshotAcquired);
                    }
                }
            }
            KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        }
        if (snapshotAcquired)
        {
            VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
        }
        if (!NT_SUCCESS(status))
        {
            // BeginAllocationDestroy deliberately leaves every previously
            // reserved object closed on failure.  A busy or transport-unknown
            // result must not reopen the object to a new Open/Render race.
            return status;
        }
    }

    for (UINT index = 0; index < destroyAllocation->NumAllocations; ++index)
    {
        VIOGPU_WDDM_ALLOCATION *allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(destroyAllocation->pAllocationList[index]);
        if (allocation->NativeContext != NULL)
        {
            NTSTATUS rangeStatus = UnregisterNativeAllocationRange(allocation);
            if (!NT_SUCCESS(rangeStatus))
            {
                return rangeStatus;
            }
            BOOLEAN dereferenced = VioGpuAdapter::DereferenceNativeContextAllocation(allocation->NativeContext);
            NT_ASSERT(dereferenced);
            UNREFERENCED_PARAMETER(dereferenced);
            allocation->NativeContext = NULL;
        }
        if (allocation->Resource != NULL)
        {
            LONG remaining = InterlockedDecrement(&allocation->Resource->AllocationCount);
            NT_ASSERT(remaining >= 0);
            UNREFERENCED_PARAMETER(remaining);
        }
        ClearPagingRanges(allocation);
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
        if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
            allocation->Adapter != device->Adapter ||
            (allocation->Resource != NULL && (allocation->Resource->Signature != VIOGPU_WDDM_RESOURCE_SIGNATURE ||
                                              allocation->Resource->Adapter != device->Adapter)))
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
        if (NT_SUCCESS(status))
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
    KeInitializeSpinLock(&context->SubmissionLock);
    context->SubmissionReferences = 0;
    context->SubmissionClosing = FALSE;
    context->UmdFenceHead = 0;
    context->UmdFenceCount = 0;
    RtlZeroMemory(context->UmdFences, sizeof(context->UmdFences));
    context->SubmittedUmdFence = 0;
    context->CompletedUmdFence = 0;
    context->Device = device;
    context->RuntimeContext = NULL;
    context->NodeOrdinal = createContext->NodeOrdinal;
    context->EngineAffinity = createContext->EngineAffinity;
    KeInitializeSpinLock(&context->NativeContext.BindingLock);
    InitializeListHead(&context->NativeContext.AllocationRanges);
    InitializeListHead(&context->PendingSubmissions);
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

    NTSTATUS submissionStatus = BeginContextSubmissionRundown(context);
    if (!NT_SUCCESS(submissionStatus))
    {
        return submissionStatus;
    }

    KIRQL submissionIrql;
    KeAcquireSpinLock(&context->SubmissionLock, &submissionIrql);
    BOOLEAN hasPendingSubmissions = !IsListEmpty(&context->PendingSubmissions);
    KeReleaseSpinLock(&context->SubmissionLock, submissionIrql);
    if (hasPendingSubmissions)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
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

BOOLEAN QueryNativePlacementPoolGeneration(_In_ const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                                           _In_ ULONGLONG placementOffset,
                                           _In_ SIZE_T backingSize,
                                           _Out_ ULONGLONG *poolGeneration)
{
    if (snapshot == NULL || snapshot->Adapter == NULL || poolGeneration == NULL || backingSize == 0 ||
        placementOffset > MAXULONG_PTR)
    {
        return FALSE;
    }
    *poolGeneration = 0;

    VioGpuGuestPoolMapping mapping;
    KeEnterGuardedRegion();
    BOOLEAN acquired = AcquireNativeGuestPoolMapping(snapshot, &mapping);
    BOOLEAN valid = acquired && mapping.GetBaseAddress() != NULL && mapping.GetGeneration() != 0 &&
                    placementOffset <= mapping.GetSize() &&
                    backingSize <= mapping.GetSize() - static_cast<SIZE_T>(placementOffset);
    if (valid)
    {
        *poolGeneration = mapping.GetGeneration();
    }
    mapping.Release();
    KeLeaveGuardedRegion();
    return valid;
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
    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
    BOOLEAN snapshotAcquired = AcquireAllocationNativeContextSnapshot(allocation, &snapshot);
    if (!snapshotAcquired || snapshot.ContextId != transaction->ContextId ||
        snapshot.Generation != transaction->ContextGeneration ||
        snapshot.ResetGeneration != transaction->ResetGeneration)
    {
        if (snapshotAcquired)
        {
            VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
        }
        return STATUS_DEVICE_NOT_READY;
    }

    NTSTATUS status = AcquireAllocationLifecycle(allocation);
    if (!NT_SUCCESS(status))
    {
        VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
        return status;
    }

    BOOLEAN pageIn = (transaction->Flags & VioGpuWddmPagingFlagPageIn) != 0;
    BOOLEAN pageOut = (transaction->Flags & VioGpuWddmPagingFlagPageOut) != 0;
    BOOLEAN fill = (transaction->Flags & VioGpuWddmPagingFlagFill) != 0;
    BOOLEAN discard = (transaction->Flags & VioGpuWddmPagingFlagDiscard) != 0;
    BOOLEAN transferStart = (transaction->Flags & VioGpuWddmPagingFlagTransferStart) != 0;
    BOOLEAN transferEnd = (transaction->Flags & VioGpuWddmPagingFlagTransferEnd) != 0;
    if (allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE || allocation->Adapter != transaction->Adapter ||
        allocation->ResourceId != transaction->ResourceId || allocation->BlobId != transaction->ResourceId ||
        allocation->ContextId != transaction->ContextId ||
        allocation->ContextGeneration != transaction->ContextGeneration ||
        allocation->ContextResetGeneration != transaction->ResetGeneration)
    {
        status = STATUS_DEVICE_NOT_READY;
    }
    if (NT_SUCCESS(status) && (pageIn || pageOut) && !transaction->TransferDataComplete)
    {
        status = STATUS_INVALID_DEVICE_STATE;
    }

    if (NT_SUCCESS(status) && pageIn)
    {
        BOOLEAN continuingPageIn = allocation->PagingState == VioGpuWddmAllocationPagingIn &&
                                   allocation->PagingPlacementOffset == transaction->PlacementOffset &&
                                   allocation->PagingPoolGeneration == transaction->PoolGeneration &&
                                   allocation->HostState == VioGpuWddmAllocationHostNone && !allocation->PlacementValid;
        if (transferStart)
        {
            if (continuingPageIn)
            {
                /* VidMm may repeat TransferStart on continuation DMA
                 * buffers belonging to one logical transfer. */
            }
            else if (allocation->PagingState != VioGpuWddmAllocationPagingIdle ||
                     allocation->HostState != VioGpuWddmAllocationHostNone || allocation->PlacementValid)
            {
                status = STATUS_INVALID_DEVICE_STATE;
            }
            else
            {
                ClearPagingRanges(allocation);
                allocation->PagingState = VioGpuWddmAllocationPagingIn;
                allocation->PagingPlacementOffset = transaction->PlacementOffset;
                allocation->PagingPoolGeneration = transaction->PoolGeneration;
            }
        }
        else if (!continuingPageIn)
        {
            status = STATUS_INVALID_DEVICE_STATE;
        }

        VIOGPU_WDDM_PAGING_RANGE *pagingRange = NULL;
        ULONGLONG observedPoolGeneration = 0;
        if (NT_SUCCESS(status))
        {
            status = AddPagingRange(allocation, transaction->TransferOffset, transaction->TransferSize, &pagingRange);
        }
        if (NT_SUCCESS(status))
        {
            status = QueryNativePlacementPoolGeneration(&snapshot,
                                                        transaction->PlacementOffset,
                                                        allocation->BackingSize,
                                                        &observedPoolGeneration)
                                                                                                                         ? STATUS_SUCCESS
                                                                                                                         : STATUS_DEVICE_NOT_READY;
        }
        if (!NT_SUCCESS(status) && pagingRange != NULL)
        {
            RemovePagingRange(allocation, pagingRange);
        }
        if (NT_SUCCESS(status) && observedPoolGeneration != transaction->PoolGeneration)
        {
            RemovePagingRange(allocation, pagingRange);
            status = STATUS_DEVICE_NOT_READY;
        }
        if (NT_SUCCESS(status) && transferEnd && allocation->PagingCoveredBytes != allocation->BackingSize)
        {
            status = STATUS_INVALID_PARAMETER;
        }
        if (NT_SUCCESS(status) && transferEnd)
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
                                                                                              transaction->PlacementOffset,
                                                                                              transaction->PoolGeneration,
                                                                                              msmFlags,
                                                                                              blobFlags,
                                                                                              &ownershipRetained);
            if (ownershipRetained)
            {
                PublishNativePlacement(allocation,
                                       &snapshot,
                                       transaction->PlacementOffset,
                                       transaction->PoolGeneration,
                                       result == VioGpuHostContextConfirmed ? VioGpuWddmAllocationHostLive
                                                                            : VioGpuWddmAllocationHostUnknown);
            }
            status = result == VioGpuHostContextConfirmed && ownershipRetained ? STATUS_SUCCESS
                                                                               : STATUS_DEVICE_NOT_READY;
        }
        if (transferEnd || !NT_SUCCESS(status))
        {
            ClearNativePagingState(allocation);
        }
    }
    else if (NT_SUCCESS(status) && fill)
    {
        if (allocation->PagingState != VioGpuWddmAllocationPagingIdle ||
            allocation->HostState != VioGpuWddmAllocationHostNone || allocation->PlacementValid)
        {
            status = STATUS_INVALID_DEVICE_STATE;
        }
        ULONGLONG observedPoolGeneration = 0;
        if (NT_SUCCESS(status))
        {
            status = FillNativePlacement(allocation,
                                         transaction->PlacementOffset,
                                         transaction->TransferSize,
                                         transaction->FillPattern,
                                         transaction->PoolGeneration,
                                         &snapshot,
                                         &observedPoolGeneration);
        }
        if (NT_SUCCESS(status) && observedPoolGeneration != transaction->PoolGeneration)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
        if (NT_SUCCESS(status))
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
                                                                                              transaction->PlacementOffset,
                                                                                              transaction->PoolGeneration,
                                                                                              msmFlags,
                                                                                              blobFlags,
                                                                                              &ownershipRetained);
            if (ownershipRetained)
            {
                PublishNativePlacement(allocation,
                                       &snapshot,
                                       transaction->PlacementOffset,
                                       transaction->PoolGeneration,
                                       result == VioGpuHostContextConfirmed ? VioGpuWddmAllocationHostLive
                                                                            : VioGpuWddmAllocationHostUnknown);
            }
            status = result == VioGpuHostContextConfirmed && ownershipRetained ? STATUS_SUCCESS
                                                                               : STATUS_DEVICE_NOT_READY;
        }
    }
    else if (NT_SUCCESS(status) && pageOut)
    {
        BOOLEAN continuingPageOut = allocation->PagingState == VioGpuWddmAllocationPagingOut &&
                                    allocation->PagingPlacementOffset == transaction->PlacementOffset &&
                                    allocation->PagingPoolGeneration == transaction->PoolGeneration &&
                                    allocation->HostState == VioGpuWddmAllocationHostLive &&
                                    allocation->PlacementValid &&
                                    allocation->PlacementOffset == transaction->PlacementOffset &&
                                    allocation->PoolGeneration == transaction->PoolGeneration;
        if (transferStart)
        {
            if (continuingPageOut)
            {
                /* See the page-in continuation case above. */
            }
            else if (allocation->PagingState != VioGpuWddmAllocationPagingIdle ||
                     allocation->HostState != VioGpuWddmAllocationHostLive || !allocation->PlacementValid ||
                     allocation->PlacementOffset != transaction->PlacementOffset ||
                     allocation->PoolGeneration != transaction->PoolGeneration)
            {
                status = STATUS_INVALID_DEVICE_STATE;
            }
            else
            {
                ClearPagingRanges(allocation);
                allocation->PagingState = VioGpuWddmAllocationPagingOut;
                allocation->PagingPlacementOffset = transaction->PlacementOffset;
                allocation->PagingPoolGeneration = transaction->PoolGeneration;
            }
        }
        else if (!continuingPageOut)
        {
            status = STATUS_INVALID_DEVICE_STATE;
        }

        VIOGPU_WDDM_PAGING_RANGE *pagingRange = NULL;
        ULONGLONG observedPoolGeneration = 0;
        if (NT_SUCCESS(status))
        {
            status = AddPagingRange(allocation, transaction->TransferOffset, transaction->TransferSize, &pagingRange);
        }
        if (NT_SUCCESS(status))
        {
            status = QueryNativePlacementPoolGeneration(&snapshot,
                                                        transaction->PlacementOffset,
                                                        allocation->BackingSize,
                                                        &observedPoolGeneration)
                                                                                                                         ? STATUS_SUCCESS
                                                                                                                         : STATUS_DEVICE_NOT_READY;
        }
        if (!NT_SUCCESS(status) && pagingRange != NULL)
        {
            RemovePagingRange(allocation, pagingRange);
        }
        if (NT_SUCCESS(status) && observedPoolGeneration != transaction->PoolGeneration)
        {
            RemovePagingRange(allocation, pagingRange);
            status = STATUS_DEVICE_NOT_READY;
        }
        if (NT_SUCCESS(status) && transferEnd && allocation->PagingCoveredBytes != allocation->BackingSize)
        {
            status = STATUS_INVALID_PARAMETER;
        }
        if (NT_SUCCESS(status) && transferEnd)
        {
            status = ReleaseAllocationHostOwnership(allocation, &snapshot, TRUE);
            if (NT_SUCCESS(status))
            {
                ClearNativePlacement(allocation);
            }
        }
        if (transferEnd || !NT_SUCCESS(status))
        {
            ClearNativePagingState(allocation);
        }
    }
    else if (NT_SUCCESS(status) && discard)
    {
        if (allocation->PagingState != VioGpuWddmAllocationPagingIdle || !allocation->PlacementValid ||
            allocation->PlacementOffset != transaction->PlacementOffset ||
            allocation->PoolGeneration != transaction->PoolGeneration)
        {
            status = STATUS_INVALID_DEVICE_STATE;
        }
        else if ((transaction->Flags & VioGpuWddmPagingFlagAllocationIdle) == 0 &&
                 allocation->HostState != VioGpuWddmAllocationHostNone)
        {
            status = STATUS_GRAPHICS_ALLOCATION_BUSY;
        }
        else
        {
            status = ReleaseAllocationHostOwnership(allocation, &snapshot, TRUE);
            if (NT_SUCCESS(status))
            {
                ClearNativePlacement(allocation);
                ClearNativePagingState(allocation);
            }
        }
    }
    else if (NT_SUCCESS(status))
    {
        status = STATUS_INVALID_PARAMETER;
    }

    KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
    VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
    return status;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmBuildPagingBuffer(CONST HANDLE hAdapter,
                                                                     DXGKARG_BUILDPAGINGBUFFER *pagingBuffer)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || pagingBuffer == NULL || pagingBuffer->pDmaBuffer == NULL ||
        pagingBuffer->pDmaBufferPrivateData == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    PVOID dmaBuffer = pagingBuffer->pDmaBuffer;
    UINT dmaSize = pagingBuffer->DmaSize;
    PVOID dmaPrivateBuffer = pagingBuffer->pDmaBufferPrivateData;
    UINT dmaPrivateSize = pagingBuffer->DmaBufferPrivateDataSize;
    if (dmaSize < sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET) || dmaPrivateSize < sizeof(VIOGPU_WDDM_PAGING_PRIVATE))
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
        allocation->Adapter != adapter || !IsNativeAllocation(allocation) ||
        allocation->ResourceId < VIOGPU_NATIVE_RESOURCE_ID_START || allocation->BlobId == 0 ||
        allocation->ResourceId != allocation->BlobId)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    BOOLEAN transfer = (packetFlags & (VioGpuWddmPagingFlagPageIn | VioGpuWddmPagingFlagPageOut)) != 0;
    if (transfer && transferSize == 0)
    {
        NTSTATUS resetStatus = AcquireAllocationLifecycle(allocation);
        if (NT_SUCCESS(resetStatus))
        {
            resetStatus = AllocationResetRetired(allocation) ? STATUS_SUCCESS : STATUS_GRAPHICS_ALLOCATION_BUSY;
            if (NT_SUCCESS(resetStatus))
            {
                ClearAllocationHostBinding(allocation);
                ClearNativePlacement(allocation);
                ClearNativePagingState(allocation);
            }
            KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        }
        return NT_SUCCESS(resetStatus) ? STATUS_SUCCESS : STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    if ((transfer &&
         (!ValidateTransferMdlRange(transferMdl, mdlOffset, transferSize) || transferOffset > allocation->BackingSize ||
          transferSize > allocation->BackingSize - transferOffset)) ||
        ((packetFlags & VioGpuWddmPagingFlagFill) != 0 &&
         (transferSize != allocation->BackingSize || (transferSize & (sizeof(ULONG) - 1)) != 0)) ||
        ((packetFlags & VioGpuWddmPagingFlagPageOut) != 0 && (packetFlags & VioGpuWddmPagingFlagAllocationIdle) == 0))
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    ULONGLONG placementOffset = 0;
    if (!NT_SUCCESS(ValidateNativePlacement(allocation, segmentAddress, &placementOffset)))
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
    BOOLEAN snapshotAcquired = AcquireAllocationNativeContextSnapshot(allocation, &snapshot);
    if (!snapshotAcquired)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    NTSTATUS status = AcquireAllocationLifecycle(allocation);
    BOOLEAN lifecycleAcquired = NT_SUCCESS(status);
    BOOLEAN allocationReference = FALSE;
    ULONGLONG poolGeneration = 0;
    if (NT_SUCCESS(status) &&
        (allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE || allocation->Adapter != adapter ||
         allocation->ResourceId < VIOGPU_NATIVE_RESOURCE_ID_START || allocation->ResourceId != allocation->BlobId ||
         allocation->HostState == VioGpuWddmAllocationHostUnknown))
    {
        status = STATUS_DEVICE_NOT_READY;
    }
    if (NT_SUCCESS(status))
    {
        BOOLEAN pageInOrFill = (packetFlags & (VioGpuWddmPagingFlagPageIn | VioGpuWddmPagingFlagFill)) != 0;
        if (pageInOrFill)
        {
            status = QueryNativePlacementPoolGeneration(&snapshot,
                                                        placementOffset,
                                                        allocation->BackingSize,
                                                        &poolGeneration)
                                                                                                                         ? STATUS_SUCCESS
                                                                                                                         : STATUS_DEVICE_NOT_READY;
        }
        else if (!allocation->PlacementValid || allocation->PlacementOffset != placementOffset ||
                 allocation->PoolGeneration == 0)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
        else
        {
            poolGeneration = allocation->PoolGeneration;
        }
    }
    if (NT_SUCCESS(status))
    {
        status = AcquireAllocationSubmissionReference(allocation, adapter);
        allocationReference = NT_SUCCESS(status);
    }
    BOOLEAN transferDataComplete = FALSE;
    if (NT_SUCCESS(status) &&
        ((packetFlags & VioGpuWddmPagingFlagPageIn) != 0 || (packetFlags & VioGpuWddmPagingFlagPageOut) != 0))
    {
        PVOID systemAddress = NULL;
        status = ResolveTransferMdlAddress(transferMdl, mdlOffset, transferSize, &systemAddress);
        if (NT_SUCCESS(status))
        {
            ULONGLONG observedPoolGeneration = 0;
            status = CopyNativePlacement(allocation,
                                         placementOffset,
                                         transferOffset,
                                         transferSize,
                                         poolGeneration,
                                         systemAddress,
                                         (packetFlags & VioGpuWddmPagingFlagPageIn) != 0,
                                         &snapshot,
                                         &observedPoolGeneration);
            if (NT_SUCCESS(status) && observedPoolGeneration != poolGeneration)
            {
                status = STATUS_DEVICE_NOT_READY;
            }
            transferDataComplete = NT_SUCCESS(status);
        }
    }

    if (NT_SUCCESS(status))
    {
        VIOGPU_WDDM_PAGING_DMA_PACKET *packet = static_cast<VIOGPU_WDDM_PAGING_DMA_PACKET *>(dmaBuffer);
        VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate = static_cast<VIOGPU_WDDM_PAGING_PRIVATE *>(dmaPrivateBuffer);
        RtlZeroMemory(packet, sizeof(*packet));
        packet->Signature = VIOGPU_WDDM_PAGING_DMA_SIGNATURE;
        packet->Version = VioGpuWddmDmaPrivateVersion;
        packet->Size = static_cast<USHORT>(sizeof(*packet));
        packet->Operation = static_cast<UINT>(pagingBuffer->Operation);
        packet->Flags = packetFlags;
        packet->ResourceId = allocation->ResourceId;
        packet->ContextId = snapshot.ContextId;
        packet->ContextGeneration = snapshot.Generation;
        packet->ResetGeneration = snapshot.ResetGeneration;
        packet->PlacementOffset = placementOffset;
        packet->PoolGeneration = poolGeneration;
        packet->TransferOffset = transferOffset;
        packet->TransferSize = transferSize;
        packet->Reserved = 0;

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
        transaction->PoolGeneration = poolGeneration;
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
        pagingPrivate->Work.Context = pagingPrivate;
        KeMemoryBarrier();
        InterlockedExchange(&transaction->State, VioGpuWddmPagingTransactionBuilt);

        pagingBuffer->pDmaBuffer = static_cast<BYTE *>(dmaBuffer) + sizeof(*packet);
        pagingBuffer->DmaSize = dmaSize - sizeof(*packet);
        pagingBuffer->pDmaBufferPrivateData = static_cast<BYTE *>(dmaPrivateBuffer) + sizeof(*pagingPrivate);
        pagingBuffer->DmaBufferPrivateDataSize = dmaPrivateSize - sizeof(*pagingPrivate);
        allocationReference = FALSE;
    }

    if (lifecycleAcquired)
    {
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
    }
    if (allocationReference)
    {
        ReleaseAllocationSubmissionReference(allocation);
    }
    VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
    return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_GRAPHICS_ALLOCATION_BUSY;
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
        BOOLEAN workLinkAvailable = pagingPrivate->Work.Link.Flink == &pagingPrivate->Work.Link &&
                                    pagingPrivate->Work.Link.Blink == &pagingPrivate->Work.Link;
        if (header->DmaBuffer != packet || header->DmaBufferSize != dmaBufferSize - packetOffset ||
            header->Submission != pagingPrivate || transaction->Adapter != adapter || !stateMatches ||
            pagingPrivate->Work.Routine != NativePagingBatchWorker || pagingPrivate->Work.Context != pagingPrivate ||
            (expectedState != VioGpuWddmPagingTransactionAny && !workLinkAvailable) || !packetValid)
        {
            return FALSE;
        }
        if (index == 0)
        {
            *firstPrivate = pagingPrivate;
        }
    }
    *recordCount = count;
    return TRUE;
}

VOID RollbackCancelledPagingTransaction(_Inout_ VIOGPU_WDDM_PAGING_TRANSACTION *transaction)
{
    if (transaction == NULL || transaction->Allocation == NULL ||
        InterlockedCompareExchange(&transaction->ExecutionStarted, 0, 0) == 0)
    {
        return;
    }
    VIOGPU_WDDM_ALLOCATION *allocation = transaction->Allocation;
    NTSTATUS status = AcquireAllocationLifecycle(allocation);
    if (!NT_SUCCESS(status))
    {
        return;
    }
    BOOLEAN matchingPageIn = (transaction->Flags & VioGpuWddmPagingFlagPageIn) != 0 &&
                             allocation->PagingState == VioGpuWddmAllocationPagingIn;
    BOOLEAN matchingPageOut = (transaction->Flags & VioGpuWddmPagingFlagPageOut) != 0 &&
                              allocation->PagingState == VioGpuWddmAllocationPagingOut;
    if ((matchingPageIn || matchingPageOut) && allocation->PagingPlacementOffset == transaction->PlacementOffset &&
        allocation->PagingPoolGeneration == transaction->PoolGeneration)
    {
        ClearNativePagingState(allocation);
    }
    KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
}

VOID RollbackPagingBatch(_In_ PVOID privateBuffer,
                         _In_ UINT privateBufferSize,
                         _In_ UINT privateStart,
                         _In_ UINT privateEnd,
                         _In_ UINT count)
{
    if (privateBuffer == NULL || privateStart >= privateEnd || privateEnd > privateBufferSize ||
        privateEnd - privateStart < sizeof(VIOGPU_WDDM_PAGING_PRIVATE))
    {
        return;
    }
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
        RollbackCancelledPagingTransaction(&pagingPrivate->Transaction);
    }
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
            /* A second cancel callback, or a callback racing completion, has
             * still identified a valid owner. */
            return state == VioGpuWddmPagingTransactionCancelled || state == VioGpuWddmPagingTransactionFinished;
        }
        LONG observed = InterlockedCompareExchange(&transaction->State, VioGpuWddmPagingTransactionCancelled, state);
        if (observed == state)
        {
            RollbackCancelledPagingTransaction(transaction);
            ReleasePagingTransactionReference(transaction);
            return TRUE;
        }
        state = observed;
    }
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

    BOOLEAN rangeValid = privateBuffer != NULL && privateStart < privateEnd && privateEnd <= privateBufferSize &&
                         privateEnd - privateStart >= sizeof(VIOGPU_WDDM_PAGING_PRIVATE) &&
                         ((privateEnd - privateStart) % sizeof(VIOGPU_WDDM_PAGING_PRIVATE)) == 0;
    if (rangeValid)
    {
        count = (privateEnd - privateStart) / sizeof(VIOGPU_WDDM_PAGING_PRIVATE);
    }
    if (adapter == NULL || !rangeValid ||
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

    if (!NT_SUCCESS(status))
    {
        RollbackPagingBatch(privateBuffer, privateBufferSize, privateStart, privateEnd, count);
    }

    /* Keep every executed allocation alive until batch rollback is no longer
     * possible.  ReferenceHeld makes cancellation and worker retirement
     * converge on one release. */
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
        if (NT_SUCCESS(status))
        {
            adapter->NotifyNativeSoftwareCompletion(fenceId, nodeOrdinal, engineOrdinal);
        }
        else
        {
            adapter->NotifyNativeSubmissionFault(fenceId,
                                                 STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                                 nodeOrdinal,
                                                 engineOrdinal,
                                                 TRUE);
        }
        adapter->ReleaseNativeSubmissionOperation();
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
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE)
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
                                           render->DmaBufferPrivateDataSize,
                                           render->CommandLength,
                                           virtioBuffer,
                                           &snapshot);
        if (NT_SUCCESS(status))
        {
            submissionPublished = TRUE;

            render->pDmaBuffer = static_cast<BYTE *>(dmaBuffer) + render->CommandLength;
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

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmPatch(CONST HANDLE hAdapter, CONST DXGKARG_PATCH *patchArguments)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
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
        return STATUS_INVALID_PARAMETER;
    }

    if (patchArguments->Flags.Value == 1)
    {
        VIOGPU_WDDM_PAGING_PRIVATE *firstPrivate = NULL;
        UINT recordCount = 0;
        BOOLEAN exact = patchArguments->hContext == NULL && patchArguments->pAllocationList == NULL &&
                        patchArguments->AllocationListSize == 0 && patchArguments->pPatchLocationList == NULL &&
                        patchArguments->PatchLocationListSize == 0 &&
                        patchArguments->PatchLocationListSubmissionStart == 0 &&
                        patchArguments->PatchLocationListSubmissionLength == 0 &&
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
                                           &recordCount);
        UNREFERENCED_PARAMETER(firstPrivate);
        UNREFERENCED_PARAMETER(recordCount);
        return exact ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }

    if (patchArguments->hContext == NULL || patchArguments->pAllocationList == NULL ||
        patchArguments->pPatchLocationList == NULL || patchArguments->Flags.Value != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!adapter->AcquireNativeSubmissionOperation())
    {
        return STATUS_DEVICE_NOT_READY;
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
                        patchArguments->DmaBufferPrivateDataSize - patchArguments->DmaBufferPrivateDataSubmissionStartOffset ==
                                                                                                                            submission->DmaPrivateDataSize &&
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

    UINT patchedResourceIds[VioGpuWddmSubmissionAllocationLimit] = {};
    ULONGLONG patchedIovas[VioGpuWddmSubmissionAllocationLimit] = {};
    if (NT_SUCCESS(status))
    {
        for (UINT index = 0; index < submission->AllocationCount; ++index)
        {
            const VIOGPU_WDDM_SUBMISSION_REFERENCE *reference = &submission->References[index];
            const D3DDDI_PATCHLOCATIONLIST *patch = &patchArguments->pPatchLocationList[patchArguments->PatchLocationListSubmissionStart +
                                                                                        index];
            if (reference->Allocation == NULL || reference->AllocationIndex >= patchArguments->AllocationListSize ||
                patch->AllocationIndex != reference->AllocationIndex ||
                patch->AllocationOffset != reference->AllocationOffset ||
                patch->PatchOffset != submission->CommandStreamOffset + reference->PatchOffset ||
                patch->Reserved != 0 || patch->DriverId != 0 || patch->SplitOffset != 0)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            const DXGK_ALLOCATIONLIST *allocationEntry = &patchArguments->pAllocationList[reference->AllocationIndex];
            VIOGPU_WDDM_OPEN_ALLOCATION *openAllocation = reinterpret_cast<VIOGPU_WDDM_OPEN_ALLOCATION *>(allocationEntry->hDeviceSpecificAllocation);
            VIOGPU_WDDM_ALLOCATION *allocation = reference->Allocation;
            status = AcquireAllocationLifecycle(allocation);
            if (!NT_SUCCESS(status))
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
                            allocation->PoolGeneration != 0 &&
                            allocation->ResourceId >= VIOGPU_NATIVE_RESOURCE_ID_START && allocation->BlobId != 0 &&
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
    }
    else if (submission != NULL)
    {
        ReleasePreparedSubmission(submission);
        submission = NULL;
    }

    adapter->ReleaseNativeSubmissionOperation();
    return status;
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
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || submitCommand == NULL || KeGetCurrentIrql() != DISPATCH_LEVEL ||
        submitCommand->pDmaBufferPrivateData == NULL || submitCommand->DmaBufferSegmentId != 0 ||
        submitCommand->EngineOrdinal != 0 || submitCommand->NodeOrdinal != 0 || submitCommand->SubmissionFenceId == 0 ||
        submitCommand->SubmissionFenceId > MAXUINT ||
        submitCommand->DmaBufferSubmissionStartOffset > submitCommand->DmaBufferSubmissionEndOffset ||
        submitCommand->DmaBufferSubmissionEndOffset > submitCommand->DmaBufferSize ||
        submitCommand->DmaBufferPrivateDataSubmissionStartOffset > submitCommand->DmaBufferPrivateDataSubmissionEndOffset ||
        submitCommand->DmaBufferPrivateDataSubmissionEndOffset > submitCommand->DmaBufferPrivateDataSize)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!adapter->AcquireNativeSubmissionOperation())
    {
        adapter->NotifyNativeSubmissionFault(submitCommand->SubmissionFenceId,
                                             STATUS_DEVICE_NOT_READY,
                                             submitCommand->NodeOrdinal,
                                             submitCommand->EngineOrdinal,
                                             TRUE);
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

    if (NT_SUCCESS(status) && privateData->Kind == VioGpuWddmDmaKindPaging)
    {
        VIOGPU_WDDM_PAGING_PRIVATE *firstPrivate = NULL;
        UINT recordCount = 0;
        UINT recordLimit = privateEnd >= sizeof(VIOGPU_WDDM_PAGING_PRIVATE) ? privateEnd - sizeof(VIOGPU_WDDM_PAGING_PRIVATE)
                                                                            : 0;
        BOOLEAN valid = submitCommand->hContext == NULL && submitCommand->Flags.Value == 1 &&
                        ResolvePagingBatch(NULL,
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
                VIOGPU_WDDM_PAGING_DMA_PACKET *packet = static_cast<VIOGPU_WDDM_PAGING_DMA_PACKET *>(pagingPrivate->Header.Packet);
                if (packet->ContextId == 0 ||
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
        BOOLEAN recorded = FALSE;
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
            recorded = adapter->RecordNativeSubmissionFence(submitCommand->SubmissionFenceId);
            queued = recorded && adapter->QueueNativePassiveWork(&firstPrivate->Work);
            if (!queued)
            {
                valid = FALSE;
            }
        }
        if (queued)
        {
            /* The passive worker now owns the native operation reference. */
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
            CancelPagingTransaction(&pagingPrivate->Transaction);
        }
        if (recorded)
        {
            adapter->NotifyNativeSubmissionFault(submitCommand->SubmissionFenceId,
                                                 STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                                 submitCommand->NodeOrdinal,
                                                 submitCommand->EngineOrdinal,
                                                 TRUE);
        }
        else
        {
            adapter->NotifyNativeSubmissionFault(submitCommand->SubmissionFenceId,
                                                 STATUS_DEVICE_NOT_READY,
                                                 submitCommand->NodeOrdinal,
                                                 submitCommand->EngineOrdinal,
                                                 TRUE);
        }
        adapter->ReleaseNativeSubmissionOperation();
        return STATUS_SUCCESS;
    }
    else if (NT_SUCCESS(status) && privateData->Kind != VioGpuWddmDmaKindRender)
    {
        status = STATUS_INVALID_PARAMETER;
    }

    VIOGPU_WDDM_SUBMISSION *submission = NULL;
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
        BOOLEAN exact = submitCommand->hContext != NULL && submitCommand->Flags.Value == 0 &&
                        privateLength == sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) &&
                        submitCommand->DmaBufferSize - submitCommand->DmaBufferSubmissionStartOffset == submission->DmaBufferSize &&
                        submitCommand->DmaBufferPrivateDataSize - privateStart == submission->DmaPrivateDataSize &&
                        dmaLength == submission->CommandLength && submission->Context != NULL &&
                        submission->Context->NodeOrdinal == submitCommand->NodeOrdinal &&
                        submission->Context->EngineAffinity == 1 && submission->PatchApplied &&
                        submission->FenceId == submitCommand->SubmissionFenceId &&
                        InterlockedCompareExchange(&submission->State,
                                                   VioGpuWddmSubmissionPrepared,
                                                   VioGpuWddmSubmissionPrepared) == VioGpuWddmSubmissionPrepared;
        if (!exact)
        {
            status = STATUS_INVALID_PARAMETER;
        }
    }

    if (NT_SUCCESS(status))
    {
        LONG previous = InterlockedCompareExchange(&submission->State,
                                                   VioGpuWddmSubmissionQueued,
                                                   VioGpuWddmSubmissionPrepared);
        if (previous != VioGpuWddmSubmissionPrepared)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
    }

    if (NT_SUCCESS(status))
    {
        PGPU_VBUFFER buffer = submission->VirtioBuffer;
        UINT fenceId = submitCommand->SubmissionFenceId;
        BOOLEAN recorded = adapter->RecordNativeSubmissionFence(fenceId);
        BOOLEAN umdFenceRecorded = recorded && RecordContextUmdFence(submission->Context, submission->UmdFenceId);
        int queueResult = umdFenceRecorded ? adapter->QueueNativeSubmit(buffer, fenceId) : -1;
        if (!recorded || !umdFenceRecorded || queueResult < 0)
        {
            InvalidateContextUmdFenceTracker(submission->Context);
            ReleaseQueuedSubmission(submission, TRUE);
            adapter->NotifyNativeSubmissionFault(fenceId,
                                                 recorded && umdFenceRecorded ? STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE
                                                          : STATUS_INSUFFICIENT_RESOURCES,
                                                 submitCommand->NodeOrdinal,
                                                 submitCommand->EngineOrdinal,
                                                 TRUE);
        }
        adapter->ReleaseNativeSubmissionOperation();
        return STATUS_SUCCESS;
    }

    if (submission != NULL)
    {
        ReleasePreparedSubmission(submission);
    }
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
                if (ownership != VioGpuNativePassiveOwnershipWorkerOwned)
                {
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
                        cleaned = CancelPagingTransaction(&pagingPrivate->Transaction) || cleaned;
                    }
                }
                else
                {
                    /* The system worker already dequeued this batch and owns
                     * both the callback context and every allocation reference. */
                    cleaned = TRUE;
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
                if (recordCountFallback != 0 && fallbackFirstPrivate->Header.Kind == VioGpuWddmDmaKindPaging &&
                    fallbackFirstPrivate->Transaction.Adapter == adapter)
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
                    if (ownership != VioGpuNativePassiveOwnershipWorkerOwned &&
                        pagingPrivate->Header.Kind == VioGpuWddmDmaKindPaging &&
                        pagingPrivate->Transaction.Adapter == adapter)
                    {
                        cleaned = CancelPagingTransaction(&pagingPrivate->Transaction) || cleaned;
                    }
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
                    ReleasePreparedSubmission(submission);
                    cleaned = TRUE;
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
    if (!valid || adapter->IsHardwareResetRequested() || !adapter->IsNativeFenceQueueEmpty())
    {
        /* Native Context has no Host cancellation primitive.  Do not fabricate
         * a preemption while a command may still access guest memory; gate the
         * transport and let the scheduler enter adapter-wide TDR. */
        adapter->ResetDevice();
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

#pragma code_seg(push)
#pragma code_seg("PAGE")

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
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(setVidPnSourceAddress);
    return STATUS_NOT_SUPPORTED;
}
