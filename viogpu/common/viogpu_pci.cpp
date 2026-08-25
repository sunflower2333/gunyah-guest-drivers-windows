/*
 * Virtio PCI driver
 *
 * This module allows virtio devices to be used over a virtual PCI device.
 * This can be used with QEMU based VMMs like KVM or Xen.
 *
 * Copyright IBM Corp. 2007
 *
 * Authors:
 *  Anthony Liguori  <aliguori@us.ibm.com>
 *  Windows porting - Yan Vugenfirer <yvugenfi@redhat.com>
 *  WDDM porting - Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */
/**********************************************************************
 * Copyright (c) 2012-2020 Red Hat, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 **********************************************************************/
#include "viogpu.h"
#include "..\viogpudo\viogpudo.h"
#if !DBG
#include "viogpu_pci.tmh"
#endif

u32 ReadVirtIODeviceRegister(ULONG_PTR ulRegister)
{
    if (ulRegister & ~PORT_MASK)
    {
        return READ_REGISTER_ULONG((PULONG)(ulRegister));
    }
    else
    {
        return READ_PORT_ULONG((PULONG)(ulRegister));
    }
}

void WriteVirtIODeviceRegister(ULONG_PTR ulRegister, u32 ulValue)
{
    if (ulRegister & ~PORT_MASK)
    {
        WRITE_REGISTER_ULONG((PULONG)(ulRegister), (ULONG)(ulValue));
    }
    else
    {
        WRITE_PORT_ULONG((PULONG)(ulRegister), (ULONG)(ulValue));
    }
}

u8 ReadVirtIODeviceByte(ULONG_PTR ulRegister)
{
    if (ulRegister & ~PORT_MASK)
    {
        return READ_REGISTER_UCHAR((PUCHAR)(ulRegister));
    }
    else
    {
        return READ_PORT_UCHAR((PUCHAR)(ulRegister));
    }
}

void WriteVirtIODeviceByte(ULONG_PTR ulRegister, u8 bValue)
{
    if (ulRegister & ~PORT_MASK)
    {
        WRITE_REGISTER_UCHAR((PUCHAR)(ulRegister), (UCHAR)(bValue));
    }
    else
    {
        WRITE_PORT_UCHAR((PUCHAR)(ulRegister), (UCHAR)(bValue));
    }
}

u16 ReadVirtIODeviceWord(ULONG_PTR ulRegister)
{
    if (ulRegister & ~PORT_MASK)
    {
        return READ_REGISTER_USHORT((PUSHORT)(ulRegister));
    }
    else
    {
        return READ_PORT_USHORT((PUSHORT)(ulRegister));
    }
}

void WriteVirtIODeviceWord(ULONG_PTR ulRegister, u16 wValue)
{
    if (ulRegister & ~PORT_MASK)
    {
        WRITE_REGISTER_USHORT((PUSHORT)(ulRegister), (USHORT)(wValue));
    }
    else
    {
        WRITE_PORT_USHORT((PUSHORT)(ulRegister), (USHORT)(wValue));
    }
}

void *mem_alloc_contiguous_pages(void *context, size_t size)
{
    PHYSICAL_ADDRESS HighestAcceptable;
    PVOID ptr = NULL;

    UNREFERENCED_PARAMETER(context);

    HighestAcceptable.QuadPart = 0xFFFFFFFFFF;
    ptr = MmAllocateContiguousMemory(size, HighestAcceptable);
    if (ptr)
    {
        RtlZeroMemory(ptr, size);
    }
    else
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Ran out of memory in alloc_pages_exact(%Id)\n", size));
    }
    return ptr;
}

void mem_free_contiguous_pages(void *context, void *virt)
{
    UNREFERENCED_PARAMETER(context);
    if (virt)
    {
        MmFreeContiguousMemory(virt);
    }
}

ULONGLONG mem_get_physical_address(void *context, void *virt)
{
    UNREFERENCED_PARAMETER(context);
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(virt);
    return pa.QuadPart;
}

