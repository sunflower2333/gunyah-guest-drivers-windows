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
const ULONG VIOGPU_WDDM_SUBMISSION_SIGNATURE = 'sWGV';

const UINT VIOGPU_WDDM_SEGMENT_ID = 1;
const UINT VIOGPU_WDDM_DMA_BUFFER_SIZE = 64 * 1024;
const UINT VIOGPU_WDDM_ALLOCATION_LIST_SIZE = 64;
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
    if (allocation == NULL || allocation->ContextRange == NULL)
    {
        return STATUS_SUCCESS;
    }

    VIOGPU_WDDM_ALLOCATION_RANGE *range = allocation->ContextRange;
    VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration = allocation->NativeContext;
    if (registration == NULL || range->Registration != registration)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&registration->BindingLock, &oldIrql);
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
    if (!IsNativeAllocation(allocation) || allocation->NativeContext == NULL || allocation->ContextRange == NULL)
    {
        return FALSE;
    }
    VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration = allocation->NativeContext;
    VIOGPU_WDDM_ALLOCATION_RANGE *range = allocation->ContextRange;
    KIRQL oldIrql;
    KeAcquireSpinLock(&registration->BindingLock, &oldIrql);
    BOOLEAN valid = range->Linked && range->Registration == registration &&
                    range->Iova == allocation->PrivateData.RequestedIova && range->Length == allocation->BackingSize;
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
        header->CommandStreamSize == 0 || header->CommandStreamOffset >= commandLength ||
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

