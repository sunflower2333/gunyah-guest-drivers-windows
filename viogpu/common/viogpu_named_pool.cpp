#include <initguid.h>
#include <wdmguid.h>
#include "viogpu_named_pool.h"

#define VIOGPU_NAMED_POOL_TAG 'PNGV'

static const CHAR VIOGPU_DRM_HOST_POOL_NAME[] = "drm2kgsl_host";
static const CHAR VIOGPU_GUEST_POOL_NAME[] = "gpu_guest";
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
                                       _In_reads_(expectedNameLength) const CHAR *expectedName,
                                       _In_ ULONG expectedNameLength,
                                       _Out_ BOOLEAN *isMatch)
{
    *isMatch = FALSE;
    if (expectedName == NULL || expectedNameLength == 0 || expectedNameLength >= DROIDVMPOOL_NAME_CAPACITY ||
        expectedName[expectedNameLength] != '\0' || information != sizeof(*query) ||
        query->InterfaceVersion != DROIDVMPOOL_INTERFACE_VERSION || query->StructureSize != sizeof(*query) ||
        query->PoolNameLength == 0 || query->PoolNameLength >= DROIDVMPOOL_NAME_CAPACITY ||
        query->PoolName[query->PoolNameLength] != '\0' || query->PageSize != PAGE_SIZE ||
        query->BasePhysicalAddress.QuadPart < 0 ||
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

    *isMatch = query->PoolNameLength == expectedNameLength &&
               RtlCompareMemory(query->PoolName, expectedName, expectedNameLength) == expectedNameLength;
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
                                        _In_reads_(expectedNameLength) const CHAR *expectedName,
                                        _In_ ULONG expectedNameLength,
                                        _Out_ PVIOGPU_NAMED_POOL_CONNECTION connection,
                                        _Out_ BOOLEAN *isMatch)
{
    RtlZeroMemory(connection, sizeof(*connection));
    *isMatch = FALSE;

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
        status = ValidateNamedPoolQuery(&query, ioctlResult.Information, expectedName, expectedNameLength, isMatch);
    }
    if (!NT_SUCCESS(status) || !*isMatch)
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

VioGpuNamedPoolMapping::VioGpuNamedPoolMapping()
    : m_Owner(NULL), m_OwningThread(NULL), m_AcquireIrql(PASSIVE_LEVEL), m_Generation(0)
{
    RtlZeroMemory(&m_Mapping, sizeof(m_Mapping));
}

VioGpuNamedPoolMapping::~VioGpuNamedPoolMapping()
{
    Release();
}

void VioGpuNamedPoolMapping::Release(void)
{
    if (m_Owner != NULL)
    {
        m_Owner->ReleaseMapping(this);
    }
    NT_ASSERT(m_Owner == NULL);
}

VioGpuNamedPool::VioGpuNamedPool(_In_reads_(expectedNameLength) const CHAR *expectedName,
                                 _In_ ULONG expectedNameLength,
                                 _In_ VIOGPU_NAMED_POOL_FAILURE_CALLBACK failureCallback,
                                 _In_opt_ PVOID failureContext)
    : m_Ready(FALSE), m_Generation(0), m_PnpCleanupQueued(FALSE), m_PnpCleanupGeneration(0),
      m_PnpCleanupStatus(STATUS_SUCCESS), m_PnpCleanupWorkerState(VioGpuNamedPoolWorkerIdle), m_RundownCompleted(FALSE),
      m_NotificationRundownCompleted(FALSE), m_DisconnectInProgress(FALSE), m_ShuttingDown(FALSE),
      m_RemovalLatched(FALSE), m_PnpState(VioGpuNamedPoolDisconnected), m_ExpectedName(expectedName),
      m_ExpectedNameLength(expectedNameLength), m_FailureCallback(failureCallback), m_FailureContext(failureContext),
      m_NotificationDriverObject(NULL), m_NotificationEntry(NULL), m_FileObject(NULL), m_Size(0)
{
    NT_ASSERT(expectedName != NULL && expectedNameLength != 0 && expectedNameLength < DROIDVMPOOL_NAME_CAPACITY &&
              expectedName[expectedNameLength] == '\0');
    NT_ASSERT(failureCallback != NULL);
    KeInitializeMutex(&m_StateLock, 0);
    KeInitializeSpinLock(&m_PnpCleanupLock);
    ExInitializeRundownProtection(&m_Operations);
    ExInitializeRundownProtection(&m_NotificationCallbacks);
    ExInitializeRundownProtection(&m_PnpCleanupWorkerReferences);
    ExInitializeWorkItem(&m_PnpCleanupWorkItem, PnpCleanupWorker, this);
    KeInitializeEvent(&m_PnpCleanupComplete, NotificationEvent, TRUE);
    RtlZeroMemory(&m_InterfaceName, sizeof(m_InterfaceName));
    RtlZeroMemory(&m_DirectInterface, sizeof(m_DirectInterface));
    m_BasePA.QuadPart = 0;
}

VioGpuNamedPool::~VioGpuNamedPool()
{
    PAGED_CODE();
    NTSTATUS status = Disconnect();
    NT_ASSERT(NT_SUCCESS(status));
    UNREFERENCED_PARAMETER(status);
    NT_ASSERT(!HasConnectionOwner());
}

VioGpuDrmHostPool::VioGpuDrmHostPool(_In_ VIOGPU_NAMED_POOL_FAILURE_CALLBACK failureCallback,
                                     _In_opt_ PVOID failureContext)
    : VioGpuNamedPool(VIOGPU_DRM_HOST_POOL_NAME,
                      static_cast<ULONG>(RTL_NUMBER_OF(VIOGPU_DRM_HOST_POOL_NAME) - 1),
                      failureCallback,
                      failureContext)
{
}

VioGpuGuestPool::VioGpuGuestPool(_In_ VIOGPU_NAMED_POOL_FAILURE_CALLBACK failureCallback, _In_opt_ PVOID failureContext)
    : VioGpuNamedPool(VIOGPU_GUEST_POOL_NAME,
                      static_cast<ULONG>(RTL_NUMBER_OF(VIOGPU_GUEST_POOL_NAME) - 1),
                      failureCallback,
                      failureContext)
{
}

void VioGpuNamedPool::LockState(void) const
{
    NTSTATUS status = KeWaitForSingleObject(&m_StateLock, Executive, KernelMode, FALSE, NULL);
    NT_ASSERT(NT_SUCCESS(status));
    UNREFERENCED_PARAMETER(status);
}

void VioGpuNamedPool::UnlockState(void) const
{
    KeReleaseMutex(&m_StateLock, FALSE);
}

BOOLEAN VioGpuNamedPool::HasConnectionOwner(void) const
{
    PAGED_CODE();

    LockState();
    LONG pnpState = InterlockedCompareExchange(&m_PnpState, 0, 0);
    PFILE_OBJECT fileObject = reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOID volatile *)&m_FileObject,
                                                                                               NULL,
                                                                                               NULL));
    BOOLEAN owned = InterlockedCompareExchange(&m_PnpCleanupQueued, FALSE, FALSE) != FALSE ||
                    KeReadStateEvent(&m_PnpCleanupComplete) == 0 ||
                    InterlockedCompareExchange(&m_RemovalLatched, FALSE, FALSE) != FALSE ||
                    pnpState != VioGpuNamedPoolDisconnected || m_NotificationEntry != NULL ||
                    m_NotificationDriverObject != NULL || m_InterfaceName.Buffer != NULL || fileObject != NULL ||
                    m_DirectInterface.InterfaceHeader.Context != NULL;
    UnlockState();
    return owned;
}