void *mem_alloc_nonpaged_block(void *context, size_t size)
{
    UNREFERENCED_PARAMETER(context);
    PVOID ptr = ExAllocatePoolUninitialized(NonPagedPoolNx, size, VIOGPUTAG);
    if (ptr)
    {
        RtlZeroMemory(ptr, size);
    }
    else
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Ran out of memory in alloc_pages_exact(%Id)\n", size));
    }
    return ptr;
}

void mem_free_nonpaged_block(void *context, void *addr)
{
    UNREFERENCED_PARAMETER(context);
    if (addr)
    {
        ExFreePoolWithTag(addr, VIOGPUTAG);
    }
}

PAGED_CODE_SEG_BEGIN
static int PCIReadConfig(IVioGpuPCI *pDev, int where, void *buffer, size_t length)
{
    PAGED_CODE();

    NTSTATUS Status;
    PDXGKRNL_INTERFACE pDxgkInterface = pDev->GetDxgkInterface();
    ULONG BytesRead = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    Status = pDxgkInterface->DxgkCbReadDeviceSpace(pDxgkInterface->DeviceHandle,
                                                   DXGK_WHICHSPACE_CONFIG,
                                                   buffer,
                                                   where,
                                                   (ULONG)length,
                                                   &BytesRead);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkCbReadDeviceSpace failed with status 0x%X\n", Status));
        return -1;
    }
    if (BytesRead != length)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("[%s] read %d bytes at %d\n", __FUNCTION__, BytesRead, where));
        return -1;
    }
    return 0;
}

static int pci_read_config_byte(void *context, int where, u8 *bVal)
{
    PAGED_CODE();
    IVioGpuPCI *pdev = static_cast<IVioGpuPCI *>(context);
    return PCIReadConfig(pdev, where, bVal, sizeof(*bVal));
}

int pci_read_config_word(void *context, int where, u16 *wVal)
{
    PAGED_CODE();
    IVioGpuPCI *pdev = static_cast<IVioGpuPCI *>(context);
    return PCIReadConfig(pdev, where, wVal, sizeof(*wVal));
}

int pci_read_config_dword(void *context, int where, u32 *dwVal)
{
    PAGED_CODE();
    IVioGpuPCI *pdev = static_cast<IVioGpuPCI *>(context);
    return PCIReadConfig(pdev, where, dwVal, sizeof(*dwVal));
}
PAGED_CODE_SEG_END

size_t pci_get_resource_len(void *context, int bar)
{
    IVioGpuPCI *pdev = static_cast<IVioGpuPCI *>(context);
    ULONGLONG size = pdev->GetPciResources()->GetBarSize(bar);
    return size > MAXULONG_PTR ? MAXULONG_PTR : static_cast<size_t>(size);
}

void *pci_map_address_range(void *context, int bar, size_t offset, size_t maxlen)
{
    UNREFERENCED_PARAMETER(maxlen);

    IVioGpuPCI *pdev = static_cast<IVioGpuPCI *>(context);
    return pdev->GetPciResources()->GetMappedAddress(bar, (ULONG)offset);
}

u16 vdev_get_msix_vector(void *context, int queue)
{
    IVioGpuPCI *pdev = static_cast<IVioGpuPCI *>(context);
    u16 vector = VIRTIO_MSI_NO_VECTOR;

    if (pdev->IsMSIEnabled())
    {
        if (queue >= 0)
        {
            /* queue interrupt */
            vector = (u16)(queue + 1);
        }
        else
        {
            vector = VIRTIO_GPU_MSIX_CONFIG_VECTOR;
        }
    }
    return vector;
}

void vdev_sleep(void *context, unsigned int msecs)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    UNREFERENCED_PARAMETER(context);

    if (KeGetCurrentIrql() <= APC_LEVEL)
    {
        LARGE_INTEGER delay;
        delay.QuadPart = Int32x32To64(msecs, -10000);
        status = KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    if (!NT_SUCCESS(status))
    {
        KeStallExecutionProcessor(1000 * msecs);
    }
}

