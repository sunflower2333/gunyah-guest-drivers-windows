/*
 * NetKVM Restricted DMA Pool support - implementation.
 * See ParaNdis_RdmaPool.h. Mirrors VirtIO/WDF/VirtIOWdf.c + Dma.c.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#include "ndis56common.h"
#include "kdebugprint.h"
#include "ParaNdis_RdmaPool.h"

#include <wdmguid.h>
#include <initguid.h>
#include "rdmapool_interface.h"

#include "Trace.h"
#ifdef NETKVM_WPP_ENABLED
#include "ParaNdis_RdmaPool.tmh"
#endif

#define NETKVM_RDMAPOOL_ALLOC_TAG 'aRKN'

typedef struct _NETKVM_RDMAPOOL_ALLOCATION
{
    LIST_ENTRY ListEntry;
    PVOID VirtualAddress;
    ULONG NumPages;
    ULONG64 AllocationToken;
    BOOLEAN FreeSubmitted;
    NTSTATUS FreeStatus;
} NETKVM_RDMAPOOL_ALLOCATION, *PNETKVM_RDMAPOOL_ALLOCATION;

/* Issue a synchronous IOCTL to the rdmapool device. PASSIVE_LEVEL only. */
static NTSTATUS RdmaPoolIoctl(PARANDIS_ADAPTER *pContext,
                              ULONG ControlCode,
                              PVOID InBuf,
                              ULONG InLen,
                              PVOID OutBuf,
                              ULONG OutLen,
                              PULONG_PTR Information,
                              PBOOLEAN Submitted)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    PIO_STACK_LOCATION irpStack;
    NTSTATUS status;

    *Information = 0;
    *Submitted = FALSE;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));
    iosb.Status = STATUS_UNSUCCESSFUL;
    irp = IoBuildDeviceIoControlRequest(ControlCode,
                                        pContext->RdmaPoolDeviceObject,
                                        InBuf,
                                        InLen,
                                        OutBuf,
                                        OutLen,
                                        FALSE,
                                        &event,
                                        &iosb);
    if (irp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irpStack = IoGetNextIrpStackLocation(irp);
    irpStack->FileObject = pContext->RdmaPoolFileObject;

    *Submitted = TRUE;
    status = IoCallDriver(pContext->RdmaPoolDeviceObject, irp);
    if (status == STATUS_PENDING)
    {
        status = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        if (NT_SUCCESS(status))
        {
            status = iosb.Status;
        }
    }
    *Information = iosb.Information;
    return status;
}

static VOID RdmaPoolLock(PARANDIS_ADAPTER *pContext)
{
    (void)KeWaitForSingleObject(&pContext->RdmaPoolAutoDisconnect.m_IoctlMutex, Executive, KernelMode, FALSE, NULL);
}

static VOID RdmaPoolUnlock(PARANDIS_ADAPTER *pContext)
{
    KeReleaseMutex(&pContext->RdmaPoolAutoDisconnect.m_IoctlMutex, FALSE);
}

static VOID RdmaPoolCloseFileLocked(PARANDIS_ADAPTER *pContext)
{
    if (pContext->RdmaPoolFileObject != NULL)
    {
        ObDereferenceObject(pContext->RdmaPoolFileObject);
        pContext->RdmaPoolFileObject = NULL;
    }
    pContext->RdmaPoolDeviceObject = NULL;
    pContext->RdmaPoolAutoDisconnect.m_OwnerOpen = FALSE;
}