NTSTATUS VioGpuNamedPool::Connect(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    NTSTATUS cleanupWait = WaitForPnpCleanupIdle();
    if (!NT_SUCCESS(cleanupWait))
    {
        return cleanupWait;
    }

    LockState();
    if (IsActive())
    {
        UnlockState();
        return STATUS_SUCCESS;
    }
    LONG pnpState = InterlockedCompareExchange(&m_PnpState, 0, 0);
    PFILE_OBJECT fileObject = reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOID volatile *)&m_FileObject,
                                                                                               NULL,
                                                                                               NULL));
    if (InterlockedCompareExchange(&m_PnpCleanupQueued, FALSE, FALSE) != FALSE ||
        InterlockedCompareExchange(&m_RemovalLatched, FALSE, FALSE) != FALSE ||
        pnpState != VioGpuNamedPoolDisconnected || m_NotificationDriverObject != NULL || m_NotificationEntry != NULL ||
        m_InterfaceName.Buffer != NULL || fileObject != NULL || m_DirectInterface.InterfaceHeader.Context != NULL ||
        m_Size != 0)
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
    InterlockedExchange(&m_ShuttingDown, FALSE);
    InterlockedExchange(&m_RemovalLatched, FALSE);
    InterlockedExchange(&m_PnpCleanupStatus, STATUS_SUCCESS);

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
        BOOLEAN isMatch = FALSE;
        status = OpenNamedPoolConnection(&deviceName, m_ExpectedName, m_ExpectedNameLength, &connection, &isMatch);
        if (!NT_SUCCESS(status))
        {
            if (NT_SUCCESS(firstFailure))
            {
                firstFailure = status;
            }
            continue;
        }
        if (!isMatch)
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

    /*
     * Publish the target identity and a callback-visible Connecting state before
     * registration.  The registration API may deliver a target-device event
     * before it returns, so the callback must be able to veto/remove this
     * connection without relying on the state mutex or a fully published owner.
     */
    m_BasePA = selectedConnection.Query.BasePhysicalAddress;
    m_Size = (SIZE_T)selectedConnection.Query.TotalSize;
    LONG64 registrationGeneration = InterlockedIncrement64(&m_Generation);
    InterlockedExchange(&m_RemovalLatched, FALSE);
    InterlockedExchange(&m_Ready, FALSE);
    InterlockedExchange(&m_PnpState, VioGpuNamedPoolConnecting);
    InterlockedExchangePointer((PVOID volatile *)&m_FileObject, selectedConnection.FileObject);

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
        InterlockedExchangePointer((PVOID volatile *)&m_FileObject, NULL);
        m_BasePA.QuadPart = 0;
        m_Size = 0;
        InterlockedExchange(&m_Ready, FALSE);
        InterlockedExchange(&m_PnpState, VioGpuNamedPoolFailed);
        InterlockedExchange(&m_ShuttingDown, TRUE);

        /*
         * A failed registration may still have a callback in flight.  Join
         * that callback before dropping the provisional target references or
         * allowing a concurrent Disconnect()/Connect() to touch the same
         * rundown object.  Keep m_StateLock held for the entire provisional
         * ownership handoff; the callback path never takes this mutex.
         */
        ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);
        ExRundownCompleted(&m_NotificationCallbacks);
        ReleaseNamedPoolConnection(&selectedConnection);
        FreeInterfaceName(&selectedName);

        m_NotificationRundownCompleted = TRUE;
        InterlockedExchange(&m_PnpState, VioGpuNamedPoolDisconnected);
        InterlockedExchange(&m_RemovalLatched, FALSE);
        InterlockedExchange(&m_ShuttingDown, FALSE);
        UnlockState();
        return status;
    }

    m_NotificationDriverObject = notificationDriverObject;
    m_NotificationEntry = notificationEntry;
    m_InterfaceName = selectedName;
    /* m_FileObject was published before registration and remains owned here. */
    m_DirectInterface = selectedConnection.DirectInterface;
    m_BasePA = selectedConnection.Query.BasePhysicalAddress;
    m_Size = (SIZE_T)selectedConnection.Query.TotalSize;
    RtlZeroMemory(&selectedName, sizeof(selectedName));
    RtlZeroMemory(&selectedConnection, sizeof(selectedConnection));

    LONG previousState = InterlockedCompareExchange(&m_PnpState, VioGpuNamedPoolConnected, VioGpuNamedPoolConnecting);
    BOOLEAN removalLatched = InterlockedCompareExchange(&m_RemovalLatched, FALSE, FALSE) != FALSE;
    LONG64 publishedGeneration = InterlockedCompareExchange64(&m_Generation, 0, 0);
    BOOLEAN commitValid = previousState == VioGpuNamedPoolConnecting && !removalLatched &&
                          publishedGeneration == registrationGeneration;
    if (commitValid)
    {
        InterlockedExchange(&m_Ready, TRUE);
        commitValid = InterlockedCompareExchange(&m_PnpState, 0, 0) == VioGpuNamedPoolConnected &&
                      InterlockedCompareExchange(&m_RemovalLatched, FALSE, FALSE) == FALSE &&
                      InterlockedCompareExchange64(&m_Generation, 0, 0) == registrationGeneration;
    }
    if (!commitValid)
    {
        InterlockedExchange(&m_Ready, FALSE);
        if (InterlockedCompareExchange(&m_PnpState, 0, 0) == VioGpuNamedPoolConnecting)
        {
            InterlockedCompareExchange(&m_PnpState, VioGpuNamedPoolFailed, VioGpuNamedPoolConnecting);
        }
        UnlockState();
        QueuePnpCleanup();
        /* The queued worker owns provisional teardown after the callback race. */
        return STATUS_DEVICE_NOT_READY;
    }

    PHYSICAL_ADDRESS basePA = m_BasePA;
    SIZE_T size = m_Size;
    UnlockState();

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu droidvmpool: %s PA=0x%llx size=0x%Ix\n",
               m_ExpectedName,
               basePA.QuadPart,
               size);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuNamedPool::DisconnectInternal(void)
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
    InterlockedExchange(&m_ShuttingDown, TRUE);
    InterlockedIncrement64(&m_Generation);
    InterlockedExchange(&m_Ready, FALSE);
    if (!m_RundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_Operations);
        ExRundownCompleted(&m_Operations);
        m_RundownCompleted = TRUE;
    }
    connection.FileObject = reinterpret_cast<PFILE_OBJECT>(InterlockedExchangePointer((PVOID volatile *)&m_FileObject,
                                                                                      NULL));
    connection.DirectInterface = m_DirectInterface;
    RtlZeroMemory(&m_DirectInterface, sizeof(m_DirectInterface));
    notificationEntry = m_NotificationEntry;
    m_NotificationEntry = NULL;
    InterlockedExchange(&m_PnpState, VioGpuNamedPoolFailed);
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
        NT_ASSERT(InterlockedCompareExchangePointer((PVOID volatile *)&m_FileObject, NULL, NULL) == NULL &&
                  m_DirectInterface.InterfaceHeader.Context == NULL);
        InterlockedExchangePointer((PVOID volatile *)&m_FileObject, connection.FileObject);
        m_DirectInterface = connection.DirectInterface;
        RtlZeroMemory(&connection, sizeof(connection));
        m_DisconnectInProgress = FALSE;
        UnlockState();
        return status;
    }

    if (!m_NotificationRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);
        ExRundownCompleted(&m_NotificationCallbacks);
        m_NotificationRundownCompleted = TRUE;
    }

    ReleaseNamedPoolConnection(&connection);
    LockState();
    m_BasePA.QuadPart = 0;
    m_Size = 0;
    FreeInterfaceName(&m_InterfaceName);
    m_NotificationDriverObject = NULL;
    InterlockedExchange(&m_PnpState, VioGpuNamedPoolDisconnected);
    InterlockedExchange(&m_RemovalLatched, FALSE);
    InterlockedExchange(&m_ShuttingDown, FALSE);
    m_DisconnectInProgress = FALSE;
    UnlockState();
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuNamedPool::WaitForPnpCleanupIdle(void)
{
    PAGED_CODE();

    for (;;)
    {
        NTSTATUS waitStatus = KeWaitForSingleObject(&m_PnpCleanupComplete, Executive, KernelMode, FALSE, NULL);
        if (!NT_SUCCESS(waitStatus))
        {
            return waitStatus;
        }

        BOOLEAN finalizeWorker = FALSE;
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_PnpCleanupLock, &oldIrql);
        LONG cleanupState = InterlockedCompareExchange(&m_PnpCleanupQueued, 0, 0);
        LONG workerState = InterlockedCompareExchange(&m_PnpCleanupWorkerState, 0, 0);
        if (cleanupState == VioGpuNamedPoolCleanupIdle && workerState == VioGpuNamedPoolWorkerIdle)
        {
            KeReleaseSpinLock(&m_PnpCleanupLock, oldIrql);
            return STATUS_SUCCESS;
        }
        if (cleanupState == VioGpuNamedPoolCleanupTeardown && workerState == VioGpuNamedPoolWorkerRunning)
        {
            /* Keep Teardown published until the work-item routine has released its rundown reference. */
            InterlockedExchange(&m_PnpCleanupWorkerState, VioGpuNamedPoolWorkerFinalizing);
            KeClearEvent(&m_PnpCleanupComplete);
            finalizeWorker = TRUE;
        }
        KeReleaseSpinLock(&m_PnpCleanupLock, oldIrql);

        if (finalizeWorker)
        {
            ExWaitForRundownProtectionRelease(&m_PnpCleanupWorkerReferences);
            ExRundownCompleted(&m_PnpCleanupWorkerReferences);
            /* Idle may be published only after PASSIVE_LEVEL reinitialization makes the next acquire legal. */
            ExReInitializeRundownProtection(&m_PnpCleanupWorkerReferences);

            KeAcquireSpinLock(&m_PnpCleanupLock, &oldIrql);
            NT_ASSERT(InterlockedCompareExchange(&m_PnpCleanupWorkerState, 0, 0) == VioGpuNamedPoolWorkerFinalizing);
            InterlockedExchange(&m_PnpCleanupWorkerState, VioGpuNamedPoolWorkerIdle);
            InterlockedExchange(&m_PnpCleanupQueued, VioGpuNamedPoolCleanupIdle);
            KeSetEvent(&m_PnpCleanupComplete, IO_NO_INCREMENT, FALSE);
            KeReleaseSpinLock(&m_PnpCleanupLock, oldIrql);
        }
    }
}

