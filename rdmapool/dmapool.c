/*
 * Restricted DMA Pool - Bitmap Page Allocator Implementation
 *
 * Manages a fixed physical memory region using a bitmap allocator.
 * Each bit represents one PAGE_SIZE page. Set = allocated.
 *
 * Thread-safe: protected by a spinlock.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <ntddk.h>
#include "rdmapool.h"
#include "dmapool.h"

#define RDMAPOOL_TAG            'PMDR'
#define RDMAPOOL_ALLOCATION_TAG 'AMDR'

typedef struct _RDMAPOOL_ALLOCATION_RECORD
{
    LIST_ENTRY ListEntry;
    PRDMAPOOL_FILE_CONTEXT Owner;
    PVOID VirtualAddress;
    ULONG StartPage;
    ULONG NumPages;
    ULONG64 Token;
    BOOLEAN Initializing;
    BOOLEAN Cancelled;
} RDMAPOOL_ALLOCATION_RECORD, *PRDMAPOOL_ALLOCATION_RECORD;

static PHYSICAL_ADDRESS gPoolPhysBase;
static PVOID gPoolVirtBase;
static ULONG gPoolTotalPages;
static SIZE_T gPoolTotalSize;

static PUCHAR gBitmap;
static ULONG gBitmapBytes;
static KSPIN_LOCK gPoolLock;
static LIST_ENTRY gAllocationList;
static ULONG64 gNextAllocationToken;
static ULONG gInitializingCount;
static KEVENT gNoInitializersEvent;
static BOOLEAN gPoolReady;

static __forceinline BOOLEAN BitmapTestBit(ULONG Index)
{
    return (gBitmap[Index / 8] & (1U << (Index % 8))) != 0;
}

static __forceinline VOID BitmapSetBit(ULONG Index)
{
    gBitmap[Index / 8] |= (UCHAR)(1U << (Index % 8));
}

static __forceinline VOID BitmapClearBit(ULONG Index)
{
    gBitmap[Index / 8] &= (UCHAR) ~(1U << (Index % 8));
}

static VOID BitmapQueryFreePages(_Out_ ULONG *FreePages, _Out_ ULONG *LargestFreeRunPages)
{
    ULONG CurrentFreeRun = 0;
    ULONG i;

    *FreePages = 0;
    *LargestFreeRunPages = 0;
    for (i = 0; i < gPoolTotalPages; i++)
    {
        if (BitmapTestBit(i))
        {
            CurrentFreeRun = 0;
        }
        else
        {
            (*FreePages)++;
            CurrentFreeRun++;
            if (CurrentFreeRun > *LargestFreeRunPages)
            {
                *LargestFreeRunPages = CurrentFreeRun;
            }
        }
    }
}

/*
 * Find a contiguous run of free pages in the bitmap.
 */
static BOOLEAN BitmapFindFreeRun(_In_ ULONG NumPages, _Out_ ULONG *StartPage)
{
    ULONG RunStart = 0;
    ULONG RunLen = 0;
    ULONG i;

    for (i = 0; i < gPoolTotalPages; i++)
    {
        if (BitmapTestBit(i))
        {
            RunLen = 0;
            RunStart = i + 1;
        }
        else
        {
            RunLen++;
            if (RunLen >= NumPages)
            {
                *StartPage = RunStart;
                return TRUE;
            }
        }
    }

    return FALSE;
}

NTSTATUS
DmaPoolInit(_In_ PHYSICAL_ADDRESS PhysicalBase, _In_ PVOID VirtualBase, _In_ SIZE_T TotalSize)
{
    ULONG64 physicalBase = (ULONG64)PhysicalBase.QuadPart;

    if (VirtualBase == NULL || ((ULONG_PTR)VirtualBase & (PAGE_SIZE - 1)) != 0 || PhysicalBase.QuadPart < 0 ||
        (physicalBase & (PAGE_SIZE - 1)) != 0 || TotalSize < PAGE_SIZE || (TotalSize & (PAGE_SIZE - 1)) != 0 ||
        (TotalSize / PAGE_SIZE) > MAXULONG || (ULONG_PTR)VirtualBase > MAXULONG_PTR - (TotalSize - 1) ||
        physicalBase > MAXULONGLONG - (TotalSize - 1))
    {
        return STATUS_INVALID_PARAMETER;
    }

    gPoolPhysBase = PhysicalBase;
    gPoolVirtBase = VirtualBase;
    gPoolTotalSize = TotalSize;
    gPoolTotalPages = (ULONG)(TotalSize / PAGE_SIZE);

    gBitmapBytes = (gPoolTotalPages / 8) + ((gPoolTotalPages % 8) != 0 ? 1 : 0);
    gBitmap = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, gBitmapBytes, RDMAPOOL_TAG);
    if (gBitmap == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(gBitmap, gBitmapBytes);
    KeInitializeSpinLock(&gPoolLock);
    InitializeListHead(&gAllocationList);
    gInitializingCount = 0;
    KeInitializeEvent(&gNoInitializersEvent, NotificationEvent, TRUE);
    gPoolReady = TRUE;

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "rdmapool: DmaPoolInit base PA=0x%llx VA=%p size=0x%llx (%u pages)\n",
               PhysicalBase.QuadPart,
               VirtualBase,
               (ULONG64)TotalSize,
               gPoolTotalPages);

    return STATUS_SUCCESS;
}

