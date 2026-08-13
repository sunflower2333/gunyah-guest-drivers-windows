/*
 * Implementation of virtio_system_ops VirtioLib callbacks
 *
 * Copyright (c) 2016-2017 Red Hat, Inc.
 *
 * Author(s):
 *  Yuri Benditovich <ybendito@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "osdep.h"
#include "virtio_pci.h"
#include "VirtIOWdf.h"
#include "private.h"
#include <devpropdef.h>
#include "rdmapool_interface.h"

static EVT_WDF_OBJECT_CONTEXT_DESTROY OnDmaTransactionDestroy;
static EVT_WDF_PROGRAM_DMA OnDmaTransactionProgramDma;

/* Helper: check if a VA falls within the rdmapool region */
static BOOLEAN IsRdmaPoolAddress(PVIRTIO_WDF_DRIVER pWdfDriver, PVOID va)
{
    ULONG_PTR addr;
    ULONG_PTR base;

    if (pWdfDriver->RdmaPoolBaseVA == NULL) {
        return FALSE;
    }
    addr = (ULONG_PTR)va;
    base = (ULONG_PTR)pWdfDriver->RdmaPoolBaseVA;
    return addr >= base && (ULONG64)(addr - base) < pWdfDriver->RdmaPoolSize;
}

/* Tracking entry for rdmapool allocations */
typedef struct _RDMAPOOL_ALLOC_ENTRY {
    LIST_ENTRY ListEntry;
    PVOID VirtualAddress;
    ULONG NumPages;
    ULONG64 AllocationToken;
    ULONG GroupTag;
    BOOLEAN FreeAttempted;
    NTSTATUS FreeStatus;
} RDMAPOOL_ALLOC_ENTRY, *PRDMAPOOL_ALLOC_ENTRY;

#define RDMAPOOL_ALLOC_TAG 'ARDR'

typedef struct _RDMAPOOL_IOCTL_RESULT {
    NTSTATUS Status;
    ULONG_PTR Information;
    BOOLEAN Submitted;
} RDMAPOOL_IOCTL_RESULT;

static RDMAPOOL_IOCTL_RESULT RdmaPoolIoctl(PVIRTIO_WDF_DRIVER pWdfDriver, ULONG controlCode,
                                           PVOID inputBuffer, ULONG inputLength, PVOID outputBuffer,
                                           ULONG outputLength)
{
    RDMAPOOL_IOCTL_RESULT result = { STATUS_INSUFFICIENT_RESOURCES, 0, FALSE };
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    PIO_STACK_LOCATION irpStack;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));

    irp = IoBuildDeviceIoControlRequest(controlCode, pWdfDriver->RdmaPoolDeviceObject, inputBuffer,
                                        inputLength, outputBuffer, outputLength, FALSE, &event,
                                        &iosb);

    if (irp == NULL) {
        return result;
    }

    irpStack = IoGetNextIrpStackLocation(irp);
    irpStack->FileObject = pWdfDriver->RdmaPoolFileObject;

    result.Submitted = TRUE;
    status = IoCallDriver(pWdfDriver->RdmaPoolDeviceObject, irp);
    if (status == STATUS_PENDING) {
        status = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        if (NT_SUCCESS(status)) {
            status = iosb.Status;
        }
    }
    result.Status = status;
    result.Information = iosb.Information;
    return result;
}

static BOOLEAN ValidateRdmaPoolAllocation(PVIRTIO_WDF_DRIVER pWdfDriver,
                                          const RDMAPOOL_ALLOCATE_OUTPUT *output,
                                          ULONG expectedPages)
{
    ULONG_PTR baseVa = (ULONG_PTR)pWdfDriver->RdmaPoolBaseVA;
    ULONG_PTR allocationVa = (ULONG_PTR)output->VirtualAddress;
    ULONG64 basePa = (ULONG64)pWdfDriver->RdmaPoolBasePA.QuadPart;
    ULONG64 allocationPa;
    ULONG64 allocationSize = (ULONG64)expectedPages * PAGE_SIZE;
    ULONG64 vaOffset;
    ULONG64 paOffset;

    if (output->InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2 ||
        output->NumPages != expectedPages || output->AllocationToken == 0 ||
        output->VirtualAddress == NULL || (allocationVa & (PAGE_SIZE - 1)) != 0 ||
        output->PhysicalAddress.QuadPart < 0 || allocationVa < baseVa ||
        allocationSize > pWdfDriver->RdmaPoolSize) {
        return FALSE;
    }

    allocationPa = (ULONG64)output->PhysicalAddress.QuadPart;
    if ((allocationPa & (PAGE_SIZE - 1)) != 0 || allocationPa < basePa) {
        return FALSE;
    }

    vaOffset = (ULONG64)(allocationVa - baseVa);
    paOffset = allocationPa - basePa;
    return vaOffset == paOffset && vaOffset <= pWdfDriver->RdmaPoolSize - allocationSize;
}