NTSTATUS VioGpuNamedPool::BeginPnpCleanupTeardown(void)
{
    PAGED_CODE();

    for (;;)
    {
        NTSTATUS waitStatus = WaitForPnpCleanupIdle();
        if (!NT_SUCCESS(waitStatus))
        {
            return waitStatus;
        }

        KIRQL oldIrql;
        KeAcquireSpinLock(&m_PnpCleanupLock, &oldIrql);
        LONG cleanupState = InterlockedCompareExchange(&m_PnpCleanupQueued, 0, 0);
        if (cleanupState == VioGpuNamedPoolCleanupIdle &&
            InterlockedCompareExchange(&m_PnpCleanupWorkerState, 0, 0) == VioGpuNamedPoolWorkerIdle)
        {
            InterlockedExchange(&m_PnpCleanupQueued, VioGpuNamedPoolCleanupTeardown);
            KeClearEvent(&m_PnpCleanupComplete);
            KeReleaseSpinLock(&m_PnpCleanupLock, oldIrql);
            return STATUS_SUCCESS;
        }
        KeReleaseSpinLock(&m_PnpCleanupLock, oldIrql);
    }
}

BOOLEAN VioGpuNamedPool::ClaimQueuedPnpCleanup(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PnpCleanupLock, &oldIrql);
    LONG cleanupState = InterlockedCompareExchange(&m_PnpCleanupQueued, 0, 0);
    BOOLEAN claimed = cleanupState == VioGpuNamedPoolCleanupQueued;
    if (claimed)
    {
        InterlockedExchange(&m_PnpCleanupQueued, VioGpuNamedPoolCleanupTeardown);
        KeClearEvent(&m_PnpCleanupComplete);
    }
    KeReleaseSpinLock(&m_PnpCleanupLock, oldIrql);
    return claimed;
}

