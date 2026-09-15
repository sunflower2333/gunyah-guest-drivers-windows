/*
 * Interrupt related functions
 *
 * Copyright (c) 2016-2017 Red Hat, Inc.
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
#include "IsrDpc.tmh"
#endif

// Enable callbacks for the two existing queues; do not change the IRQ mode.
static VOID VIOInputEnableInterrupt(PINPUT_DEVICE pContext)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INTERRUPT, "--> %s enable\n", __FUNCTION__);

    if (!pContext)
    {
        return;
    }

    if (pContext->EventQ)
    {
        virtqueue_enable_cb(pContext->EventQ);
        virtqueue_kick(pContext->EventQ);
    }
    if (pContext->StatusQ)
    {
        virtqueue_enable_cb(pContext->StatusQ);
        virtqueue_kick(pContext->StatusQ);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INTERRUPT, "<-- %s enable\n", __FUNCTION__);
}

// Disable callbacks without accessing a queue that has not been created.
static VOID VIOInputDisableInterrupt(PINPUT_DEVICE pContext)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INTERRUPT, "--> %s disable\n", __FUNCTION__);

    if (!pContext)
    {
        return;
    }

    if (pContext->EventQ)
    {
        virtqueue_disable_cb(pContext->EventQ);
    }
    if (pContext->StatusQ)
    {
        virtqueue_disable_cb(pContext->StatusQ);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INTERRUPT, "<-- %s disable\n", __FUNCTION__);
}

// Enable device queue notifications through the existing WDF interrupt.
NTSTATUS VIOInputInterruptEnable(
    IN WDFINTERRUPT Interrupt,
    IN WDFDEVICE AssociatedDevice)
{
    UNREFERENCED_PARAMETER(AssociatedDevice);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INTERRUPT, "--> %s\n", __FUNCTION__);
    VIOInputEnableInterrupt(GetDeviceContext(WdfInterruptGetDevice(Interrupt)));
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INTERRUPT, "<-- %s\n", __FUNCTION__);
    return STATUS_SUCCESS;
}

// Disable device queue notifications through the existing WDF interrupt.
NTSTATUS VIOInputInterruptDisable(
    IN WDFINTERRUPT Interrupt,
    IN WDFDEVICE AssociatedDevice)
{
    UNREFERENCED_PARAMETER(AssociatedDevice);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INTERRUPT, "--> %s\n", __FUNCTION__);
    VIOInputDisableInterrupt(GetDeviceContext(WdfInterruptGetDevice(Interrupt)));
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INTERRUPT, "<-- %s\n", __FUNCTION__);
    return STATUS_SUCCESS;
}

// Only identify our interrupt and schedule deferred queue work in the ISR.
BOOLEAN VIOInputInterruptIsr(
    IN WDFINTERRUPT Interrupt,
    IN ULONG MessageID)
{
    PINPUT_DEVICE pContext = GetDeviceContext(WdfInterruptGetDevice(Interrupt));
    WDF_INTERRUPT_INFO info;
    BOOLEAN serviced;

    UNREFERENCED_PARAMETER(MessageID);
    WDF_INTERRUPT_INFO_INIT(&info);
    WdfInterruptGetInfo(Interrupt, &info);

    // A shared legacy interrupt must actually belong to this device.
    if (info.MessageSignaled || VirtIOWdfGetISRStatus(&pContext->VDevice))
    {
        WdfInterruptQueueDpcForIsr(Interrupt);
        serviced = TRUE;
    }
    else
    {
        serviced = FALSE;
    }

    return serviced;
}

// Drain input and output completions; never complete a request under StatusQLock.
VOID VIOInputQueuesInterruptDpc(
    IN WDFINTERRUPT Interrupt,
    IN WDFOBJECT AssociatedObject)
{
    WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
    PINPUT_DEVICE pContext = GetDeviceContext(Device);
    PVIRTIO_INPUT_EVENT pEvent;
    PVIRTIO_INPUT_EVENT_WITH_REQUEST pEventReq;
    WDFREQUEST request;
    UINT len;
    ULONG invalidEvents = 0;
    ULONG repostFailures = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(AssociatedObject);

    WdfSpinLockAcquire(pContext->EventQLock);
    while (pContext->EventQ &&
           (pEvent = virtqueue_get_buf(pContext->EventQ, &len)) != NULL)
    {
        // We posted exactly one 8-byte event. A short completion must not
        // replay stale bytes from the previous use of the DMA buffer.
        if (len == sizeof(*pEvent))
        {
            ProcessInputEvent(pContext, pEvent);
        }
        else
        {
            ++invalidEvents;
        }

        status = VIOInputAddInBuf(
            pContext->EventQ,
            pEvent,
            VirtIOWdfDeviceGetPhysicalAddress(&pContext->VDevice.VIODevice, pEvent));
        if (!NT_SUCCESS(status))
        {
            // The failed add never transferred ownership back to the host.
            pContext->EventQMemBlock->return_slice(pContext->EventQMemBlock, pEvent);
            ++repostFailures;
        }
    }
    WdfSpinLockRelease(pContext->EventQLock);

    if (invalidEvents || repostFailures)
    {
        TraceEvents(TRACE_LEVEL_WARNING,
                    DBG_DPC,
                    "Input completion errors\ninvalid_events=%lu\nrepost_failures=%lu\n",
                    invalidEvents,
                    repostFailures);
    }

    for (;;)
    {
        WdfSpinLockAcquire(pContext->StatusQLock);
        pEventReq = pContext->StatusQ ? virtqueue_get_buf(pContext->StatusQ, &len) : NULL;
        if (!pEventReq)
        {
            WdfSpinLockRelease(pContext->StatusQLock);
            break;
        }

        // used length is NOT an Android playback result. The status buffer is
        // device-readable, so a zero used length is also a valid completion.
        request = pEventReq->Request;
        pEventReq->Request = NULL;
        pContext->StatusQMemBlock->return_slice(pContext->StatusQMemBlock, pEventReq);
        WdfSpinLockRelease(pContext->StatusQLock);

        // Completion can invoke upper layers and cause another output request.
        // The buffer is already reclaimed; nothing below touches its old cookie.
        if (request != NULL)
        {
            WdfRequestComplete(request, STATUS_SUCCESS);
        }
    }
}
