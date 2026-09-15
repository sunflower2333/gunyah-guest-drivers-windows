/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 DroidVM contributors
 * Dedicated, capability-gated XInputHID gamepad. No changes to generic HID devices.
 */
#include "precomp.h"
#include "vioinput.h"
#if defined(EVENT_TRACING)
#include "HidGamepad.tmh"
#endif

typedef struct INPUT_CLASS_GAMEPAD
{
    INPUT_CLASS_COMMON Common;
    DvhPad Pad;
} INPUT_CLASS_GAMEPAD, *PINPUT_CLASS_GAMEPAD;

// Translate input and reset dropped synchronization frames without replaying held buttons.
static NTSTATUS HIDGamepadEventToReport(PINPUT_CLASS_COMMON Class, PVIRTIO_INPUT_EVENT Event)
{
    PINPUT_CLASS_GAMEPAD gamepad = (PINPUT_CLASS_GAMEPAD)Class;
    BOOLEAN changed = FALSE;
    if (Event->type == EV_SYN && Event->code == 3) // SYN_DROPPED
    {
        DvhPadNeutral(&gamepad->Pad);
        changed = TRUE;
    }
    else
    {
        changed = DvhPadEvent(&gamepad->Pad, Event->type, Event->code, (LONG)Event->value) ? TRUE : FALSE;
    }
    if (changed)
    {
        RtlCopyMemory(Class->pHidReport, gamepad->Pad.report, DVH_PAD_INPUT_BYTES);
        Class->bDirty = TRUE;
    }
    return STATUS_SUCCESS;
}

// Create one complete standard gamepad; do not let generic probes consume its axes or buttons.
NTSTATUS HIDGamepadProbe(
    PINPUT_DEVICE pContext,
    PDYNAMIC_ARRAY Descriptor,
    PVIRTIO_INPUT_CFG_DATA Axes,
    PVIRTIO_INPUT_CFG_DATA Buttons)
{
    static const ULONG axes[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ};
    PINPUT_CLASS_GAMEPAD gamepad;
    ULONG i;
    NTSTATUS status;
    if (!InputCfgDataHasBit(Buttons, BTN_GAMEPAD) || pContext->uNumOfClasses != 0)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    gamepad = VIOInputAlloc(sizeof(*gamepad));
    if (!gamepad)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (i = 0; i < ARRAYSIZE(axes); ++i)
    {
        struct virtio_input_absinfo info;
        if (!InputCfgDataHasBit(Axes, axes[i]))
        {
            VIOInputFree((PVOID *)&gamepad);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        GetAbsAxisInfo(pContext, axes[i], &info);
        gamepad->Pad.minimum[i] = (LONG)info.min;
        gamepad->Pad.maximum[i] = (LONG)info.max;
        if (gamepad->Pad.maximum[i] <= gamepad->Pad.minimum[i])
        {
            VIOInputFree((PVOID *)&gamepad);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    if (!DynamicArrayAppend(Descriptor, (PVOID)DvhGamepadDescriptor, sizeof(DvhGamepadDescriptor)))
    {
        VIOInputFree((PVOID *)&gamepad);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    gamepad->Common.uReportID = 1;
    gamepad->Common.cbHidReportSize = DVH_PAD_INPUT_BYTES;
    gamepad->Common.EventToReportFunc = HIDGamepadEventToReport;
    status = RegisterClass(pContext, &gamepad->Common);
    if (!NT_SUCCESS(status))
    {
        VIOInputFree((PVOID *)&gamepad);
        return status;
    }
    DvhPadNeutral(&gamepad->Pad);
    RtlCopyMemory(gamepad->Common.pHidReport, gamepad->Pad.report, DVH_PAD_INPUT_BYTES);
    gamepad->Common.bDirty = TRUE;
    pContext->Haptics.Enabled = TRUE;
    return STATUS_SUCCESS;
}

// Clear latched controls across power transitions; caller excludes the event DPC.
VOID HIDGamepadReset(PINPUT_DEVICE pContext)
{
    if (pContext->Haptics.Enabled && pContext->uNumOfClasses == 1)
    {
        PINPUT_CLASS_GAMEPAD gamepad = (PINPUT_CLASS_GAMEPAD)pContext->InputClasses[0];
        DvhPadNeutral(&gamepad->Pad);
        RtlCopyMemory(gamepad->Common.pHidReport, gamepad->Pad.report, DVH_PAD_INPUT_BYTES);
        gamepad->Common.bDirty = TRUE;
    }
}

// Serve the standard input snapshot without fabricating feature or output reports.
NTSTATUS HIDGamepadGetInput(PINPUT_DEVICE pContext, PHID_XFER_PACKET Packet)
{
    if (!Packet || Packet->reportId != 1 || !Packet->reportBuffer || Packet->reportBufferLen < DVH_PAD_INPUT_BYTES)
    {
        return STATUS_INVALID_PARAMETER;
    }
    WdfSpinLockAcquire(pContext->EventQLock);
    RtlCopyMemory(Packet->reportBuffer, pContext->InputClasses[0]->pHidReport, DVH_PAD_INPUT_BYTES);
    WdfSpinLockRelease(pContext->EventQLock);
    return STATUS_SUCCESS;
}