void VioGpuNamedPool::CompletePnpCleanupTeardown(_In_ NTSTATUS status, _In_ BOOLEAN worker)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PnpCleanupLock, &oldIrql);
    InterlockedExchange(&m_PnpCleanupStatus, status);
    if (!worker)
    {
        NT_ASSERT(InterlockedCompareExchange(&m_PnpCleanupWorkerState, 0, 0) == VioGpuNamedPoolWorkerIdle);
        InterlockedExchange(&m_PnpCleanupQueued, VioGpuNamedPoolCleanupIdle);
    }
    /* A worker leaves Teardown published until a waiter joins its last-object-access barrier. */
    KeSetEvent(&m_PnpCleanupComplete, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&m_PnpCleanupLock, oldIrql);
}

NTSTATUS VioGpuNamedPool::Disconnect(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    NTSTATUS waitStatus = BeginPnpCleanupTeardown();
    if (!NT_SUCCESS(waitStatus))
    {
        return waitStatus;
    }

    LONG cleanupGeneration = InterlockedCompareExchange(&m_PnpCleanupGeneration, 0, 0);
    NTSTATUS status = DisconnectInternal();
    CompletePnpCleanupTeardown(status, FALSE);
    waitStatus = KeWaitForSingleObject(&m_PnpCleanupComplete, Executive, KernelMode, FALSE, NULL);
    if (!NT_SUCCESS(waitStatus))
    {
        return waitStatus;
    }
    LONG finalCleanupGeneration = InterlockedCompareExchange(&m_PnpCleanupGeneration, 0, 0);
    if (status == STATUS_DEVICE_BUSY && cleanupGeneration != finalCleanupGeneration)
    {
        status = (NTSTATUS)InterlockedCompareExchange(&m_PnpCleanupStatus, 0, 0);
    }
    return status;
}

