/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 DroidVM contributors
 */
#pragma once
#include "GamepadCore.h"

#define VIOINPUT_HAPTICS_SLOTS 16u
#define VIOINPUT_HAPTICS_CONTROL_SLOTS 2u

typedef struct VIOINPUT_HAPTICS_SLOT
{
    BOOLEAN Busy;
    WDFREQUEST Request;
    ULONGLONG PublishedMs;
} VIOINPUT_HAPTICS_SLOT, *PVIOINPUT_HAPTICS_SLOT;

typedef struct VIOINPUT_HAPTICS
{
    BOOLEAN Enabled;
    BOOLEAN Closing;
    BOOLEAN StopPending;
    BOOLEAN ReadySent;
    PUCHAR Wire;
    PHYSICAL_ADDRESS WirePa;
    WDFTIMER Timer;
    DvhCaps Caps;
    DvhGuestState State;
    DvhReceiver Receiver;
    ULONGLONG LastEpoch;
    ULONGLONG HostSequence;
    ULONGLONG LastSendMs;
    ULONGLONG ReadyDeadlineMs;
    VIOINPUT_HAPTICS_SLOT Slots[VIOINPUT_HAPTICS_SLOTS];
} VIOINPUT_HAPTICS;

typedef struct VIOINPUT_HAPTICS_REQUEST
{
    struct _tagInputDevice *Owner;
    PVIOINPUT_HAPTICS_SLOT Slot;
    ULONGLONG Epoch;
    ULONGLONG Revision;
} VIOINPUT_HAPTICS_REQUEST, *PVIOINPUT_HAPTICS_REQUEST;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VIOINPUT_HAPTICS_REQUEST, GetHapticsRequest)
