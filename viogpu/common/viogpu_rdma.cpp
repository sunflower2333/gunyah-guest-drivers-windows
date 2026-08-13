#include "viogpu_rdma.h"

#include <wdmguid.h>
#include <initguid.h>
#include "../../rdmapool/rdmapool_interface.h"

#define VIOGPU_RDMAPOOL_TAG     'GDRG'
#define VIOGPU_RDMA_ALLOC_MAGIC 'ADRG'

typedef struct _VIOGPU_RDMA_ALLOCATION
{
    ULONG Magic;
    ULONG StartPage;
    ULONG PageCount;
    PVOID Address;
} VIOGPU_RDMA_ALLOCATION, *PVIOGPU_RDMA_ALLOCATION;

static BOOLEAN BitmapTest(PUCHAR bitmap, ULONG bit)
{
    return (bitmap[bit / 8] & (1u << (bit % 8))) != 0;
}

static void BitmapSet(PUCHAR bitmap, ULONG bit, BOOLEAN value)
{
    UCHAR mask = (UCHAR)(1u << (bit % 8));
    if (value)
    {
        bitmap[bit / 8] |= mask;
    }
    else
    {
        bitmap[bit / 8] &= (UCHAR)~mask;
    }
}

typedef struct _VIOGPU_RDMA_IOCTL_RESULT
{
    NTSTATUS Status;
    ULONG_PTR Information;
    BOOLEAN Submitted;
} VIOGPU_RDMA_IOCTL_RESULT;

static VIOGPU_RDMA_IOCTL_RESULT RdmaPoolIoctl(PDEVICE_OBJECT deviceObject,
                                              PFILE_OBJECT fileObject,
                                              ULONG controlCode,
                                              PVOID inputBuffer,
                                              ULONG inputLength,
                                              PVOID outputBuffer,
                                              ULONG outputLength)
{
    VIOGPU_RDMA_IOCTL_RESULT result = {STATUS_INSUFFICIENT_RESOURCES, 0, FALSE};
    KEVENT event;
    IO_STATUS_BLOCK ioStatus = {};
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    PIRP irp = IoBuildDeviceIoControlRequest(controlCode,
                                             deviceObject,
                                             inputBuffer,
                                             inputLength,
                                             outputBuffer,
                                             outputLength,
                                             FALSE,
                                             &event,
                                             &ioStatus);
    if (irp == NULL)
    {
        return result;
    }

    result.Submitted = TRUE;
    IoGetNextIrpStackLocation(irp)->FileObject = fileObject;
    NTSTATUS status = IoCallDriver(deviceObject, irp);
    if (status == STATUS_PENDING)
    {
        // IoBuildDeviceIoControlRequest keeps references to this stack event, IOSB, and the caller buffers.
        // Keep the synchronous wait unbounded until the provider completes the request.
        NTSTATUS waitStatus = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = NT_SUCCESS(waitStatus) ? ioStatus.Status : waitStatus;
    }
    result.Status = status;
    result.Information = ioStatus.Information;
    return result;
}

VioGpuRdmaPool::VioGpuRdmaPool()
    : m_ArenaOwned(FALSE), m_Ready(FALSE), m_RundownCompleted(FALSE), m_DisconnectAttempted(FALSE),
      m_DisconnectStatus(STATUS_SUCCESS), m_FileObject(NULL), m_DeviceObject(NULL), m_BaseVA(NULL), m_Size(0),
      m_PageCount(0), m_AllocationToken(0), m_Bitmap(NULL), m_VidMmBaseVA(NULL), m_VidMmSize(0)
{
    m_BasePA.QuadPart = 0;
    m_VidMmBasePA.QuadPart = 0;
    KeInitializeSpinLock(&m_Lock);
    ExInitializeRundownProtection(&m_Operations);
}

VioGpuRdmaPool::~VioGpuRdmaPool()
{
    NTSTATUS status = Disconnect();
    NT_ASSERT(NT_SUCCESS(status));
}

