/*
 * rdmapool client library — shared by the Gunyah protected-VM storage
 * miniports (viostor, vioscsi). See rdmaclient.h for the design overview.
 *
 * Uses only WDM APIs (no StorPort), so it compiles unchanged into any
 * kernel-mode driver; all StorPort interaction stays in the caller.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <ntddk.h>

#include "rdmaclient.h"

#include <wdmguid.h>
#include <initguid.h>
#include "rdmapool_interface.h"

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* ------------------------------------------------------------------ */
/* rdmapool connect / IOCTL                                           */
/* ------------------------------------------------------------------ */

typedef struct _RDMA_CLIENT_IOCTL_RESULT
{
    NTSTATUS Status;
    ULONG_PTR Information;
    BOOLEAN Submitted;
} RDMA_CLIENT_IOCTL_RESULT;

static RDMA_CLIENT_IOCTL_RESULT RdmaClientIoctl(PRDMA_CLIENT c,
                                                ULONG IoControlCode,
                                                PVOID InputBuffer,
                                                ULONG InputBufferLength,
                                                PVOID OutputBuffer,
                                                ULONG OutputBufferLength)
{
    RDMA_CLIENT_IOCTL_RESULT result = {STATUS_INSUFFICIENT_RESOURCES, 0, FALSE};
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    PIO_STACK_LOCATION irpStack;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));

    irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        c->PoolDeviceObject,
                                        InputBuffer,
                                        InputBufferLength,
                                        OutputBuffer,
                                        OutputBufferLength,
                                        FALSE,
                                        &event,
                                        &iosb);
    if (irp == NULL)
    {
        return result;
    }

    irpStack = IoGetNextIrpStackLocation(irp);
    irpStack->FileObject = c->PoolFileObject;

    result.Submitted = TRUE;
    status = IoCallDriver(c->PoolDeviceObject, irp);
    if (status == STATUS_PENDING)
    {
        status = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        if (NT_SUCCESS(status))
        {
            status = iosb.Status;
        }
    }
    result.Status = status;
    result.Information = iosb.Information;
    return result;
}

static VOID RdmaClientClosePoolFile(PRDMA_CLIENT c)
{
    if (c->PoolFileObject != NULL)
    {
        ObDereferenceObject(c->PoolFileObject);
        c->PoolFileObject = NULL;
    }
    c->PoolDeviceObject = NULL;
}

static BOOLEAN RdmaClientValidPoolInfo(const RDMAPOOL_QUERY_POOL_OUTPUT *Query)
{
    ULONG64 physicalBase;

    if (Query->BaseVirtualAddress == NULL || ((ULONG_PTR)Query->BaseVirtualAddress & (PAGE_SIZE - 1)) != 0 ||
        Query->BasePhysicalAddress.QuadPart < 0 || Query->TotalSize < PAGE_SIZE ||
        (Query->TotalSize & (PAGE_SIZE - 1)) != 0 || Query->TotalSize > MAXULONG_PTR ||
        Query->TotalSize / PAGE_SIZE > MAXULONG)
    {
        return FALSE;
    }

    physicalBase = (ULONG64)Query->BasePhysicalAddress.QuadPart;
    return (physicalBase & (PAGE_SIZE - 1)) == 0 &&
           (ULONG_PTR)Query->BaseVirtualAddress <= MAXULONG_PTR - (ULONG_PTR)(Query->TotalSize - 1) &&
           physicalBase <= MAXULONGLONG - (Query->TotalSize - 1);
}

