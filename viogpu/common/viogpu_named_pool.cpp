#include <initguid.h>
#include <wdmguid.h>
#include "viogpu_named_pool.h"

#define VIOGPU_NAMED_POOL_TAG 'PNGV'

static const CHAR VIOGPU_DRM_HOST_POOL_NAME[] = "drm2kgsl_host";
static PVOID volatile g_VioGpuNamedPoolNotificationDriverObject = NULL;

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

typedef struct _VIOGPU_NAMED_POOL_CONNECTION
{
    PFILE_OBJECT FileObject;
    DROIDVMPOOL_DIRECT_INTERFACE DirectInterface;
    DROIDVMPOOL_QUERY_OUTPUT Query;
} VIOGPU_NAMED_POOL_CONNECTION, *PVIOGPU_NAMED_POOL_CONNECTION;

VOID VioGpuSetNamedPoolNotificationDriverObject(_In_ PDRIVER_OBJECT driverObject)
{
    NT_ASSERT(driverObject != NULL);
    PVOID previous = InterlockedCompareExchangePointer(&g_VioGpuNamedPoolNotificationDriverObject, driverObject, NULL);
    NT_ASSERT(previous == NULL || previous == driverObject);
    UNREFERENCED_PARAMETER(previous);
}

VOID VioGpuClearNamedPoolNotificationDriverObject(void)
{
    InterlockedExchangePointer(&g_VioGpuNamedPoolNotificationDriverObject, NULL);
}

