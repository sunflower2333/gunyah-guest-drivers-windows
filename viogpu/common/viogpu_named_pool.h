#pragma once

#include <ntddk.h>

#include "../../droidvmpool/droidvmpool_interface.h"

class VioGpuDrmHostPool;

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
    void Disconnect(void);
    BOOLEAN AcquireMapping(_Out_ VioGpuDrmHostPoolMapping *mapping) const;
    BOOLEAN IsActive(void) const
    {
        return InterlockedCompareExchange(&m_Ready, FALSE, FALSE) != FALSE;
    }
    BOOLEAN HasConnectionOwner(void) const
    {
        return m_FileObject != NULL || m_DirectInterface.InterfaceHeader.Context != NULL;
    }

  private:
    friend class VioGpuDrmHostPoolMapping;

    void ReleaseMapping(_Inout_ VioGpuDrmHostPoolMapping *mapping) const;

    mutable EX_RUNDOWN_REF m_Operations;
    mutable volatile LONG m_Ready;
    BOOLEAN m_RundownCompleted;
    PFILE_OBJECT m_FileObject;
    DROIDVMPOOL_DIRECT_INTERFACE m_DirectInterface;
    PHYSICAL_ADDRESS m_BasePA;
    SIZE_T m_Size;
};
