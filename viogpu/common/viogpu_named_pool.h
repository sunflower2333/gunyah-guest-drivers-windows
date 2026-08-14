#pragma once

#include <ntddk.h>

#include "../../droidvmpool/droidvmpool_interface.h"

class VioGpuNamedPool;

typedef VOID (*VIOGPU_NAMED_POOL_FAILURE_CALLBACK)(_In_opt_ PVOID context);

VOID VioGpuSetNamedPoolNotificationDriverObject(_In_ PDRIVER_OBJECT driverObject);
VOID VioGpuClearNamedPoolNotificationDriverObject(void);

class VioGpuNamedPoolMapping
{
  public:
    VioGpuNamedPoolMapping();
    ~VioGpuNamedPoolMapping();

    BOOLEAN IsActive(void) const
    {
        return m_Owner != NULL;
    }
    PVOID GetBaseAddress(void) const
    {
        return m_Mapping.BaseVirtualAddress;
    }
    PHYSICAL_ADDRESS GetPhysicalAddress(void) const
    {
        return m_Mapping.BasePhysicalAddress;
    }
    SIZE_T GetSize(void) const
    {
        return (SIZE_T)m_Mapping.TotalSize;
    }
    ULONGLONG GetGeneration(void) const
    {
        return m_Generation;
    }
    void Release(void);

  private:
    friend class VioGpuNamedPool;

    VioGpuNamedPoolMapping(const VioGpuNamedPoolMapping &);
    VioGpuNamedPoolMapping &operator=(const VioGpuNamedPoolMapping &);

    const VioGpuNamedPool *m_Owner;
    PKTHREAD m_OwningThread;
    KIRQL m_AcquireIrql;
    ULONGLONG m_Generation;
    DROIDVMPOOL_MAPPING m_Mapping;
};

class VioGpuNamedPool
{
  public:
    ~VioGpuNamedPool();

    NTSTATUS Connect(void);
    NTSTATUS Disconnect(void);
    BOOLEAN AcquireMapping(_Out_ VioGpuNamedPoolMapping *mapping) const;
    BOOLEAN QueryPhysicalRange(_Out_ PPHYSICAL_ADDRESS physicalAddress, _Out_ SIZE_T *size) const;
    BOOLEAN IsActive(void) const
    {
        return InterlockedCompareExchange(&m_Ready, FALSE, FALSE) != FALSE &&
               InterlockedCompareExchange(&m_PnpState, 0, 0) == VioGpuNamedPoolConnected;
    }
    BOOLEAN HasConnectionOwner(void) const;

  private:
    friend class VioGpuNamedPoolMapping;

    enum VIOGPU_NAMED_POOL_PNP_STATE
    {
        VioGpuNamedPoolDisconnected = 0,
        VioGpuNamedPoolConnecting,
        VioGpuNamedPoolConnected,
        VioGpuNamedPoolFailed,
    };

    enum VIOGPU_NAMED_POOL_CLEANUP_STATE
    {
        VioGpuNamedPoolCleanupIdle = 0,
        VioGpuNamedPoolCleanupPublishing,
        VioGpuNamedPoolCleanupQueued,
        VioGpuNamedPoolCleanupTeardown,
    };

    enum VIOGPU_NAMED_POOL_WORKER_STATE
    {
        VioGpuNamedPoolWorkerIdle = 0,
        VioGpuNamedPoolWorkerRunning,
        VioGpuNamedPoolWorkerFinalizing,
    };

  protected:
    VioGpuNamedPool(_In_reads_(expectedNameLength) const CHAR *expectedName,
                    _In_ ULONG expectedNameLength,
                    _In_ VIOGPU_NAMED_POOL_FAILURE_CALLBACK failureCallback,
                    _In_opt_ PVOID failureContext);

  private:
    static DRIVER_NOTIFICATION_CALLBACK_ROUTINE PnpNotificationCallback;
    static WORKER_THREAD_ROUTINE PnpCleanupWorker;

    NTSTATUS HandlePnpNotification(_In_ PVOID notificationStructure);
    NTSTATUS HandleQueryRemove(_In_ PFILE_OBJECT notificationFileObject);
    NTSTATUS HandleRemoveComplete(void);
    NTSTATUS DisconnectInternal(void);
    NTSTATUS WaitForPnpCleanupIdle(void);
    NTSTATUS BeginPnpCleanupTeardown(void);
    BOOLEAN ClaimQueuedPnpCleanup(void);
    void CompletePnpCleanupTeardown(_In_ NTSTATUS status, _In_ BOOLEAN worker);
    void QueuePnpCleanup(void);
    void LockState(void) const;
    void UnlockState(void) const;
    void ReleaseMapping(_Inout_ VioGpuNamedPoolMapping *mapping) const;

    mutable KMUTEX m_StateLock;
    mutable EX_RUNDOWN_REF m_Operations;
    EX_RUNDOWN_REF m_NotificationCallbacks;
    EX_RUNDOWN_REF m_PnpCleanupWorkerReferences;
    mutable volatile LONG m_Ready;
    mutable DECLSPEC_ALIGN(8) volatile LONG64 m_Generation;
    WORK_QUEUE_ITEM m_PnpCleanupWorkItem;
    KEVENT m_PnpCleanupComplete;
    KSPIN_LOCK m_PnpCleanupLock;
    mutable volatile LONG m_PnpCleanupQueued;
    volatile LONG m_PnpCleanupGeneration;
    volatile LONG m_PnpCleanupStatus;
    volatile LONG m_PnpCleanupWorkerState;
    BOOLEAN m_RundownCompleted;
    BOOLEAN m_NotificationRundownCompleted;
    BOOLEAN m_DisconnectInProgress;
    volatile LONG m_ShuttingDown;
    volatile LONG m_RemovalLatched;
    volatile LONG m_PnpState;
    const CHAR *m_ExpectedName;
    ULONG m_ExpectedNameLength;
    VIOGPU_NAMED_POOL_FAILURE_CALLBACK m_FailureCallback;
    PVOID m_FailureContext;
    PDRIVER_OBJECT m_NotificationDriverObject;
    PVOID m_NotificationEntry;
    UNICODE_STRING m_InterfaceName;
    PFILE_OBJECT volatile m_FileObject;
    DROIDVMPOOL_DIRECT_INTERFACE m_DirectInterface;
    PHYSICAL_ADDRESS m_BasePA;
    SIZE_T m_Size;
};

class VioGpuDrmHostPool final : public VioGpuNamedPool
{
  public:
    VioGpuDrmHostPool(_In_ VIOGPU_NAMED_POOL_FAILURE_CALLBACK failureCallback, _In_opt_ PVOID failureContext);
};

class VioGpuGuestPool final : public VioGpuNamedPool
{
  public:
    VioGpuGuestPool(_In_ VIOGPU_NAMED_POOL_FAILURE_CALLBACK failureCallback, _In_opt_ PVOID failureContext);
};

typedef VioGpuNamedPoolMapping VioGpuDrmHostPoolMapping;