void VioGpuNamedPool::QueuePnpCleanup(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PnpCleanupLock, &oldIrql);
    if (InterlockedCompareExchange(&m_PnpCleanupQueued, 0, 0) != VioGpuNamedPoolCleanupIdle ||
        InterlockedCompareExchange(&m_PnpCleanupWorkerState, 0, 0) != VioGpuNamedPoolWorkerIdle)
    {
        KeReleaseSpinLock(&m_PnpCleanupLock, oldIrql);
        return;
    }

    if (!ExAcquireRundownProtection(&m_PnpCleanupWorkerReferences))
    {
        KeReleaseSpinLock(&m_PnpCleanupLock, oldIrql);
        return;
    }

    InterlockedExchange(&m_PnpCleanupQueued, VioGpuNamedPoolCleanupPublishing);
    InterlockedExchange(&m_PnpCleanupWorkerState, VioGpuNamedPoolWorkerRunning);
    InterlockedExchange(&m_PnpCleanupStatus, STATUS_PENDING);
    KeClearEvent(&m_PnpCleanupComplete);
    InterlockedIncrement(&m_PnpCleanupGeneration);
    InterlockedExchange(&m_PnpCleanupQueued, VioGpuNamedPoolCleanupQueued);
    ExQueueWorkItem(&m_PnpCleanupWorkItem, DelayedWorkQueue);
    KeReleaseSpinLock(&m_PnpCleanupLock, oldIrql);
}