static BOOLEAN RdmaClientValidAllocation(const RDMAPOOL_QUERY_POOL_OUTPUT *Query,
                                         const RDMAPOOL_ALLOCATE_OUTPUT *Output,
                                         ULONG ExpectedPages)
{
    ULONG64 allocationSize = (ULONG64)ExpectedPages * PAGE_SIZE;
    ULONG_PTR baseVa = (ULONG_PTR)Query->BaseVirtualAddress;
    ULONG_PTR allocationVa = (ULONG_PTR)Output->VirtualAddress;
    ULONG64 basePa = (ULONG64)Query->BasePhysicalAddress.QuadPart;
    ULONG64 allocationPa;
    ULONG64 vaOffset;
    ULONG64 paOffset;

    if (Output->InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2 || Output->NumPages != ExpectedPages ||
        Output->AllocationToken == 0 || Output->VirtualAddress == NULL || (allocationVa & (PAGE_SIZE - 1)) != 0 ||
        Output->PhysicalAddress.QuadPart < 0 || allocationVa < baseVa || allocationSize > Query->TotalSize)
    {
        return FALSE;
    }

    allocationPa = (ULONG64)Output->PhysicalAddress.QuadPart;
    if ((allocationPa & (PAGE_SIZE - 1)) != 0 || allocationPa < basePa)
    {
        return FALSE;
    }

    vaOffset = (ULONG64)(allocationVa - baseVa);
    paOffset = allocationPa - basePa;
    return vaOffset == paOffset && vaOffset <= Query->TotalSize - allocationSize;
}