VOID DmaPoolDestroy(VOID)
{
    KIRQL OldIrql;
    LIST_ENTRY ReleasedAllocations;
    PLIST_ENTRY Entry;

    InitializeListHead(&ReleasedAllocations);

    KeAcquireSpinLock(&gPoolLock, &OldIrql);
    gPoolReady = FALSE;
    for (Entry = gAllocationList.Flink; Entry != &gAllocationList; Entry = Entry->Flink)
    {
        PRDMAPOOL_ALLOCATION_RECORD Allocation = CONTAINING_RECORD(Entry, RDMAPOOL_ALLOCATION_RECORD, ListEntry);
        if (Allocation->Initializing)
        {
            Allocation->Cancelled = TRUE;
        }
    }
    KeReleaseSpinLock(&gPoolLock, OldIrql);

    (void)KeWaitForSingleObject(&gNoInitializersEvent, Executive, KernelMode, FALSE, NULL);

    KeAcquireSpinLock(&gPoolLock, &OldIrql);
    while (!IsListEmpty(&gAllocationList))
    {
        Entry = RemoveHeadList(&gAllocationList);
        InsertTailList(&ReleasedAllocations, Entry);
    }
    KeReleaseSpinLock(&gPoolLock, OldIrql);

    while (!IsListEmpty(&ReleasedAllocations))
    {
        Entry = RemoveHeadList(&ReleasedAllocations);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, RDMAPOOL_ALLOCATION_RECORD, ListEntry), RDMAPOOL_ALLOCATION_TAG);
    }

    if (gBitmap != NULL)
    {
        ExFreePoolWithTag(gBitmap, RDMAPOOL_TAG);
        gBitmap = NULL;
    }
    gPoolVirtBase = NULL;
    gPoolTotalPages = 0;
    gPoolTotalSize = 0;
}

