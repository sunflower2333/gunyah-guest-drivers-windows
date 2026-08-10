#include "viogpu_rdma.h"

#include <wdmguid.h>
#include <initguid.h>
#include "../../rdmapool/rdmapool_interface.h"

#define VIOGPU_RDMAPOOL_TAG      'GDRG'
#define VIOGPU_RDMA_ALLOC_MAGIC  'ADRG'

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

static NTSTATUS RdmaPoolIoctl(PDEVICE_OBJECT deviceObject,
                              PFILE_OBJECT fileObject,
                              ULONG controlCode,
                              PVOID inputBuffer,
                              ULONG inputLength,
                              PVOID outputBuffer,
                              ULONG outputLength)
{
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
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IoGetNextIrpStackLocation(irp)->FileObject = fileObject;
    NTSTATUS status = IoCallDriver(deviceObject, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }
    return status;
}

VioGpuRdmaPool::VioGpuRdmaPool()
        : m_Active(FALSE), m_FileObject(NULL), m_DeviceObject(NULL), m_BaseVA(NULL), m_Size(0), m_PageCount(0),
            m_Bitmap(NULL)
{
    m_BasePA.QuadPart = 0;
    KeInitializeSpinLock(&m_Lock);
}

VioGpuRdmaPool::~VioGpuRdmaPool()
{
    Disconnect();
}

NTSTATUS VioGpuRdmaPool::Connect(void)
{
    if (m_Active)
    {
        return STATUS_SUCCESS;
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
    do
    {
        status = RdmaPoolIoctl(m_DeviceObject,
                               m_FileObject,
                               (ULONG)IOCTL_RDMAPOOL_QUERY_POOL,
                               NULL,
                               0,
                               &query,
                               sizeof(query));
        if (!NT_SUCCESS(status))
        {
            break;
        }

        status = RdmaPoolIoctl(m_DeviceObject,
                               m_FileObject,
                               (ULONG)IOCTL_RDMAPOOL_QUERY_ALLOCATION,
                               NULL,
                               0,
                               &allocation,
                               sizeof(allocation));
        if (!NT_SUCCESS(status))
        {
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

        input.NumPages = arenaPages;
        status = RdmaPoolIoctl(m_DeviceObject,
                               m_FileObject,
                               (ULONG)IOCTL_RDMAPOOL_ALLOCATE,
                               &input,
                               sizeof(input),
                               &output,
                               sizeof(output));
    } while (status == STATUS_INSUFFICIENT_RESOURCES);
    if (!NT_SUCCESS(status))
    {
        Disconnect();
        return status;
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
    SIZE_T bitmapSize = (m_PageCount + 7) / 8;
    m_Bitmap = (PUCHAR)ExAllocatePoolUninitialized(NonPagedPoolNx, bitmapSize, VIOGPU_RDMAPOOL_TAG);
    if (m_Bitmap == NULL)
    {
        m_Active = TRUE;
        Disconnect();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(m_Bitmap, bitmapSize);
    m_Active = TRUE;
    RtlZeroMemory(m_BaseVA, m_Size);
    return STATUS_SUCCESS;
}

void VioGpuRdmaPool::Disconnect(void)
{
    if (m_Active && m_FileObject != NULL)
    {
        RDMAPOOL_FREE_INPUT input = {};
        input.VirtualAddress = m_BaseVA;
        input.NumPages = (ULONG)(m_Size / PAGE_SIZE);
        (void)RdmaPoolIoctl(m_DeviceObject,
                            m_FileObject,
                            (ULONG)IOCTL_RDMAPOOL_FREE,
                            &input,
                            sizeof(input),
                            NULL,
                            0);
    }
    if (m_FileObject != NULL)
    {
        ObDereferenceObject(m_FileObject);
    }
    if (m_Bitmap != NULL)
    {
        ExFreePoolWithTag(m_Bitmap, VIOGPU_RDMAPOOL_TAG);
    }
    m_Active = FALSE;
    m_FileObject = NULL;
    m_DeviceObject = NULL;
    m_BaseVA = NULL;
    m_BasePA.QuadPart = 0;
    m_Size = 0;
    m_PageCount = 0;
    m_Bitmap = NULL;
}

PVOID VioGpuRdmaPool::Allocate(SIZE_T size, SIZE_T alignment)
{
    if (!m_Active || size == 0 || KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        return NULL;
    }
    if (alignment < sizeof(PVOID))
    {
        alignment = sizeof(PVOID);
    }
    if ((alignment & (alignment - 1)) != 0 ||
        size > MAXULONG_PTR - (alignment - 1) - sizeof(VIOGPU_RDMA_ALLOCATION))
    {
        return NULL;
    }

    SIZE_T totalSize = size + alignment - 1 + sizeof(VIOGPU_RDMA_ALLOCATION);
    if (totalSize > m_Size)
    {
        return NULL;
    }
    ULONG pageCount = (ULONG)ADDRESS_AND_SIZE_TO_SPAN_PAGES(m_BaseVA, totalSize);
    ULONG startPage = 0;
    BOOLEAN found = FALSE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_Lock, &oldIrql);
    for (ULONG candidate = 0; candidate + pageCount <= m_PageCount; candidate++)
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
    return allocation;
}

void VioGpuRdmaPool::Free(PVOID address)
{
    if (!Contains(address) || KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        return;
    }
    PVIOGPU_RDMA_ALLOCATION header = (PVIOGPU_RDMA_ALLOCATION)((PUCHAR)address - sizeof(*header));
    if (header->Magic != VIOGPU_RDMA_ALLOC_MAGIC || header->Address != address || header->PageCount == 0 ||
        header->StartPage >= m_PageCount || header->PageCount > m_PageCount - header->StartPage)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_Lock, &oldIrql);
    for (ULONG page = 0; page < header->PageCount; page++)
    {
        if (!BitmapTest(m_Bitmap, header->StartPage + page))
        {
            KeReleaseSpinLock(&m_Lock, oldIrql);
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
}

BOOLEAN VioGpuRdmaPool::Contains(PVOID address) const
{
    return m_Active && address >= m_BaseVA && (PUCHAR)address < (PUCHAR)m_BaseVA + m_Size;
}

PHYSICAL_ADDRESS VioGpuRdmaPool::GetPhysicalAddress(PVOID address) const
{
    PHYSICAL_ADDRESS physicalAddress = {};
    if (Contains(address))
    {
        physicalAddress.QuadPart = m_BasePA.QuadPart + ((PUCHAR)address - (PUCHAR)m_BaseVA);
    }
    return physicalAddress;
}