static NTSTATUS FreeRdmaPoolEntryLocked(PVIRTIO_WDF_DRIVER pWdfDriver, PRDMAPOOL_ALLOC_ENTRY entry)
{
    RDMAPOOL_FREE_INPUT freeInput;
    RDMAPOOL_IOCTL_RESULT ioctlResult;

    if (entry->FreeAttempted) {
        return entry->FreeStatus;
    }

    RtlZeroMemory(&freeInput, sizeof(freeInput));
    freeInput.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
    freeInput.NumPages = entry->NumPages;
    freeInput.VirtualAddress = entry->VirtualAddress;
    freeInput.AllocationToken = entry->AllocationToken;

    ioctlResult = RdmaPoolIoctl(pWdfDriver, (ULONG)IOCTL_RDMAPOOL_FREE, &freeInput,
                                sizeof(freeInput), NULL, 0);
    if (!ioctlResult.Submitted) {
        entry->FreeStatus = ioctlResult.Status;
        return entry->FreeStatus;
    }
    entry->FreeAttempted = TRUE;
    entry->FreeStatus = NT_SUCCESS(ioctlResult.Status) && ioctlResult.Information != 0 ?
                            STATUS_DATA_ERROR :
                            ioctlResult.Status;
    if (!NT_SUCCESS(entry->FreeStatus)) {
        pWdfDriver->RdmaPoolClosing = TRUE;
    }
    return entry->FreeStatus;
}

static BOOLEAN GetTrackedRdmaPoolPhysicalAddress(PVIRTIO_WDF_DRIVER pWdfDriver, PVOID va,
                                                 PHYSICAL_ADDRESS *pa)
{
    PLIST_ENTRY listEntry;
    ULONG_PTR address = (ULONG_PTR)va;
    BOOLEAN found = FALSE;

    pa->QuadPart = 0;
    if (!pWdfDriver->RdmaPoolActive || pWdfDriver->RdmaPoolBaseVA == NULL) {
        return FALSE;
    }

    WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
    for (listEntry = pWdfDriver->RdmaPoolAllocList.Flink;
         listEntry != &pWdfDriver->RdmaPoolAllocList; listEntry = listEntry->Flink) {
        PRDMAPOOL_ALLOC_ENTRY allocation =
            CONTAINING_RECORD(listEntry, RDMAPOOL_ALLOC_ENTRY, ListEntry);
        ULONG_PTR allocationBase = (ULONG_PTR)allocation->VirtualAddress;
        ULONG64 allocationSize = (ULONG64)allocation->NumPages * PAGE_SIZE;

        if (!allocation->FreeAttempted && address >= allocationBase &&
            (ULONG64)(address - allocationBase) < allocationSize) {
            pa->QuadPart = pWdfDriver->RdmaPoolBasePA.QuadPart +
                           (address - (ULONG_PTR)pWdfDriver->RdmaPoolBaseVA);
            found = TRUE;
            break;
        }
    }
    WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
    return found;
}

static NTSTATUS FreeTrackedRdmaPoolAllocation(PVIRTIO_WDF_DRIVER pWdfDriver, PVOID va)
{
    PLIST_ENTRY listEntry;
    PRDMAPOOL_ALLOC_ENTRY allocation = NULL;
    NTSTATUS status = STATUS_NOT_FOUND;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || pWdfDriver->RdmaPoolIoctlLock == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    WdfWaitLockAcquire(pWdfDriver->RdmaPoolIoctlLock, NULL);
    WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
    for (listEntry = pWdfDriver->RdmaPoolAllocList.Flink;
         listEntry != &pWdfDriver->RdmaPoolAllocList; listEntry = listEntry->Flink) {
        PRDMAPOOL_ALLOC_ENTRY candidate =
            CONTAINING_RECORD(listEntry, RDMAPOOL_ALLOC_ENTRY, ListEntry);
        if (candidate->VirtualAddress == va) {
            allocation = candidate;
            break;
        }
    }
    WdfSpinLockRelease(pWdfDriver->DmaSpinlock);

    if (allocation != NULL) {
        status = FreeRdmaPoolEntryLocked(pWdfDriver, allocation);
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
            RemoveEntryList(&allocation->ListEntry);
            WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
        }
    }
    WdfWaitLockRelease(pWdfDriver->RdmaPoolIoctlLock);

    if (allocation != NULL && NT_SUCCESS(status)) {
        ExFreePoolWithTag(allocation, RDMAPOOL_ALLOC_TAG);
    }
    return status;
}

