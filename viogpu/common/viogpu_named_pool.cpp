#include <initguid.h>
#include "viogpu_named_pool.h"

static const CHAR VIOGPU_DRM_HOST_POOL_NAME[] = "drm2kgsl_host";

static_assert(sizeof(DROIDVMPOOL_QUERY_OUTPUT) == 96, "unexpected droidvmpool query ABI size");
static_assert(FIELD_OFFSET(DROIDVMPOOL_QUERY_OUTPUT, PoolName) == 32, "unexpected droidvmpool name offset");
static_assert(sizeof(DROIDVMPOOL_MAPPING) == 24, "unexpected droidvmpool mapping ABI size");
static_assert(sizeof(DROIDVMPOOL_DIRECT_INTERFACE) == 48, "unexpected droidvmpool direct-interface ABI size");
static_assert(FIELD_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE, AcquireMapping) == 32,
              "unexpected droidvmpool acquire callback offset");
static_assert(FIELD_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE, ReleaseMapping) == 40,
              "unexpected droidvmpool release callback offset");

typedef struct _VIOGPU_NAMED_POOL_IOCTL_RESULT
{
    NTSTATUS Status;
    ULONG_PTR Information;
} VIOGPU_NAMED_POOL_IOCTL_RESULT;

static VIOGPU_NAMED_POOL_IOCTL_RESULT NamedPoolQuery(_In_ PDEVICE_OBJECT deviceObject,
                                                     _In_ PFILE_OBJECT fileObject,
                                                     _Out_ PDROIDVMPOOL_QUERY_OUTPUT output)
{
    VIOGPU_NAMED_POOL_IOCTL_RESULT result = {STATUS_INSUFFICIENT_RESOURCES, 0};
    KEVENT event;
    IO_STATUS_BLOCK ioStatus = {};

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    PIRP irp = IoBuildDeviceIoControlRequest((ULONG)IOCTL_DROIDVMPOOL_QUERY,
                                             deviceObject,
                                             NULL,
                                             0,
                                             output,
                                             sizeof(*output),
                                             FALSE,
                                             &event,
                                             &ioStatus);
    if (irp == NULL)
    {
        return result;
    }

    IoGetNextIrpStackLocation(irp)->FileObject = fileObject;
    NTSTATUS status = IoCallDriver(deviceObject, irp);
    if (status == STATUS_PENDING)
    {
        NTSTATUS waitStatus = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = NT_SUCCESS(waitStatus) ? ioStatus.Status : waitStatus;
    }
    result.Status = status;
    result.Information = ioStatus.Information;
    return result;
}

