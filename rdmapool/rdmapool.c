/*
 * Restricted DMA Pool Driver - Main Implementation
 *
 * KMDF driver that manages a restricted DMA memory pool for protected VM
 * environments. Matches ACPI device RDMA0000, reads Memory32Fixed resource
 * from _CRS, maps the physical region, and provides a bitmap-based page
 * allocator accessible via IOCTLs.
 *
 * Other kernel drivers (e.g., VirtIO) discover this driver via its device
 * interface GUID and send IOCTLs to allocate/free DMA memory from the
 * restricted pool.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "rdmapool.h"
#include <initguid.h>
#include "rdmapool_interface.h"
#include "dmapool.h"

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "rdmapool: DriverEntry\n");

    WDF_DRIVER_CONFIG_INIT(&config, RdmaPoolEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "rdmapool: WdfDriverCreate failed 0x%x\n", status);
    }

    return status;
}

NTSTATUS
RdmaPoolEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDFQUEUE queue;

    UNREFERENCED_PARAMETER(Driver);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "rdmapool: EvtDeviceAdd\n");

    /* Set up PnP/Power callbacks */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = RdmaPoolEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = RdmaPoolEvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, RdmaPoolEvtDeviceFileCreate, RdmaPoolEvtFileClose, WDF_NO_EVENT_CALLBACK);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, RDMAPOOL_FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &fileAttributes);

    /* Create the device */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, RDMAPOOL_DEVICE_CONTEXT);
    deviceAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "rdmapool: WdfDeviceCreate failed 0x%x\n", status);
        return status;
    }

    /* Create device interface so clients can discover us */
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_RDMAPOOL, NULL);
    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "rdmapool: WdfDeviceCreateDeviceInterface failed 0x%x\n",
                   status);
        return status;
    }

    /* Create default I/O queue for IOCTL dispatch */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = RdmaPoolEvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "rdmapool: WdfIoQueueCreate failed 0x%x\n", status);
        return status;
    }

    return STATUS_SUCCESS;
}

VOID RdmaPoolEvtDeviceFileCreate(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ WDFFILEOBJECT FileObject)
{
    PRDMAPOOL_FILE_CONTEXT fileContext = RdmaPoolGetFileContext(FileObject);

    UNREFERENCED_PARAMETER(Device);

    fileContext->Closing = FALSE;
    fileContext->InitializingCount = 0;
    KeInitializeEvent(&fileContext->NoInitializersEvent, NotificationEvent, TRUE);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID RdmaPoolEvtFileClose(_In_ WDFFILEOBJECT FileObject)
{
    PRDMAPOOL_FILE_CONTEXT fileContext = RdmaPoolGetFileContext(FileObject);
    ULONG released = DmaPoolCloseOwner(fileContext);

    if (released != 0)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_WARNING_LEVEL,
                   "rdmapool: file close reclaimed %u allocation(s)\n",
                   released);
    }
}

NTSTATUS
RdmaPoolEvtDevicePrepareHardware(_In_ WDFDEVICE Device,
                                 _In_ WDFCMRESLIST ResourcesRaw,
                                 _In_ WDFCMRESLIST ResourcesTranslated)
{
    NTSTATUS status = STATUS_NOT_FOUND;
    PRDMAPOOL_DEVICE_CONTEXT devCtx;
    ULONG i, count;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    devCtx = RdmaPoolGetDeviceContext(Device);

    count = WdfCmResourceListGetCount(ResourcesTranslated);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "rdmapool: PrepareHardware, %u resources\n", count);

    /* Find the Memory32Fixed resource from ACPI _CRS */
    for (i = 0; i < count; i++)
    {
        desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc == NULL)
        {
            continue;
        }

        if (desc->Type == CmResourceTypeMemory)
        {
            devCtx->PoolPhysicalBase = desc->u.Memory.Start;
            devCtx->PoolSize = desc->u.Memory.Length;

            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_INFO_LEVEL,
                       "rdmapool: Found memory resource PA=0x%llx Size=0x%llx\n",
                       devCtx->PoolPhysicalBase.QuadPart,
                       (ULONG64)devCtx->PoolSize);

            /* Map the physical region into kernel virtual address space.
             * Use cached mapping: this is real RAM (shared region in pVM),
             * not device MMIO.  Uncached (PAGE_NOCACHE) would map as Device
             * memory on ARM64, which forbids DC ZVA and requires strict
             * alignment for SIMD stores, causing BSOD 0x7E in memset/RtlZeroMemory. */
#if defined(NTDDI_WINTHRESHOLD) && (NTDDI_VERSION >= NTDDI_WINTHRESHOLD)
            devCtx->PoolVirtualBase = MmMapIoSpaceEx(devCtx->PoolPhysicalBase, devCtx->PoolSize, PAGE_READWRITE);
#else
            devCtx->PoolVirtualBase = MmMapIoSpace(devCtx->PoolPhysicalBase, devCtx->PoolSize, MmCached);
#endif
            if (devCtx->PoolVirtualBase == NULL)
            {
                DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "rdmapool: MmMapIoSpace failed\n");
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "rdmapool: Mapped pool VA=%p\n", devCtx->PoolVirtualBase);

            /* Initialize the bitmap allocator */
            status = DmaPoolInit(devCtx->PoolPhysicalBase, devCtx->PoolVirtualBase, devCtx->PoolSize);

            if (NT_SUCCESS(status))
            {
                devCtx->PoolInitialized = TRUE;
            }
            else
            {
                MmUnmapIoSpace(devCtx->PoolVirtualBase, devCtx->PoolSize);
                devCtx->PoolVirtualBase = NULL;
            }

            break;
        }
    }

    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "rdmapool: No suitable memory resource found\n");
    }

    return status;
}