/* Allocate DMA memory from rdmapool via IOCTL */
static void *AllocateFromRdmaPool(PVIRTIO_WDF_DRIVER pWdfDriver, size_t size, ULONG groupTag)
{
    RDMAPOOL_ALLOCATE_INPUT allocInput;
    RDMAPOOL_ALLOCATE_OUTPUT allocOutput;
    PRDMAPOOL_ALLOC_ENTRY entry;
    ULONG64 pageCount;
    RDMAPOOL_IOCTL_RESULT ioctlResult;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || size == 0 || pWdfDriver->RdmaPoolIoctlLock == NULL) {
        return NULL;
    }

    pageCount = (ULONG64)(size / PAGE_SIZE) + ((size % PAGE_SIZE) != 0);
    if (pageCount == 0 || pageCount > MAXULONG) {
        return NULL;
    }

    entry = (PRDMAPOOL_ALLOC_ENTRY)ExAllocatePoolUninitialized(NonPagedPool, sizeof(*entry),
                                                               RDMAPOOL_ALLOC_TAG);
    if (entry == NULL) {
        return NULL;
    }
    RtlZeroMemory(entry, sizeof(*entry));

    RtlZeroMemory(&allocInput, sizeof(allocInput));
    RtlZeroMemory(&allocOutput, sizeof(allocOutput));
    allocInput.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
    allocInput.NumPages = (ULONG)pageCount;

    WdfWaitLockAcquire(pWdfDriver->RdmaPoolIoctlLock, NULL);
    if (!pWdfDriver->RdmaPoolActive || pWdfDriver->RdmaPoolClosing ||
        pWdfDriver->RdmaPoolFileObject == NULL) {
        WdfWaitLockRelease(pWdfDriver->RdmaPoolIoctlLock);
        ExFreePoolWithTag(entry, RDMAPOOL_ALLOC_TAG);
        return NULL;
    }

    ioctlResult = RdmaPoolIoctl(pWdfDriver, (ULONG)IOCTL_RDMAPOOL_ALLOCATE, &allocInput,
                                sizeof(allocInput), &allocOutput, sizeof(allocOutput));

    if (!NT_SUCCESS(ioctlResult.Status)) {
        DPrintf(0, "%s: IOCTL_RDMAPOOL_ALLOCATE failed 0x%x (size=0x%x)\n", __FUNCTION__,
                ioctlResult.Status, (ULONG)size);
        WdfWaitLockRelease(pWdfDriver->RdmaPoolIoctlLock);
        ExFreePoolWithTag(entry, RDMAPOOL_ALLOC_TAG);
        return NULL;
    }

    entry->VirtualAddress = allocOutput.VirtualAddress;
    entry->NumPages = allocOutput.NumPages;
    entry->AllocationToken = allocOutput.AllocationToken;
    entry->GroupTag = groupTag;

    if (ioctlResult.Information != sizeof(allocOutput) ||
        !ValidateRdmaPoolAllocation(pWdfDriver, &allocOutput, allocInput.NumPages)) {
        NTSTATUS rollbackStatus = STATUS_INVALID_PARAMETER;
        BOOLEAN hasOwnerTuple =
            entry->VirtualAddress != NULL && entry->NumPages != 0 && entry->AllocationToken != 0;

        if (hasOwnerTuple) {
            rollbackStatus = FreeRdmaPoolEntryLocked(pWdfDriver, entry);
        }
        if (NT_SUCCESS(rollbackStatus)) {
            WdfWaitLockRelease(pWdfDriver->RdmaPoolIoctlLock);
            ExFreePoolWithTag(entry, RDMAPOOL_ALLOC_TAG);
        } else if (!hasOwnerTuple) {
            /* A successful but malformed response may still have published an
             * allocation whose identity cannot be expressed by V2 FREE. Keep
             * the connection poisoned until shutdown can close the file after
             * all device DMA and queue access has stopped. */
            pWdfDriver->RdmaPoolClosing = TRUE;
            pWdfDriver->RdmaPoolOwnerUnknown = TRUE;
            WdfWaitLockRelease(pWdfDriver->RdmaPoolIoctlLock);
            ExFreePoolWithTag(entry, RDMAPOOL_ALLOC_TAG);
            DPrintf(0, "%s: malformed ALLOCATE omitted the owner tuple; file close required\n",
                    __FUNCTION__);
        } else {
            WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
            InsertTailList(&pWdfDriver->RdmaPoolAllocList, &entry->ListEntry);
            pWdfDriver->RdmaPoolClosing = TRUE;
            WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
            WdfWaitLockRelease(pWdfDriver->RdmaPoolIoctlLock);
            DPrintf(0, "%s: malformed ALLOCATE rollback failed 0x%x; retaining owner\n",
                    __FUNCTION__, rollbackStatus);
        }
        return NULL;
    }

    WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
    InsertTailList(&pWdfDriver->RdmaPoolAllocList, &entry->ListEntry);
    WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
    WdfWaitLockRelease(pWdfDriver->RdmaPoolIoctlLock);

    DPrintf(1, "%s: rdmapool alloc VA=%p PA=0x%llx size=0x%x\n", __FUNCTION__,
            allocOutput.VirtualAddress, allocOutput.PhysicalAddress.QuadPart, (ULONG)size);

    return allocOutput.VirtualAddress;
}

