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
        if (!virtqueue_enable_cb(pContext->EventQ))
        {
            WdfInterruptQueueDpcForIsr(pContext->QueuesInterrupt);
        }
        virtqueue_kick(pContext->EventQ);
    }
    if (pContext->StatusQ)
    {
        if (!virtqueue_enable_cb(pContext->StatusQ))
        {
            WdfInterruptQueueDpcForIsr(pContext->QueuesInterrupt);
        }
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

// Drain bounded batches and close notification races; complete output requests outside all locks.
VOID VIOInputQueuesInterruptDpc(IN WDFINTERRUPT Interrupt, IN WDFOBJECT AssociatedObject)
{
    PINPUT_DEVICE pContext = GetDeviceContext(WdfInterruptGetDevice(Interrupt));
    PVIRTIO_INPUT_EVENT pEvent;
    PVOID cookie;
    WDFREQUEST request;
    UINT len;
    ULONG invalidEvents = 0, repostFailures = 0, budget = 256;
    BOOLEAN reschedule = FALSE, more;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(AssociatedObject);

    if (!InterlockedCompareExchange(&pContext->QueuesRunning, 0, 0))
    {
        return;
    }
    WdfSpinLockAcquire(pContext->EventQLock);
    while (pContext->EventQ)
    {
        virtqueue_disable_cb(pContext->EventQ);
        while (budget && (pEvent = virtqueue_get_buf(pContext->EventQ, &len)) != NULL)
        {
            --budget;
            if (len == sizeof(*pEvent))
            {
                ProcessInputEvent(pContext, pEvent);
            }
            else
            {
                ++invalidEvents;
            }
            status = VIOInputAddInBuf(pContext->EventQ, pEvent,
                                     VirtIOWdfDeviceGetPhysicalAddress(&pContext->VDevice.VIODevice, pEvent));
            if (!NT_SUCCESS(status))
            {
                pContext->EventQMemBlock->return_slice(pContext->EventQMemBlock, pEvent);
                ++repostFailures;
            }
        }
        if (virtqueue_enable_cb(pContext->EventQ))
        {
            break;
        }
        if (!budget)
        {
            reschedule = TRUE;
            break;
        }
    }
    WdfSpinLockRelease(pContext->EventQLock);
    if (invalidEvents || repostFailures)
    {
        TraceEvents(TRACE_LEVEL_WARNING, DBG_DPC,
                    "Input completion errors\ninvalid_events=%lu\nrepost_failures=%lu\n",
                    invalidEvents, repostFailures);
    }

    budget = 64;
    for (;;)
    {
        WdfSpinLockAcquire(pContext->StatusQLock);
        if (!pContext->StatusQ)
        {
            WdfSpinLockRelease(pContext->StatusQLock);
            break;
        }
        virtqueue_disable_cb(pContext->StatusQ);
        cookie = budget ? virtqueue_get_buf(pContext->StatusQ, &len) : NULL;
        if (!cookie)
        {
            more = !virtqueue_enable_cb(pContext->StatusQ);
            WdfSpinLockRelease(pContext->StatusQLock);
            if (more && budget)
            {
                continue; // a used entry arrived between draining and rearming
            }
            reschedule = reschedule || more;
            break;
        }
        --budget;
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
            // used completion returns DMA ownership, not proof of Android playback.
            WdfRequestComplete(request, STATUS_SUCCESS);
        }
    }
    if (reschedule && InterlockedCompareExchange(&pContext->QueuesRunning, 0, 0))
    {
        WdfInterruptQueueDpcForIsr(Interrupt);
    }
}