NTSTATUS RdmaClientConnect(PRDMA_CLIENT c, const char *Tag, ULONG RingPages, ULONG MetaPages)
{
    NTSTATUS status;
    PWSTR deviceInterfaceList = NULL;
    UNICODE_STRING deviceName;
    RDMAPOOL_QUERY_POOL_OUTPUT queryOutput;
    RDMAPOOL_ALLOCATE_INPUT allocInput;
    RDMAPOOL_ALLOCATE_OUTPUT allocOutput;
    RDMAPOOL_FREE_INPUT freeInput;
    ULONG totalPages, poolPages, bouncePages;
    RDMA_CLIENT_IOCTL_RESULT ioctlResult;
    ULONG64 bouncePageCount;
    ULONG64 requestedPages;

    if (c->Active || c->PoolFileObject != NULL || c->PoolDeviceObject != NULL || c->BaseVA != NULL || c->Size != 0 ||
        c->AllocationPages != 0 || c->AllocationToken != 0 || c->DisconnectAttempted)
    {
        return NT_SUCCESS(c->DisconnectStatus) ? STATUS_DEVICE_BUSY : c->DisconnectStatus;
    }

    c->Active = FALSE;
    c->Tag = Tag ? Tag : "rdmaclient";
    c->BaseVA = NULL;
    c->BasePA.QuadPart = 0;
    c->Size = 0;
    c->AllocationPages = 0;
    c->AllocationToken = 0;
    c->DisconnectAttempted = FALSE;
    c->DisconnectStatus = STATUS_SUCCESS;

    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_RDMAPOOL, NULL, 0, &deviceInterfaceList);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s rdmapool: interface enumeration failed 0x%x\n", c->Tag, status);
        if (deviceInterfaceList)
        {
            ExFreePool(deviceInterfaceList);
        }
        return status;
    }
    if (deviceInterfaceList == NULL || *deviceInterfaceList == L'\0')
    {
        DbgPrint("%s rdmapool: interface not found, using normal DMA\n", c->Tag);
        if (deviceInterfaceList)
        {
            ExFreePool(deviceInterfaceList);
        }
        return STATUS_NOT_FOUND;
    }

    RtlInitUnicodeString(&deviceName, deviceInterfaceList);
    status = IoGetDeviceObjectPointer(&deviceName, FILE_ALL_ACCESS, &c->PoolFileObject, &c->PoolDeviceObject);
    ExFreePool(deviceInterfaceList);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s rdmapool: IoGetDeviceObjectPointer failed 0x%x\n", c->Tag, status);
        c->PoolFileObject = NULL;
        c->PoolDeviceObject = NULL;
        return status;
    }

    RtlZeroMemory(&queryOutput, sizeof(queryOutput));
    ioctlResult = RdmaClientIoctl(c, (ULONG)IOCTL_RDMAPOOL_QUERY_POOL, NULL, 0, &queryOutput, sizeof(queryOutput));
    status = ioctlResult.Status;
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s rdmapool: QUERY_POOL failed 0x%x\n", c->Tag, status);
        RdmaClientClosePoolFile(c);
        return status;
    }
    if (ioctlResult.Information != sizeof(queryOutput))
    {
        DbgPrint("%s rdmapool: QUERY_POOL returned %Iu bytes, expected %Iu\n",
                 c->Tag,
                 ioctlResult.Information,
                 sizeof(queryOutput));
        RdmaClientClosePoolFile(c);
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (queryOutput.InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2)
    {
        RdmaClientClosePoolFile(c);
        return STATUS_REVISION_MISMATCH;
    }
    if (queryOutput.PageSize != PAGE_SIZE || !RdmaClientValidPoolInfo(&queryOutput))
    {
        RdmaClientClosePoolFile(c);
        return STATUS_DATA_ERROR;
    }

    poolPages = (ULONG)(queryOutput.TotalSize / PAGE_SIZE);

    /* Region = vrings + caller metadata (control slots / event area) + a data
     * area capped at 32MB / half the pool (the pool is shared with the other
     * pVM drivers). */
    bouncePageCount = (ULONG64)MetaPages + min(8192u, poolPages / 2);
    bouncePages = bouncePageCount > MAXULONG ? MAXULONG : (ULONG)bouncePageCount;
    requestedPages = (ULONG64)RingPages + bouncePageCount;
    if ((ULONG64)RingPages + MetaPages > poolPages)
    {
        RdmaClientClosePoolFile(c);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    totalPages = requestedPages > poolPages ? poolPages : (ULONG)requestedPages;
    if (totalPages == 0)
    {
        RdmaClientClosePoolFile(c);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&allocInput, sizeof(allocInput));
    RtlZeroMemory(&allocOutput, sizeof(allocOutput));
    allocInput.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
    allocInput.NumPages = totalPages;
    ioctlResult = RdmaClientIoctl(c,
                                  (ULONG)IOCTL_RDMAPOOL_ALLOCATE,
                                  &allocInput,
                                  sizeof(allocInput),
                                  &allocOutput,
                                  sizeof(allocOutput));
    status = ioctlResult.Status;
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s rdmapool: ALLOCATE %u pages failed 0x%x\n", c->Tag, totalPages, status);
        RdmaClientClosePoolFile(c);
        return status;
    }
    if (ioctlResult.Information != sizeof(allocOutput) ||
        !RdmaClientValidAllocation(&queryOutput, &allocOutput, totalPages))
    {
        RDMA_CLIENT_IOCTL_RESULT rollbackResult = {STATUS_INVALID_PARAMETER, 0, FALSE};
        BOOLEAN hasOwnerTuple = allocOutput.VirtualAddress != NULL && allocOutput.NumPages != 0 &&
                                allocOutput.AllocationToken != 0;

        if (hasOwnerTuple)
        {
            RtlZeroMemory(&freeInput, sizeof(freeInput));
            freeInput.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
            freeInput.NumPages = allocOutput.NumPages;
            freeInput.VirtualAddress = allocOutput.VirtualAddress;
            freeInput.AllocationToken = allocOutput.AllocationToken;
            rollbackResult = RdmaClientIoctl(c, (ULONG)IOCTL_RDMAPOOL_FREE, &freeInput, sizeof(freeInput), NULL, 0);
        }
        if (hasOwnerTuple && (!NT_SUCCESS(rollbackResult.Status) || rollbackResult.Information != 0))
        {
            c->BaseVA = allocOutput.VirtualAddress;
            c->BasePA = allocOutput.PhysicalAddress;
            c->Size = (ULONG64)allocOutput.NumPages * PAGE_SIZE;
            c->AllocationPages = allocOutput.NumPages;
            c->AllocationToken = allocOutput.AllocationToken;
            c->DisconnectAttempted = rollbackResult.Submitted;
            c->DisconnectStatus = NT_SUCCESS(rollbackResult.Status) ? STATUS_DATA_ERROR : rollbackResult.Status;
            DbgPrint("%s rdmapool: malformed ALLOCATE rollback failed 0x%x, info=%Iu\n",
                     c->Tag,
                     c->DisconnectStatus,
                     rollbackResult.Information);
        }
        if (c->AllocationToken == 0)
        {
            RdmaClientClosePoolFile(c);
        }
        return ioctlResult.Information == sizeof(allocOutput) ? STATUS_DATA_ERROR : STATUS_INFO_LENGTH_MISMATCH;
    }

    c->BaseVA = allocOutput.VirtualAddress;
    c->BasePA = allocOutput.PhysicalAddress;
    c->Size = (ULONG64)totalPages * PAGE_SIZE;
    c->AllocationPages = totalPages;
    c->AllocationToken = allocOutput.AllocationToken;
    c->Active = TRUE;

    DbgPrint("%s rdmapool: connected VA=%p PA=0x%I64x pages=%u (rings=%u bounce=%u)\n",
             c->Tag,
             c->BaseVA,
             c->BasePA.QuadPart,
             totalPages,
             RingPages,
             bouncePages);
    return STATUS_SUCCESS;
}