static void *AllocateCommonBuffer(PVIRTIO_WDF_DRIVER pWdfDriver, size_t size, ULONG groupTag)
{
    NTSTATUS status;
    WDFCOMMONBUFFER commonBuffer;
    PVIRTIO_WDF_MEMORY_BLOCK_CONTEXT context;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, VIRTIO_WDF_MEMORY_BLOCK_CONTEXT);

    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        DPrintf(0, "%s FAILED(irql)\n", __FUNCTION__);
        return NULL;
    }
    status = WdfCommonBufferCreate(pWdfDriver->DmaEnabler, size, &attr, &commonBuffer);
    if (!NT_SUCCESS(status)) {
        return NULL;
    }
    WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
    status = WdfCollectionAdd(pWdfDriver->MemoryBlockCollection, commonBuffer);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(commonBuffer);
        WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
        return NULL;
    }
    context = GetMemoryBlockContext(commonBuffer);
    context->WdfBuffer = commonBuffer;
    context->Length = size;
    context->PhysicalAddress = WdfCommonBufferGetAlignedLogicalAddress(commonBuffer);
    context->pVirtualAddress = WdfCommonBufferGetAlignedVirtualAddress(commonBuffer);
    context->groupTag = groupTag;
    context->bToBeDeleted = FALSE;
    WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
    RtlZeroMemory(context->pVirtualAddress, size);

    DPrintf(1, "%s done %p@%I64x(tag %08X), size 0x%x\n", __FUNCTION__, context->pVirtualAddress,
            context->PhysicalAddress.QuadPart, context->groupTag, (ULONG)size);

    return context->pVirtualAddress;
}

void *VirtIOWdfDeviceAllocDmaMemory(VirtIODevice *vdev, size_t size, ULONG groupTag)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;

    /* If restricted DMA pool is active, allocate from it */
    if (pWdfDriver->RdmaPoolActive) {
        return AllocateFromRdmaPool(pWdfDriver, size, groupTag);
    }

    return AllocateCommonBuffer(pWdfDriver, size, groupTag);
}

static BOOLEAN FindCommonBuffer(PVIRTIO_WDF_DRIVER pWdfDriver, void *p, PHYSICAL_ADDRESS *ppa,
                                size_t *pOffset, BOOLEAN bRemoval)
{
    BOOLEAN b = FALSE;
    ULONG_PTR va = (ULONG_PTR)p;
    ULONG i, n;
    WDFOBJECT obj = NULL;
    WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
    n = WdfCollectionGetCount(pWdfDriver->MemoryBlockCollection);
    for (i = 0; i < n; ++i) {
        obj = WdfCollectionGetItem(pWdfDriver->MemoryBlockCollection, i);
        if (!obj) {
            break;
        }
        PVIRTIO_WDF_MEMORY_BLOCK_CONTEXT context = GetMemoryBlockContext(obj);
        if (context->bToBeDeleted && !bRemoval) {
            continue;
        }
        ULONG_PTR currentVaStart = (ULONG_PTR)context->pVirtualAddress;
        if (va >= currentVaStart && va < (currentVaStart + context->Length)) {
            *ppa = context->PhysicalAddress;
            *pOffset = va - currentVaStart;
            b = TRUE;
            if (bRemoval) {
                b = *pOffset == 0;
                if (b) {
                    context->bToBeDeleted = TRUE;
                }
            }
            break;
        }
    }
    WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
    if (!b) {
        DPrintf(0, "%s(%s) FAILED!\n", __FUNCTION__, bRemoval ? "Remove" : "Locate");
    } else if (bRemoval) {
        if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
            WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
            WdfCollectionRemove(pWdfDriver->MemoryBlockCollection, obj);
            WdfSpinLockRelease(pWdfDriver->DmaSpinlock);

            WdfObjectDelete(obj);
            DPrintf(1, "%s %p freed (%d common buffers)\n", __FUNCTION__, va, n - 1);
        } else {
            DPrintf(0, "%s %p marked for deletion\n", __FUNCTION__, va);
        }
    }
    return b;
}

static PHYSICAL_ADDRESS GetPhysicalAddress(PVIRTIO_WDF_DRIVER pWdfDriver, PVOID va)
{
    PHYSICAL_ADDRESS pa;
    size_t offset;
    pa.QuadPart = 0;
    if (FindCommonBuffer(pWdfDriver, va, &pa, &offset, FALSE)) {
        pa.QuadPart += offset;
    }
    return pa;
}

PHYSICAL_ADDRESS VirtIOWdfDeviceGetPhysicalAddress(VirtIODevice *vdev, void *va)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;

    /* Pool-wide bounds are insufficient: another file owner may hold the VA. */
    if (IsRdmaPoolAddress(pWdfDriver, va)) {
        PHYSICAL_ADDRESS pa;
        if (!GetTrackedRdmaPoolPhysicalAddress(pWdfDriver, va, &pa)) {
            DPrintf(0, "%s: VA=%p is not a live rdmapool allocation\n", __FUNCTION__, va);
        }
        return pa;
    }

    return GetPhysicalAddress(pWdfDriver, va);
}

