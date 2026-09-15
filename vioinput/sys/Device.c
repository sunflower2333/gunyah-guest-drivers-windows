/*
 * Device related functions
 *
 * Copyright (c) 2016-2017 Red Hat, Inc.
 *
 * Author(s):
 *  Ladi Prosek <lprosek@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "precomp.h"
#include "vioinput.h"

#if defined(EVENT_TRACING)
#include "Device.tmh"
#endif

EVT_WDF_DEVICE_PREPARE_HARDWARE VIOInputEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE VIOInputEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY VIOInputEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT VIOInputEvtDeviceD0Exit;
static EVT_WDF_IO_QUEUE_IO_STOP VIOInputEvtIoStop;
static VOID VIOInputResetQueues(PINPUT_DEVICE pContext, WDFDEVICE Device);

static NTSTATUS VIOInputInitInterruptHandling(IN WDFDEVICE hDevice);
static NTSTATUS VIOInputInitAllQueues(IN WDFOBJECT hDevice);
static VOID VIOInputShutDownAllQueues(IN WDFOBJECT WdfDevice);
static NTSTATUS VIOInputCreateChildPdo(IN WDFDEVICE hDevice);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VIOInputEvtDeviceAdd)
#pragma alloc_text(PAGE, VIOInputEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, VIOInputEvtDeviceReleaseHardware)
#pragma alloc_text(PAGE, VIOInputCreateChildPdo)
#pragma alloc_text(PAGE, VIOInputEvtDeviceD0Exit)
#endif

static NTSTATUS VIOInputInitInterruptHandling(IN WDFDEVICE hDevice)
{
    WDF_INTERRUPT_CONFIG interruptConfig;
    NTSTATUS status = STATUS_SUCCESS;
    PINPUT_DEVICE pContext = GetDeviceContext(hDevice);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_HW_ACCESS, "--> %s\n", __FUNCTION__);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_HW_ACCESS, "Failed to create control queue interrupt: %x\n", status);
        return status;
    }

    WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, VIOInputInterruptIsr, VIOInputQueuesInterruptDpc);

    interruptConfig.EvtInterruptEnable = VIOInputInterruptEnable;
    interruptConfig.EvtInterruptDisable = VIOInputInterruptDisable;

    status = WdfInterruptCreate(hDevice, &interruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &pContext->QueuesInterrupt);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_HW_ACCESS, "Failed to create general queue interrupt: %x\n", status);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<-- %s\n", __FUNCTION__);
    return status;
}

NTSTATUS
VIOInputEvtDeviceAdd(IN WDFDRIVER Driver, IN PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE hDevice;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    PINPUT_DEVICE pContext = NULL;
    WDF_IO_QUEUE_CONFIG queueConfig;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "--> %s\n", __FUNCTION__);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = VIOInputEvtDevicePrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = VIOInputEvtDeviceReleaseHardware;
    PnpPowerCallbacks.EvtDeviceD0Entry = VIOInputEvtDeviceD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = VIOInputEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, VIOINPUT_HAPTICS_REQUEST);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &Attributes);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, INPUT_DEVICE);
    Attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    status = WdfDeviceCreate(&DeviceInit, &Attributes, &hDevice);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfDeviceCreate failed - 0x%x\n", status);
        return status;
    }

    status = VIOInputInitInterruptHandling(hDevice);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "VIOInputInitInterruptHandling failed - 0x%x\n", status);
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(hDevice, &GUID_VIOINPUT_CONTROLLER, NULL);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfDeviceCreateDeviceInterface failed - 0x%x\n", status);
        return status;
    }

    pContext = GetDeviceContext(hDevice);

    pContext->EventQMemBlock = pContext->StatusQMemBlock = NULL;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

    queueConfig.EvtIoInternalDeviceControl = EvtIoDeviceControl;
    queueConfig.EvtIoStop = VIOInputEvtIoStop;

    status = WdfIoQueueCreate(hDevice, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &pContext->IoctlQueue);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfIoQueueCreate failed - 0x%x\n", status);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

    status = WdfIoQueueCreate(hDevice, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &pContext->HidQueue);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfIoQueueCreate failed - 0x%x\n", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = hDevice;
    status = WdfSpinLockCreate(&Attributes, &pContext->EventQLock);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfSpinLockCreate failed - 0x%x\n", status);
        return status;
    }
    status = WdfSpinLockCreate(&Attributes, &pContext->StatusQLock);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfSpinLockCreate failed - 0x%x\n", status);
        return status;
    }

    status = VIOInputHapticsInitialize(hDevice);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    RtlZeroMemory(&pContext->HidDeviceAttributes, sizeof(HID_DEVICE_ATTRIBUTES));
    pContext->HidDeviceAttributes.Size = sizeof(HID_DEVICE_ATTRIBUTES);
    pContext->HidDeviceAttributes.VendorID = HIDMINI_VID;
    pContext->HidDeviceAttributes.ProductID = HIDMINI_PID;
    pContext->HidDeviceAttributes.VersionNumber = HIDMINI_VERSION;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<-- %s\n", __FUNCTION__);
    return status;
}

// Free allocations only after all published descriptors have been detached.
// Release partially built class state as well as successfully prepared descriptors.
static VOID VIOInputFreeClasses(PINPUT_DEVICE pContext)
{
    ULONG i;
    for (i = 0; i < pContext->uNumOfClasses; i++)
    {
        PINPUT_CLASS_COMMON pClass = pContext->InputClasses[i];
        if (pClass->CleanupFunc)
        {
            pClass->CleanupFunc(pClass);
        }
        VIOInputFree(&pClass->pHidReport);
        VIOInputFree(&pClass);
    }
    pContext->uNumOfClasses = 0;

    VIOInputFree(&pContext->HidReportDescriptor);
    pContext->Haptics.Enabled = FALSE;
}

// Free only DMA storage for which the queues have already relinquished ownership.
static void VIOInputFreeMemBlocks(PINPUT_DEVICE pContext)
{
    VIOInputHapticsFree(pContext);
    if (pContext->EventQMemBlock)
    {
        pContext->EventQMemBlock->destroy(pContext->EventQMemBlock);
        pContext->EventQMemBlock = NULL;
    }
    if (pContext->StatusQMemBlock)
    {
        pContext->StatusQMemBlock->destroy(pContext->StatusQMemBlock);
        pContext->StatusQMemBlock = NULL;
    }
}

NTSTATUS
VIOInputEvtDevicePrepareHardware(IN WDFDEVICE Device, IN WDFCMRESLIST ResourcesRaw, IN WDFCMRESLIST ResourcesTranslated)
{
    PINPUT_DEVICE pContext = GetDeviceContext(Device);
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "--> %s\n", __FUNCTION__);

    status = VirtIOWdfInitialize(&pContext->VDevice, Device, ResourcesTranslated, NULL, VIOINPUT_DRIVER_MEMORY_TAG);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_HW_ACCESS, "VirtIOWdfInitialize failed with %x\n", status);
        return status;
    }

    pContext->EventQMemBlock = VirtIOWdfDeviceAllocDmaMemorySliced(&pContext->VDevice.VIODevice,
                                                                   PAGE_SIZE,
                                                                   sizeof(VIRTIO_INPUT_EVENT));
    pContext->StatusQMemBlock = VirtIOWdfDeviceAllocDmaMemorySliced(&pContext->VDevice.VIODevice,
                                                                    PAGE_SIZE,
                                                                    sizeof(VIRTIO_INPUT_EVENT_WITH_REQUEST));

    if (!pContext->EventQMemBlock || !pContext->StatusQMemBlock)
    {
        VIOInputFreeMemBlocks(pContext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Figure out what kind of input device this is and build a
    // corresponding HID report descriptor.
    status = VIOInputBuildReportDescriptor(pContext);
    if (NT_SUCCESS(status) && pContext->bChildPdoCreated &&
        pContext->ChildIsGamepad != pContext->Haptics.Enabled)
    {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (NT_SUCCESS(status))
    {
        status = VIOInputHapticsAllocate(pContext);
    }

    if (NT_SUCCESS(status) && !pContext->bChildPdoCreated)
    {
        // Create a child PDO with an instance path based on the
        // HID report descriptor (hash). This is to make sure that
        // the devnode won't be reused when a different virtio
        // input device is plugged into the same PCI slot.
        // viohidkmdf.sys (build by the hidpassthrough project) is
        // the FDO for the child, passing IOCTL IRPs back to us.
        status = VIOInputCreateChildPdo(Device);
        if (NT_SUCCESS(status))
        {
            pContext->bChildPdoCreated = TRUE;
            pContext->ChildIsGamepad = pContext->Haptics.Enabled;
        }
    }

    if (!NT_SUCCESS(status))
    {
        VIOInputFreeMemBlocks(pContext);
        VIOInputFreeClasses(pContext);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<-- %s\n", __FUNCTION__);
    return status;
}

static NTSTATUS VIOInputCreateChildPdo(IN WDFDEVICE hDevice)
{
    PINPUT_DEVICE pContext = GetDeviceContext(hDevice);
    PWDFDEVICE_INIT pDeviceInit = NULL;
    PPDO_EXTENSION PdoExtension;
    WDFDEVICE hChild;
    WDF_OBJECT_ATTRIBUTES pdoAttributes;
    WDF_DEVICE_PNP_CAPABILITIES PnpCaps;
    NTSTATUS status = STATUS_SUCCESS;

    DECLARE_CONST_UNICODE_STRING(deviceLocation, L"VIOINPUT");
    DECLARE_CONST_UNICODE_STRING(legacyId, L"VIOINPUT\\REV_01");
    DECLARE_CONST_UNICODE_STRING(gamepadId, L"VIOINPUT\\XINPUTHID_01");
    PCUNICODE_STRING deviceId = pContext->Haptics.Enabled ? &gamepadId : &legacyId;
    DECLARE_UNICODE_STRING_SIZE(buffer, 32);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "--> %s\n", __FUNCTION__);

    pDeviceInit = WdfPdoInitAllocate(hDevice);
    if (pDeviceInit == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    status = WdfPdoInitAssignDeviceID(pDeviceInit, deviceId);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }
    status = WdfPdoInitAddHardwareID(pDeviceInit, deviceId);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }
    status = WdfPdoInitAddCompatibleID(pDeviceInit, deviceId);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

    status = RtlUnicodeStringPrintf(&buffer, L"%08I64x", pContext->HidReportDescriptorHash);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }
    status = WdfPdoInitAssignInstanceID(pDeviceInit, &buffer);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&pdoAttributes, PDO_EXTENSION);
    status = WdfDeviceCreate(&pDeviceInit, &pdoAttributes, &hChild);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }
    pDeviceInit = NULL;

    // hide the child from Device Manager
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&PnpCaps);
    PnpCaps.NoDisplayInUI = WdfTrue;
    PnpCaps.UniqueID = WdfFalse;
    WdfDeviceSetPnpCapabilities(hChild, &PnpCaps);

    // initialize the PDO extension
    PdoExtension = PdoGetExtension(hChild);
    RtlZeroMemory(PdoExtension, sizeof(PDO_EXTENSION));
    PdoExtension->Version = PDO_EXTENSION_VERSION;
    PdoExtension->BusFdo = WdfDeviceWdmGetDeviceObject(hDevice);

    // add the child
    status = WdfFdoAddStaticChild(hDevice, hChild);
    if (!NT_SUCCESS(status))
    {
        WdfObjectDelete(hChild);
    }

Exit:
    if (pDeviceInit != NULL)
    {
        WdfDeviceInitFree(pDeviceInit);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "<-- %s\n", __FUNCTION__);
    return status;
}

NTSTATUS
VIOInputEvtDeviceReleaseHardware(IN WDFDEVICE Device, IN WDFCMRESLIST ResourcesTranslated)
{
    PINPUT_DEVICE pContext = GetDeviceContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "--> %s\n", __FUNCTION__);

    VIOInputHapticsQuiesce(pContext);
    if (pContext->EventQ || pContext->StatusQ)
    {
        VIOInputResetQueues(pContext, Device);
    }
    VIOInputFreeMemBlocks(pContext);
    status = VirtIOWdfShutdown(&pContext->VDevice);

    VIOInputFreeClasses(pContext);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<-- %s\n", __FUNCTION__);
    return status;
}

static NTSTATUS VIOInputInitAllQueues(IN WDFOBJECT Device)
{
    NTSTATUS status = STATUS_SUCCESS;
    PINPUT_DEVICE pContext = GetDeviceContext(Device);

    struct virtqueue *vqs[2];
    VIRTIO_WDF_QUEUE_PARAM params[2];

    // event
    params[0].Interrupt = pContext->QueuesInterrupt;

    // status
    params[1].Interrupt = pContext->QueuesInterrupt;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    status = VirtIOWdfInitQueues(&pContext->VDevice, 2, vqs, params);
    if (NT_SUCCESS(status))
    {
        pContext->EventQ = vqs[0];
        pContext->StatusQ = vqs[1];
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "VirtIOWdfInitQueues returned %x\n", status);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s\n", __FUNCTION__);
    return status;
}

VOID VIOInputShutDownAllQueues(IN WDFOBJECT WdfDevice)
{
    PINPUT_DEVICE pContext = GetDeviceContext(WdfDevice);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    VirtIOWdfDestroyQueues(&pContext->VDevice);
    pContext->EventQ = NULL;
    pContext->StatusQ = NULL;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s\n", __FUNCTION__);
}

// Prime a bounded number of EventQ buffers; pool exhaustion after progress is not failure.
NTSTATUS VIOInputFillEventQueue(PINPUT_DEVICE pContext)
{
    ULONG posted = 0;
    ULONG target = min(virtio_get_queue_size(pContext->EventQ), PAGE_SIZE / sizeof(VIRTIO_INPUT_EVENT));
    NTSTATUS status;
    while (posted < target)
    {
        PHYSICAL_ADDRESS pa;
        PVIRTIO_INPUT_EVENT buf = pContext->EventQMemBlock->get_slice(pContext->EventQMemBlock, &pa);
        if (!buf)
        {
            break;
        }
        WdfSpinLockAcquire(pContext->EventQLock);
        status = VIOInputAddInBuf(pContext->EventQ, buf, pa);
        WdfSpinLockRelease(pContext->EventQLock);
        if (!NT_SUCCESS(status))
        {
            pContext->EventQMemBlock->return_slice(pContext->EventQMemBlock, buf);
            break;
        }
        ++posted;
    }
    return posted ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static NTSTATUS VIOInputAddBuf(IN struct virtqueue *vq,
                               IN PVIRTIO_INPUT_EVENT buf,
                               IN PHYSICAL_ADDRESS pa,
                               IN BOOLEAN out)
{
    NTSTATUS status = STATUS_SUCCESS;
    struct VirtIOBufferDescriptor sg;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s  buf = %p, pa %I64x\n", __FUNCTION__, buf, pa.QuadPart);
    if (buf == NULL)
    {
        ASSERT(0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (vq == NULL)
    {
        ASSERT(0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    sg.physAddr = pa;
    sg.length = sizeof(VIRTIO_INPUT_EVENT);

    if (0 > virtqueue_add_buf(vq, &sg, (out ? 1 : 0), (out ? 0 : 1), buf, NULL, 0))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_QUEUEING, "<-- %s cannot add_buf\n", __FUNCTION__);
        status = STATUS_INSUFFICIENT_RESOURCES;
    }

    virtqueue_kick(vq);
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
    return status;
}

NTSTATUS
VIOInputAddInBuf(IN struct virtqueue *vq, IN PVIRTIO_INPUT_EVENT buf, IN PHYSICAL_ADDRESS pa)
{
    return VIOInputAddBuf(vq, buf, pa, FALSE);
}

NTSTATUS
VIOInputAddOutBuf(IN struct virtqueue *vq, IN PVIRTIO_INPUT_EVENT buf, IN PHYSICAL_ADDRESS pa)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_QUEUEING, "%s %p\n", __FUNCTION__, buf);
    return VIOInputAddBuf(vq, buf, pa, TRUE);
}

NTSTATUS
VIOInputEvtDeviceD0Entry(IN WDFDEVICE Device, IN WDF_POWER_DEVICE_STATE PreviousState)
{
    NTSTATUS status = STATUS_SUCCESS;
    PINPUT_DEVICE pContext = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    status = VIOInputInitAllQueues(Device);
    if (NT_SUCCESS(status))
    {
        WdfSpinLockAcquire(pContext->StatusQLock);
        InterlockedExchange(&pContext->QueuesRunning, TRUE);
        WdfSpinLockRelease(pContext->StatusQLock);
        HIDGamepadReset(pContext);
        VirtIOWdfSetDriverOK(&pContext->VDevice);
        status = VIOInputFillEventQueue(pContext);
        if (NT_SUCCESS(status))
        {
            status = VIOInputHapticsStart(pContext);
        }
        if (!NT_SUCCESS(status))
        {
            VIOInputResetQueues(pContext, Device);
            VirtIOWdfSetDriverFailed(&pContext->VDevice);
        }
    }
    else
    {
        VirtIOWdfSetDriverFailed(&pContext->VDevice);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s\n", __FUNCTION__);

    return status;
}

// Haptics stops detach requests immediately; legacy LED requests are drained by D0Exit.
static VOID VIOInputEvtIoStop(WDFQUEUE Queue, WDFREQUEST Request, ULONG ActionFlags)
{
    PINPUT_DEVICE pContext = GetDeviceContext(WdfIoQueueGetDevice(Queue));
    UNREFERENCED_PARAMETER(ActionFlags);
    if (pContext->Haptics.Enabled)
    {
        VIOInputHapticsStopRequest(pContext, Request);
        return;
    }
    WdfSpinLockAcquire(pContext->StatusQLock);
    InterlockedExchange(&pContext->QueuesRunning, FALSE);
    WdfRequestStopAcknowledge(Request, FALSE);
    WdfSpinLockRelease(pContext->StatusQLock);
}

// Reset establishes DMA quiescence; only then detach and reclaim BOTH existing queues.
static VOID VIOInputResetQueues(PINPUT_DEVICE pContext, WDFDEVICE Device)
{
    PVOID cookie;
    WDFREQUEST request;
    VIOInputHapticsQuiesce(pContext);
    WdfSpinLockAcquire(pContext->StatusQLock);
    InterlockedExchange(&pContext->QueuesRunning, FALSE);
    WdfSpinLockRelease(pContext->StatusQLock);
    // Producers are closed before this PASSIVE_LEVEL-only slow-path join.
    // DPCs queued after QueuesRunning was cleared return without touching queues.
    // KeFlushQueuedDpcs also joins the DPC that observed the old running state.
    KeFlushQueuedDpcs();
    virtio_device_reset(&pContext->VDevice.VIODevice);

    WdfSpinLockAcquire(pContext->EventQLock);
    if (pContext->EventQ)
    {
        while ((cookie = virtqueue_detach_unused_buf(pContext->EventQ)) != NULL)
        {
            pContext->EventQMemBlock->return_slice(pContext->EventQMemBlock, cookie);
        }
    }
    HIDGamepadReset(pContext);
    WdfSpinLockRelease(pContext->EventQLock);

    for (;;)
    {
        WdfSpinLockAcquire(pContext->StatusQLock);
        cookie = pContext->StatusQ ? virtqueue_detach_unused_buf(pContext->StatusQ) : NULL;
        if (!cookie)
        {
            WdfSpinLockRelease(pContext->StatusQLock);
            break;
        }
        request = NULL;
        if (!VIOInputHapticsCompleteLocked(pContext, cookie, &request))
        {
            PVIRTIO_INPUT_EVENT_WITH_REQUEST legacy = cookie;
            request = legacy->Request;
            legacy->Request = NULL;
            pContext->StatusQMemBlock->return_slice(pContext->StatusQMemBlock, cookie);
        }
        WdfSpinLockRelease(pContext->StatusQLock);
        if (request)
        {
            WdfRequestCompleteWithInformation(request, STATUS_DEVICE_NOT_READY, 0);
        }
    }
    VIOInputShutDownAllQueues(Device);
}

// No unbounded wait for the host; mandatory host lease/reset handling stops its actuator.
NTSTATUS VIOInputEvtDeviceD0Exit(IN WDFDEVICE Device, IN WDF_POWER_DEVICE_STATE TargetState)
{
    PINPUT_DEVICE pContext = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(TargetState);
    PAGED_CODE();
    VIOInputResetQueues(pContext, Device);
    return STATUS_SUCCESS;
}