NTSTATUS
DmaPoolAllocatePages(_In_ PRDMAPOOL_FILE_CONTEXT Owner,
                     _In_ ULONG NumPages,
                     _Out_ PVOID *VirtualAddress,
                     _Out_ PHYSICAL_ADDRESS *PhysicalAddress,
                     _Out_ ULONG64 *AllocationToken)
{
    KIRQL OldIrql;
    ULONG StartPage;
    ULONG i;
    PRDMAPOOL_ALLOCATION_RECORD Allocation;
    PVOID allocationVa;
    PHYSICAL_ADDRESS allocationPa;
    ULONG64 token;
    NTSTATUS status;

    *VirtualAddress = NULL;
    PhysicalAddress->QuadPart = 0;
    *AllocationToken = 0;

    if (Owner == NULL || NumPages == 0 || gBitmap == NULL || NumPages > gPoolTotalPages)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Allocation = (PRDMAPOOL_ALLOCATION_RECORD)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                              sizeof(*Allocation),
                                                              RDMAPOOL_ALLOCATION_TAG);
    if (Allocation == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Allocation, sizeof(*Allocation));

    KeAcquireSpinLock(&gPoolLock, &OldIrql);

    if (!gPoolReady || Owner->Closing)
    {
        KeReleaseSpinLock(&gPoolLock, OldIrql);
        ExFreePoolWithTag(Allocation, RDMAPOOL_ALLOCATION_TAG);
        return Owner->Closing ? STATUS_FILE_CLOSED : STATUS_DEVICE_NOT_READY;
    }

    if (gNextAllocationToken == MAXULONGLONG)
    {
        KeReleaseSpinLock(&gPoolLock, OldIrql);
        ExFreePoolWithTag(Allocation, RDMAPOOL_ALLOCATION_TAG);
        return STATUS_INTEGER_OVERFLOW;
    }

    if (!BitmapFindFreeRun(NumPages, &StartPage))
    {
        ULONG FreePages;
        ULONG LargestFreeRun;

        BitmapQueryFreePages(&FreePages, &LargestFreeRun);

        KeReleaseSpinLock(&gPoolLock, OldIrql);
        ExFreePoolWithTag(Allocation, RDMAPOOL_ALLOCATION_TAG);
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "rdmapool: pool exhausted (requested %u pages, free %u, largest run %u, total %u)\n",
                   NumPages,
                   FreePages,
                   LargestFreeRun,
                   gPoolTotalPages);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = StartPage; i < StartPage + NumPages; i++)
    {
        BitmapSetBit(i);
    }

    gNextAllocationToken++;

    Allocation->Owner = Owner;
    Allocation->VirtualAddress = (PUCHAR)gPoolVirtBase + ((SIZE_T)StartPage * PAGE_SIZE);
    Allocation->StartPage = StartPage;
    Allocation->NumPages = NumPages;
    Allocation->Token = gNextAllocationToken;
    Allocation->Initializing = TRUE;
    Allocation->Cancelled = FALSE;
    InsertTailList(&gAllocationList, &Allocation->ListEntry);
    gInitializingCount++;
    if (gInitializingCount == 1)
    {
        KeClearEvent(&gNoInitializersEvent);
    }
    Owner->InitializingCount++;
    if (Owner->InitializingCount == 1)
    {
        KeClearEvent(&Owner->NoInitializersEvent);
    }

    allocationVa = Allocation->VirtualAddress;
    allocationPa.QuadPart = gPoolPhysBase.QuadPart + ((LONGLONG)StartPage * PAGE_SIZE);
    token = Allocation->Token;

    KeReleaseSpinLock(&gPoolLock, OldIrql);

    RtlZeroMemory(allocationVa, (SIZE_T)NumPages * PAGE_SIZE);

    KeAcquireSpinLock(&gPoolLock, &OldIrql);
    status = STATUS_SUCCESS;
    if (!gPoolReady || Owner->Closing || Allocation->Cancelled)
    {
        for (i = StartPage; i < StartPage + NumPages; i++)
        {
            BitmapClearBit(i);
        }
        RemoveEntryList(&Allocation->ListEntry);
        status = Owner->Closing ? STATUS_FILE_CLOSED : STATUS_DEVICE_NOT_READY;
    }
    else
    {
        Allocation->Initializing = FALSE;
    }
    gInitializingCount--;
    if (gInitializingCount == 0)
    {
        KeSetEvent(&gNoInitializersEvent, IO_NO_INCREMENT, FALSE);
    }
    Owner->InitializingCount--;
    if (Owner->InitializingCount == 0)
    {
        KeSetEvent(&Owner->NoInitializersEvent, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&gPoolLock, OldIrql);

    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(Allocation, RDMAPOOL_ALLOCATION_TAG);
        return status;
    }

    *VirtualAddress = allocationVa;
    *PhysicalAddress = allocationPa;
    *AllocationToken = token;

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_TRACE_LEVEL,
               "rdmapool: allocated %u pages @ VA=%p PA=0x%llx\n",
               NumPages,
               *VirtualAddress,
               PhysicalAddress->QuadPart);

    return STATUS_SUCCESS;
}