void VirtIOWdfDeviceFreeDmaMemory(VirtIODevice *vdev, void *va)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;

    /* If the VA is within the rdmapool region, free it there */
    if (IsRdmaPoolAddress(pWdfDriver, va)) {
        NTSTATUS status = FreeTrackedRdmaPoolAllocation(pWdfDriver, va);
        if (!NT_SUCCESS(status)) {
            DPrintf(0, "%s: rdmapool FREE VA=%p failed 0x%x; record retained\n", __FUNCTION__, va,
                    status);
        }
        return;
    }

    PHYSICAL_ADDRESS pa;
    size_t offset;
    FindCommonBuffer(pWdfDriver, va, &pa, &offset, TRUE);
}

static BOOLEAN FindCommonBufferByTag(PVIRTIO_WDF_DRIVER pWdfDriver, ULONG tag)
{
    BOOLEAN b = FALSE;
    ULONG i, n;
    WDFOBJECT obj = NULL;
    PVIRTIO_WDF_MEMORY_BLOCK_CONTEXT context = NULL;
    WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
    n = WdfCollectionGetCount(pWdfDriver->MemoryBlockCollection);
    for (i = 0; i < n; ++i) {
        obj = WdfCollectionGetItem(pWdfDriver->MemoryBlockCollection, i);
        if (!obj) {
            break;
        }
        context = GetMemoryBlockContext(obj);
        if (context->groupTag == tag) {
            b = TRUE;
            break;
        }
    }
    WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
    if (b) {
        DPrintf(1, "%s %p (tag %08X) freed (%d common buffers)\n", __FUNCTION__,
                context->pVirtualAddress, tag, n - 1);
        WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
        WdfCollectionRemove(pWdfDriver->MemoryBlockCollection, obj);
        WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
        WdfObjectDelete(obj);
    }
    return b;
}

void VirtIOWdfDeviceFreeDmaMemoryByTag(VirtIODevice *vdev, ULONG groupTag)
{
    PVIRTIO_WDF_DRIVER pWdfDriver;

    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        DPrintf(0, "%s FAILED(irql)\n", __FUNCTION__);
        return;
    }
    if (!groupTag) {
        DPrintf(0, "%s FAILED(default tag)\n", __FUNCTION__);
        return;
    }
    if (!vdev->DeviceContext) {
        DPrintf(0, "%s was not initialized\n", __FUNCTION__);
        return;
    }
    pWdfDriver = vdev->DeviceContext;

    for (;;) {
        PLIST_ENTRY listEntry;
        PVOID address = NULL;

        WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
        for (listEntry = pWdfDriver->RdmaPoolAllocList.Flink;
             listEntry != &pWdfDriver->RdmaPoolAllocList; listEntry = listEntry->Flink) {
            PRDMAPOOL_ALLOC_ENTRY candidate =
                CONTAINING_RECORD(listEntry, RDMAPOOL_ALLOC_ENTRY, ListEntry);
            if (candidate->GroupTag == groupTag) {
                address = candidate->VirtualAddress;
                break;
            }
        }
        WdfSpinLockRelease(pWdfDriver->DmaSpinlock);

        if (address == NULL) {
            break;
        }
        if (!NT_SUCCESS(FreeTrackedRdmaPoolAllocation(pWdfDriver, address))) {
            break;
        }
    }
    while (FindCommonBufferByTag(vdev->DeviceContext, groupTag))
        ;
}

static void FreeSlicedBlock(PVIRTIO_DMA_MEMORY_SLICED p)
{
    /* If rdmapool is active and this VA is in the pool, free via rdmapool */
    if (IsRdmaPoolAddress(p->drv, p->va)) {
        NTSTATUS status = FreeTrackedRdmaPoolAllocation(p->drv, p->va);
        if (!NT_SUCCESS(status)) {
            DPrintf(0, "%s: rdmapool FREE VA=%p failed 0x%x; record retained\n", __FUNCTION__,
                    p->va, status);
        }
    } else {
        size_t offset;
        FindCommonBuffer(p->drv, p->va, &p->pa, &offset, TRUE);
    }
    ExFreePoolWithTag(p, p->drv->MemoryTag);
}

static PVOID AllocateSlice(PVIRTIO_DMA_MEMORY_SLICED p, PHYSICAL_ADDRESS *ppa)
{
    ULONG offset, index = RtlFindClearBitsAndSet(&p->bitmap, 1, 0);
    if (index >= p->bitmap.SizeOfBitMap) {
        return NULL;
    }
    offset = p->slice * index;
    ppa->QuadPart = p->pa.QuadPart + offset;
    return (PUCHAR)p->va + offset;
}