NTSTATUS RdmaClientDisconnect(PRDMA_CLIENT c)
{
    NTSTATUS status = STATUS_SUCCESS;
    RDMA_CLIENT_IOCTL_RESULT ioctlResult;

    if (c->PollThread != NULL)
    {
        return STATUS_DEVICE_BUSY;
    }

    /* Once dispatched, a failed FREE has indeterminate ownership semantics.
     * Keep the exact tuple and return the cached result without resubmitting. */
    if (c->DisconnectAttempted)
    {
        return c->DisconnectStatus;
    }

    if (c->AllocationToken != 0 || c->AllocationPages != 0 || c->BaseVA != NULL)
    {
        RDMAPOOL_FREE_INPUT freeInput;

        if (c->PoolFileObject == NULL || c->PoolDeviceObject == NULL || c->BaseVA == NULL || c->AllocationPages == 0 ||
            c->AllocationToken == 0)
        {
            c->DisconnectStatus = STATUS_INVALID_DEVICE_STATE;
            return c->DisconnectStatus;
        }

        RtlZeroMemory(&freeInput, sizeof(freeInput));
        freeInput.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
        freeInput.VirtualAddress = c->BaseVA;
        freeInput.NumPages = c->AllocationPages;
        freeInput.AllocationToken = c->AllocationToken;
        ioctlResult = RdmaClientIoctl(c, (ULONG)IOCTL_RDMAPOOL_FREE, &freeInput, sizeof(freeInput), NULL, 0);
        status = ioctlResult.Status;
        if (!ioctlResult.Submitted)
        {
            c->DisconnectStatus = status;
            return c->DisconnectStatus;
        }
        c->DisconnectAttempted = TRUE;
        if (!NT_SUCCESS(status) || ioctlResult.Information != 0)
        {
            c->DisconnectStatus = NT_SUCCESS(status) ? STATUS_DATA_ERROR : status;
            DbgPrint("%s rdmapool: FREE failed 0x%x, info=%Iu; retaining owner\n",
                     c->Tag,
                     c->DisconnectStatus,
                     ioctlResult.Information);
            return c->DisconnectStatus;
        }
    }

    RdmaClientClosePoolFile(c);
    c->BaseVA = NULL;
    c->BasePA.QuadPart = 0;
    c->Size = 0;
    c->AllocationPages = 0;
    c->AllocationToken = 0;
    c->DisconnectAttempted = FALSE;
    c->Active = FALSE;
    c->BounceInitialized = FALSE;
    c->DisconnectStatus = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* VA <-> PA (whole region is one contiguous rdmapool allocation)     */
/* ------------------------------------------------------------------ */

PHYSICAL_ADDRESS RdmaClientVAtoPA(PRDMA_CLIENT c, PVOID va)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = c->BasePA.QuadPart + ((ULONG_PTR)va - (ULONG_PTR)c->BaseVA);
    return pa;
}