static PDRIVER_OBJECT GetNamedPoolNotificationDriverObject(void)
{
    return reinterpret_cast<PDRIVER_OBJECT>(InterlockedCompareExchangePointer(&g_VioGpuNamedPoolNotificationDriverObject,
                                                                              NULL,
                                                                              NULL));
}

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
    irpStack->Parameters.QueryInterface.Version = DROIDVMPOOL_DIRECT_VERSION;
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
    if (information != sizeof(*query) || query->InterfaceVersion != DROIDVMPOOL_INTERFACE_VERSION ||
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
        directInterface->InterfaceHeader.Version != DROIDVMPOOL_DIRECT_VERSION ||
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

static void ReleaseNamedPoolConnection(_Inout_ PVIOGPU_NAMED_POOL_CONNECTION connection)
{
    DereferenceDirectInterface(&connection->DirectInterface);
    if (connection->FileObject != NULL)
    {
        ObDereferenceObject(connection->FileObject);
    }
    RtlZeroMemory(connection, sizeof(*connection));
}

static NTSTATUS DuplicateInterfaceName(_In_ const UNICODE_STRING *source, _Out_ PUNICODE_STRING destination)
{
    RtlZeroMemory(destination, sizeof(*destination));
    if (source == NULL || source->Buffer == NULL || source->Length == 0 || source->Length > MAXUSHORT - sizeof(WCHAR))
    {
        return STATUS_INVALID_PARAMETER;
    }

    USHORT maximumLength = (USHORT)(source->Length + sizeof(WCHAR));
    PWSTR buffer = (PWSTR)ExAllocatePoolUninitialized(PagedPool, maximumLength, VIOGPU_NAMED_POOL_TAG);
    if (buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(buffer, source->Buffer, source->Length);
    buffer[source->Length / sizeof(WCHAR)] = L'\0';
    destination->Buffer = buffer;
    destination->Length = source->Length;
    destination->MaximumLength = maximumLength;
    return STATUS_SUCCESS;
}

static void FreeInterfaceName(_Inout_ PUNICODE_STRING name)
{
    if (name->Buffer != NULL)
    {
        ExFreePoolWithTag(name->Buffer, VIOGPU_NAMED_POOL_TAG);
    }
    RtlZeroMemory(name, sizeof(*name));
}

static NTSTATUS OpenNamedPoolConnection(_In_ PUNICODE_STRING deviceName,
                                        _Out_ PVIOGPU_NAMED_POOL_CONNECTION connection,
                                        _Out_ BOOLEAN *isDrmHost)
{
    RtlZeroMemory(connection, sizeof(*connection));
    *isDrmHost = FALSE;

    PFILE_OBJECT fileObject = NULL;
    PDEVICE_OBJECT deviceObject = NULL;
    NTSTATUS status = IoGetDeviceObjectPointer(deviceName, FILE_ALL_ACCESS, &fileObject, &deviceObject);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    DROIDVMPOOL_QUERY_OUTPUT query = {};
    VIOGPU_NAMED_POOL_IOCTL_RESULT ioctlResult = NamedPoolQuery(deviceObject, fileObject, &query);
    status = ioctlResult.Status;
    if (NT_SUCCESS(status))
    {
        status = ValidateNamedPoolQuery(&query, ioctlResult.Information, isDrmHost);
    }
    if (!NT_SUCCESS(status) || !*isDrmHost)
    {
        ObDereferenceObject(fileObject);
        return status;
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
        KeEnterGuardedRegion();
        if (!directInterface.AcquireMapping(directInterface.InterfaceHeader.Context, &mapping))
        {
            status = STATUS_DEVICE_NOT_READY;
        }
        else
        {
            status = ValidateNamedPoolMapping(&mapping, &query);
            directInterface.ReleaseMapping(directInterface.InterfaceHeader.Context);
        }
        KeLeaveGuardedRegion();
    }
    if (!NT_SUCCESS(status))
    {
        if (directInterfaceReferenced)
        {
            DereferenceDirectInterface(&directInterface);
        }
        ObDereferenceObject(fileObject);
        return status;
    }

    connection->FileObject = fileObject;
    connection->DirectInterface = directInterface;
    connection->Query = query;
    return STATUS_SUCCESS;
}

VioGpuDrmHostPoolMapping::VioGpuDrmHostPoolMapping() : m_Owner(NULL), m_OwningThread(NULL), m_AcquireIrql(PASSIVE_LEVEL)
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

VioGpuDrmHostPool::VioGpuDrmHostPool()
    : m_Ready(FALSE), m_RundownCompleted(FALSE), m_NotificationRundownCompleted(FALSE), m_DisconnectInProgress(FALSE),
      m_ShuttingDown(FALSE), m_PnpState(VioGpuDrmHostPoolDisconnected), m_NotificationDriverObject(NULL),
      m_NotificationEntry(NULL), m_FileObject(NULL), m_Size(0)
{
    KeInitializeMutex(&m_StateLock, 0);
    ExInitializeRundownProtection(&m_Operations);
    ExInitializeRundownProtection(&m_NotificationCallbacks);
    RtlZeroMemory(&m_InterfaceName, sizeof(m_InterfaceName));
    RtlZeroMemory(&m_DirectInterface, sizeof(m_DirectInterface));
    m_BasePA.QuadPart = 0;
}

VioGpuDrmHostPool::~VioGpuDrmHostPool()
{
    PAGED_CODE();
    NTSTATUS status = Disconnect();
    NT_ASSERT(NT_SUCCESS(status));
    UNREFERENCED_PARAMETER(status);
    NT_ASSERT(!HasConnectionOwner());
}

void VioGpuDrmHostPool::LockState(void) const
{
    NTSTATUS status = KeWaitForSingleObject(&m_StateLock, Executive, KernelMode, FALSE, NULL);
    NT_ASSERT(NT_SUCCESS(status));
    UNREFERENCED_PARAMETER(status);
}

void VioGpuDrmHostPool::UnlockState(void) const
{
    KeReleaseMutex(&m_StateLock, FALSE);
}

BOOLEAN VioGpuDrmHostPool::HasConnectionOwner(void) const
{
    PAGED_CODE();

    LockState();
    BOOLEAN owned = m_PnpState != VioGpuDrmHostPoolDisconnected || m_NotificationEntry != NULL ||
                    m_NotificationDriverObject != NULL || m_InterfaceName.Buffer != NULL || m_FileObject != NULL ||
                    m_DirectInterface.InterfaceHeader.Context != NULL;
    UnlockState();
    return owned;
}

NTSTATUS VioGpuDrmHostPool::Connect(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    LockState();
    if (IsActive())
    {
        UnlockState();
        return STATUS_SUCCESS;
    }
    if (m_PnpState != VioGpuDrmHostPoolDisconnected || m_NotificationDriverObject != NULL ||
        m_NotificationEntry != NULL || m_InterfaceName.Buffer != NULL || m_FileObject != NULL ||
        m_DirectInterface.InterfaceHeader.Context != NULL || m_Size != 0)
    {
        UnlockState();
        return STATUS_DEVICE_NOT_READY;
    }

    PDRIVER_OBJECT notificationDriverObject = GetNamedPoolNotificationDriverObject();
    if (notificationDriverObject == NULL)
    {
        UnlockState();
        return STATUS_DEVICE_NOT_READY;
    }
    if (m_RundownCompleted)
    {
        ExReInitializeRundownProtection(&m_Operations);
        m_RundownCompleted = FALSE;
    }
    if (m_NotificationRundownCompleted)
    {
        ExReInitializeRundownProtection(&m_NotificationCallbacks);
        m_NotificationRundownCompleted = FALSE;
    }
    m_ShuttingDown = FALSE;

    PWSTR interfaceList = NULL;
    NTSTATUS status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_DROIDVMPOOL, NULL, 0, &interfaceList);
    if (!NT_SUCCESS(status))
    {
        UnlockState();
        return status;
    }
    if (interfaceList == NULL || *interfaceList == L'\0')
    {
        if (interfaceList != NULL)
        {
            ExFreePool(interfaceList);
        }
        UnlockState();
        return STATUS_NOT_FOUND;
    }

    NTSTATUS firstFailure = STATUS_SUCCESS;
    ULONG matchCount = 0;
    VIOGPU_NAMED_POOL_CONNECTION selectedConnection = {};
    UNICODE_STRING selectedName = {};

    UNICODE_STRING deviceName;
    for (PWSTR interfaceName = interfaceList; *interfaceName != L'\0';
         interfaceName += (deviceName.Length / sizeof(WCHAR)) + 1)
    {
        RtlInitUnicodeString(&deviceName, interfaceName);
        VIOGPU_NAMED_POOL_CONNECTION connection = {};
        BOOLEAN isDrmHost = FALSE;
        status = OpenNamedPoolConnection(&deviceName, &connection, &isDrmHost);
        if (!NT_SUCCESS(status))
        {
            if (NT_SUCCESS(firstFailure))
            {
                firstFailure = status;
            }
            continue;
        }
        if (!isDrmHost)
        {
            continue;
        }

        ++matchCount;
        if (matchCount == 1)
        {
            status = DuplicateInterfaceName(&deviceName, &selectedName);
            if (!NT_SUCCESS(status))
            {
                firstFailure = status;
                ReleaseNamedPoolConnection(&connection);
                continue;
            }
            selectedConnection = connection;
        }
        else
        {
            ReleaseNamedPoolConnection(&connection);
        }
    }
    ExFreePool(interfaceList);

    if (!NT_SUCCESS(firstFailure) || matchCount != 1)
    {
        ReleaseNamedPoolConnection(&selectedConnection);
        FreeInterfaceName(&selectedName);
        status = !NT_SUCCESS(firstFailure) ? firstFailure
                                           : (matchCount == 0 ? STATUS_NOT_FOUND : STATUS_DEVICE_CONFIGURATION_ERROR);
        UnlockState();
        return status;
    }

    PVOID notificationEntry = NULL;
    status = IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange,
                                            0,
                                            selectedConnection.FileObject,
                                            notificationDriverObject,
                                            PnpNotificationCallback,
                                            this,
                                            &notificationEntry);
    if (!NT_SUCCESS(status))
    {
        ReleaseNamedPoolConnection(&selectedConnection);
        FreeInterfaceName(&selectedName);
        UnlockState();
        return status;
    }

    m_NotificationDriverObject = notificationDriverObject;
    m_NotificationEntry = notificationEntry;
    m_InterfaceName = selectedName;
    m_FileObject = selectedConnection.FileObject;
    m_DirectInterface = selectedConnection.DirectInterface;
    m_BasePA = selectedConnection.Query.BasePhysicalAddress;
    m_Size = (SIZE_T)selectedConnection.Query.TotalSize;
    m_PnpState = VioGpuDrmHostPoolConnected;
    InterlockedExchange(&m_Ready, TRUE);
    RtlZeroMemory(&selectedName, sizeof(selectedName));
    RtlZeroMemory(&selectedConnection, sizeof(selectedConnection));

    PHYSICAL_ADDRESS basePA = m_BasePA;
    SIZE_T size = m_Size;
    UnlockState();

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu droidvmpool: drm2kgsl_host PA=0x%llx size=0x%Ix\n",
               basePA.QuadPart,
               size);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDrmHostPool::Disconnect(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    VIOGPU_NAMED_POOL_CONNECTION connection = {};
    PVOID notificationEntry = NULL;

    LockState();
    if (m_DisconnectInProgress)
    {
        UnlockState();
        return STATUS_DEVICE_BUSY;
    }
    m_DisconnectInProgress = TRUE;
    m_ShuttingDown = TRUE;
    InterlockedExchange(&m_Ready, FALSE);
    if (!m_RundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_Operations);
        m_RundownCompleted = TRUE;
    }
    connection.FileObject = m_FileObject;
    connection.DirectInterface = m_DirectInterface;
    m_FileObject = NULL;
    RtlZeroMemory(&m_DirectInterface, sizeof(m_DirectInterface));
    notificationEntry = m_NotificationEntry;
    m_NotificationEntry = NULL;
    m_PnpState = VioGpuDrmHostPoolFailed;
    UnlockState();

    NTSTATUS status = STATUS_SUCCESS;
    if (notificationEntry != NULL)
    {
        status = IoUnregisterPlugPlayNotificationEx(notificationEntry);
    }
    if (!NT_SUCCESS(status))
    {
        LockState();
        if (m_NotificationEntry == NULL)
        {
            m_NotificationEntry = notificationEntry;
        }
        NT_ASSERT(m_FileObject == NULL && m_DirectInterface.InterfaceHeader.Context == NULL);
        m_FileObject = connection.FileObject;
        m_DirectInterface = connection.DirectInterface;
        RtlZeroMemory(&connection, sizeof(connection));
        m_DisconnectInProgress = FALSE;
        UnlockState();
        return status;
    }

    if (!m_NotificationRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);
        m_NotificationRundownCompleted = TRUE;
    }

    LockState();
    notificationEntry = m_NotificationEntry;
    m_NotificationEntry = NULL;
    if (connection.FileObject == NULL && connection.DirectInterface.InterfaceHeader.Context == NULL)
    {
        connection.FileObject = m_FileObject;
        connection.DirectInterface = m_DirectInterface;
        m_FileObject = NULL;
        RtlZeroMemory(&m_DirectInterface, sizeof(m_DirectInterface));
    }
    else
    {
        NT_ASSERT(m_FileObject == NULL && m_DirectInterface.InterfaceHeader.Context == NULL);
    }
    UnlockState();
    if (notificationEntry != NULL)
    {
        status = IoUnregisterPlugPlayNotificationEx(notificationEntry);
        if (!NT_SUCCESS(status))
        {
            LockState();
            m_NotificationEntry = notificationEntry;
            NT_ASSERT(m_FileObject == NULL && m_DirectInterface.InterfaceHeader.Context == NULL);
            m_FileObject = connection.FileObject;
            m_DirectInterface = connection.DirectInterface;
            RtlZeroMemory(&connection, sizeof(connection));
            m_DisconnectInProgress = FALSE;
            UnlockState();
            return status;
        }
    }

    ReleaseNamedPoolConnection(&connection);
    LockState();
    m_BasePA.QuadPart = 0;
    m_Size = 0;
    FreeInterfaceName(&m_InterfaceName);
    m_NotificationDriverObject = NULL;
    m_PnpState = VioGpuDrmHostPoolDisconnected;
    m_ShuttingDown = FALSE;
    m_DisconnectInProgress = FALSE;
    UnlockState();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS VioGpuDrmHostPool::PnpNotificationCallback(PVOID notificationStructure, PVOID context)
{
    VioGpuDrmHostPool *pool = reinterpret_cast<VioGpuDrmHostPool *>(context);
    if (pool == NULL || !ExAcquireRundownProtection(&pool->m_NotificationCallbacks))
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS status = pool->HandlePnpNotification(notificationStructure);
    ExReleaseRundownProtection(&pool->m_NotificationCallbacks);
    return status;
}