NTSTATUS
RdmaPoolEvtDeviceReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PRDMAPOOL_DEVICE_CONTEXT devCtx;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    devCtx = RdmaPoolGetDeviceContext(Device);

    if (devCtx->PoolInitialized)
    {
        DmaPoolDestroy();
        devCtx->PoolInitialized = FALSE;
    }

    if (devCtx->PoolVirtualBase != NULL)
    {
        MmUnmapIoSpace(devCtx->PoolVirtualBase, devCtx->PoolSize);
        devCtx->PoolVirtualBase = NULL;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "rdmapool: ReleaseHardware done\n");

    return STATUS_SUCCESS;
}

VOID RdmaPoolEvtIoDeviceControl(_In_ WDFQUEUE Queue,
                                _In_ WDFREQUEST Request,
                                _In_ size_t OutputBufferLength,
                                _In_ size_t InputBufferLength,
                                _In_ ULONG IoControlCode)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t bytesReturned = 0;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PRDMAPOOL_DEVICE_CONTEXT devCtx = RdmaPoolGetDeviceContext(device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    PRDMAPOOL_FILE_CONTEXT fileContext = fileObject != NULL ? RdmaPoolGetFileContext(fileObject) : NULL;

    if (WdfRequestGetRequestorMode(Request) != KernelMode)
    {
        WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
        return;
    }

    if (!devCtx->PoolInitialized)
    {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    switch (IoControlCode)
    {

        case IOCTL_RDMAPOOL_ALLOCATE:
            {
                PRDMAPOOL_ALLOCATE_INPUT input;
                PRDMAPOOL_ALLOCATE_OUTPUT output;
                RDMAPOOL_ALLOCATE_INPUT inputValue;
                RDMAPOOL_ALLOCATE_OUTPUT outputValue;

                if (InputBufferLength != sizeof(RDMAPOOL_ALLOCATE_INPUT) ||
                    OutputBufferLength != sizeof(RDMAPOOL_ALLOCATE_OUTPUT))
                {
                    status = STATUS_INFO_LENGTH_MISMATCH;
                    break;
                }

                status = WdfRequestRetrieveInputBuffer(Request, sizeof(*input), (PVOID *)&input, NULL);
                if (!NT_SUCCESS(status))
                {
                    break;
                }

                status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID *)&output, NULL);
                if (!NT_SUCCESS(status))
                {
                    break;
                }

                inputValue = *input;
                if (inputValue.NumPages == 0)
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }

                if (fileContext == NULL || inputValue.InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2)
                {
                    status = STATUS_REVISION_MISMATCH;
                    break;
                }

                RtlZeroMemory(&outputValue, sizeof(outputValue));
                status = DmaPoolAllocatePages(fileContext,
                                              inputValue.NumPages,
                                              &outputValue.VirtualAddress,
                                              &outputValue.PhysicalAddress,
                                              &outputValue.AllocationToken);

                if (NT_SUCCESS(status))
                {
                    outputValue.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
                    outputValue.NumPages = inputValue.NumPages;
                    *output = outputValue;
                    bytesReturned = sizeof(*output);
                }
                break;
            }

        case IOCTL_RDMAPOOL_FREE:
            {
                PRDMAPOOL_FREE_INPUT input;
                RDMAPOOL_FREE_INPUT inputValue;

                if (InputBufferLength != sizeof(RDMAPOOL_FREE_INPUT) || OutputBufferLength != 0)
                {
                    status = STATUS_INFO_LENGTH_MISMATCH;
                    break;
                }

                status = WdfRequestRetrieveInputBuffer(Request, sizeof(*input), (PVOID *)&input, NULL);
                if (!NT_SUCCESS(status))
                {
                    break;
                }

                inputValue = *input;
                if (fileContext == NULL || inputValue.InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2)
                {
                    status = STATUS_REVISION_MISMATCH;
                    break;
                }

                if (inputValue.VirtualAddress == NULL || inputValue.NumPages == 0 || inputValue.AllocationToken == 0)
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }

                status = DmaPoolFreePages(fileContext,
                                          inputValue.VirtualAddress,
                                          inputValue.NumPages,
                                          inputValue.AllocationToken);
                break;
            }

        case IOCTL_RDMAPOOL_QUERY_POOL:
            {
                PRDMAPOOL_QUERY_POOL_OUTPUT output;

                if (InputBufferLength != 0 || OutputBufferLength != sizeof(RDMAPOOL_QUERY_POOL_OUTPUT))
                {
                    status = STATUS_INFO_LENGTH_MISMATCH;
                    break;
                }

                status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID *)&output, NULL);
                if (!NT_SUCCESS(status))
                {
                    break;
                }

                RtlZeroMemory(output, sizeof(*output));
                DmaPoolQueryInfo(&output->BaseVirtualAddress, &output->BasePhysicalAddress, &output->TotalSize);
                output->InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
                output->PageSize = PAGE_SIZE;

                bytesReturned = sizeof(*output);
                status = STATUS_SUCCESS;
                break;
            }

        case IOCTL_RDMAPOOL_QUERY_ALLOCATION:
            {
                PRDMAPOOL_QUERY_ALLOCATION_OUTPUT output;

                if (InputBufferLength != 0 || OutputBufferLength != sizeof(RDMAPOOL_QUERY_ALLOCATION_OUTPUT))
                {
                    status = STATUS_INFO_LENGTH_MISMATCH;
                    break;
                }

                status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID *)&output, NULL);
                if (!NT_SUCCESS(status))
                {
                    break;
                }

                RtlZeroMemory(output, sizeof(*output));
                DmaPoolQueryAllocation(&output->FreePages, &output->LargestFreeRunPages);
                bytesReturned = sizeof(*output);
                status = STATUS_SUCCESS;
                break;
            }

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