NTSTATUS VioGpuRdmaPool::Connect(void)
{
    if (m_Ready)
    {
        return STATUS_SUCCESS;
    }
    if (m_ArenaOwned || m_FileObject != NULL || m_DeviceObject != NULL || m_Bitmap != NULL)
    {
        return NT_SUCCESS(m_DisconnectStatus) ? STATUS_DEVICE_NOT_READY : m_DisconnectStatus;
    }

    PWSTR interfaceList = NULL;
    NTSTATUS status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_RDMAPOOL, NULL, 0, &interfaceList);
    if (!NT_SUCCESS(status) || interfaceList == NULL || *interfaceList == L'\0')
    {
        if (interfaceList != NULL)
        {
            ExFreePool(interfaceList);
        }
        return STATUS_NOT_FOUND;
    }

    UNICODE_STRING deviceName;
    RtlInitUnicodeString(&deviceName, interfaceList);
    status = IoGetDeviceObjectPointer(&deviceName, FILE_ALL_ACCESS, &m_FileObject, &m_DeviceObject);
    ExFreePool(interfaceList);
    if (!NT_SUCCESS(status))
    {
        m_FileObject = NULL;
        m_DeviceObject = NULL;
        return status;
    }

    RDMAPOOL_QUERY_POOL_OUTPUT query = {};
    RDMAPOOL_QUERY_ALLOCATION_OUTPUT allocation = {};
    RDMAPOOL_ALLOCATE_INPUT input = {};
    RDMAPOOL_ALLOCATE_OUTPUT output = {};
    ULONG arenaPages = 0;
    VIOGPU_RDMA_IOCTL_RESULT ioctlResult;
    do
    {
        ioctlResult = RdmaPoolIoctl(m_DeviceObject,
                                    m_FileObject,
                                    (ULONG)IOCTL_RDMAPOOL_QUERY_POOL,
                                    NULL,
                                    0,
                                    &query,
                                    sizeof(query));
        status = ioctlResult.Status;
        if (!NT_SUCCESS(status))
        {
            break;
        }
        if (ioctlResult.Information != sizeof(query) || query.InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2 ||
            query.PageSize != PAGE_SIZE || query.BaseVirtualAddress == NULL ||
            ((ULONG_PTR)query.BaseVirtualAddress & (PAGE_SIZE - 1)) != 0 || query.BasePhysicalAddress.QuadPart < 0 ||
            ((ULONG64)query.BasePhysicalAddress.QuadPart & (PAGE_SIZE - 1)) != 0 || query.TotalSize < PAGE_SIZE ||
            (query.TotalSize & (PAGE_SIZE - 1)) != 0 || query.TotalSize > MAXULONG_PTR ||
            query.TotalSize / PAGE_SIZE > MAXULONG ||
            (ULONG_PTR)query.BaseVirtualAddress > MAXULONG_PTR - (SIZE_T)(query.TotalSize - 1) ||
            (ULONG64)query.BasePhysicalAddress.QuadPart > MAXULONGLONG - (query.TotalSize - 1))
        {
            status = STATUS_DATA_ERROR;
            break;
        }

        ioctlResult = RdmaPoolIoctl(m_DeviceObject,
                                    m_FileObject,
                                    (ULONG)IOCTL_RDMAPOOL_QUERY_ALLOCATION,
                                    NULL,
                                    0,
                                    &allocation,
                                    sizeof(allocation));
        status = ioctlResult.Status;
        if (!NT_SUCCESS(status))
        {
            break;
        }
        if (ioctlResult.Information != sizeof(allocation) ||
            allocation.LargestFreeRunPages > query.TotalSize / PAGE_SIZE ||
            allocation.FreePages > query.TotalSize / PAGE_SIZE)
        {
            status = STATUS_DATA_ERROR;
            break;
        }

        ULONG targetPages = (ULONG)(query.TotalSize / PAGE_SIZE / 2);
        ULONG previousArenaPages = arenaPages;
        arenaPages = min(targetPages, allocation.LargestFreeRunPages);
        if (arenaPages == 0 || arenaPages == previousArenaPages)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        input.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
        input.NumPages = arenaPages;
        RtlZeroMemory(&output, sizeof(output));
        ioctlResult = RdmaPoolIoctl(m_DeviceObject,
                                    m_FileObject,
                                    (ULONG)IOCTL_RDMAPOOL_ALLOCATE,
                                    &input,
                                    sizeof(input),
                                    &output,
                                    sizeof(output));
        status = ioctlResult.Status;
    } while (status == STATUS_INSUFFICIENT_RESOURCES);
    if (!NT_SUCCESS(status))
    {
        (void)Disconnect();
        return status;
    }

    ULONG64 arenaSize = (ULONG64)arenaPages * PAGE_SIZE;
    ULONG64 vaOffset = output.VirtualAddress != NULL && (ULONG_PTR)output.VirtualAddress >= (ULONG_PTR)query.BaseVirtualAddress
                                                                                                                           ? (ULONG64)((ULONG_PTR)output.VirtualAddress -
                                                                                                                                       (ULONG_PTR)query.BaseVirtualAddress)
                                                                                                                           : MAXULONGLONG;
    ULONG64 paOffset = output.PhysicalAddress.QuadPart >= query.BasePhysicalAddress.QuadPart ? (ULONG64)(output.PhysicalAddress.QuadPart -
                                                                                                         query.BasePhysicalAddress.QuadPart)
                                                                                             : MAXULONGLONG;
    if (ioctlResult.Information != sizeof(output) || output.InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2 ||
        output.NumPages != arenaPages || output.AllocationToken == 0 || output.VirtualAddress == NULL ||
        ((ULONG_PTR)output.VirtualAddress & (PAGE_SIZE - 1)) != 0 || output.PhysicalAddress.QuadPart < 0 ||
        ((ULONG64)output.PhysicalAddress.QuadPart & (PAGE_SIZE - 1)) != 0 || arenaSize > query.TotalSize ||
        vaOffset != paOffset || vaOffset > query.TotalSize - arenaSize)
    {
        // A malformed success may still own provider pages. Attempt the exact V2 identity returned by the
        // provider, then close the file object so provider file-close remains the final ownership boundary.
        if (output.VirtualAddress != NULL && output.NumPages != 0 && output.AllocationToken != 0)
        {
            RDMAPOOL_FREE_INPUT cleanup = {};
            cleanup.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
            cleanup.NumPages = output.NumPages;
            cleanup.VirtualAddress = output.VirtualAddress;
            cleanup.AllocationToken = output.AllocationToken;
            (void)RdmaPoolIoctl(m_DeviceObject,
                                m_FileObject,
                                (ULONG)IOCTL_RDMAPOOL_FREE,
                                &cleanup,
                                sizeof(cleanup),
                                NULL,
                                0);
        }
        ClearConnection();
        return STATUS_DATA_ERROR;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu rdmapool arena: %lu pages (free %lu, largest run %lu, ACPI total %I64u bytes)\n",
               input.NumPages,
               allocation.FreePages,
               allocation.LargestFreeRunPages,
               query.TotalSize);

    m_BaseVA = output.VirtualAddress;
    m_BasePA = output.PhysicalAddress;
    m_Size = (SIZE_T)arenaPages * PAGE_SIZE;
    m_PageCount = input.NumPages;
    m_AllocationToken = output.AllocationToken;
    m_ArenaOwned = TRUE;
    SIZE_T bitmapSize = (m_PageCount + 7) / 8;
    m_Bitmap = (PUCHAR)ExAllocatePoolUninitialized(NonPagedPoolNx, bitmapSize, VIOGPU_RDMAPOOL_TAG);
    if (m_Bitmap == NULL)
    {
        NTSTATUS disconnectStatus = Disconnect();
        return NT_SUCCESS(disconnectStatus) ? STATUS_INSUFFICIENT_RESOURCES : disconnectStatus;
    }
    RtlZeroMemory(m_Bitmap, bitmapSize);
    ULONG vidMmStartPage = m_PageCount / 2;
    for (ULONG page = vidMmStartPage; page < m_PageCount; page++)
    {
        BitmapSet(m_Bitmap, page, TRUE);
    }
    m_VidMmBaseVA = (PUCHAR)m_BaseVA + ((SIZE_T)vidMmStartPage * PAGE_SIZE);
    m_VidMmBasePA.QuadPart = m_BasePA.QuadPart + ((ULONGLONG)vidMmStartPage * PAGE_SIZE);
    m_VidMmSize = (SIZE_T)(m_PageCount - vidMmStartPage) * PAGE_SIZE;
    RtlZeroMemory(m_BaseVA, m_Size);
    if (m_RundownCompleted)
    {
        ExReInitializeRundownProtection(&m_Operations);
        m_RundownCompleted = FALSE;
    }
    m_Ready = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuRdmaPool::Disconnect(void)
{
    if (!m_ArenaOwned)
    {
        ClearConnection();
        return STATUS_SUCCESS;
    }
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_Lock, &oldIrql);
    m_Ready = FALSE;
    KeReleaseSpinLock(&m_Lock, oldIrql);
    if (!m_RundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_Operations);
        m_RundownCompleted = TRUE;
    }
    if (m_FileObject == NULL || m_DeviceObject == NULL || m_BaseVA == NULL || m_Size == 0 || m_PageCount == 0 ||
        m_AllocationToken == 0)
    {
        m_DisconnectStatus = STATUS_DEVICE_NOT_READY;
        return m_DisconnectStatus;
    }
    if (m_DisconnectAttempted)
    {
        return m_DisconnectStatus;
    }

    RDMAPOOL_FREE_INPUT input = {};
    input.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
    input.VirtualAddress = m_BaseVA;
    input.NumPages = m_PageCount;
    input.AllocationToken = m_AllocationToken;
    VIOGPU_RDMA_IOCTL_RESULT ioctlResult = RdmaPoolIoctl(m_DeviceObject,
                                                         m_FileObject,
                                                         (ULONG)IOCTL_RDMAPOOL_FREE,
                                                         &input,
                                                         sizeof(input),
                                                         NULL,
                                                         0);
    if (!ioctlResult.Submitted)
    {
        m_DisconnectStatus = ioctlResult.Status;
        return m_DisconnectStatus;
    }
    m_DisconnectAttempted = TRUE;
    m_DisconnectStatus = ioctlResult.Status;
    if (!NT_SUCCESS(m_DisconnectStatus))
    {
        // Once submitted, the provider may have consumed the allocation even when completion reports failure.
        // Keep the exact owner tuple and never issue a second FREE for this token.
        return m_DisconnectStatus;
    }

    ClearConnection();
    return STATUS_SUCCESS;
}