// clang-format off
VirtIOSystemOps VioGpuSystemOps = {
    ReadVirtIODeviceByte,
    ReadVirtIODeviceWord,
    ReadVirtIODeviceRegister,
    WriteVirtIODeviceByte,
    WriteVirtIODeviceWord,
    WriteVirtIODeviceRegister,
    mem_alloc_contiguous_pages,
    mem_free_contiguous_pages,
    mem_get_physical_address,
    mem_alloc_nonpaged_block,
    mem_free_nonpaged_block,
    pci_read_config_byte,
    pci_read_config_word,
    pci_read_config_dword,
    pci_get_resource_len,
    pci_map_address_range,
    vdev_get_msix_vector,
    vdev_sleep,
};
// clang-format on

PVOID CPciBar::GetVA(PDXGKRNL_INTERFACE pDxgkInterface)
{
    NTSTATUS Status;
    if (m_uSize == 0 || m_uSize > MAXULONG)
    {
        return nullptr;
    }
    ULONG length = static_cast<ULONG>(m_uSize);
    if (m_BaseVA == nullptr)
    {
        if (m_bPortSpace)
        {
            if (m_bIoMapped)
            {
                Status = pDxgkInterface->DxgkCbMapMemory(pDxgkInterface->DeviceHandle,
                                                         m_BasePA,
                                                         length,
                                                         TRUE,
                                                         FALSE,
                                                         MmNonCached,
                                                         &m_BaseVA);
                if (Status == STATUS_SUCCESS)
                {
                    DbgPrint(TRACE_LEVEL_VERBOSE, ("[%s] mapped port BAR at %x\n", __FUNCTION__, m_BasePA.LowPart));
                }
                else
                {
                    m_BaseVA = nullptr;
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("DxgkCbMapMemor (CmResourceTypePort) failed with status 0x%X\n", Status));
                }
            }
            else
            {
                m_BaseVA = (PUCHAR)(ULONG_PTR)m_BasePA.QuadPart;
            }
        }
        else
        {
            Status = pDxgkInterface->DxgkCbMapMemory(pDxgkInterface->DeviceHandle,
                                                     m_BasePA,
                                                     length,
                                                     FALSE,
                                                     FALSE,
                                                     MmNonCached,
                                                     &m_BaseVA);
            if (Status == STATUS_SUCCESS)
            {
                DbgPrint(TRACE_LEVEL_VERBOSE, ("[%s] mapped memory BAR at %I64x\n", __FUNCTION__, m_BasePA.QuadPart));
            }
            else
            {
                m_BaseVA = nullptr;
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("[%s] failed to map memory BAR at %I64x\n", __FUNCTION__, m_BasePA.QuadPart));
            }
        }
    }
    return m_BaseVA;
}

NTSTATUS CPciBar::Unmap(PDXGKRNL_INTERFACE pDxgkInterface)
{
    if (m_BaseVA == nullptr)
    {
        return STATUS_SUCCESS;
    }

    // Memory BARs and translated port BARs are both created through
    // DxgkCbMapMemory.  Only direct port-space addresses need no unmap.
    if (!m_bPortSpace || m_bIoMapped)
    {
        if (pDxgkInterface == nullptr || pDxgkInterface->DxgkCbUnmapMemory == nullptr)
        {
            return STATUS_DEVICE_NOT_READY;
        }
        NTSTATUS status = pDxgkInterface->DxgkCbUnmapMemory(pDxgkInterface->DeviceHandle, m_BaseVA);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }
    m_BaseVA = nullptr;
    return STATUS_SUCCESS;
}