PVOID RdmaClientPAtoVA(PRDMA_CLIENT c, PHYSICAL_ADDRESS pa)
{
    return (PVOID)((ULONG_PTR)c->BaseVA + (ULONG_PTR)(pa.QuadPart - c->BasePA.QuadPart));
}

BOOLEAN RdmaClientOwnsVA(PRDMA_CLIENT c, PVOID va)
{
    ULONG_PTR addr = (ULONG_PTR)va;
    ULONG_PTR base = (ULONG_PTR)c->BaseVA;
    return (c->Active && c->BaseVA != NULL && addr >= base && (ULONG64)(addr - base) < c->Size);
}

/* ------------------------------------------------------------------ */
/* SLIST sub-allocator: control slots + contiguous data chunks         */
/* ------------------------------------------------------------------ */

PVOID RdmaClientAllocCtl(PRDMA_CLIENT c)
{
    return (PVOID)InterlockedPopEntrySList(&c->CtlFreeList);
}

VOID RdmaClientFreeCtl(PRDMA_CLIENT c, PVOID slot)
{
    InterlockedPushEntrySList(&c->CtlFreeList, (PSLIST_ENTRY)slot);
}

PVOID RdmaClientAllocChunk(PRDMA_CLIENT c)
{
    return (PVOID)InterlockedPopEntrySList(&c->DataFreeList);
}

VOID RdmaClientFreeChunk(PRDMA_CLIENT c, PVOID chunk)
{
    InterlockedPushEntrySList(&c->DataFreeList, (PSLIST_ENTRY)chunk);
}