static VOID RdmaPoolPublishOwnerCleanupLocked(PARANDIS_ADAPTER *pContext)
{
    PLIST_ENTRY entry;

    for (entry = pContext->RdmaPoolAutoDisconnect.m_Allocations.Flink;
         entry != &pContext->RdmaPoolAutoDisconnect.m_Allocations;
         entry = entry->Flink)
    {
        PNETKVM_RDMAPOOL_ALLOCATION allocation = CONTAINING_RECORD(entry, NETKVM_RDMAPOOL_ALLOCATION, ListEntry);
        allocation->FreeSubmitted = TRUE;
        allocation->FreeStatus = STATUS_SUCCESS;
    }

    pContext->RdmaPoolAutoDisconnect.m_EmergencyActive = FALSE;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyVirtualAddress = NULL;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyNumPages = 0;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyAllocationToken = 0;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeSubmitted = TRUE;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeStatus = STATUS_SUCCESS;
    pContext->RdmaPoolAutoDisconnect.m_Status = STATUS_SUCCESS;
}

static BOOLEAN RdmaPoolValidPoolInfo(const RDMAPOOL_QUERY_POOL_OUTPUT *Query)
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

static BOOLEAN RdmaPoolValidAllocation(PARANDIS_ADAPTER *pContext,
                                       const RDMAPOOL_ALLOCATE_OUTPUT *Output,
                                       ULONG ExpectedPages)
{
    ULONG64 allocationSize = (ULONG64)ExpectedPages * PAGE_SIZE;
    ULONG_PTR baseVa = (ULONG_PTR)pContext->RdmaPoolBaseVA;
    ULONG_PTR allocationVa = (ULONG_PTR)Output->VirtualAddress;
    ULONG64 basePa = (ULONG64)pContext->RdmaPoolBasePA.QuadPart;
    ULONG64 allocationPa;
    ULONG64 vaOffset;
    ULONG64 paOffset;

    if (Output->InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2 || Output->NumPages != ExpectedPages ||
        Output->AllocationToken == 0 || Output->VirtualAddress == NULL || (allocationVa & (PAGE_SIZE - 1)) != 0 ||
        Output->PhysicalAddress.QuadPart < 0 || allocationVa < baseVa || allocationSize > pContext->RdmaPoolSize)
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
    return vaOffset == paOffset && vaOffset <= pContext->RdmaPoolSize - allocationSize;
}

static BOOLEAN RdmaPoolValidFreeIdentity(PARANDIS_ADAPTER *pContext,
                                         const RDMAPOOL_ALLOCATE_OUTPUT *Output,
                                         ULONG ExpectedPages)
{
    ULONG64 allocationSize = (ULONG64)ExpectedPages * PAGE_SIZE;
    ULONG_PTR baseVa = (ULONG_PTR)pContext->RdmaPoolBaseVA;
    ULONG_PTR allocationVa = (ULONG_PTR)Output->VirtualAddress;

    return Output->InterfaceVersion == RDMAPOOL_INTERFACE_VERSION_V2 && Output->NumPages == ExpectedPages &&
           Output->AllocationToken != 0 && Output->VirtualAddress != NULL && (allocationVa & (PAGE_SIZE - 1)) == 0 &&
           allocationVa >= baseVa && allocationSize <= pContext->RdmaPoolSize &&
           (ULONG64)(allocationVa - baseVa) <= pContext->RdmaPoolSize - allocationSize;
}