NTSTATUS CPciResources::Close(void)
{
    NTSTATUS firstFailure = STATUS_SUCCESS;

    if (m_HostVisibleMappedVA != nullptr)
    {
        if (m_pDxgkInterface == nullptr || m_pDxgkInterface->DxgkCbUnmapMemory == nullptr)
        {
            return STATUS_DEVICE_NOT_READY;
        }

        NTSTATUS status = m_pDxgkInterface->DxgkCbUnmapMemory(m_pDxgkInterface->DeviceHandle, m_HostVisibleMappedVA);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        m_HostVisibleMappedVA = nullptr;
        m_HostVisibleMappedOffset = 0;
        m_HostVisibleMappedSize = 0;
    }

    for (UINT bar = 0; bar < PCI_TYPE0_ADDRESSES; ++bar)
    {
        NTSTATUS status = m_Bars[bar].Unmap(m_pDxgkInterface);
        if (!NT_SUCCESS(status) && NT_SUCCESS(firstFailure))
        {
            firstFailure = status;
        }
    }
    if (!NT_SUCCESS(firstFailure))
    {
        return firstFailure;
    }

    for (UINT bar = 0; bar < PCI_TYPE0_ADDRESSES; ++bar)
    {
        m_Bars[bar] = CPciBar();
    }
    m_InterruptFlags = 0;
    m_InterruptMessageCount = 0;
    m_InterruptMessageCountKnown = FALSE;
    m_HostVisibleBar = MAXUINT;
    m_HostVisibleOffset = 0;
    m_HostVisibleSize = 0;
    m_HostVisibleMappedVA = nullptr;
    m_HostVisibleMappedOffset = 0;
    m_HostVisibleMappedSize = 0;
    m_pDxgkInterface = nullptr;
    return STATUS_SUCCESS;
}

