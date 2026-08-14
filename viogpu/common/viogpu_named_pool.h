#pragma once

#include <ntddk.h>

#include "../../droidvmpool/droidvmpool_interface.h"

class VioGpuDrmHostPool;

VOID VioGpuSetNamedPoolNotificationDriverObject(_In_ PDRIVER_OBJECT driverObject);
VOID VioGpuClearNamedPoolNotificationDriverObject(void);

class VioGpuDrmHostPoolMapping
{
  public:
    VioGpuDrmHostPoolMapping();
    ~VioGpuDrmHostPoolMapping();

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
    void Release(void);

  private:
    friend class VioGpuDrmHostPool;

    VioGpuDrmHostPoolMapping(const VioGpuDrmHostPoolMapping &);
    VioGpuDrmHostPoolMapping &operator=(const VioGpuDrmHostPoolMapping &);

    const VioGpuDrmHostPool *m_Owner;
    DROIDVMPOOL_MAPPING m_Mapping;
};

class VioGpuDrmHostPool
{
  public:
    VioGpuDrmHostPool();
    ~VioGpuDrmHostPool();

    NTSTATUS Connect(void);
    NTSTATUS Disconnect(void);
    BOOLEAN AcquireMapping(_Out_ VioGpuDrmHostPoolMapping *mapping) const;
    BOOLEAN IsActive(void) const
    {
        return InterlockedCompareExchange(&m_Ready, FALSE, FALSE) != FALSE;
    }
    BOOLEAN HasConnectionOwner(void) const;

  private:
    friend class VioGpuDrmHostPoolMapping;

    enum VIOGPU_DRM_HOST_POOL_PNP_STATE
    {
        VioGpuDrmHostPoolDisconnected = 0,
        VioGpuDrmHostPoolConnected,
        VioGpuDrmHostPoolQueryRemove,
        VioGpuDrmHostPoolReconnecting,
        VioGpuDrmHostPoolRemoved,
        VioGpuDrmHostPoolFailed,
    };

    static DRIVER_NOTIFICATION_CALLBACK_ROUTINE PnpNotificationCallback;

    NTSTATUS HandlePnpNotification(_In_ PVOID notificationStructure);
    NTSTATUS HandleQueryRemove(_In_ PFILE_OBJECT notificationFileObject);
    NTSTATUS HandleRemoveCancelled(void);
    NTSTATUS HandleRemoveComplete(void);
    void LockState(void) const;
    void UnlockState(void) const;
    void ReleaseMapping(_Inout_ VioGpuDrmHostPoolMapping *mapping) const;

    mutable KMUTEX m_StateLock;
    mutable EX_RUNDOWN_REF m_Operations;
    EX_RUNDOWN_REF m_NotificationCallbacks;
    mutable volatile LONG m_Ready;
    BOOLEAN m_RundownCompleted;
    BOOLEAN m_NotificationRundownCompleted;
    BOOLEAN m_DisconnectInProgress;
    BOOLEAN m_ShuttingDown;
    VIOGPU_DRM_HOST_POOL_PNP_STATE m_PnpState;
    PDRIVER_OBJECT m_NotificationDriverObject;
    PVOID m_NotificationEntry;
    UNICODE_STRING m_InterfaceName;
    PFILE_OBJECT m_FileObject;
    DROIDVMPOOL_DIRECT_INTERFACE m_DirectInterface;
    PHYSICAL_ADDRESS m_BasePA;
    SIZE_T m_Size;
};
