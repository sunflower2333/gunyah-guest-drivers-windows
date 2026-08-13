#pragma once

#include <ntddk.h>

class VioGpuRdmaPool
{
  public:
    VioGpuRdmaPool();
    ~VioGpuRdmaPool();

    NTSTATUS Connect(void);
    NTSTATUS Disconnect(void);
    PVOID Allocate(SIZE_T size, SIZE_T alignment);
    void Free(PVOID address);
    BOOLEAN Contains(PVOID address) const;
    PHYSICAL_ADDRESS GetPhysicalAddress(PVOID address);
    BOOLEAN QueryVidMmSegment(PVOID *baseAddress, PPHYSICAL_ADDRESS physicalAddress, SIZE_T *size) const;
    BOOLEAN IsActive(void) const
    {
        return m_Ready;
    }
    BOOLEAN HasArenaOwner(void) const
    {
        return m_ArenaOwned;
    }

  private:
    void ClearConnection(void);

    BOOLEAN m_ArenaOwned;
    BOOLEAN m_Ready;
    BOOLEAN m_RundownCompleted;
    BOOLEAN m_DisconnectAttempted;
    NTSTATUS m_DisconnectStatus;
    PFILE_OBJECT m_FileObject;
    PDEVICE_OBJECT m_DeviceObject;
    PVOID m_BaseVA;
    PHYSICAL_ADDRESS m_BasePA;
    SIZE_T m_Size;
    ULONG m_PageCount;
    ULONG64 m_AllocationToken;
    PUCHAR m_Bitmap;
    PVOID m_VidMmBaseVA;
    PHYSICAL_ADDRESS m_VidMmBasePA;
    SIZE_T m_VidMmSize;
    KSPIN_LOCK m_Lock;
    mutable EX_RUNDOWN_REF m_Operations;
};