bool CPciResources::Init(PDXGKRNL_INTERFACE pDxgkInterface, PCM_RESOURCE_LIST pResList)
{
    PCI_COMMON_HEADER pci_config = {0};
    ULONG BytesRead = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    bool interrupt_found = false;
    bool interrupt_resources_valid = true;
    int bar = -1;

    if (pDxgkInterface == nullptr || pResList == nullptr || m_pDxgkInterface != nullptr)
    {
        return false;
    }
    m_pDxgkInterface = pDxgkInterface;

    // The translated list contains one descriptor per MSI-X message, but
    // ordinary MSI has one descriptor regardless of its raw MessageCount.
    // With no raw list available, one message descriptor has an unknown count.
    // Two or more message descriptors prove MSI-X and therefore expose the
    // complete count. Retain both the observed count and whether it is known
    // before any PCI operation can fail.
    for (ULONG i = 0; i < pResList->Count; ++i)
    {
        PCM_FULL_RESOURCE_DESCRIPTOR pFullResDescriptor = &pResList->List[i];
        for (ULONG j = 0; j < pFullResDescriptor->PartialResourceList.Count; ++j)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR pResDescriptor = &pFullResDescriptor->PartialResourceList.PartialDescriptors[j];
            if (pResDescriptor->Type != CmResourceTypeInterrupt)
            {
                continue;
            }

            BOOLEAN messageSignaled = (pResDescriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) != 0;
            if (!interrupt_found)
            {
                m_InterruptFlags = pResDescriptor->Flags;
                interrupt_found = true;
            }
            else if (messageSignaled != IsMSIEnabled())
            {
                interrupt_resources_valid = false;
            }

            ++m_InterruptMessageCount;
            if (!messageSignaled && m_InterruptMessageCount != 1)
            {
                interrupt_resources_valid = false;
            }
        }
    }
    m_InterruptMessageCountKnown = interrupt_found && interrupt_resources_valid &&
                                   ((!IsMSIEnabled() && m_InterruptMessageCount == 1U) ||
                                    (IsMSIEnabled() && m_InterruptMessageCount >= 2U));
    bool interrupt_layout_accepted = m_InterruptMessageCountKnown &&
                                     (!IsMSIEnabled() ||
                                      (m_InterruptMessageCount >= 3U && m_InterruptMessageCount <= 4U));

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    Status = m_pDxgkInterface->DxgkCbReadDeviceSpace(m_pDxgkInterface->DeviceHandle,
                                                     DXGK_WHICHSPACE_CONFIG,
                                                     &pci_config,
                                                     0,
                                                     sizeof(pci_config),
                                                     &BytesRead);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s BytesRead = %d\n", __FUNCTION__, BytesRead));

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkCbReadDeviceSpace failed with status 0x%X\n", Status));
        return false;
    }

    if (BytesRead != sizeof(pci_config))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("[%s] could not read PCI config space\n", __FUNCTION__));
        return false;
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s ListCount = %d\n", __FUNCTION__, pResList->Count));

    for (ULONG i = 0; i < pResList->Count; ++i)
    {
        PCM_FULL_RESOURCE_DESCRIPTOR pFullResDescriptor = &pResList->List[i];

        for (ULONG j = 0; j < pFullResDescriptor->PartialResourceList.Count; ++j)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR pResDescriptor = &pFullResDescriptor->PartialResourceList.PartialDescriptors[j];
            switch (pResDescriptor->Type)
            {
                case CmResourceTypePort:
                    {
                        DbgPrint(TRACE_LEVEL_FATAL, ("CmResourceTypePort\n"));
                        break;
                    }
                    break;
                case CmResourceTypeInterrupt:
                    {
                        if ((pResDescriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) != 0)
                        {
                            DbgPrint(TRACE_LEVEL_FATAL,
                                     ("Found MSI Interrupt vector %d, level %d, affinity 0x%X, flags %X\n",
                                      pResDescriptor->u.MessageInterrupt.Translated.Vector,
                                      pResDescriptor->u.MessageInterrupt.Translated.Level,
                                      (ULONG)pResDescriptor->u.MessageInterrupt.Translated.Affinity,
                                      pResDescriptor->Flags));
                        }
                        else
                        {
                            DbgPrint(TRACE_LEVEL_FATAL,
                                     ("Found Interrupt vector %d, level %d, affinity 0x%X, flags %X\n",
                                      pResDescriptor->u.Interrupt.Vector,
                                      pResDescriptor->u.Interrupt.Level,
                                      (ULONG)pResDescriptor->u.Interrupt.Affinity,
                                      pResDescriptor->Flags));
                        }
                    }
                    break;
                case CmResourceTypeMemory:
                case CmResourceTypeMemoryLarge:
                    {
                        PHYSICAL_ADDRESS Start = {};
                        ULONGLONG len = 0;
                        if (pResDescriptor->Type == CmResourceTypeMemory)
                        {
                            Start = pResDescriptor->u.Memory.Start;
                            len = pResDescriptor->u.Memory.Length;
                        }
                        else
                        {
                            switch (pResDescriptor->Flags & CM_RESOURCE_MEMORY_LARGE)
                            {
                                case CM_RESOURCE_MEMORY_LARGE_40:
                                    Start = pResDescriptor->u.Memory40.Start;
                                    len = static_cast<ULONGLONG>(pResDescriptor->u.Memory40.Length40) << 8;
                                    break;
                                case CM_RESOURCE_MEMORY_LARGE_48:
                                    Start = pResDescriptor->u.Memory48.Start;
                                    len = static_cast<ULONGLONG>(pResDescriptor->u.Memory48.Length48) << 16;
                                    break;
                                case CM_RESOURCE_MEMORY_LARGE_64:
                                    Start = pResDescriptor->u.Memory64.Start;
                                    len = static_cast<ULONGLONG>(pResDescriptor->u.Memory64.Length64) << 32;
                                    break;
                                default:
                                    break;
                            }
                        }
                        if (len == 0)
                        {
                            DbgPrint(TRACE_LEVEL_ERROR,
                                     ("Unsupported or empty memory resource flags 0x%X\n", pResDescriptor->Flags));
                            break;
                        }
                        bar = virtio_get_bar_index(&pci_config, Start);
                        DbgPrint(TRACE_LEVEL_FATAL,
                                 ("Found IO memory at %08I64X(%I64u) bar %d\n", Start.QuadPart, len, bar));
                        if (bar < 0)
                        {
                            break;
                        }
                        m_Bars[bar] = CPciBar(Start, len, false, true);
                    }
                    break;
                case CmResourceTypeDma:
                    DbgPrint(TRACE_LEVEL_FATAL, ("Dma\n"));
                    break;
                case CmResourceTypeDeviceSpecific:
                    DbgPrint(TRACE_LEVEL_FATAL, ("Device Specific\n"));
                    break;
                case CmResourceTypeBusNumber:
                    DbgPrint(TRACE_LEVEL_FATAL, ("Bus number\n"));
                    break;
                default:
                    DbgPrint(TRACE_LEVEL_ERROR, ("Unsupported descriptor type = %d\n", pResDescriptor->Type));
                    break;
            }
        }
    }
    if (bar < 0 || !interrupt_layout_accepted)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("[%s] resource enumeration failed\n", __FUNCTION__));
        return false;
    }

    UCHAR capabilityOffset = pci_config.u.type0.CapabilitiesPtr;
    for (UINT capabilityCount = 0; capabilityOffset >= sizeof(PCI_COMMON_HEADER) && capabilityCount < 48;
         ++capabilityCount)
    {
        struct virtio_pci_cap64 capability = {};
        BytesRead = 0;
        Status = m_pDxgkInterface->DxgkCbReadDeviceSpace(m_pDxgkInterface->DeviceHandle,
                                                         DXGK_WHICHSPACE_CONFIG,
                                                         &capability,
                                                         capabilityOffset,
                                                         sizeof(capability),
                                                         &BytesRead);
        if (!NT_SUCCESS(Status) || BytesRead < sizeof(struct virtio_pci_cap))
        {
            break;
        }

        UCHAR nextOffset = capability.cap.cap_next;
        if (capability.cap.cap_vndr == PCI_CAPABILITY_ID_VENDOR_SPECIFIC &&
            capability.cap.cap_len >= sizeof(capability) &&
            capability.cap.cfg_type == VIRTIO_PCI_CAP_SHARED_MEMORY_CFG && capability.cap.id == 1 &&
            capability.cap.bar < PCI_TYPE0_ADDRESSES && BytesRead == sizeof(capability))
        {
            ULONGLONG regionOffset = (static_cast<ULONGLONG>(capability.offset_hi) << 32) | capability.cap.offset;
            ULONGLONG regionSize = (static_cast<ULONGLONG>(capability.length_hi) << 32) | capability.cap.length;
            ULONGLONG barSize = m_Bars[capability.cap.bar].GetSize();
            if (regionSize >= PAGE_SIZE && (regionSize & (PAGE_SIZE - 1)) == 0 && regionOffset <= barSize &&
                regionSize <= barSize - regionOffset && (regionOffset & (PAGE_SIZE - 1)) == 0)
            {
                m_HostVisibleBar = capability.cap.bar;
                m_HostVisibleOffset = regionOffset;
                m_HostVisibleSize = regionSize;
            }
            break;
        }

        if (nextOffset == 0 || nextOffset == capabilityOffset || (nextOffset & (sizeof(ULONG) - 1)) != 0)
        {
            break;
        }
        capabilityOffset = nextOffset;
    }
    return true;
}