NTSTATUS CopyNativePlacement(VIOGPU_WDDM_ALLOCATION *allocation,
                             ULONGLONG segmentOffset,
                             SIZE_T allocationOffset,
                             SIZE_T transferSize,
                             ULONGLONG expectedPoolGeneration,
                             PVOID systemAddress,
                             BOOLEAN toSegment,
                             ULONGLONG *poolGeneration)
{
    if (allocation == NULL || allocation->Adapter == NULL || systemAddress == NULL || poolGeneration == NULL ||
        transferSize == 0 || allocationOffset > allocation->BackingSize ||
        transferSize > allocation->BackingSize - allocationOffset || segmentOffset > MAXULONG_PTR)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *poolGeneration = 0;

    VioGpuGuestPoolMapping mapping;
    KeEnterGuardedRegion();
    BOOLEAN acquired = allocation->Adapter->AcquireGpuGuestPoolMapping(&mapping);
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
                             ULONGLONG *poolGeneration)
{
    if (allocation == NULL || allocation->Adapter == NULL || poolGeneration == NULL || segmentOffset > MAXULONG_PTR ||
        fillSize == 0 || fillSize > allocation->BackingSize || (fillSize & (sizeof(ULONG) - 1)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *poolGeneration = 0;

    VioGpuGuestPoolMapping mapping;
    KeEnterGuardedRegion();
    BOOLEAN acquired = allocation->Adapter->AcquireGpuGuestPoolMapping(&mapping);
    BOOLEAN valid = acquired && mapping.GetBaseAddress() != NULL && mapping.GetGeneration() != 0 &&
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
        request.VaStart != 0 || request.VaSize != 0 || request.ResetGeneration != 0 || request.ContextId != 0)
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
                                    UINT allocationListSize)
{
    if (header == NULL || device == NULL || allocationList == NULL ||
        header->CommandStreamSize < sizeof(MSM_CCMD_GEM_SUBMIT_REQ) ||
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
        request->queue_id == 0 || request->fence == 0 || request->nr_bos != header->AllocationReferenceCount ||
        request->nr_bos == 0 || request->nr_bos > VioGpuWddmSubmissionAllocationLimit || request->nr_cmds == 0)
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
            allocation->Adapter != device->Adapter || bos[index].Handle != allocation->ResourceId ||
            bos[index].Presumed != 0 || bos[index].Flags == 0 || (bos[index].Flags & ~validBoFlags) != 0 ||
            (bos[index].Flags & (VIOGPU_WDDM_MSM_SUBMIT_BO_READ | VIOGPU_WDDM_MSM_SUBMIT_BO_WRITE)) != expectedAccess ||
            reference->PatchOffset != expectedPatchOffset)
        {
            return STATUS_INVALID_PARAMETER;
        }
        for (UINT previous = 0; previous < index; ++previous)
        {
            if (bos[previous].Handle == bos[index].Handle)
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

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmBuildPagingBuffer(CONST HANDLE hAdapter,
                                                                     DXGKARG_BUILDPAGINGBUFFER *pagingBuffer)
{
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || pagingBuffer == NULL || pagingBuffer->pDmaBuffer == NULL ||
        pagingBuffer->pDmaBufferPrivateData == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Keep the output cursors stable until the complete synchronous operation succeeds.
    PVOID dmaBuffer = pagingBuffer->pDmaBuffer;
    UINT dmaSize = pagingBuffer->DmaSize;
    PVOID dmaPrivateBuffer = pagingBuffer->pDmaBufferPrivateData;
    UINT dmaPrivateSize = pagingBuffer->DmaBufferPrivateDataSize;

    VIOGPU_WDDM_ALLOCATION *allocation = NULL;
    LARGE_INTEGER segmentAddress = {};
    BOOLEAN pageIn = FALSE;
    BOOLEAN pageOut = FALSE;
    BOOLEAN fill = FALSE;
    BOOLEAN discard = FALSE;
    PMDL transferMdl = NULL;
    UINT mdlOffset = 0;
    UINT transferOffset = 0;
    SIZE_T transferSize = 0;
    BOOLEAN transferStart = FALSE;
    BOOLEAN transferEnd = FALSE;
    UINT packetFlags = VioGpuWddmPagingFlagSoftwareCompleted;
    SIZE_T packetTransferSize = 0;
    ULONGLONG packetPoolGeneration = 0;

    switch (pagingBuffer->Operation)
    {
        case DXGK_OPERATION_TRANSFER:
            allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(pagingBuffer->Transfer.hAllocation);
            if ((pagingBuffer->Transfer.Flags.Value & ~0x1CU) != 0)
            {
                return STATUS_INVALID_PARAMETER;
            }
            transferOffset = pagingBuffer->Transfer.TransferOffset;
            transferSize = pagingBuffer->Transfer.TransferSize;
            transferStart = pagingBuffer->Transfer.Flags.TransferStart;
            transferEnd = pagingBuffer->Transfer.Flags.TransferEnd;
            if (pagingBuffer->Transfer.Source.SegmentId == 0 &&
                pagingBuffer->Transfer.Destination.SegmentId == VIOGPU_WDDM_SEGMENT_ID)
            {
                pageIn = TRUE;
                packetFlags |= VioGpuWddmPagingFlagPageIn;
                segmentAddress = pagingBuffer->Transfer.Destination.SegmentAddress;
                transferMdl = pagingBuffer->Transfer.Source.pMdl;
            }
            else if (pagingBuffer->Transfer.Source.SegmentId == VIOGPU_WDDM_SEGMENT_ID &&
                     pagingBuffer->Transfer.Destination.SegmentId == 0)
            {
                pageOut = TRUE;
                packetFlags |= VioGpuWddmPagingFlagPageOut;
                segmentAddress = pagingBuffer->Transfer.Source.SegmentAddress;
                transferMdl = pagingBuffer->Transfer.Destination.pMdl;
            }
            else
            {
                return STATUS_NOT_SUPPORTED;
            }
            mdlOffset = pagingBuffer->Transfer.MdlOffset;
            if (transferStart)
            {
                packetFlags |= VioGpuWddmPagingFlagTransferStart;
            }
            if (transferEnd)
            {
                packetFlags |= VioGpuWddmPagingFlagTransferEnd;
            }
            if (pagingBuffer->Transfer.Flags.AllocationIsIdle)
            {
                packetFlags |= VioGpuWddmPagingFlagAllocationIdle;
            }
            packetTransferSize = transferSize;
            break;

        case DXGK_OPERATION_FILL:
            allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(pagingBuffer->Fill.hAllocation);
            if (pagingBuffer->Fill.Destination.SegmentId != VIOGPU_WDDM_SEGMENT_ID)
            {
                return STATUS_NOT_SUPPORTED;
            }
            fill = TRUE;
            packetFlags |= VioGpuWddmPagingFlagFill;
            segmentAddress = pagingBuffer->Fill.Destination.SegmentAddress;
            packetTransferSize = pagingBuffer->Fill.FillSize;
            break;

        case DXGK_OPERATION_DISCARD_CONTENT:
            allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(pagingBuffer->DiscardContent.hAllocation);
            if ((pagingBuffer->DiscardContent.Flags.Value & ~1U) != 0)
            {
                return STATUS_INVALID_PARAMETER;
            }
            if (pagingBuffer->DiscardContent.SegmentId != VIOGPU_WDDM_SEGMENT_ID)
            {
                return STATUS_SUCCESS;
            }
            discard = TRUE;
            packetFlags |= VioGpuWddmPagingFlagDiscard;
            if (pagingBuffer->DiscardContent.Flags.AllocationIsIdle)
            {
                packetFlags |= VioGpuWddmPagingFlagAllocationIdle;
            }
            segmentAddress = pagingBuffer->DiscardContent.SegmentAddress;
            break;

        default:
            return STATUS_NOT_SUPPORTED;
    }

    if (allocation == NULL || allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE ||
        allocation->Adapter != adapter)
    {
        return STATUS_INVALID_HANDLE;
    }
    if (!IsNativeAllocation(allocation))
    {
        return discard ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    }
    if (allocation->ResourceId < VIOGPU_NATIVE_RESOURCE_ID_START || allocation->BlobId == 0 ||
        allocation->ResourceId != allocation->BlobId)
    {
        return STATUS_INVALID_HANDLE;
    }

    if (dmaSize < sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET) || dmaPrivateSize < sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE))
    {
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }

    if ((pageIn || pageOut) && pagingBuffer->Transfer.TransferSize == 0)
    {
        NTSTATUS status = AcquireAllocationLifecycle(allocation);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        status = AllocationResetRetired(allocation) ? STATUS_SUCCESS : STATUS_GRAPHICS_ALLOCATION_BUSY;
        if (NT_SUCCESS(status))
        {
            ClearAllocationHostBinding(allocation);
            ClearNativePlacement(allocation);
            ClearNativePagingState(allocation);
        }
        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
        return status;
    }

    if ((pageIn || pageOut) && (transferMdl == NULL || transferSize == 0 || transferOffset > allocation->BackingSize ||
                                transferSize > allocation->BackingSize - transferOffset))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (fill && (pagingBuffer->Fill.FillSize != allocation->BackingSize ||
                 (pagingBuffer->Fill.FillSize & (sizeof(ULONG) - 1)) != 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (pageOut && !pagingBuffer->Transfer.Flags.AllocationIsIdle)
    {
        return STATUS_GRAPHICS_ALLOCATION_BUSY;
    }

    ULONGLONG placementOffset = 0;
    NTSTATUS status = ValidateNativePlacement(allocation, segmentAddress, &placementOffset);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    PVOID systemAddress = NULL;
    if (pageIn || pageOut)
    {
        status = ResolveTransferMdlAddress(transferMdl, mdlOffset, transferSize, &systemAddress);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
    BOOLEAN snapshotAcquired = AcquireAllocationNativeContextSnapshot(allocation, &snapshot);
    if (!snapshotAcquired && (pageIn || pageOut || fill))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    status = AcquireAllocationLifecycle(allocation);
    if (!NT_SUCCESS(status))
    {
        if (snapshotAcquired)
        {
            VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
        }
        return status;
    }

    if (allocation->Signature != VIOGPU_WDDM_ALLOCATION_SIGNATURE || allocation->Adapter != adapter)
    {
        status = STATUS_INVALID_HANDLE;
    }
    else if (pageIn)
    {
        packetPoolGeneration = allocation->PoolGeneration;
        if (transferStart)
        {
            if (allocation->PagingState != VioGpuWddmAllocationPagingIdle ||
                allocation->HostState != VioGpuWddmAllocationHostNone || allocation->PlacementValid)
            {
                status = STATUS_INVALID_DEVICE_STATE;
            }
            else
            {
                ClearPagingRanges(allocation);
                allocation->PagingState = VioGpuWddmAllocationPagingIn;
                allocation->PagingPlacementOffset = placementOffset;
                allocation->PagingPoolGeneration = 0;
            }
        }
        else if (allocation->PagingState != VioGpuWddmAllocationPagingIn ||
                 allocation->PagingPlacementOffset != placementOffset ||
                 allocation->HostState != VioGpuWddmAllocationHostNone || allocation->PlacementValid)
        {
            status = STATUS_INVALID_DEVICE_STATE;
        }

        if (NT_SUCCESS(status))
        {
            VIOGPU_WDDM_PAGING_RANGE *pagingRange = NULL;
            status = AddPagingRange(allocation, transferOffset, transferSize, &pagingRange);
            ULONGLONG poolGeneration = 0;
            if (NT_SUCCESS(status))
            {
                status = CopyNativePlacement(allocation,
                                             placementOffset,
                                             transferOffset,
                                             transferSize,
                                             allocation->PagingPoolGeneration,
                                             systemAddress,
                                             TRUE,
                                             &poolGeneration);
            }
            if (!NT_SUCCESS(status) && pagingRange != NULL)
            {
                RemovePagingRange(allocation, pagingRange);
            }
            if (NT_SUCCESS(status) && allocation->PagingPoolGeneration != 0 &&
                poolGeneration != allocation->PagingPoolGeneration)
            {
                if (pagingRange != NULL)
                {
                    RemovePagingRange(allocation, pagingRange);
                }
                status = STATUS_DEVICE_NOT_READY;
            }
            if (NT_SUCCESS(status))
            {
                allocation->PagingPoolGeneration = poolGeneration;
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
                                                                                                  placementOffset,
                                                                                                  allocation->PagingPoolGeneration,
                                                                                                  msmFlags,
                                                                                                  blobFlags,
                                                                                                  &ownershipRetained);
                if (ownershipRetained)
                {
                    PublishNativePlacement(allocation,
                                           &snapshot,
                                           placementOffset,
                                           allocation->PagingPoolGeneration,
                                           result == VioGpuHostContextConfirmed ? VioGpuWddmAllocationHostLive
                                                                                : VioGpuWddmAllocationHostUnknown);
                    packetPoolGeneration = allocation->PoolGeneration;
                }
                status = result == VioGpuHostContextConfirmed && ownershipRetained ? STATUS_SUCCESS
                                                                                   : STATUS_DEVICE_NOT_READY;
                if (ownershipRetained)
                {
                    ClearNativePagingState(allocation);
                }
                else if (!NT_SUCCESS(status))
                {
                    ClearNativePagingState(allocation);
                }
            }
            else if (!NT_SUCCESS(status))
            {
                ClearNativePagingState(allocation);
            }
        }
    }
    else if (fill)
    {
        packetPoolGeneration = allocation->PoolGeneration;
        if (allocation->PagingState != VioGpuWddmAllocationPagingIdle ||
            allocation->HostState != VioGpuWddmAllocationHostNone || allocation->PlacementValid)
        {
            status = STATUS_INVALID_DEVICE_STATE;
        }
        else
        {
            ULONGLONG poolGeneration = 0;
            status = FillNativePlacement(allocation,
                                         placementOffset,
                                         pagingBuffer->Fill.FillSize,
                                         pagingBuffer->Fill.FillPattern,
                                         &poolGeneration);
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
                                                                                                  placementOffset,
                                                                                                  poolGeneration,
                                                                                                  msmFlags,
                                                                                                  blobFlags,
                                                                                                  &ownershipRetained);
                if (ownershipRetained)
                {
                    PublishNativePlacement(allocation,
                                           &snapshot,
                                           placementOffset,
                                           poolGeneration,
                                           result == VioGpuHostContextConfirmed ? VioGpuWddmAllocationHostLive
                                                                                : VioGpuWddmAllocationHostUnknown);
                    packetPoolGeneration = allocation->PoolGeneration;
                }
                status = result == VioGpuHostContextConfirmed && ownershipRetained ? STATUS_SUCCESS
                                                                                   : STATUS_DEVICE_NOT_READY;
            }
        }
    }
    else if (pageOut)
    {
        packetPoolGeneration = allocation->PoolGeneration;
        BOOLEAN pagingStarted = FALSE;
        if (transferStart)
        {
            if (allocation->PagingState != VioGpuWddmAllocationPagingIdle ||
                allocation->HostState != VioGpuWddmAllocationHostLive || !allocation->PlacementValid ||
                allocation->PlacementOffset != placementOffset || allocation->PoolGeneration == 0)
            {
                status = STATUS_INVALID_DEVICE_STATE;
            }
            else
            {
                ClearPagingRanges(allocation);
                allocation->PagingState = VioGpuWddmAllocationPagingOut;
                allocation->PagingPlacementOffset = placementOffset;
                allocation->PagingPoolGeneration = allocation->PoolGeneration;
                pagingStarted = TRUE;
            }
        }
        else if (allocation->PagingState != VioGpuWddmAllocationPagingOut ||
                 allocation->PagingPlacementOffset != placementOffset ||
                 allocation->HostState != VioGpuWddmAllocationHostLive || !allocation->PlacementValid ||
                 allocation->PlacementOffset != placementOffset ||
                 allocation->PagingPoolGeneration != allocation->PoolGeneration)
        {
            status = STATUS_INVALID_DEVICE_STATE;
        }

        if (NT_SUCCESS(status))
        {
            VIOGPU_WDDM_PAGING_RANGE *pagingRange = NULL;
            status = AddPagingRange(allocation, transferOffset, transferSize, &pagingRange);
            ULONGLONG observedPoolGeneration = 0;
            if (NT_SUCCESS(status))
            {
                status = CopyNativePlacement(allocation,
                                             placementOffset,
                                             transferOffset,
                                             transferSize,
                                             allocation->PagingPoolGeneration,
                                             systemAddress,
                                             FALSE,
                                             &observedPoolGeneration);
            }
            if (!NT_SUCCESS(status) && pagingRange != NULL)
            {
                RemovePagingRange(allocation, pagingRange);
            }
            if (NT_SUCCESS(status) && observedPoolGeneration != allocation->PagingPoolGeneration)
            {
                if (pagingRange != NULL)
                {
                    RemovePagingRange(allocation, pagingRange);
                }
                status = STATUS_DEVICE_NOT_READY;
            }
            if (NT_SUCCESS(status) && transferEnd && allocation->PagingCoveredBytes != allocation->BackingSize)
            {
                status = STATUS_INVALID_PARAMETER;
            }
            if (NT_SUCCESS(status) && transferEnd)
            {
                status = ReleaseAllocationHostOwnership(allocation, &snapshot, snapshotAcquired);
                if (NT_SUCCESS(status))
                {
                    ClearNativePlacement(allocation);
                    ClearNativePagingState(allocation);
                }
                else
                {
                    ClearNativePagingState(allocation);
                }
            }
            else if (!NT_SUCCESS(status))
            {
                ClearNativePagingState(allocation);
            }
        }
    }
    else
    {
        packetPoolGeneration = allocation->PoolGeneration;
        if (allocation->PagingState != VioGpuWddmAllocationPagingIdle || !allocation->PlacementValid ||
            allocation->PlacementOffset != placementOffset)
        {
            status = STATUS_INVALID_PARAMETER;
        }
        else if (discard && !pagingBuffer->DiscardContent.Flags.AllocationIsIdle &&
                 allocation->HostState != VioGpuWddmAllocationHostNone)
        {
            status = STATUS_GRAPHICS_ALLOCATION_BUSY;
        }
        else
        {
            status = ReleaseAllocationHostOwnership(allocation, &snapshot, snapshotAcquired);
            if (NT_SUCCESS(status))
            {
                ClearNativePlacement(allocation);
                ClearNativePagingState(allocation);
            }
        }
    }

    if (NT_SUCCESS(status))
    {
        VIOGPU_WDDM_PAGING_DMA_PACKET *packet = static_cast<VIOGPU_WDDM_PAGING_DMA_PACKET *>(dmaBuffer);
        VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData = static_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE *>(dmaPrivateBuffer);
        RtlZeroMemory(packet, sizeof(*packet));
        packet->Signature = VIOGPU_WDDM_PAGING_DMA_SIGNATURE;
        packet->Version = VioGpuWddmDmaPrivateVersion;
        packet->Size = static_cast<USHORT>(sizeof(*packet));
        packet->Operation = static_cast<UINT>(pagingBuffer->Operation);
        packet->Flags = packetFlags;
        packet->ResourceId = allocation->ResourceId;
        packet->ContextId = snapshotAcquired ? snapshot.ContextId : 0;
        packet->ContextGeneration = snapshotAcquired ? snapshot.Generation : 0;
        packet->ResetGeneration = snapshotAcquired ? snapshot.ResetGeneration : 0;
        packet->PlacementOffset = placementOffset;
        packet->PoolGeneration = packetPoolGeneration;
        packet->TransferOffset = transferOffset;
        packet->TransferSize = static_cast<ULONGLONG>(packetTransferSize);
        packet->Reserved = 0;
        RtlZeroMemory(privateData, sizeof(*privateData));
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
        pagingBuffer->pDmaBuffer = static_cast<BYTE *>(dmaBuffer) + sizeof(*packet);
        pagingBuffer->DmaSize = dmaSize - sizeof(*packet);
        pagingBuffer->pDmaBufferPrivateData = static_cast<BYTE *>(dmaPrivateBuffer) + sizeof(*privateData);
        pagingBuffer->DmaBufferPrivateDataSize = dmaPrivateSize - sizeof(*privateData);
    }

    KeReleaseMutex(&allocation->LifecycleMutex, FALSE);
    if (snapshotAcquired)
    {
        VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);
    }
    return status;
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
                                                    render->AllocationListSize);
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
        patchArguments->hContext == NULL || patchArguments->pDmaBuffer == NULL ||
        patchArguments->pDmaBufferPrivateData == NULL || patchArguments->pAllocationList == NULL ||
        patchArguments->pPatchLocationList == NULL || patchArguments->DmaBufferSegmentId != 0 ||
        patchArguments->Flags.Value != 0 || patchArguments->EngineOrdinal != 0 ||
        patchArguments->SubmissionFenceId == 0 ||
        patchArguments->DmaBufferSubmissionStartOffset > patchArguments->DmaBufferSubmissionEndOffset ||
        patchArguments->DmaBufferSubmissionEndOffset > patchArguments->DmaBufferSize ||
        patchArguments->DmaBufferPrivateDataSubmissionStartOffset > patchArguments->DmaBufferPrivateDataSubmissionEndOffset ||
        patchArguments->DmaBufferPrivateDataSubmissionEndOffset > patchArguments->DmaBufferPrivateDataSize ||
        patchArguments->PatchLocationListSubmissionStart > patchArguments->PatchLocationListSize ||
        patchArguments->PatchLocationListSubmissionLength > patchArguments->PatchLocationListSize - patchArguments->PatchLocationListSubmissionStart)
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
                            allocation->PoolGeneration != 0 && allocation->ContextId == submission->ContextId &&
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
            PVOID patchAddress = static_cast<BYTE *>(submission->CommandStream) + reference->PatchOffset;
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
        UINT dmaLength = submitCommand->DmaBufferSubmissionEndOffset - submitCommand->DmaBufferSubmissionStartOffset;
        VIOGPU_WDDM_PAGING_DMA_PACKET *packet = static_cast<VIOGPU_WDDM_PAGING_DMA_PACKET *>(privateData->Packet);
        BOOLEAN valid = submitCommand->hContext == NULL && submitCommand->Flags.Value == 1 &&
                        privateLength == sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) &&
                        submitCommand->DmaBufferSize - submitCommand->DmaBufferSubmissionStartOffset == privateData->DmaBufferSize &&
                        submitCommand->DmaBufferPrivateDataSize - privateStart >= sizeof(*privateData) &&
                        dmaLength == sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET) &&
                        privateData->CommandLength == sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET) &&
                        privateData->Submission == NULL && packet != NULL &&
                        packet->Signature == VIOGPU_WDDM_PAGING_DMA_SIGNATURE &&
                        packet->Version == VioGpuWddmDmaPrivateVersion && packet->Size == sizeof(*packet) &&
                        packet->Flags == privateData->Flags &&
                        (packet->Flags & VioGpuWddmPagingFlagSoftwareCompleted) != 0 &&
                        packet->ContextId == privateData->ContextId &&
                        packet->ContextGeneration == privateData->Generation &&
                        packet->ResetGeneration == privateData->ResetGeneration && packet->Reserved == 0;
        if (valid)
        {
            if (adapter->RecordNativeSubmissionFence(submitCommand->SubmissionFenceId))
            {
                adapter->NotifyNativeSoftwareCompletion(submitCommand->SubmissionFenceId,
                                                        submitCommand->NodeOrdinal,
                                                        submitCommand->EngineOrdinal);
            }
            else
            {
                adapter->NotifyNativeSubmissionFault(submitCommand->SubmissionFenceId,
                                                     STATUS_INSUFFICIENT_RESOURCES,
                                                     submitCommand->NodeOrdinal,
                                                     submitCommand->EngineOrdinal,
                                                     TRUE);
            }
            adapter->ReleaseNativeSubmissionOperation();
            return STATUS_SUCCESS;
        }
        status = STATUS_INVALID_PARAMETER;
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
        int queueResult = recorded ? adapter->QueueNativeSubmit(buffer, fenceId) : -1;
        if (!recorded || queueResult < 0)
        {
            ReleaseQueuedSubmission(submission, TRUE);
            adapter->NotifyNativeSubmissionFault(fenceId,
                                                 recorded ? STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE
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
    VioGpuDod *adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || currentFence == NULL || currentFence->NodeOrdinal != 0 || currentFence->EngineOrdinal != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    currentFence->CurrentFence = adapter->QueryNativeCompletedFence();
    return STATUS_SUCCESS;
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