_Use_decl_annotations_ VOID VioGpuNamedPool::PnpCleanupWorker(PVOID context)
{
    VioGpuNamedPool *pool = reinterpret_cast<VioGpuNamedPool *>(context);
    if (pool == NULL)
    {
        return;
    }
    if (!pool->ClaimQueuedPnpCleanup())
    {
        ExReleaseRundownProtection(&pool->m_PnpCleanupWorkerReferences);
        return;
    }

    NTSTATUS status = pool->DisconnectInternal();
    if (!NT_SUCCESS(status) && status != STATUS_DEVICE_BUSY)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu droidvmpool: asynchronous PnP cleanup failed, status=0x%08X\n",
                   status);
    }
    pool->CompletePnpCleanupTeardown(status, TRUE);
    /* The work item was dequeued before this routine ran; no pool access follows this release. */
    ExReleaseRundownProtection(&pool->m_PnpCleanupWorkerReferences);
}

_Use_decl_annotations_ NTSTATUS VioGpuNamedPool::PnpNotificationCallback(PVOID notificationStructure, PVOID context)
{
    VioGpuNamedPool *pool = reinterpret_cast<VioGpuNamedPool *>(context);
    if (pool == NULL || !ExAcquireRundownProtection(&pool->m_NotificationCallbacks))
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS status = pool->HandlePnpNotification(notificationStructure);
    ExReleaseRundownProtection(&pool->m_NotificationCallbacks);
    return status;
}

NTSTATUS VioGpuNamedPool::HandlePnpNotification(_In_ PVOID notificationStructure)
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
        return STATUS_SUCCESS;
    }
    if (IsEqualGUID(header->Event, GUID_TARGET_DEVICE_REMOVE_COMPLETE))
    {
        return HandleRemoveComplete();
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuNamedPool::HandleQueryRemove(_In_ PFILE_OBJECT notificationFileObject)
{
    PAGED_CODE();

    if (InterlockedCompareExchange(&m_ShuttingDown, FALSE, FALSE) != FALSE)
    {
        return STATUS_SUCCESS;
    }

    LONG pnpState = InterlockedCompareExchange(&m_PnpState, 0, 0);
    if (pnpState == VioGpuNamedPoolConnecting)
    {
        return STATUS_UNSUCCESSFUL;
    }

    PFILE_OBJECT currentFileObject = reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOID volatile *)&m_FileObject,
                                                                                                      NULL,
                                                                                                      NULL));
    BOOLEAN targetOwned = notificationFileObject == NULL || notificationFileObject == currentFileObject;
    return targetOwned && pnpState == VioGpuNamedPoolConnected ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