NTSTATUS
DmaPoolFreePages(_In_ PRDMAPOOL_FILE_CONTEXT Owner,
                 _In_ PVOID VirtualAddress,
                 _In_ ULONG NumPages,
                 _In_ ULONG64 AllocationToken)
{
    KIRQL OldIrql;
    ULONG_PTR Offset;
    ULONG StartPage;
    ULONG i;
    PLIST_ENTRY Entry;
    PRDMAPOOL_ALLOCATION_RECORD Allocation = NULL;

    if (Owner == NULL || VirtualAddress == NULL || NumPages == 0 || AllocationToken == 0 || gBitmap == NULL ||
        gPoolVirtBase == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((ULONG_PTR)VirtualAddress < (ULONG_PTR)gPoolVirtBase)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "rdmapool: free VA=%p below pool base %p\n",
                   VirtualAddress,
                   gPoolVirtBase);
        return STATUS_INVALID_ADDRESS;
    }

    Offset = (ULONG_PTR)VirtualAddress - (ULONG_PTR)gPoolVirtBase;
    if ((Offset & (PAGE_SIZE - 1)) != 0)
    {
        return STATUS_DATATYPE_MISALIGNMENT;
    }
    StartPage = (ULONG)(Offset / PAGE_SIZE);

    if (StartPage >= gPoolTotalPages || NumPages > gPoolTotalPages - StartPage)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "rdmapool: free out of range VA=%p pages=%u\n",
                   VirtualAddress,
                   NumPages);
        return STATUS_INVALID_ADDRESS;
    }

    KeAcquireSpinLock(&gPoolLock, &OldIrql);

    for (Entry = gAllocationList.Flink; Entry != &gAllocationList; Entry = Entry->Flink)
    {
        PRDMAPOOL_ALLOCATION_RECORD Candidate = CONTAINING_RECORD(Entry, RDMAPOOL_ALLOCATION_RECORD, ListEntry);
        if (Candidate->Token == AllocationToken)
        {
            Allocation = Candidate;
            break;
        }
    }

    if (Allocation == NULL || Allocation->Owner != Owner)
    {
        KeReleaseSpinLock(&gPoolLock, OldIrql);
        return STATUS_NOT_FOUND;
    }

    if (Allocation->Initializing || Allocation->Cancelled || Allocation->VirtualAddress != VirtualAddress ||
        Allocation->StartPage != StartPage || Allocation->NumPages != NumPages)
    {
        KeReleaseSpinLock(&gPoolLock, OldIrql);
        return STATUS_INVALID_PARAMETER;
    }

    for (i = StartPage; i < StartPage + NumPages; i++)
    {
        if (!BitmapTestBit(i))
        {
            KeReleaseSpinLock(&gPoolLock, OldIrql);
            return STATUS_DATA_ERROR;
        }
    }

    for (i = StartPage; i < StartPage + NumPages; i++)
    {
        BitmapClearBit(i);
    }
    RemoveEntryList(&Allocation->ListEntry);

    KeReleaseSpinLock(&gPoolLock, OldIrql);
    ExFreePoolWithTag(Allocation, RDMAPOOL_ALLOCATION_TAG);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_TRACE_LEVEL, "rdmapool: freed %u pages @ VA=%p\n", NumPages, VirtualAddress);
    return STATUS_SUCCESS;
}

ULONG DmaPoolCloseOwner(_In_ PRDMAPOOL_FILE_CONTEXT Owner)
{
    KIRQL OldIrql;
    PLIST_ENTRY Entry;
    PLIST_ENTRY Next;
    LIST_ENTRY ReleasedAllocations;
    ULONG Released = 0;

    if (Owner == NULL || gBitmap == NULL)
    {
        return 0;
    }

    InitializeListHead(&ReleasedAllocations);
    KeAcquireSpinLock(&gPoolLock, &OldIrql);
    Owner->Closing = TRUE;
    for (Entry = gAllocationList.Flink; Entry != &gAllocationList; Entry = Next)
    {
        PRDMAPOOL_ALLOCATION_RECORD Allocation = CONTAINING_RECORD(Entry, RDMAPOOL_ALLOCATION_RECORD, ListEntry);
        Next = Entry->Flink;
        if (Allocation->Owner == Owner)
        {
            if (Allocation->Initializing)
            {
                Allocation->Cancelled = TRUE;
                continue;
            }
            ULONG Page;
            for (Page = Allocation->StartPage; Page < Allocation->StartPage + Allocation->NumPages; Page++)
            {
                BitmapClearBit(Page);
            }
            RemoveEntryList(Entry);
            InsertTailList(&ReleasedAllocations, Entry);
            Released++;
        }
    }
    KeReleaseSpinLock(&gPoolLock, OldIrql);

    while (!IsListEmpty(&ReleasedAllocations))
    {
        Entry = RemoveHeadList(&ReleasedAllocations);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, RDMAPOOL_ALLOCATION_RECORD, ListEntry), RDMAPOOL_ALLOCATION_TAG);
    }

    /* Initializers retain Owner until they publish cancellation. Waiting on
     * this file only avoids coupling close to allocations owned by peers. */
    (void)KeWaitForSingleObject(&Owner->NoInitializersEvent, Executive, KernelMode, FALSE, NULL);
    return Released;
}

VOID DmaPoolQueryInfo(_Out_ PVOID *BaseVirtualAddress,
                      _Out_ PHYSICAL_ADDRESS *BasePhysicalAddress,
                      _Out_ ULONG64 *TotalSize)
{
    *BaseVirtualAddress = gPoolVirtBase;
    *BasePhysicalAddress = gPoolPhysBase;
    *TotalSize = (ULONG64)gPoolTotalSize;
}

VOID DmaPoolQueryAllocation(_Out_ ULONG *FreePages, _Out_ ULONG *LargestFreeRunPages)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&gPoolLock, &OldIrql);
    BitmapQueryFreePages(FreePages, LargestFreeRunPages);
    KeReleaseSpinLock(&gPoolLock, OldIrql);
}