BOOLEAN CPciResources::QueryHostVisibleRegion(_Out_ PUINT bar, _Out_ PULONGLONG offset, _Out_ PULONGLONG size) const
{
    if (bar == NULL || offset == NULL || size == NULL || m_HostVisibleBar >= PCI_TYPE0_ADDRESSES ||
        m_HostVisibleSize < PAGE_SIZE)
    {
        return FALSE;
    }
    *bar = m_HostVisibleBar;
    *offset = m_HostVisibleOffset;
    *size = m_HostVisibleSize;
    return TRUE;
}

NTSTATUS CPciResources::MapHostVisibleAddress(_In_ ULONGLONG regionOffset,
                                              _In_ SIZE_T length,
                                              _Outptr_result_bytebuffer_(length) PVOID *address)
{
    if (address == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *address = NULL;
    if (m_pDxgkInterface == nullptr || m_pDxgkInterface->DxgkCbMapMemory == nullptr ||
        m_HostVisibleBar >= PCI_TYPE0_ADDRESSES || length == 0 || length > MAXULONG ||
        (length & (PAGE_SIZE - 1)) != 0 || (regionOffset & (PAGE_SIZE - 1)) != 0 || regionOffset > m_HostVisibleSize ||
        length > m_HostVisibleSize - regionOffset || m_HostVisibleOffset > MAXULONGLONG - regionOffset)
    {
        return STATUS_INVALID_PARAMETER;
    }

    CPciBar *bar = &m_Bars[m_HostVisibleBar];
    ULONGLONG barSize = bar->GetSize();
    if (m_HostVisibleOffset > MAXULONGLONG - regionOffset)
    {
        return STATUS_INVALID_PARAMETER;
    }
    ULONGLONG barOffset = m_HostVisibleOffset + regionOffset;
    if (barSize > MAXULONG || barOffset > barSize || length > barSize - barOffset)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ULONGLONG requestedEnd = regionOffset + static_cast<ULONGLONG>(length);
    if (requestedEnd < regionOffset)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (m_HostVisibleMappedVA == nullptr)
    {
        // Map from the first requested control slot through the end of the
        // host-visible region.  crosvm leaves the BAR prefix as an unmapped
        // guard, so mapping the entire BAR can raise an external abort.
        ULONGLONG mappedSize = m_HostVisibleSize - regionOffset;
        if (mappedSize == 0 || mappedSize > MAXULONG || m_HostVisibleOffset > MAXULONGLONG - regionOffset)
        {
            return STATUS_INVALID_PARAMETER;
        }
        if (bar->GetPA().QuadPart < 0 || static_cast<ULONGLONG>(bar->GetPA().QuadPart) > MAXULONGLONG - barOffset)
        {
            return STATUS_INVALID_PARAMETER;
        }

        PHYSICAL_ADDRESS mappedPA = bar->GetPA();
        mappedPA.QuadPart += barOffset;
        PVOID mappedVA = nullptr;
        NTSTATUS status = m_pDxgkInterface->DxgkCbMapMemory(m_pDxgkInterface->DeviceHandle,
                                                            mappedPA,
                                                            static_cast<ULONG>(mappedSize),
                                                            FALSE,
                                                            FALSE,
                                                            // The shared-memory BAR is ordinary host RAM, not device
                                                            // registers. Keep the mapping cacheable on ARM64; the
                                                            // control protocol uses explicit barriers for
                                                            // producer/consumer order.
                                                            MmCached,
                                                            &mappedVA);
        if (!NT_SUCCESS(status) || mappedVA == nullptr)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("[%s] failed to map host-visible suffix at %I64x length %I64x, status 0x%X\n",
                      __FUNCTION__,
                      mappedPA.QuadPart,
                      mappedSize,
                      status));
            return NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
        }
        m_HostVisibleMappedVA = mappedVA;
        m_HostVisibleMappedOffset = regionOffset;
        m_HostVisibleMappedSize = mappedSize;
    }
    else if (regionOffset < m_HostVisibleMappedOffset ||
             requestedEnd - m_HostVisibleMappedOffset > m_HostVisibleMappedSize)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *address = static_cast<PUCHAR>(m_HostVisibleMappedVA) + (regionOffset - m_HostVisibleMappedOffset);
    return STATUS_SUCCESS;
}