static void FreeSlice(PVIRTIO_DMA_MEMORY_SLICED p, PVOID va)
{
    size_t offset;

    /* For rdmapool addresses, compute offset directly */
    if (IsRdmaPoolAddress(p->drv, va)) {
        offset = (ULONG_PTR)va - (ULONG_PTR)p->va;
    } else {
        PHYSICAL_ADDRESS pa;
        if (!FindCommonBuffer(p->drv, va, &pa, &offset, FALSE)) {
            DPrintf(0, "%s: block with va %p not found\n", __FUNCTION__, va);
            return;
        }
    }

    if (offset % p->slice) {
        DPrintf(0, "%s: offset %d is wrong for slice %d\n", __FUNCTION__, (ULONG)offset, p->slice);
        return;
    }
    ULONG index = (ULONG)(offset / p->slice);
    if (!RtlTestBit(&p->bitmap, index)) {
        DPrintf(0, "%s: bit %d is NOT set\n", __FUNCTION__, index);
        return;
    }
    RtlClearBit(&p->bitmap, index);
}

PVIRTIO_DMA_MEMORY_SLICED VirtIOWdfDeviceAllocDmaMemorySliced(VirtIODevice *vdev, size_t blockSize,
                                                              ULONG sliceSize)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;
    size_t allocSize =
        sizeof(VIRTIO_DMA_MEMORY_SLICED) + (blockSize / sliceSize) / 8 + sizeof(ULONG);
    PVIRTIO_DMA_MEMORY_SLICED p =
        ExAllocatePoolUninitialized(NonPagedPool, allocSize, pWdfDriver->MemoryTag);
    if (!p) {
        return NULL;
    }
    __analysis_assume(allocSize > sizeof(*p));
    RtlZeroMemory(p, sizeof(*p));

    /* Allocate the backing DMA buffer */
    if (pWdfDriver->RdmaPoolActive) {
        p->va = AllocateFromRdmaPool(pWdfDriver, blockSize, 0);
        if (p->va) {
            /* Compute PA directly from pool base offsets */
            p->pa.QuadPart = pWdfDriver->RdmaPoolBasePA.QuadPart +
                             ((ULONG_PTR)p->va - (ULONG_PTR)pWdfDriver->RdmaPoolBaseVA);
        }
    } else {
        p->va = AllocateCommonBuffer(pWdfDriver, blockSize, 0);
        p->pa = GetPhysicalAddress(pWdfDriver, p->va);
    }

    if (!p->va) {
        ExFreePoolWithTag(p, pWdfDriver->MemoryTag);
        return NULL;
    }
    p->slice = sliceSize;
    p->drv = pWdfDriver;
    RtlInitializeBitMap(&p->bitmap, p->bitmap_buffer, (ULONG)blockSize / sliceSize);
    p->return_slice = FreeSlice;
    p->get_slice = AllocateSlice;
    p->destroy = FreeSlicedBlock;
    return p;
}

NTSTATUS VirtIOWdfReleaseRdmaPoolAllocations(PVIRTIO_WDF_DRIVER pWdfDriver)
{
    NTSTATUS firstFailure = STATUS_SUCCESS;

    if (pWdfDriver->RdmaPoolIoctlLock == NULL) {
        return STATUS_SUCCESS;
    }

    WdfWaitLockAcquire(pWdfDriver->RdmaPoolIoctlLock, NULL);
    pWdfDriver->RdmaPoolClosing = TRUE;
    if (pWdfDriver->RdmaPoolOwnerUnknown) {
        /* This is called only after VirtIOWdfShutdown has reset the device and
         * deleted every queue. Closing the sole file owner is therefore the
         * only safe recovery for an allocation whose V2 identity was malformed. */
        if (pWdfDriver->RdmaPoolFileObject == NULL) {
            WdfWaitLockRelease(pWdfDriver->RdmaPoolIoctlLock);
            return STATUS_INVALID_DEVICE_STATE;
        }
        ObDereferenceObject(pWdfDriver->RdmaPoolFileObject);
        pWdfDriver->RdmaPoolFileObject = NULL;
        pWdfDriver->RdmaPoolDeviceObject = NULL;
        pWdfDriver->RdmaPoolOwnerUnknown = FALSE;
        while (!IsListEmpty(&pWdfDriver->RdmaPoolAllocList)) {
            PLIST_ENTRY listEntry = RemoveHeadList(&pWdfDriver->RdmaPoolAllocList);
            ExFreePoolWithTag(CONTAINING_RECORD(listEntry, RDMAPOOL_ALLOC_ENTRY, ListEntry),
                              RDMAPOOL_ALLOC_TAG);
        }
        WdfWaitLockRelease(pWdfDriver->RdmaPoolIoctlLock);
        return STATUS_SUCCESS;
    }
    for (;;) {
        PRDMAPOOL_ALLOC_ENTRY allocation;
        NTSTATUS status;

        WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
        if (IsListEmpty(&pWdfDriver->RdmaPoolAllocList)) {
            WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
            break;
        }
        allocation =
            CONTAINING_RECORD(pWdfDriver->RdmaPoolAllocList.Flink, RDMAPOOL_ALLOC_ENTRY, ListEntry);
        WdfSpinLockRelease(pWdfDriver->DmaSpinlock);

        status = FreeRdmaPoolEntryLocked(pWdfDriver, allocation);
        if (!NT_SUCCESS(status)) {
            firstFailure = status;
            break;
        }

        WdfSpinLockAcquire(pWdfDriver->DmaSpinlock);
        RemoveEntryList(&allocation->ListEntry);
        WdfSpinLockRelease(pWdfDriver->DmaSpinlock);
        ExFreePoolWithTag(allocation, RDMAPOOL_ALLOC_TAG);
    }
    WdfWaitLockRelease(pWdfDriver->RdmaPoolIoctlLock);
    return firstFailure;
}