static NTSTATUS NamedPoolQueryDirectInterface(_In_ PDEVICE_OBJECT deviceObject,
                                              _Out_ PDROIDVMPOOL_DIRECT_INTERFACE directInterface)
{
    KEVENT event;
    IO_STATUS_BLOCK ioStatus = {};
    PDEVICE_OBJECT targetObject = IoGetAttachedDeviceReference(deviceObject);

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    PIRP irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, targetObject, NULL, 0, NULL, &event, &ioStatus);
    if (irp == NULL)
    {
        ObDereferenceObject(targetObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PIO_STACK_LOCATION irpStack = IoGetNextIrpStackLocation(irp);
    irpStack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    irpStack->Parameters.QueryInterface.InterfaceType = (LPGUID)&GUID_DROIDVMPOOL_DIRECT_INTERFACE;
    irpStack->Parameters.QueryInterface.Size = sizeof(*directInterface);
    irpStack->Parameters.QueryInterface.Version = DROIDVMPOOL_DIRECT_VERSION_V1;
    irpStack->Parameters.QueryInterface.Interface = (PINTERFACE)directInterface;
    irpStack->Parameters.QueryInterface.InterfaceSpecificData = NULL;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    NTSTATUS status = IoCallDriver(targetObject, irp);
    if (status == STATUS_PENDING)
    {
        NTSTATUS waitStatus = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = NT_SUCCESS(waitStatus) ? ioStatus.Status : waitStatus;
    }
    ObDereferenceObject(targetObject);
    return status;
}

static BOOLEAN NamedPoolCharacterIsValid(CHAR character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
}

static NTSTATUS ValidateNamedPoolQuery(_In_ const DROIDVMPOOL_QUERY_OUTPUT *query,
                                       _In_ ULONG_PTR information,
                                       _Out_ BOOLEAN *isDrmHost)
{
    *isDrmHost = FALSE;
    if (information != sizeof(*query) || query->InterfaceVersion != DROIDVMPOOL_INTERFACE_VERSION_V1 ||
        query->StructureSize != sizeof(*query) || query->PoolNameLength == 0 ||
        query->PoolNameLength >= DROIDVMPOOL_NAME_CAPACITY || query->PoolName[query->PoolNameLength] != '\0' ||
        query->PageSize != PAGE_SIZE || query->BasePhysicalAddress.QuadPart < 0 ||
        ((ULONG64)query->BasePhysicalAddress.QuadPart & (PAGE_SIZE - 1)) != 0 || query->TotalSize < PAGE_SIZE ||
        (query->TotalSize & (PAGE_SIZE - 1)) != 0 || query->TotalSize > MAXULONG_PTR ||
        (ULONG64)query->BasePhysicalAddress.QuadPart > MAXULONGLONG - (query->TotalSize - 1))
    {
        return STATUS_DATA_ERROR;
    }

    for (ULONG index = 0; index < query->PoolNameLength; ++index)
    {
        if (query->PoolName[index] == '\0' || !NamedPoolCharacterIsValid(query->PoolName[index]))
        {
            return STATUS_DATA_ERROR;
        }
    }
    for (ULONG index = query->PoolNameLength + 1; index < DROIDVMPOOL_NAME_CAPACITY; ++index)
    {
        if (query->PoolName[index] != '\0')
        {
            return STATUS_DATA_ERROR;
        }
    }

    *isDrmHost = query->PoolNameLength == sizeof(VIOGPU_DRM_HOST_POOL_NAME) - 1 &&
                 RtlCompareMemory(query->PoolName,
                                  VIOGPU_DRM_HOST_POOL_NAME,
                                  sizeof(VIOGPU_DRM_HOST_POOL_NAME) - 1) == sizeof(VIOGPU_DRM_HOST_POOL_NAME) - 1;
    return STATUS_SUCCESS;
}

static NTSTATUS ValidateDirectInterface(_In_ const DROIDVMPOOL_DIRECT_INTERFACE *directInterface)
{
    if (directInterface->InterfaceHeader.Size != sizeof(*directInterface) ||
        directInterface->InterfaceHeader.Version != DROIDVMPOOL_DIRECT_VERSION_V1 ||
        directInterface->InterfaceHeader.Context == NULL ||
        directInterface->InterfaceHeader.InterfaceReference == NULL ||
        directInterface->InterfaceHeader.InterfaceDereference == NULL || directInterface->AcquireMapping == NULL ||
        directInterface->ReleaseMapping == NULL)
    {
        return STATUS_REVISION_MISMATCH;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ValidateNamedPoolMapping(_In_ const DROIDVMPOOL_MAPPING *mapping,
                                         _In_ const DROIDVMPOOL_QUERY_OUTPUT *query)
{
    if (mapping->BaseVirtualAddress == NULL || ((ULONG_PTR)mapping->BaseVirtualAddress & (PAGE_SIZE - 1)) != 0 ||
        mapping->BasePhysicalAddress.QuadPart != query->BasePhysicalAddress.QuadPart ||
        mapping->TotalSize != query->TotalSize || mapping->TotalSize > MAXULONG_PTR ||
        (ULONG_PTR)mapping->BaseVirtualAddress > MAXULONG_PTR - (SIZE_T)(mapping->TotalSize - 1))
    {
        return STATUS_DATA_ERROR;
    }
    return STATUS_SUCCESS;
}

static void DereferenceDirectInterface(_Inout_ PDROIDVMPOOL_DIRECT_INTERFACE directInterface)
{
    if (directInterface->InterfaceHeader.InterfaceDereference != NULL &&
        directInterface->InterfaceHeader.Context != NULL)
    {
        directInterface->InterfaceHeader.InterfaceDereference(directInterface->InterfaceHeader.Context);
    }
    RtlZeroMemory(directInterface, sizeof(*directInterface));
}

VioGpuDrmHostPoolMapping::VioGpuDrmHostPoolMapping() : m_Owner(NULL)
{
    RtlZeroMemory(&m_Mapping, sizeof(m_Mapping));
}

VioGpuDrmHostPoolMapping::~VioGpuDrmHostPoolMapping()
{
    Release();
}

void VioGpuDrmHostPoolMapping::Release(void)
{
    if (m_Owner != NULL)
    {
        m_Owner->ReleaseMapping(this);
    }
    NT_ASSERT(m_Owner == NULL);
}

VioGpuDrmHostPool::VioGpuDrmHostPool() : m_Ready(FALSE), m_RundownCompleted(FALSE), m_FileObject(NULL), m_Size(0)
{
    ExInitializeRundownProtection(&m_Operations);
    RtlZeroMemory(&m_DirectInterface, sizeof(m_DirectInterface));
    m_BasePA.QuadPart = 0;
}

VioGpuDrmHostPool::~VioGpuDrmHostPool()
{
    PAGED_CODE();
    Disconnect();
    NT_ASSERT(!HasConnectionOwner());
}

NTSTATUS VioGpuDrmHostPool::Connect(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (IsActive())
    {
        return STATUS_SUCCESS;
    }
    if (HasConnectionOwner() || m_Size != 0)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (m_RundownCompleted)
    {
        ExReInitializeRundownProtection(&m_Operations);
        m_RundownCompleted = FALSE;
    }

    PWSTR interfaceList = NULL;
    NTSTATUS status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_DROIDVMPOOL, NULL, 0, &interfaceList);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (interfaceList == NULL || *interfaceList == L'\0')
    {
        if (interfaceList != NULL)
        {
            ExFreePool(interfaceList);
        }
        return STATUS_NOT_FOUND;
    }

    NTSTATUS firstFailure = STATUS_SUCCESS;
    ULONG matchCount = 0;
    PFILE_OBJECT selectedFileObject = NULL;
    DROIDVMPOOL_DIRECT_INTERFACE selectedDirectInterface = {};
    DROIDVMPOOL_QUERY_OUTPUT selectedQuery = {};

    UNICODE_STRING deviceName;
    for (PWSTR interfaceName = interfaceList; *interfaceName != L'\0';
         interfaceName += (deviceName.Length / sizeof(WCHAR)) + 1)
    {
        PFILE_OBJECT fileObject = NULL;
        PDEVICE_OBJECT deviceObject = NULL;
        RtlInitUnicodeString(&deviceName, interfaceName);
        status = IoGetDeviceObjectPointer(&deviceName, FILE_ALL_ACCESS, &fileObject, &deviceObject);
        if (!NT_SUCCESS(status))
        {
            if (NT_SUCCESS(firstFailure))
            {
                firstFailure = status;
            }
            continue;
        }

        DROIDVMPOOL_QUERY_OUTPUT query = {};
        VIOGPU_NAMED_POOL_IOCTL_RESULT ioctlResult = NamedPoolQuery(deviceObject, fileObject, &query);
        BOOLEAN isDrmHost = FALSE;
        status = ioctlResult.Status;
        if (NT_SUCCESS(status))
        {
            status = ValidateNamedPoolQuery(&query, ioctlResult.Information, &isDrmHost);
        }
        if (!NT_SUCCESS(status))
        {
            if (NT_SUCCESS(firstFailure))
            {
                firstFailure = status;
            }
            ObDereferenceObject(fileObject);
            continue;
        }

        if (!isDrmHost)
        {
            ObDereferenceObject(fileObject);
            continue;
        }

        DROIDVMPOOL_DIRECT_INTERFACE directInterface = {};
        status = NamedPoolQueryDirectInterface(deviceObject, &directInterface);
        BOOLEAN directInterfaceReferenced = NT_SUCCESS(status);
        if (NT_SUCCESS(status))
        {
            status = ValidateDirectInterface(&directInterface);
        }
        if (NT_SUCCESS(status))
        {
            DROIDVMPOOL_MAPPING mapping = {};
            if (!directInterface.AcquireMapping(directInterface.InterfaceHeader.Context, &mapping))
            {
                status = STATUS_DEVICE_NOT_READY;
            }
            else
            {
                status = ValidateNamedPoolMapping(&mapping, &query);
                directInterface.ReleaseMapping(directInterface.InterfaceHeader.Context);
            }
        }
        if (!NT_SUCCESS(status))
        {
            if (NT_SUCCESS(firstFailure))
            {
                firstFailure = status;
            }
            if (directInterfaceReferenced)
            {
                DereferenceDirectInterface(&directInterface);
            }
            ObDereferenceObject(fileObject);
            continue;
        }

        ++matchCount;
        if (matchCount == 1)
        {
            selectedFileObject = fileObject;
            selectedDirectInterface = directInterface;
            selectedQuery = query;
        }
        else
        {
            DereferenceDirectInterface(&directInterface);
            ObDereferenceObject(fileObject);
        }
    }
    ExFreePool(interfaceList);

    if (!NT_SUCCESS(firstFailure) || matchCount != 1)
    {
        if (selectedFileObject != NULL)
        {
            DereferenceDirectInterface(&selectedDirectInterface);
            ObDereferenceObject(selectedFileObject);
        }
        if (!NT_SUCCESS(firstFailure))
        {
            return firstFailure;
        }
        return matchCount == 0 ? STATUS_NOT_FOUND : STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    m_FileObject = selectedFileObject;
    m_DirectInterface = selectedDirectInterface;
    m_BasePA = selectedQuery.BasePhysicalAddress;
    m_Size = (SIZE_T)selectedQuery.TotalSize;
    InterlockedExchange(&m_Ready, TRUE);

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu droidvmpool: drm2kgsl_host PA=0x%llx size=0x%Ix\n",
               m_BasePA.QuadPart,
               m_Size);
    return STATUS_SUCCESS;
}

void VioGpuDrmHostPool::Disconnect(void)
{
    PAGED_CODE();

    InterlockedExchange(&m_Ready, FALSE);
    if (!m_RundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_Operations);
        m_RundownCompleted = TRUE;
    }

    DROIDVMPOOL_DIRECT_INTERFACE directInterface = m_DirectInterface;
    RtlZeroMemory(&m_DirectInterface, sizeof(m_DirectInterface));
    m_BasePA.QuadPart = 0;
    m_Size = 0;
    DereferenceDirectInterface(&directInterface);

    if (m_FileObject != NULL)
    {
        PFILE_OBJECT fileObject = m_FileObject;
        m_FileObject = NULL;
        ObDereferenceObject(fileObject);
    }
}

BOOLEAN VioGpuDrmHostPool::AcquireMapping(_Out_ VioGpuDrmHostPoolMapping *mapping) const
{
    if (mapping == NULL || mapping->m_Owner != NULL || KeGetCurrentIrql() > DISPATCH_LEVEL ||
        !ExAcquireRundownProtection(&m_Operations))
    {
        return FALSE;
    }

    BOOLEAN acquired = FALSE;
    DROIDVMPOOL_MAPPING mappingValue = {};
    if (IsActive() && m_FileObject != NULL && m_DirectInterface.InterfaceHeader.Context != NULL &&
        m_DirectInterface.AcquireMapping != NULL && m_DirectInterface.ReleaseMapping != NULL)
    {
        acquired = m_DirectInterface.AcquireMapping(m_DirectInterface.InterfaceHeader.Context, &mappingValue);
    }
    if (!acquired)
    {
        ExReleaseRundownProtection(&m_Operations);
        return FALSE;
    }

    if (mappingValue.BaseVirtualAddress == NULL ||
        ((ULONG_PTR)mappingValue.BaseVirtualAddress & (PAGE_SIZE - 1)) != 0 ||
        mappingValue.BasePhysicalAddress.QuadPart != m_BasePA.QuadPart || mappingValue.TotalSize != m_Size ||
        mappingValue.TotalSize > MAXULONG_PTR ||
        (ULONG_PTR)mappingValue.BaseVirtualAddress > MAXULONG_PTR - (SIZE_T)(mappingValue.TotalSize - 1))
    {
        m_DirectInterface.ReleaseMapping(m_DirectInterface.InterfaceHeader.Context);
        ExReleaseRundownProtection(&m_Operations);
        return FALSE;
    }

    mapping->m_Mapping = mappingValue;
    mapping->m_Owner = this;
    return TRUE;
}

void VioGpuDrmHostPool::ReleaseMapping(_Inout_ VioGpuDrmHostPoolMapping *mapping) const
{
    NT_ASSERT(mapping != NULL && mapping->m_Owner == this);
    NT_ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    m_DirectInterface.ReleaseMapping(m_DirectInterface.InterfaceHeader.Context);
    mapping->m_Owner = NULL;
    RtlZeroMemory(&mapping->m_Mapping, sizeof(mapping->m_Mapping));
    ExReleaseRundownProtection(&m_Operations);
}