NTSTATUS RdmaClientBounceInit(PRDMA_CLIENT c,
                              PVOID FreeStart,
                              ULONG CtlSlots,
                              ULONG CtlSlotSize,
                              ULONG EventBytes,
                              ULONG DataChunkSize)
{
    ULONG_PTR freeAddress;
    ULONG_PTR poolBase;
    ULONG_PTR poolEnd;
    ULONG_PTR alignedBase;
    PUCHAR base;
    SIZE_T avail;
    ULONG chunk, i;
    ULONG ctlSlotCount;
    ULONG dataChunkCount;
    SIZE_T ctlBytes, dataBytes, eventBytes = 0;
    SIZE_T roundedChunk;

    c->BounceInitialized = FALSE;
    c->EventBaseVA = NULL;
    c->EventBytes = 0;
    c->CtlBaseVA = NULL;
    c->CtlSlotSize = 0;
    c->CtlSlotCount = 0;
    c->DataBaseVA = NULL;
    c->DataChunkSize = 0;
    c->DataChunkCount = 0;
    InitializeSListHead(&c->CtlFreeList);
    InitializeSListHead(&c->DataFreeList);

    if (!c->Active || c->BaseVA == NULL || c->Size == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }
    if (FreeStart == NULL || CtlSlots == 0 || CtlSlotSize < sizeof(SLIST_ENTRY) ||
        (CtlSlotSize & (MEMORY_ALLOCATION_ALIGNMENT - 1)) != 0 || DataChunkSize < sizeof(SLIST_ENTRY))
    {
        return STATUS_INVALID_PARAMETER;
    }

    poolBase = (ULONG_PTR)c->BaseVA;
    if (c->Size > MAXULONG_PTR || poolBase > MAXULONG_PTR - (ULONG_PTR)c->Size)
    {
        return STATUS_INTEGER_OVERFLOW;
    }
    poolEnd = poolBase + (ULONG_PTR)c->Size;
    freeAddress = (ULONG_PTR)FreeStart;
    if (freeAddress < poolBase || freeAddress > poolEnd || freeAddress > MAXULONG_PTR - (PAGE_SIZE - 1))
    {
        return STATUS_INVALID_PARAMETER;
    }
    alignedBase = (freeAddress + PAGE_SIZE - 1) & ~((ULONG_PTR)PAGE_SIZE - 1);
    if (alignedBase >= poolEnd)
    {
        DbgPrint("%s bounce: no room after rings\n", c->Tag);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    base = (PUCHAR)alignedBase;
    avail = (SIZE_T)(poolEnd - alignedBase);

    /* Optional event area (page-aligned so the slots that follow stay aligned). */
    if (EventBytes)
    {
        if ((SIZE_T)EventBytes > MAXULONG_PTR - (PAGE_SIZE - 1))
        {
            return STATUS_INTEGER_OVERFLOW;
        }
        eventBytes = ((SIZE_T)EventBytes + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
        if (eventBytes >= avail)
        {
            DbgPrint("%s bounce: no room for event area\n", c->Tag);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        base += eventBytes;
        avail -= eventBytes;
    }

    /* Control slots: one per outstanding request, bounded by half the space. */
    ctlSlotCount = CtlSlots;
    if ((SIZE_T)ctlSlotCount > (avail / 2) / CtlSlotSize)
    {
        ctlSlotCount = (ULONG)((avail / 2) / CtlSlotSize);
    }
    if (ctlSlotCount == 0)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ctlBytes = (SIZE_T)ctlSlotCount * CtlSlotSize;

    /* Data chunks: large CONTIGUOUS blocks, each one descriptor; page-align and
     * shrink until there is at least one chunk per control slot (concurrency). */
    roundedChunk = ((SIZE_T)DataChunkSize + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
    if (roundedChunk > MAXULONG)
    {
        return STATUS_INTEGER_OVERFLOW;
    }
    chunk = (ULONG)roundedChunk;
    dataBytes = avail - ctlBytes;
    while (chunk > PAGE_SIZE && (dataBytes / chunk) < (SIZE_T)ctlSlotCount)
    {
        chunk -= PAGE_SIZE;
    }
    dataChunkCount = (ULONG)(dataBytes / chunk);
    if (dataChunkCount == 0)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    c->EventBaseVA = EventBytes ? (PUCHAR)alignedBase : NULL;
    c->EventBytes = EventBytes;
    c->CtlBaseVA = base;
    c->CtlSlotSize = CtlSlotSize;
    c->CtlSlotCount = ctlSlotCount;
    c->DataBaseVA = c->CtlBaseVA + ctlBytes;
    c->DataChunkSize = chunk;
    c->DataChunkCount = dataChunkCount;
    for (i = 0; i < c->CtlSlotCount; i++)
    {
        RdmaClientFreeCtl(c, c->CtlBaseVA + (SIZE_T)i * CtlSlotSize);
    }
    for (i = 0; i < c->DataChunkCount; i++)
    {
        RdmaClientFreeChunk(c, c->DataBaseVA + (SIZE_T)i * chunk);
    }

    c->BounceInitialized = TRUE;
    DbgPrint("%s bounce: evt=%uB ctl=%u(%uB) data=%u x %uKB @ VA=%p\n",
             c->Tag,
             c->EventBytes,
             c->CtlSlotCount,
             c->CtlSlotSize,
             c->DataChunkCount,
             c->DataChunkSize / 1024,
             (PVOID)alignedBase);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Completion poll thread (~1ms cadence; idle = fully blocked)         */
/* ------------------------------------------------------------------ */

static VOID RdmaClientPollThreadRoutine(PVOID Context)
{
    PRDMA_CLIENT c = (PRDMA_CLIENT)Context;
    LARGE_INTEGER idleTick;

    /* Negative = relative, 100ns units. */
    idleTick.QuadPart = -(LONGLONG)(10 * 1000 * RDMA_CLIENT_POLL_IDLE_MS);

    for (;;)
    {
        if (InterlockedCompareExchange(&c->PollStop, 0, 0) != 0)
        {
            break;
        }

        if (c->BusyCb(c->CbContext))
        {
            /*
             * Busy: drain, then either sleep PollIntervalUs (gentle periodic
             * poll, default 1ms: reaps within ~1ms at low CPU instead of
             * stalling until the ~250ms StorPort watchdog when a completion
             * interrupt to an idle vCPU goes missing) or tight-spin when
             * PollIntervalUs==0 (max IOPS, pegs this thread's core only while
             * I/O is in flight). This thread runs at PASSIVE_LEVEL — DrainCb
             * releases its locks before returning — so KeDelayExecutionThread
             * is legal.
             */
            c->DrainCb(c->CbContext);
            if (c->PollIntervalUs == 0)
            {
                KeStallExecutionProcessor(RDMA_CLIENT_POLL_SPIN_US);
            }
            else
            {
                LARGE_INTEGER pollDelay;
                /* relative (negative), 100ns units: 1us = 10 * 100ns */
                pollDelay.QuadPart = -(LONGLONG)(10 * (LONGLONG)c->PollIntervalUs);
                KeDelayExecutionThread(KernelMode, FALSE, &pollDelay);
            }
        }
        else
        {
            /* Idle: block until a submit kicks us (safety-net timeout). ~0 CPU. */
            (void)KeWaitForSingleObject(&c->PollWake, Executive, KernelMode, FALSE, &idleTick);
            c->DrainCb(c->CbContext);
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID RdmaClientPollKick(PRDMA_CLIENT c)
{
    if (c->PollThread)
    {
        KeSetEvent(&c->PollWake, IO_NO_INCREMENT, FALSE);
    }
}

NTSTATUS RdmaClientStartPoll(PRDMA_CLIENT c,
                             RDMA_CLIENT_BUSY_CB BusyCb,
                             RDMA_CLIENT_DRAIN_CB DrainCb,
                             PVOID CbContext,
                             ULONG PollIntervalUs)
{
    NTSTATUS status;
    HANDLE hThread = NULL;
    OBJECT_ATTRIBUTES oa;

    if (!c->Active)
    {
        return STATUS_NOT_SUPPORTED; /* poll thread only needed on the rdmapool path */
    }

    KeInitializeEvent(&c->PollWake, SynchronizationEvent, FALSE);
    c->PollStop = 0;
    c->PollThread = NULL;
    c->BusyCb = BusyCb;
    c->DrainCb = DrainCb;
    c->CbContext = CbContext;
    c->PollIntervalUs = PollIntervalUs;

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa, NULL, NULL, RdmaClientPollThreadRoutine, c);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("%s poll: PsCreateSystemThread failed 0x%x\n", c->Tag, status);
        return status;
    }

    status = ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, &c->PollThread, NULL);
    if (!NT_SUCCESS(status))
    {
        /* Thread is running but we couldn't get a reference: ask it to stop. */
        InterlockedExchange(&c->PollStop, 1);
        KeSetEvent(&c->PollWake, IO_NO_INCREMENT, FALSE);
        (void)ZwWaitForSingleObject(hThread, FALSE, NULL);
        ZwClose(hThread);
        c->PollThread = NULL;
        return status;
    }
    ZwClose(hThread);

    DbgPrint("%s poll: thread started (interval %uus, idle %ums safety net)\n",
             c->Tag,
             c->PollIntervalUs,
             RDMA_CLIENT_POLL_IDLE_MS);
    return STATUS_SUCCESS;
}

VOID RdmaClientStopPoll(PRDMA_CLIENT c)
{
    PVOID thread = c->PollThread;

    if (thread == NULL)
    {
        return;
    }
    InterlockedExchange(&c->PollStop, 1);
    KeSetEvent(&c->PollWake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(thread);
    c->PollThread = NULL;
}