void VioGpuRdmaPool::ClearConnection(void)
{
    if (m_FileObject != NULL)
    {
        ObDereferenceObject(m_FileObject);
    }
    if (m_Bitmap != NULL)
    {
        ExFreePoolWithTag(m_Bitmap, VIOGPU_RDMAPOOL_TAG);
    }
    m_ArenaOwned = FALSE;
    m_Ready = FALSE;
    m_DisconnectAttempted = FALSE;
    m_DisconnectStatus = STATUS_SUCCESS;
    m_FileObject = NULL;
    m_DeviceObject = NULL;
    m_BaseVA = NULL;
    m_BasePA.QuadPart = 0;
    m_Size = 0;
    m_PageCount = 0;
    m_AllocationToken = 0;
    m_Bitmap = NULL;
    m_VidMmBaseVA = NULL;
    m_VidMmBasePA.QuadPart = 0;
    m_VidMmSize = 0;
}

PVOID VioGpuRdmaPool::Allocate(SIZE_T size, SIZE_T alignment)
{
    if (size == 0 || KeGetCurrentIrql() > DISPATCH_LEVEL || !ExAcquireRundownProtection(&m_Operations))
    {
        return NULL;
    }
    if (alignment < sizeof(PVOID))
    {
        alignment = sizeof(PVOID);
    }
    if ((alignment & (alignment - 1)) != 0 || size > MAXULONG_PTR - (alignment - 1) - sizeof(VIOGPU_RDMA_ALLOCATION))
    {
        ExReleaseRundownProtection(&m_Operations);
        return NULL;
    }

    SIZE_T totalSize = size + alignment - 1 + sizeof(VIOGPU_RDMA_ALLOCATION);
    if (totalSize > m_Size)
    {
        ExReleaseRundownProtection(&m_Operations);
        return NULL;
    }
    ULONG pageCount = (ULONG)ADDRESS_AND_SIZE_TO_SPAN_PAGES(m_BaseVA, totalSize);
    ULONG startPage = 0;
    BOOLEAN found = FALSE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_Lock, &oldIrql);
    if (!m_Ready || m_Bitmap == NULL)
    {
        KeReleaseSpinLock(&m_Lock, oldIrql);
        ExReleaseRundownProtection(&m_Operations);
        return NULL;
    }
    for (ULONG candidate = 0; candidate < m_PageCount && pageCount <= m_PageCount - candidate; candidate++)
    {
        ULONG page = 0;
        for (; page < pageCount && !BitmapTest(m_Bitmap, candidate + page); page++)
        {
        }
        if (page == pageCount)
        {
            startPage = candidate;
            found = TRUE;
            for (page = 0; page < pageCount; page++)
            {
                BitmapSet(m_Bitmap, startPage + page, TRUE);
            }
            break;
        }
        candidate += page;
    }
    KeReleaseSpinLock(&m_Lock, oldIrql);
    if (!found)
    {
        ExReleaseRundownProtection(&m_Operations);
        return NULL;
    }

    PUCHAR allocationBase = (PUCHAR)m_BaseVA + ((SIZE_T)startPage * PAGE_SIZE);
    ULONG_PTR aligned = ((ULONG_PTR)(allocationBase + sizeof(VIOGPU_RDMA_ALLOCATION)) + alignment - 1) &
                        ~(alignment - 1);
    PVIOGPU_RDMA_ALLOCATION header = (PVIOGPU_RDMA_ALLOCATION)(aligned - sizeof(*header));
    header->Magic = VIOGPU_RDMA_ALLOC_MAGIC;
    header->StartPage = startPage;
    header->PageCount = pageCount;
    header->Address = (PVOID)aligned;
    PVOID allocation = (PVOID)aligned;
    RtlZeroMemory(allocation, size);
    ExReleaseRundownProtection(&m_Operations);
    return allocation;
}