NTSTATUS VioGpuDrmHostPool::HandlePnpNotification(_In_ PVOID notificationStructure)
{
    PAGED_CODE();

    if (notificationStructure == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    PPLUGPLAY_NOTIFICATION_HEADER header = (PPLUGPLAY_NOTIFICATION_HEADER)notificationStructure;
    if (IsEqualGUID(header->Event, GUID_TARGET_DEVICE_QUERY_REMOVE))
    {
        if (header->Size < sizeof(TARGET_DEVICE_REMOVAL_NOTIFICATION))
        {
            return STATUS_INVALID_PARAMETER;
        }
        PTARGET_DEVICE_REMOVAL_NOTIFICATION removal = (PTARGET_DEVICE_REMOVAL_NOTIFICATION)notificationStructure;
        return HandleQueryRemove(removal->FileObject);
    }
    if (IsEqualGUID(header->Event, GUID_TARGET_DEVICE_REMOVE_CANCELLED))
    {
        return HandleRemoveCancelled();
    }
    if (IsEqualGUID(header->Event, GUID_TARGET_DEVICE_REMOVE_COMPLETE))
    {
        return HandleRemoveComplete();
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDrmHostPool::HandleQueryRemove(_In_ PFILE_OBJECT notificationFileObject)
{
    PAGED_CODE();

    VIOGPU_NAMED_POOL_CONNECTION connection = {};
    LockState();
    if (m_ShuttingDown || m_PnpState == VioGpuDrmHostPoolQueryRemove)
    {
        UnlockState();
        return STATUS_SUCCESS;
    }
    if (m_PnpState != VioGpuDrmHostPoolConnected || notificationFileObject == NULL ||
        notificationFileObject != m_FileObject)
    {
        UnlockState();
        return STATUS_DEVICE_NOT_READY;
    }

    InterlockedExchange(&m_Ready, FALSE);
    if (!m_RundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_Operations);
        m_RundownCompleted = TRUE;
    }
    connection.FileObject = m_FileObject;
    connection.DirectInterface = m_DirectInterface;
    m_FileObject = NULL;
    RtlZeroMemory(&m_DirectInterface, sizeof(m_DirectInterface));
    m_PnpState = VioGpuDrmHostPoolQueryRemove;
    UnlockState();

    ReleaseNamedPoolConnection(&connection);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDrmHostPool::HandleRemoveCancelled(void)
{
    PAGED_CODE();

    PVOID oldNotificationEntry = NULL;
    LockState();
    if (m_ShuttingDown)
    {
        UnlockState();
        return STATUS_SUCCESS;
    }
    if (m_PnpState != VioGpuDrmHostPoolQueryRemove || m_NotificationEntry == NULL ||
        m_NotificationDriverObject == NULL || m_InterfaceName.Buffer == NULL)
    {
        InterlockedExchange(&m_Ready, FALSE);
        m_PnpState = VioGpuDrmHostPoolFailed;
        UnlockState();
        return STATUS_SUCCESS;
    }
    oldNotificationEntry = m_NotificationEntry;
    m_NotificationEntry = NULL;
    m_PnpState = VioGpuDrmHostPoolReconnecting;
    UnlockState();

    NTSTATUS status = IoUnregisterPlugPlayNotificationEx(oldNotificationEntry);
    if (!NT_SUCCESS(status))
    {
        LockState();
        NT_ASSERT(m_NotificationEntry == NULL || m_NotificationEntry == oldNotificationEntry);
        if (m_NotificationEntry == NULL)
        {
            m_NotificationEntry = oldNotificationEntry;
        }
        m_PnpState = VioGpuDrmHostPoolFailed;
        UnlockState();
        return STATUS_SUCCESS;
    }

    VIOGPU_NAMED_POOL_CONNECTION connection = {};
    BOOLEAN isDrmHost = FALSE;
    status = OpenNamedPoolConnection(&m_InterfaceName, &connection, &isDrmHost);
    if (NT_SUCCESS(status) && !isDrmHost)
    {
        status = STATUS_OBJECT_NAME_NOT_FOUND;
    }

    PVOID newNotificationEntry = NULL;
    BOOLEAN published = FALSE;
    LockState();
    if (NT_SUCCESS(status) && (connection.Query.BasePhysicalAddress.QuadPart != m_BasePA.QuadPart ||
                               (SIZE_T)connection.Query.TotalSize != m_Size))
    {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (NT_SUCCESS(status) && (m_ShuttingDown || m_PnpState != VioGpuDrmHostPoolReconnecting))
    {
        status = STATUS_DELETE_PENDING;
    }
    if (NT_SUCCESS(status))
    {
        status = IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange,
                                                0,
                                                connection.FileObject,
                                                m_NotificationDriverObject,
                                                PnpNotificationCallback,
                                                this,
                                                &newNotificationEntry);
        if (NT_SUCCESS(status))
        {
            if (m_RundownCompleted)
            {
                ExReInitializeRundownProtection(&m_Operations);
                m_RundownCompleted = FALSE;
            }
            m_NotificationEntry = newNotificationEntry;
            m_FileObject = connection.FileObject;
            m_DirectInterface = connection.DirectInterface;
            m_BasePA = connection.Query.BasePhysicalAddress;
            m_Size = (SIZE_T)connection.Query.TotalSize;
            m_PnpState = VioGpuDrmHostPoolConnected;
            InterlockedExchange(&m_Ready, TRUE);
            RtlZeroMemory(&connection, sizeof(connection));
            published = TRUE;
        }
    }
    if (!published && !m_ShuttingDown)
    {
        m_PnpState = VioGpuDrmHostPoolFailed;
    }
    UnlockState();

    ReleaseNamedPoolConnection(&connection);
    if (!published)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu droidvmpool: remove-cancelled reconnect failed, status=0x%08X\n",
                   status);
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDrmHostPool::HandleRemoveComplete(void)
{
    PAGED_CODE();

    VIOGPU_NAMED_POOL_CONNECTION connection = {};
    PVOID notificationEntry = NULL;

    LockState();
    if (m_ShuttingDown)
    {
        UnlockState();
        return STATUS_SUCCESS;
    }

    InterlockedExchange(&m_Ready, FALSE);
    if (!m_RundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_Operations);
        m_RundownCompleted = TRUE;
    }
    connection.FileObject = m_FileObject;
    connection.DirectInterface = m_DirectInterface;
    m_FileObject = NULL;
    RtlZeroMemory(&m_DirectInterface, sizeof(m_DirectInterface));
    notificationEntry = m_NotificationEntry;
    m_NotificationEntry = NULL;
    m_PnpState = VioGpuDrmHostPoolRemoved;
    UnlockState();

    NTSTATUS status = STATUS_SUCCESS;
    if (notificationEntry != NULL)
    {
        status = IoUnregisterPlugPlayNotificationEx(notificationEntry);
    }
    if (!NT_SUCCESS(status))
    {
        LockState();
        NT_ASSERT(m_NotificationEntry == NULL || m_NotificationEntry == notificationEntry);
        if (m_NotificationEntry == NULL)
        {
            m_NotificationEntry = notificationEntry;
        }
        NT_ASSERT(m_FileObject == NULL && m_DirectInterface.InterfaceHeader.Context == NULL);
        m_FileObject = connection.FileObject;
        m_DirectInterface = connection.DirectInterface;
        RtlZeroMemory(&connection, sizeof(connection));
        m_PnpState = VioGpuDrmHostPoolFailed;
        UnlockState();
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu droidvmpool: remove-complete unregister failed, status=0x%08X\n",
                   status);
        return STATUS_SUCCESS;
    }

    ReleaseNamedPoolConnection(&connection);
    LockState();
    m_BasePA.QuadPart = 0;
    m_Size = 0;
    UnlockState();
    return STATUS_SUCCESS;
}

BOOLEAN VioGpuDrmHostPool::AcquireMapping(_Out_ VioGpuDrmHostPoolMapping *mapping) const
{
    KIRQL currentIrql = KeGetCurrentIrql();
    if (mapping == NULL || mapping->m_Owner != NULL || mapping->m_OwningThread != NULL ||
        currentIrql > DISPATCH_LEVEL || !KeAreAllApcsDisabled())
    {
        return FALSE;
    }

    if (!ExAcquireRundownProtection(&m_Operations))
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
    mapping->m_OwningThread = KeGetCurrentThread();
    mapping->m_AcquireIrql = currentIrql;
    mapping->m_Owner = this;
    return TRUE;
}

void VioGpuDrmHostPool::ReleaseMapping(_Inout_ VioGpuDrmHostPoolMapping *mapping) const
{
    NT_ASSERT(mapping != NULL && mapping->m_Owner == this);
    NT_ASSERT(mapping->m_OwningThread == KeGetCurrentThread());
    NT_ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    NT_ASSERT(KeAreAllApcsDisabled());
    NT_ASSERT(mapping->m_AcquireIrql != DISPATCH_LEVEL || KeGetCurrentIrql() == DISPATCH_LEVEL);

    m_DirectInterface.ReleaseMapping(m_DirectInterface.InterfaceHeader.Context);
    mapping->m_Owner = NULL;
    mapping->m_OwningThread = NULL;
    mapping->m_AcquireIrql = PASSIVE_LEVEL;
    RtlZeroMemory(&mapping->m_Mapping, sizeof(mapping->m_Mapping));
    ExReleaseRundownProtection(&m_Operations);
}