VOID OnDmaTransactionDestroy(WDFOBJECT Object)
{
    PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx = GetDmaTransactionContext(Object);
    DPrintf(1, "%s %p\n", __FUNCTION__, Object);
    // the MDL is one we allocated for the buffer
    // if there is no buffer - this is the MDL provided by the caller
    if (ctx->mdl && ctx->buffer) {
        IoFreeMdl(ctx->mdl);
    }
    if (ctx->buffer) {
        ExFreePoolWithTag(ctx->buffer, ctx->parameters.allocationTag);
    }
}

static FORCEINLINE void RefTransaction(PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx)
{
    InterlockedIncrement(&ctx->refCount);
}

static FORCEINLINE void DerefTransaction(PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx)
{
    if (!InterlockedDecrement(&ctx->refCount)) {
        WdfObjectDelete(ctx->parameters.transaction);
    }
}

BOOLEAN OnDmaTransactionProgramDma(WDFDMATRANSACTION Transaction, WDFDEVICE Device,
                                   WDFCONTEXT Context, WDF_DMA_DIRECTION Direction,
                                   PSCATTER_GATHER_LIST SgList)
{
    PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx = GetDmaTransactionContext(Transaction);
    RefTransaction(ctx);
    ctx->parameters.transaction = Transaction;
    ctx->parameters.sgList = SgList;
    DPrintf(1, "-->%s %p %d frags\n", __FUNCTION__, Transaction, SgList->NumberOfElements);
    BOOLEAN bFailed = !ctx->callback(&ctx->parameters);
    DPrintf(1, "<--%s %s\n", __FUNCTION__, bFailed ? "Failed" : "OK");
    DerefTransaction(ctx);
    return TRUE;
}