NTSTATUS VioGpuNamedPool::HandleRemoveComplete(void)
{
    PAGED_CODE();

    if (InterlockedCompareExchange(&m_ShuttingDown, FALSE, FALSE) != FALSE)
    {
        return STATUS_SUCCESS;
    }

    LONG previousState = InterlockedCompareExchange(&m_PnpState, VioGpuNamedPoolFailed, VioGpuNamedPoolConnecting);
    BOOLEAN queueCleanup = FALSE;
    if (previousState == VioGpuNamedPoolConnected)
    {
        previousState = InterlockedCompareExchange(&m_PnpState, VioGpuNamedPoolFailed, VioGpuNamedPoolConnected);
        if (previousState != VioGpuNamedPoolConnected)
        {
            return STATUS_SUCCESS;
        }
        queueCleanup = TRUE;
    }
    else if (previousState != VioGpuNamedPoolConnecting)
    {
        return STATUS_SUCCESS;
    }

    InterlockedExchange(&m_RemovalLatched, TRUE);
    InterlockedExchange(&m_Ready, FALSE);
    InterlockedIncrement64(&m_Generation);
    if (m_FailureCallback != NULL)
    {
        m_FailureCallback(m_FailureContext);
    }
    if (queueCleanup)
    {
        QueuePnpCleanup();
    }
    return STATUS_SUCCESS;
}

BOOLEAN VioGpuNamedPool::QueryPhysicalRange(_Out_ PPHYSICAL_ADDRESS physicalAddress, _Out_ SIZE_T *size) const
{
    if (physicalAddress == NULL || size == NULL || KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        return FALSE;
    }
    physicalAddress->QuadPart = 0;
    *size = 0;

    if (!ExAcquireRundownProtection(&m_Operations))
    {
        return FALSE;
    }

    LONG64 generation = InterlockedCompareExchange64(&m_Generation, 0, 0);
    PHYSICAL_ADDRESS rangeBase = m_BasePA;
    SIZE_T rangeSize = m_Size;
    BOOLEAN valid = generation > 0 && IsActive() && rangeBase.QuadPart >= 0 && rangeSize >= PAGE_SIZE &&
                    ((ULONG64)rangeBase.QuadPart & (PAGE_SIZE - 1)) == 0 && (rangeSize & (PAGE_SIZE - 1)) == 0 &&
                    (ULONG64)rangeBase.QuadPart <= MAXULONGLONG - (rangeSize - 1) &&
                    generation == InterlockedCompareExchange64(&m_Generation, 0, 0) && IsActive();
    if (valid)
    {
        *physicalAddress = rangeBase;
        *size = rangeSize;
    }
    ExReleaseRundownProtection(&m_Operations);
    return valid;
}

BOOLEAN VioGpuNamedPool::AcquireMapping(_Out_ VioGpuNamedPoolMapping *mapping) const
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
    LONG64 generation = InterlockedCompareExchange64(&m_Generation, 0, 0);
    PFILE_OBJECT fileObject = reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOID volatile *)&m_FileObject,
                                                                                               NULL,
                                                                                               NULL));
    DROIDVMPOOL_MAPPING mappingValue = {};
    if (generation > 0 && IsActive() && fileObject != NULL && m_DirectInterface.InterfaceHeader.Context != NULL &&
        m_DirectInterface.AcquireMapping != NULL && m_DirectInterface.ReleaseMapping != NULL)
    {
        acquired = m_DirectInterface.AcquireMapping(m_DirectInterface.InterfaceHeader.Context, &mappingValue);
    }
    if (!acquired)
    {
        ExReleaseRundownProtection(&m_Operations);
        return FALSE;
    }

    if (generation != InterlockedCompareExchange64(&m_Generation, 0, 0) || !IsActive() ||
        mappingValue.BaseVirtualAddress == NULL ||
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
    mapping->m_Generation = (ULONGLONG)generation;
    mapping->m_Owner = this;
    return TRUE;
}

void VioGpuNamedPool::ReleaseMapping(_Inout_ VioGpuNamedPoolMapping *mapping) const
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
    mapping->m_Generation = 0;
    RtlZeroMemory(&mapping->m_Mapping, sizeof(mapping->m_Mapping));
    ExReleaseRundownProtection(&m_Operations);
}
