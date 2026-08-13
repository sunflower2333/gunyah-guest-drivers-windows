/*
 * DroidVM Pre-Shared Pool Provider - Internal Definitions
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "droidvmpool_interface.h"

typedef struct _DROIDVMPOOL_DEVICE_CONTEXT
{
    PHYSICAL_ADDRESS PoolPhysicalBase;
    SIZE_T PoolSize;
    PVOID PoolVirtualBase;
    ULONG PoolNameLength;
    CHAR PoolName[DROIDVMPOOL_NAME_CAPACITY];
    EX_RUNDOWN_REF MappingReferences;
    BOOLEAN MappingRundownCompleted;
    volatile LONG PoolReady;
} DROIDVMPOOL_DEVICE_CONTEXT, *PDROIDVMPOOL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DROIDVMPOOL_DEVICE_CONTEXT, DroidVmPoolGetDeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD DroidVmPoolEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE DroidVmPoolEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE DroidVmPoolEvtDeviceReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL DroidVmPoolEvtIoDeviceControl;

VOID DroidVmPoolInterfaceReference(_In_ PVOID context);
VOID DroidVmPoolInterfaceDereference(_In_ PVOID context);
BOOLEAN DroidVmPoolAcquireMapping(_In_ PVOID context, _Out_ PDROIDVMPOOL_MAPPING mapping);
VOID DroidVmPoolReleaseMapping(_In_ PVOID context);
