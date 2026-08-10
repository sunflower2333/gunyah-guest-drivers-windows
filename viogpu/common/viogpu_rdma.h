#pragma once

#include <ntddk.h>

class VioGpuRdmaPool
{
  public:
    VioGpuRdmaPool();
    ~VioGpuRdmaPool();

    NTSTATUS Connect(void);
    void Disconnect(void);
    PVOID Allocate(SIZE_T size, SIZE_T alignment);
    void Free(PVOID address);
    BOOLEAN Contains(PVOID address) const;
    PHYSICAL_ADDRESS GetPhysicalAddress(PVOID address) const;
    BOOLEAN IsActive(void) const
    {
        return m_Active;
    }

  private:
    BOOLEAN m_Active;
    PFILE_OBJECT m_FileObject;
    PDEVICE_OBJECT m_DeviceObject;
    PVOID m_BaseVA;
    PHYSICAL_ADDRESS m_BasePA;
    SIZE_T m_Size;
    ULONG m_PageCount;
    PUCHAR m_Bitmap;
    KSPIN_LOCK m_Lock;
};