static BOOLEAN RdmaPoolAllocationConflictsLocked(PARANDIS_ADAPTER *pContext, const RDMAPOOL_ALLOCATE_OUTPUT *Output)
{
    ULONG64 candidateStart = (ULONG64)((ULONG_PTR)Output->VirtualAddress - (ULONG_PTR)pContext->RdmaPoolBaseVA);
    ULONG64 candidateEnd = candidateStart + (ULONG64)Output->NumPages * PAGE_SIZE;
    PLIST_ENTRY entry;

    for (entry = pContext->RdmaPoolAutoDisconnect.m_Allocations.Flink;
         entry != &pContext->RdmaPoolAutoDisconnect.m_Allocations;
         entry = entry->Flink)
    {
        PNETKVM_RDMAPOOL_ALLOCATION allocation = CONTAINING_RECORD(entry, NETKVM_RDMAPOOL_ALLOCATION, ListEntry);
        ULONG64 allocationStart = (ULONG64)((ULONG_PTR)allocation->VirtualAddress -
                                            (ULONG_PTR)pContext->RdmaPoolBaseVA);
        ULONG64 allocationEnd = allocationStart + (ULONG64)allocation->NumPages * PAGE_SIZE;

        if (allocation->AllocationToken == Output->AllocationToken ||
            (candidateStart < allocationEnd && allocationStart < candidateEnd))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static PNETKVM_RDMAPOOL_ALLOCATION RdmaPoolFindAllocationLocked(PARANDIS_ADAPTER *pContext, PVOID VirtualAddress)
{
    PLIST_ENTRY entry;

    for (entry = pContext->RdmaPoolAutoDisconnect.m_Allocations.Flink;
         entry != &pContext->RdmaPoolAutoDisconnect.m_Allocations;
         entry = entry->Flink)
    {
        PNETKVM_RDMAPOOL_ALLOCATION allocation = CONTAINING_RECORD(entry, NETKVM_RDMAPOOL_ALLOCATION, ListEntry);
        if (allocation->VirtualAddress == VirtualAddress)
        {
            return allocation;
        }
    }
    return NULL;
}

static BOOLEAN RdmaPoolAddressInKnownExtentLocked(PARANDIS_ADAPTER *pContext, PVOID VirtualAddress)
{
    ULONG_PTR base = (ULONG_PTR)pContext->RdmaPoolBaseVA;
    ULONG_PTR address = (ULONG_PTR)VirtualAddress;

    return pContext->RdmaPoolBaseVA != NULL && pContext->RdmaPoolSize != 0 && address >= base &&
           (ULONG64)(address - base) < pContext->RdmaPoolSize;
}

static NTSTATUS RdmaPoolFreeAllocationLocked(PARANDIS_ADAPTER *pContext, PNETKVM_RDMAPOOL_ALLOCATION Allocation)
{
    RDMAPOOL_FREE_INPUT freeInput;
    ULONG_PTR information;
    BOOLEAN submitted;
    NTSTATUS status;

    /* A submitted FREE has an indeterminate provider-side result on every
     * error path. Never submit it again, including during disconnect. */
    if (Allocation->FreeSubmitted)
    {
        return Allocation->FreeStatus;
    }
    if (pContext->RdmaPoolFileObject == NULL || pContext->RdmaPoolDeviceObject == NULL ||
        Allocation->VirtualAddress == NULL || Allocation->NumPages == 0 || Allocation->AllocationToken == 0)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    RtlZeroMemory(&freeInput, sizeof(freeInput));
    freeInput.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
    freeInput.NumPages = Allocation->NumPages;
    freeInput.VirtualAddress = Allocation->VirtualAddress;
    freeInput.AllocationToken = Allocation->AllocationToken;

    status = RdmaPoolIoctl(pContext,
                           (ULONG)IOCTL_RDMAPOOL_FREE,
                           &freeInput,
                           sizeof(freeInput),
                           NULL,
                           0,
                           &information,
                           &submitted);
    if (NT_SUCCESS(status) && information != 0)
    {
        status = STATUS_DATA_ERROR;
    }
    if (submitted)
    {
        Allocation->FreeSubmitted = TRUE;
        Allocation->FreeStatus = status;
    }
    return status;
}

static VOID RdmaPoolRememberEmergencyAllocationLocked(PARANDIS_ADAPTER *pContext,
                                                      const RDMAPOOL_ALLOCATE_OUTPUT *Output)
{
    pContext->RdmaPoolAutoDisconnect.m_EmergencyActive = TRUE;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyVirtualAddress = Output->VirtualAddress;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyNumPages = Output->NumPages;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyAllocationToken = Output->AllocationToken;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeSubmitted = FALSE;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeStatus = STATUS_SUCCESS;
}

static NTSTATUS RdmaPoolFreeEmergencyAllocationLocked(PARANDIS_ADAPTER *pContext)
{
    NETKVM_RDMAPOOL_ALLOCATION allocation;
    NTSTATUS status;

    if (!pContext->RdmaPoolAutoDisconnect.m_EmergencyActive)
    {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&allocation, sizeof(allocation));
    allocation.VirtualAddress = pContext->RdmaPoolAutoDisconnect.m_EmergencyVirtualAddress;
    allocation.NumPages = pContext->RdmaPoolAutoDisconnect.m_EmergencyNumPages;
    allocation.AllocationToken = pContext->RdmaPoolAutoDisconnect.m_EmergencyAllocationToken;
    allocation.FreeSubmitted = pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeSubmitted;
    allocation.FreeStatus = pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeStatus;

    status = RdmaPoolFreeAllocationLocked(pContext, &allocation);
    pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeSubmitted = allocation.FreeSubmitted;
    pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeStatus = allocation.FreeStatus;
    if (NT_SUCCESS(status))
    {
        pContext->RdmaPoolAutoDisconnect.m_EmergencyActive = FALSE;
        pContext->RdmaPoolAutoDisconnect.m_EmergencyVirtualAddress = NULL;
        pContext->RdmaPoolAutoDisconnect.m_EmergencyNumPages = 0;
        pContext->RdmaPoolAutoDisconnect.m_EmergencyAllocationToken = 0;
    }
    return status;
}

NTSTATUS ParaNdis_RdmaPoolConnect(PARANDIS_ADAPTER *pContext)
{
    NTSTATUS status;
    PWSTR deviceInterfaceList = NULL;
    UNICODE_STRING deviceName;
    RDMAPOOL_QUERY_POOL_OUTPUT queryOutput;
    ULONG_PTR information;
    BOOLEAN submitted;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    RdmaPoolLock(pContext);
    if (pContext->RdmaPoolActive || pContext->RdmaPoolDeviceObject != NULL || pContext->RdmaPoolFileObject != NULL ||
        !IsListEmpty(&pContext->RdmaPoolAutoDisconnect.m_Allocations) ||
        pContext->RdmaPoolAutoDisconnect.m_EmergencyActive)
    {
        status = NT_SUCCESS(pContext->RdmaPoolAutoDisconnect.m_Status) ? STATUS_DEVICE_BUSY
                                                                       : pContext->RdmaPoolAutoDisconnect.m_Status;
        RdmaPoolUnlock(pContext);
        return status;
    }

    pContext->RdmaPoolBaseVA = NULL;
    pContext->RdmaPoolBasePA.QuadPart = 0;
    pContext->RdmaPoolSize = 0;
    pContext->RdmaPoolAutoDisconnect.m_DisconnectStarted = FALSE;
    pContext->RdmaPoolAutoDisconnect.m_Status = STATUS_SUCCESS;

    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_RDMAPOOL, NULL, 0, &deviceInterfaceList);
    if (!NT_SUCCESS(status))
    {
        DPrintf(0, "rdmapool interface enumeration failed 0x%x", status);
        if (deviceInterfaceList)
        {
            ExFreePool(deviceInterfaceList);
        }
        RdmaPoolUnlock(pContext);
        return status;
    }
    if (deviceInterfaceList == NULL)
    {
        RdmaPoolUnlock(pContext);
        return STATUS_DATA_ERROR;
    }
    if (*deviceInterfaceList == L'\0')
    {
        DPrintf(0, "rdmapool device interface not found - using normal DMA");
        ExFreePool(deviceInterfaceList);
        RdmaPoolUnlock(pContext);
        return STATUS_NOT_FOUND;
    }

    RtlInitUnicodeString(&deviceName, deviceInterfaceList);
    status = IoGetDeviceObjectPointer(&deviceName,
                                      FILE_ALL_ACCESS,
                                      &pContext->RdmaPoolFileObject,
                                      &pContext->RdmaPoolDeviceObject);
    ExFreePool(deviceInterfaceList);
    if (!NT_SUCCESS(status))
    {
        DPrintf(0, "IoGetDeviceObjectPointer(rdmapool) failed 0x%x", status);
        pContext->RdmaPoolFileObject = NULL;
        pContext->RdmaPoolDeviceObject = NULL;
        RdmaPoolUnlock(pContext);
        return status;
    }
    if (pContext->RdmaPoolFileObject == NULL || pContext->RdmaPoolDeviceObject == NULL)
    {
        RdmaPoolCloseFileLocked(pContext);
        RdmaPoolUnlock(pContext);
        return STATUS_DATA_ERROR;
    }
    pContext->RdmaPoolAutoDisconnect.m_OwnerOpen = TRUE;

    RtlZeroMemory(&queryOutput, sizeof(queryOutput));
    status = RdmaPoolIoctl(pContext,
                           (ULONG)IOCTL_RDMAPOOL_QUERY_POOL,
                           NULL,
                           0,
                           &queryOutput,
                           sizeof(queryOutput),
                           &information,
                           &submitted);
    if (!NT_SUCCESS(status))
    {
        DPrintf(0, "IOCTL_RDMAPOOL_QUERY_POOL failed 0x%x", status);
        RdmaPoolCloseFileLocked(pContext);
        RdmaPoolUnlock(pContext);
        return status;
    }
    if (information != sizeof(queryOutput))
    {
        DPrintf(0,
                "IOCTL_RDMAPOOL_QUERY_POOL returned %llu bytes, expected %llu",
                (ULONG64)information,
                (ULONG64)sizeof(queryOutput));
        RdmaPoolCloseFileLocked(pContext);
        RdmaPoolUnlock(pContext);
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (queryOutput.InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2)
    {
        RdmaPoolCloseFileLocked(pContext);
        RdmaPoolUnlock(pContext);
        return STATUS_REVISION_MISMATCH;
    }
    if (queryOutput.PageSize != PAGE_SIZE || !RdmaPoolValidPoolInfo(&queryOutput))
    {
        RdmaPoolCloseFileLocked(pContext);
        RdmaPoolUnlock(pContext);
        return STATUS_DATA_ERROR;
    }

    pContext->RdmaPoolBaseVA = queryOutput.BaseVirtualAddress;
    pContext->RdmaPoolBasePA = queryOutput.BasePhysicalAddress;
    pContext->RdmaPoolSize = queryOutput.TotalSize;
    pContext->RdmaPoolActive = TRUE;
    RdmaPoolUnlock(pContext);

    DPrintf(0,
            "Connected to rdmapool VA=%p PA=0x%llx Size=0x%llx",
            pContext->RdmaPoolBaseVA,
            pContext->RdmaPoolBasePA.QuadPart,
            pContext->RdmaPoolSize);
    return STATUS_SUCCESS;
}

NTSTATUS ParaNdis_RdmaPoolReleaseAllocations(PARANDIS_ADAPTER *pContext)
{
    PLIST_ENTRY entry;
    NTSTATUS firstFailure = STATUS_SUCCESS;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    RdmaPoolLock(pContext);
    pContext->RdmaPoolActive = FALSE;
    pContext->RdmaPoolAutoDisconnect.m_DisconnectStarted = TRUE;
    if (!NT_SUCCESS(pContext->RdmaPoolAutoDisconnect.m_Status))
    {
        firstFailure = pContext->RdmaPoolAutoDisconnect.m_Status;
    }
    for (entry = pContext->RdmaPoolAutoDisconnect.m_Allocations.Flink;
         NT_SUCCESS(firstFailure) && entry != &pContext->RdmaPoolAutoDisconnect.m_Allocations;
         entry = entry->Flink)
    {
        PNETKVM_RDMAPOOL_ALLOCATION allocation = CONTAINING_RECORD(entry, NETKVM_RDMAPOOL_ALLOCATION, ListEntry);

        status = RdmaPoolFreeAllocationLocked(pContext, allocation);
        if (!NT_SUCCESS(status))
        {
            if (allocation->FreeSubmitted)
            {
                pContext->RdmaPoolAutoDisconnect.m_Status = status;
            }
            if (NT_SUCCESS(firstFailure))
            {
                firstFailure = status;
            }
            DPrintf(0, "rdmapool disconnect FREE failed 0x%x for VA=%p", status, allocation->VirtualAddress);
        }
    }

    if (NT_SUCCESS(firstFailure))
    {
        status = RdmaPoolFreeEmergencyAllocationLocked(pContext);
        if (!NT_SUCCESS(status))
        {
            if (pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeSubmitted)
            {
                pContext->RdmaPoolAutoDisconnect.m_Status = status;
            }
            firstFailure = status;
            DPrintf(0, "rdmapool teardown emergency FREE failed 0x%x", status);
        }
    }
    if (!NT_SUCCESS(firstFailure))
    {
        /* The provider owns every allocation by this file object. Closing the
         * final reference synchronously runs its file-close reclamation after
         * all queues, DPCs, and packet owners have already been stopped. */
        RdmaPoolCloseFileLocked(pContext);
        RdmaPoolPublishOwnerCleanupLocked(pContext);
        RdmaPoolUnlock(pContext);
        return firstFailure;
    }

    RdmaPoolUnlock(pContext);
    return STATUS_SUCCESS;
}

NTSTATUS ParaNdis_RdmaPoolDisconnect(PARANDIS_ADAPTER *pContext)
{
    PLIST_ENTRY entry;
    NTSTATUS status = STATUS_SUCCESS;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    RdmaPoolLock(pContext);
    if (!pContext->RdmaPoolAutoDisconnect.m_DisconnectStarted)
    {
        status = STATUS_INVALID_DEVICE_STATE;
    }
    else if (pContext->RdmaPoolAutoDisconnect.m_EmergencyActive)
    {
        status = pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeSubmitted ? pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeStatus
                                                                           : STATUS_DEVICE_BUSY;
    }
    else if (!NT_SUCCESS(pContext->RdmaPoolAutoDisconnect.m_Status))
    {
        status = pContext->RdmaPoolAutoDisconnect.m_Status;
    }

    for (entry = pContext->RdmaPoolAutoDisconnect.m_Allocations.Flink;
         NT_SUCCESS(status) && entry != &pContext->RdmaPoolAutoDisconnect.m_Allocations;
         entry = entry->Flink)
    {
        PNETKVM_RDMAPOOL_ALLOCATION allocation = CONTAINING_RECORD(entry, NETKVM_RDMAPOOL_ALLOCATION, ListEntry);

        if (!allocation->FreeSubmitted)
        {
            status = STATUS_DEVICE_BUSY;
        }
        else if (!NT_SUCCESS(allocation->FreeStatus))
        {
            status = allocation->FreeStatus;
        }
    }

    if (!NT_SUCCESS(status))
    {
        RdmaPoolUnlock(pContext);
        return status;
    }

    /* Every provider allocation is gone now. Records left in the list are
     * successful tombstones consumed by their late-owning C++ members. */
    RdmaPoolCloseFileLocked(pContext);
    /* Retain the queried extent as provenance until adapter destruction. It
     * prevents a duplicate/stale pool VA from falling through to NDIS free. */
    RdmaPoolUnlock(pContext);
    return STATUS_SUCCESS;
}

PVOID ParaNdis_RdmaPoolAllocate(PARANDIS_ADAPTER *pContext, ULONG size, PHYSICAL_ADDRESS *pPa)
{
    RDMAPOOL_ALLOCATE_INPUT allocInput;
    RDMAPOOL_ALLOCATE_OUTPUT allocOutput;
    PNETKVM_RDMAPOOL_ALLOCATION allocation;
    ULONG_PTR information;
    ULONG pageCount;
    BOOLEAN submitted;
    NTSTATUS status;

    if (pPa)
    {
        pPa->QuadPart = 0;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || size == 0)
    {
        return NULL;
    }

    pageCount = size / PAGE_SIZE + ((size % PAGE_SIZE) != 0);
    if (pageCount == 0)
    {
        return NULL;
    }

    RtlZeroMemory(&allocInput, sizeof(allocInput));
    RtlZeroMemory(&allocOutput, sizeof(allocOutput));
    allocInput.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
    allocInput.NumPages = pageCount;

    RdmaPoolLock(pContext);
    if (!pContext->RdmaPoolActive || pContext->RdmaPoolFileObject == NULL || pContext->RdmaPoolDeviceObject == NULL ||
        !NT_SUCCESS(pContext->RdmaPoolAutoDisconnect.m_Status) || pContext->RdmaPoolAutoDisconnect.m_EmergencyActive)
    {
        RdmaPoolUnlock(pContext);
        return NULL;
    }

    status = RdmaPoolIoctl(pContext,
                           (ULONG)IOCTL_RDMAPOOL_ALLOCATE,
                           &allocInput,
                           sizeof(allocInput),
                           &allocOutput,
                           sizeof(allocOutput),
                           &information,
                           &submitted);
    if (!NT_SUCCESS(status))
    {
        DPrintf(0, "IOCTL_RDMAPOOL_ALLOCATE failed 0x%x (size=0x%x)", status, size);
        RdmaPoolUnlock(pContext);
        return NULL;
    }

    if (information != sizeof(allocOutput) || !RdmaPoolValidAllocation(pContext, &allocOutput, pageCount) ||
        RdmaPoolAllocationConflictsLocked(pContext, &allocOutput))
    {
        NTSTATUS rollbackStatus = STATUS_DATA_ERROR;
        BOOLEAN canRollback = information == sizeof(allocOutput) &&
                              RdmaPoolValidFreeIdentity(pContext, &allocOutput, pageCount) &&
                              !RdmaPoolAllocationConflictsLocked(pContext, &allocOutput);

        if (canRollback)
        {
            RdmaPoolRememberEmergencyAllocationLocked(pContext, &allocOutput);
            rollbackStatus = RdmaPoolFreeEmergencyAllocationLocked(pContext);
        }
        if (NT_SUCCESS(rollbackStatus))
        {
            RdmaPoolUnlock(pContext);
        }
        else
        {
            if (!canRollback || pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeSubmitted)
            {
                pContext->RdmaPoolAutoDisconnect.m_Status = rollbackStatus;
            }
            RdmaPoolUnlock(pContext);
            DPrintf(0, "malformed rdmapool ALLOCATE rollback failed 0x%x; retaining owner", rollbackStatus);
        }
        return NULL;
    }

    /* Allocate tracking only after the provider has returned a fully validated
     * extent. The fixed emergency slot makes tracking OOM rollback exact. */
    allocation = (PNETKVM_RDMAPOOL_ALLOCATION)NdisAllocateMemoryWithTagPriority(pContext->MiniportHandle,
                                                                                sizeof(*allocation),
                                                                                NETKVM_RDMAPOOL_ALLOC_TAG,
                                                                                NormalPoolPriority);
    if (allocation == NULL)
    {
        RdmaPoolRememberEmergencyAllocationLocked(pContext, &allocOutput);
        status = RdmaPoolFreeEmergencyAllocationLocked(pContext);
        if (!NT_SUCCESS(status))
        {
            if (pContext->RdmaPoolAutoDisconnect.m_EmergencyFreeSubmitted)
            {
                pContext->RdmaPoolAutoDisconnect.m_Status = status;
            }
            DPrintf(0, "rdmapool tracking OOM rollback failed 0x%x; retaining owner", status);
        }
        RdmaPoolUnlock(pContext);
        return NULL;
    }

    RtlZeroMemory(allocation, sizeof(*allocation));
    allocation->VirtualAddress = allocOutput.VirtualAddress;
    allocation->NumPages = allocOutput.NumPages;
    allocation->AllocationToken = allocOutput.AllocationToken;
    allocation->FreeStatus = STATUS_SUCCESS;
    InsertTailList(&pContext->RdmaPoolAutoDisconnect.m_Allocations, &allocation->ListEntry);
    RdmaPoolUnlock(pContext);

    if (pPa)
    {
        *pPa = allocOutput.PhysicalAddress;
    }
    DPrintf(2,
            "rdmapool alloc VA=%p PA=0x%llx size=0x%x",
            allocOutput.VirtualAddress,
            allocOutput.PhysicalAddress.QuadPart,
            size);
    return allocOutput.VirtualAddress;
}

NTSTATUS ParaNdis_RdmaPoolFree(PARANDIS_ADAPTER *pContext, PVOID va, ULONG size)
{
    PNETKVM_RDMAPOOL_ALLOCATION allocation;
    ULONG pageCount;
    NTSTATUS status;

    if (va == NULL || size == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        DPrintf(0, "rdmapool free of VA=%p (0x%x bytes) rejected at IRQL %u", va, size, KeGetCurrentIrql());
        return STATUS_INVALID_DEVICE_STATE;
    }

    pageCount = size / PAGE_SIZE + ((size % PAGE_SIZE) != 0);
    RdmaPoolLock(pContext);
    allocation = RdmaPoolFindAllocationLocked(pContext, va);
    if (allocation == NULL)
    {
        status = RdmaPoolAddressInKnownExtentLocked(pContext, va) ? STATUS_INVALID_PARAMETER : STATUS_NOT_FOUND;
        RdmaPoolUnlock(pContext);
        return status;
    }
    if (pageCount != allocation->NumPages)
    {
        RdmaPoolUnlock(pContext);
        return STATUS_INVALID_PARAMETER;
    }
    if (pContext->RdmaPoolAutoDisconnect.m_DisconnectStarted && !allocation->FreeSubmitted)
    {
        RdmaPoolUnlock(pContext);
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = RdmaPoolFreeAllocationLocked(pContext, allocation);
    if (!NT_SUCCESS(status))
    {
        if (allocation->FreeSubmitted)
        {
            pContext->RdmaPoolAutoDisconnect.m_Status = status;
        }
        RdmaPoolUnlock(pContext);
        DPrintf(0, "IOCTL_RDMAPOOL_FREE failed 0x%x for VA=%p; retaining owner", status, va);
        return status;
    }

    RemoveEntryList(&allocation->ListEntry);
    RdmaPoolUnlock(pContext);
    NdisFreeMemoryWithTagPriority(pContext->MiniportHandle, allocation, NETKVM_RDMAPOOL_ALLOC_TAG);
    return STATUS_SUCCESS;
}

_PARANDIS_ADAPTER::CRdmaPoolAutoDisconnect::~CRdmaPoolAutoDisconnect()
{
    while (!IsListEmpty(&m_Allocations))
    {
        PLIST_ENTRY entry = RemoveHeadList(&m_Allocations);
        PNETKVM_RDMAPOOL_ALLOCATION allocation = CONTAINING_RECORD(entry, NETKVM_RDMAPOOL_ALLOCATION, ListEntry);

        if (m_OwnerOpen || !allocation->FreeSubmitted || !NT_SUCCESS(allocation->FreeStatus))
        {
            DPrintf(0, "rdmapool terminal tracking record is not a provider-released tombstone");
            NETKVM_ASSERT(FALSE);
        }
        NdisFreeMemoryWithTagPriority(m_MiniportHandle, allocation, NETKVM_RDMAPOOL_ALLOC_TAG);
    }

    if (m_EmergencyActive || m_OwnerOpen)
    {
        DPrintf(0, "rdmapool terminal owner state was not released");
        NETKVM_ASSERT(FALSE);
    }
}