void VioGpuRdmaPool::Free(PVOID address)
{
    if (address == NULL || KeGetCurrentIrql() > DISPATCH_LEVEL || !ExAcquireRundownProtection(&m_Operations))
    {
        return;
    }
    if (!Contains(address))
    {
        ExReleaseRundownProtection(&m_Operations);
        return;
    }
    if ((ULONG_PTR)address - (ULONG_PTR)m_BaseVA < sizeof(VIOGPU_RDMA_ALLOCATION))
    {
        ExReleaseRundownProtection(&m_Operations);
        return;
    }

    PVIOGPU_RDMA_ALLOCATION header = (PVIOGPU_RDMA_ALLOCATION)((PUCHAR)address - sizeof(*header));
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_Lock, &oldIrql);
    if (!m_Ready || m_Bitmap == NULL || header->Magic != VIOGPU_RDMA_ALLOC_MAGIC || header->Address != address ||
        header->PageCount == 0 || header->StartPage >= m_PageCount ||
        header->PageCount > m_PageCount - header->StartPage)
    {
        KeReleaseSpinLock(&m_Lock, oldIrql);
        ExReleaseRundownProtection(&m_Operations);
        return;
    }
    for (ULONG page = 0; page < header->PageCount; page++)
    {
        if (!BitmapTest(m_Bitmap, header->StartPage + page))
        {
            KeReleaseSpinLock(&m_Lock, oldIrql);
            ExReleaseRundownProtection(&m_Operations);
            return;
        }
    }
    header->Magic = 0;
    header->Address = NULL;
    for (ULONG page = 0; page < header->PageCount; page++)
    {
        BitmapSet(m_Bitmap, header->StartPage + page, FALSE);
    }
    KeReleaseSpinLock(&m_Lock, oldIrql);
    ExReleaseRundownProtection(&m_Operations);
}