NTSTATUS CPciResources::UnmapHostVisibleAddress(_In_ PVOID address)
{
    if (address == NULL || m_HostVisibleBar >= PCI_TYPE0_ADDRESSES)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (m_HostVisibleMappedVA == nullptr || m_HostVisibleMappedSize == 0)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    PUCHAR begin = static_cast<PUCHAR>(m_HostVisibleMappedVA);
    PUCHAR end = begin + m_HostVisibleMappedSize;
    PUCHAR candidate = static_cast<PUCHAR>(address);
    if (candidate < begin || candidate >= end)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // This is an alias into the shared suffix mapping, not an independent
    // mapping. CPciResources::Close releases the mapping after owners retire.
    return STATUS_SUCCESS;
}

PVOID CPciResources::GetMappedAddress(UINT bar, ULONG uOffset)
{
    PVOID BaseVA = nullptr;
    ASSERT(bar < PCI_TYPE0_ADDRESSES);

    if (uOffset < m_Bars[bar].GetSize())
    {
        BaseVA = m_Bars[bar].GetVA(m_pDxgkInterface);
    }
    if (BaseVA != nullptr)
    {
        if (m_Bars[bar].IsPortSpace())
        {
            // use physical address for port I/O
            return (PUCHAR)(ULONG_PTR)m_Bars[bar].GetPA().LowPart + uOffset;
        }
        else
        {
            // use virtual address for memory I/O
            return (PUCHAR)BaseVA + uOffset;
        }
    }
    else
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("[%s] failed to map BAR %d, offset %x\n", __FUNCTION__, bar, uOffset));
        return nullptr;
    }
}