static BOOLEAN VirtIOWdfDeviceDmaAsync(VirtIODevice *vdev, PVIRTIO_DMA_TRANSACTION_PARAMS params,
                                       VirtIOWdfDmaTransactionCallback callback,
                                       WDF_DMA_DIRECTION Direction)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;
    WDFDMATRANSACTION tr;
    WDF_OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, VIRTIO_WDF_DMA_TRANSACTION_CONTEXT);
    attr.EvtDestroyCallback = OnDmaTransactionDestroy;
    status = WdfDmaTransactionCreate(pWdfDriver->DmaEnabler, &attr, &tr);
    if (!NT_SUCCESS(status)) {
        DPrintf(0, "%s FAILED(create) %X\n", __FUNCTION__, status);
        return FALSE;
    }
    PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx = GetDmaTransactionContext(tr);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->parameters = *params;
    ctx->callback = callback;
    ctx->refCount = 1;
    ctx->direction = Direction;
    if (params->req && params->req != (WDFREQUEST)WDF_INVALID_HANDLE) {
        status = WdfDmaTransactionInitializeUsingRequest(tr, params->req,
                                                         OnDmaTransactionProgramDma, Direction);
    } else if (params->req == (WDFREQUEST)WDF_INVALID_HANDLE) {
        status = WdfDmaTransactionInitializeUsingOffset(tr, OnDmaTransactionProgramDma, Direction,
                                                        (PMDL)params->buffer, 0, params->size);
    } else {
        ctx->buffer = ExAllocatePoolUninitialized(NonPagedPool, ctx->parameters.size,
                                                  ctx->parameters.allocationTag);
        if (ctx->buffer) {
            if (Direction == WdfDmaDirectionWriteToDevice) {
                RtlCopyMemory(ctx->buffer, params->buffer, params->size);
            }
            ctx->mdl = IoAllocateMdl(ctx->buffer, params->size, FALSE, FALSE, NULL);
            if (ctx->mdl) {
                MmBuildMdlForNonPagedPool(ctx->mdl);
                status = WdfDmaTransactionInitialize(tr, OnDmaTransactionProgramDma, Direction,
                                                     ctx->mdl, ctx->buffer, params->size);
            } else {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
        } else {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (!NT_SUCCESS(status)) {
        DPrintf(0, "%s FAILED(init) %X\n", __FUNCTION__, status);
        WdfObjectDelete(tr);
        return FALSE;
    }

    status = WdfDmaTransactionExecute(tr, NULL);
    if (!NT_SUCCESS(status)) {
        DPrintf(0, "%s FAILED(execution) %X\n", __FUNCTION__, status);
        WdfObjectDelete(tr);
        return FALSE;
    }

    return TRUE;
}

BOOLEAN VirtIOWdfDeviceDmaTxAsync(VirtIODevice *vdev, PVIRTIO_DMA_TRANSACTION_PARAMS params,
                                  VirtIOWdfDmaTransactionCallback callback)
{
    return VirtIOWdfDeviceDmaAsync(vdev, params, callback, WdfDmaDirectionWriteToDevice);
}

BOOLEAN VirtIOWdfDeviceDmaRxAsync(VirtIODevice *vdev, PVIRTIO_DMA_TRANSACTION_PARAMS params,
                                  VirtIOWdfDmaTransactionCallback callback)
{
    return VirtIOWdfDeviceDmaAsync(vdev, params, callback, WdfDmaDirectionReadFromDevice);
}

void VirtIOWdfDeviceDmaTxComplete(VirtIODevice *vdev, WDFDMATRANSACTION transaction)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;
    PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx = GetDmaTransactionContext(transaction);
    NTSTATUS status;
    DPrintf(1, "%s %p\n", __FUNCTION__, transaction);
    WdfDmaTransactionDmaCompletedFinal(transaction, 0, &status);
    DerefTransaction(ctx);
}

void VirtIOWdfDeviceDmaRxComplete(VirtIODevice *vdev, WDFDMATRANSACTION transaction, ULONG length)
{
    PVIRTIO_WDF_DRIVER pWdfDriver = vdev->DeviceContext;
    PVIRTIO_WDF_DMA_TRANSACTION_CONTEXT ctx = GetDmaTransactionContext(transaction);
    NTSTATUS status;
    DPrintf(1, "%s %p, len %d\n", __FUNCTION__, transaction, length);
    WdfDmaTransactionDmaCompletedFinal(transaction, length, &status);
    if (length && ctx->buffer) {
        RtlCopyMemory(ctx->parameters.buffer, ctx->buffer, length);
    }
    DerefTransaction(ctx);
}

NTSTATUS VirtIOWdfDeviceCheckIOMMUActive(PVIRTIO_WDF_DRIVER pWdfDriver, WDFDEVICE wdfDev)
{
    ULONGLONG deviceFeatures = VirtIOWdfGetDeviceFeatures(pWdfDriver);
    BOOLEAN bHasFeature = virtio_is_feature_enabled(deviceFeatures, VIRTIO_F_ACCESS_PLATFORM);

    DPrintf(0, "%s: VIRTIO_F_ACCESS_PLATFORM is %s\n", __FUNCTION__,
            bHasFeature ? "set" : "not set");

    // https://learn.microsoft.com/en-us/windows-hardware/drivers/pci/enabling-dma-remapping-for-device-drivers

    const DEVPROPKEY propKey = {
        { 0x83da6326, 0x97a6, 0x4088, { 0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29 } }, 18
    };
    ULONG value = 0, reqSize = 0;
    DEVPROPTYPE propType;
    WDF_DEVICE_PROPERTY_DATA propData;
    WDF_DEVICE_PROPERTY_DATA_INIT(&propData, &propKey);
    NTSTATUS status =
        WdfDeviceQueryPropertyEx(wdfDev, &propData, sizeof(value), &value, &reqSize, &propType);
    DPrintf(0, "%s: status %X, dma remap=%d\n", __FUNCTION__, status, value);
    pWdfDriver->IsIoMmuActive = bHasFeature && value == 2;
    if (!NT_SUCCESS(status) || value != 2 || bHasFeature) {
        return STATUS_SUCCESS;
    }
    pWdfDriver->IsIoMmuActive = FALSE;

    // the VIRTIO_F_ACCESS_PLATFORM is not set and there is
    // a possibility of DMA remapping
    WDFCOMMONBUFFER commonBuffer = NULL;
    status = WdfCommonBufferCreate(pWdfDriver->DmaEnabler, PAGE_SIZE, WDF_NO_OBJECT_ATTRIBUTES,
                                   &commonBuffer);
    if (!NT_SUCCESS(status)) {
        DPrintf(0, "%s: Can't allocate common buffer\n", __FUNCTION__);
        return status;
    }

    // let's check whether the physical address returned from common buffer API
    // is the same as returned from plain MmGetPhysicalAddress
    PHYSICAL_ADDRESS pa = WdfCommonBufferGetAlignedLogicalAddress(commonBuffer);
    PVOID va = WdfCommonBufferGetAlignedVirtualAddress(commonBuffer);
    PHYSICAL_ADDRESS plain = MmGetPhysicalAddress(va);
    DPrintf(0, "%s: buffer at %I64X, plain %I64X\n", __FUNCTION__, pa.QuadPart, plain.QuadPart);
    status = plain.QuadPart == pa.QuadPart ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
    WdfObjectDelete(commonBuffer);
    return status;
}