BOOLEAN VioGpuRdmaPool::Contains(PVOID address) const
{
    ULONG_PTR value = (ULONG_PTR)address;
    ULONG_PTR base = (ULONG_PTR)m_BaseVA;
    return m_Ready && m_BaseVA != NULL && value >= base && value - base < m_Size;
}

PHYSICAL_ADDRESS VioGpuRdmaPool::GetPhysicalAddress(PVOID address)
{
    PHYSICAL_ADDRESS physicalAddress = {};
    if (ExAcquireRundownProtection(&m_Operations))
    {
        if (Contains(address))
        {
            physicalAddress.QuadPart = m_BasePA.QuadPart + ((PUCHAR)address - (PUCHAR)m_BaseVA);
        }
        ExReleaseRundownProtection(&m_Operations);
    }
    return physicalAddress;
}

BOOLEAN VioGpuRdmaPool::QueryVidMmSegment(PVOID *baseAddress, PPHYSICAL_ADDRESS physicalAddress, SIZE_T *size) const
{
    if (baseAddress == NULL || physicalAddress == NULL || size == NULL || !ExAcquireRundownProtection(&m_Operations))
    {
        return FALSE;
    }

    if (!m_Ready || m_VidMmBaseVA == NULL || m_VidMmSize == 0)
    {
        ExReleaseRundownProtection(&m_Operations);
        return FALSE;
    }

    *baseAddress = m_VidMmBaseVA;
    *physicalAddress = m_VidMmBasePA;
    *size = m_VidMmSize;
    ExReleaseRundownProtection(&m_Operations);
    return TRUE;
}