PAGED_CODE_SEG_BEGIN
NTSTATUS
MapFrameBuffer(_In_ PHYSICAL_ADDRESS PhysicalAddress,
               _In_ ULONG Length,
               _Outptr_result_bytebuffer_(Length) VOID **VirtualAddress)
{
    PAGED_CODE();

    if ((PhysicalAddress.QuadPart == (ULONGLONG)0) || (Length == 0) || (VirtualAddress == NULL))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("One of PhysicalAddress.QuadPart (0x%I64x), Length (%lu), VirtualAddress (%p) is NULL or 0\n",
                  PhysicalAddress.QuadPart,
                  Length,
                  VirtualAddress));
        return STATUS_INVALID_PARAMETER;
    }

    *VirtualAddress = MmMapIoSpace(PhysicalAddress, Length, MmWriteCombined);
    if (*VirtualAddress == NULL)
    {

        *VirtualAddress = MmMapIoSpace(PhysicalAddress, Length, MmNonCached);
        if (*VirtualAddress == NULL)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("MmMapIoSpace returned a NULL buffer when trying to allocate %lu bytes", Length));
            return STATUS_NO_MEMORY;
        }
    }

    DbgPrint(TRACE_LEVEL_FATAL,
             ("%s PhysicalAddress.QuadPart (0x%I64x), Length (%lu), VirtualAddress (%p)\n",
              __FUNCTION__,
              PhysicalAddress.QuadPart,
              Length,
              VirtualAddress));
    return STATUS_SUCCESS;
}

NTSTATUS
UnmapFrameBuffer(_In_reads_bytes_(Length) VOID *VirtualAddress, _In_ ULONG Length)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_FATAL, ("%s Length (%lu), VirtualAddress (%p)\n", __FUNCTION__, Length, VirtualAddress));
    if ((VirtualAddress == NULL) && (Length == 0))
    {
        return STATUS_SUCCESS;
    }
    else if ((VirtualAddress == NULL) || (Length == 0))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("Only one of Length (%lu), VirtualAddress (%p) is NULL or 0", Length, VirtualAddress));
        return STATUS_INVALID_PARAMETER;
    }

    MmUnmapIoSpace(VirtualAddress, Length);

    return STATUS_SUCCESS;
}
PAGED_CODE_SEG_END
