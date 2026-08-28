/*
 * Copyright (C) 2019-2022 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
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

#include "helper.h"
#include "driver.h"
#include "viogpudo.h"
#include "baseobj.h"
#include "bitops.h"
#include "viogpum.h"
#include "edid.h"

#include <intrin.h>

#if !DBG
#include "viogpudo.tmh"
#endif

static UINT g_InstanceId = 0;
static const UINT VIOGPU_SCANLINE_REFRESH_HZ = 60U;

#if defined(VIOGPU_NATIVE_CONTEXT)
extern "C" UCHAR __ImageBase;

VOID VioGpuWddmDrainPresentTransactions(_In_ VioGpuDod *adapter);

static const ULONG VIOGPU_WIN7_DRIVERCAPS_SIZE = FIELD_OFFSET(DXGK_DRIVERCAPS, PreemptionCaps);
static_assert(VIOGPU_WIN7_DRIVERCAPS_SIZE == 528, "unexpected Win7 DXGK_DRIVERCAPS prefix size");
/* crosvm's drm2kgsl arena leaves its first 2 MiB unused so a native control
 * blob never starts at the base of the Gunyah SHARE window.  The BAR mapping
 * must use the same guard-relative offset as the host backing. */
static const ULONGLONG VIOGPU_NATIVE_CONTROL_BAR_GUARD_SIZE = 2ULL << 20;
#endif

#define VIOGPU_MAX_CAPSETS               64U
#define VIOGPU_MINIMUM_MSM_VERSION_MINOR 9U

#if defined(VIOGPU_NATIVE_CONTEXT)
#define VIOGPU_RECORD_NATIVE_START(_dod, _stage, _status, _detail)                                                     \
    (_dod)->RecordNativeStartDiagnostic((_stage), (_status), (_detail))
#else
#define VIOGPU_RECORD_NATIVE_START(_dod, _stage, _status, _detail) ((void)0)
#endif

LONG ReadNativeAllocationCount(_In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner)
{
    return owner == NULL ? -1 : InterlockedCompareExchange(const_cast<volatile LONG *>(&owner->AllocationCount), 0, 0);
}

BOOLEAN TryReferenceNativeAllocationCount(_In_ VIOGPU_NATIVE_CONTEXT_OWNER *owner)
{
    if (owner == NULL)
    {
        return FALSE;
    }

    LONG observed = ReadNativeAllocationCount(owner);
    while (observed >= 0 && observed < MAXLONG)
    {
        LONG previous = InterlockedCompareExchange(&owner->AllocationCount, observed + 1, observed);
        if (previous == observed)
        {
            return TRUE;
        }
        observed = previous;
    }
    return FALSE;
}

BOOLEAN ReleaseNativeAllocationCount(_In_ VIOGPU_NATIVE_CONTEXT_OWNER *owner)
{
    if (owner == NULL)
    {
        return FALSE;
    }

    LONG observed = ReadNativeAllocationCount(owner);
    while (observed > 0)
    {
        LONG previous = InterlockedCompareExchange(&owner->AllocationCount, observed - 1, observed);
        if (previous == observed)
        {
            return TRUE;
        }
        observed = previous;
    }
    return FALSE;
}

static BOOLEAN VioGpuInterruptBarrier(_In_opt_ PVOID context)
{
    UNREFERENCED_PARAMETER(context);
    return TRUE;
}

#if defined(VIOGPU_NATIVE_CONTEXT)
struct VIOGPU_NATIVE_SCHEDULER_NOTIFICATION
{
    PDXGKRNL_INTERFACE Interface;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA Data;
};

static BOOLEAN VioGpuNotifyNativeSchedulerAtDirql(_In_opt_ PVOID context)
{
    VIOGPU_NATIVE_SCHEDULER_NOTIFICATION *notification = static_cast<VIOGPU_NATIVE_SCHEDULER_NOTIFICATION *>(context);
    if (notification == NULL || notification->Interface == NULL ||
        notification->Interface->DxgkCbNotifyInterrupt == NULL)
    {
        return FALSE;
    }

    notification->Interface->DxgkCbNotifyInterrupt(notification->Interface->DeviceHandle, &notification->Data);
    return TRUE;
}
#endif

PAGED_CODE_SEG_BEGIN
VioGpuDod::VioGpuDod(_In_ DEVICE_OBJECT *pPhysicalDeviceObject)
    : m_pPhysicalDevice(pPhysicalDeviceObject), m_MonitorPowerState(PowerDeviceD0), m_AdapterPowerState(PowerDeviceD0),
      m_pHWDevice(NULL), m_HardwareRundownCompleted(FALSE), m_HardwareResetState(VioGpuHardwareActive)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    *((UINT *)&m_Flags) = 0;
    RtlZeroMemory(&m_DxgkInterface, sizeof(m_DxgkInterface));
    RtlZeroMemory(&m_DeviceInfo, sizeof(m_DeviceInfo));
    RtlZeroMemory(&m_CurrentMode, sizeof(m_CurrentMode));
    RtlZeroMemory(&m_SystemDisplayInfo, sizeof(m_SystemDisplayInfo));
    RtlZeroMemory(&m_PointerShape, sizeof(m_PointerShape));
    ExInitializeRundownProtection(&m_HardwareOperations);
#if defined(VIOGPU_NATIVE_CONTEXT)
    KeInitializeSpinLock(&m_NativeFenceLock);
    m_HardwareResetCallerRva = 0;
    m_NativeSubmissionFaultDiagnosticRecorded = 0;
    m_NativeSubmissionFaultCallerRva = 0;
    m_NativeSubmissionFaultExecutionDiagnosticState = 0;
    m_NativeSubmissionFaultPresentSubmitStage = 0;
    m_NativeSubmissionFaultPresentSubmitStatus = 0;
    m_NativeSubmissionFaultPresentSubmitDetail = 0;
    m_NativeFenceHead = 0;
    m_NativeFenceCount = 0;
    RtlZeroMemory(m_NativeFences, sizeof(m_NativeFences));
    m_NativeSubmittedFence = 0;
    m_NativeCompletedFence = 0;
    KeInitializeSpinLock(&m_NativePassiveLock);
    InitializeListHead(&m_NativePassiveQueue);
    ExInitializeWorkItem(&m_NativePassiveWorkItem, NativePassiveWorker, this);
    m_NativePassiveWorkerQueued = FALSE;
    m_NativePassiveActiveWork = NULL;
    m_NativePassiveClosing = TRUE;
    KeInitializeEvent(&m_NativePassiveIdleEvent, NotificationEvent, TRUE);
    ExInitializeWorkItem(&m_WddmDrainWorkItem, WddmSubmissionDrainWorker, this);
    m_WddmDrainWorkerQueued = FALSE;
    m_WddmDrainRequested = FALSE;
    KeInitializeEvent(&m_WddmDrainIdleEvent, NotificationEvent, TRUE);
    KeInitializeSpinLock(&m_WddmPresentLock);
    InitializeListHead(&m_WddmPresentTransactions);
    m_WddmPresentClosing = TRUE;
    m_NativePresentDiagnosticRecorded = 0;
    m_NativePresentExecutionDiagnosticRecorded = 0;
    m_NativePresentCopyProbeState = 0;
    m_NativePresentCopyProbeSequence = 0;
#endif
    m_PersistentDispMode0Width = 0;
    m_PersistentDispMode0Height = 0;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuDod::~VioGpuDod(void)
{
    PAGED_CODE();
    if (!m_HardwareRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_HardwareOperations);
        ExRundownCompleted(&m_HardwareOperations);
        m_HardwareRundownCompleted = TRUE;
    }
    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu adapter teardown: started=%u hardware=%u\n",
               IsDriverActive(),
               IsHardwareInit());
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s 0x%p\n", __FUNCTION__, m_pHWDevice));
    // Every path that owns a hardware adapter must complete HWClose before
    // deleting this outer object. Do not bypass its final interrupt barrier.
    NT_ASSERT(m_pHWDevice == NULL);
#if defined(VIOGPU_NATIVE_CONTEXT)
    NT_ASSERT(IsListEmpty(&m_NativePassiveQueue));
    NT_ASSERT(!m_NativePassiveWorkerQueued);
    NT_ASSERT(m_NativePassiveActiveWork == NULL);
    NT_ASSERT(m_NativePassiveClosing);
    NT_ASSERT(InterlockedCompareExchange(&m_WddmDrainWorkerQueued, 0, 0) == 0);
    NT_ASSERT(InterlockedCompareExchange(&m_WddmDrainRequested, 0, 0) == 0);
    NT_ASSERT(IsListEmpty(&m_WddmPresentTransactions));
    NT_ASSERT(m_WddmPresentClosing);
#endif
}

BOOLEAN VioGpuDod::CheckHardware()
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_GRAPHICS_DRIVER_MISMATCH;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PCI_COMMON_HEADER Header = {0};
    ULONG BytesRead;

    Status = m_DxgkInterface.DxgkCbReadDeviceSpace(m_DxgkInterface.DeviceHandle,
                                                   DXGK_WHICHSPACE_CONFIG,
                                                   &Header,
                                                   0,
                                                   sizeof(Header),
                                                   &BytesRead);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkCbReadDeviceSpace failed with status 0x%X\n", Status));
        return FALSE;
    }
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s VendorId = 0x%04X DeviceId = 0x%04X\n", __FUNCTION__, Header.VendorID, Header.DeviceID));
    if (Header.VendorID == REDHAT_PCI_VENDOR_ID && Header.DeviceID == 0x1050)
    {
        SetVgaDevice(Header.SubClass == PCI_SUBCLASS_VID_VGA_CTLR);
        return TRUE;
    }

    return FALSE;
}

NTSTATUS VioGpuDod::UnwindFailedStart(_In_ NTSTATUS failureStatus)
{
    PAGED_CODE();

    InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
    if (!m_HardwareRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_HardwareOperations);
        ExRundownCompleted(&m_HardwareOperations);
        m_HardwareRundownCompleted = TRUE;
    }

    NTSTATUS closeStatus = m_pHWDevice->HWClose();
    if (NT_SUCCESS(closeStatus))
    {
        delete m_pHWDevice;
        m_pHWDevice = NULL;
        ExReInitializeRundownProtection(&m_HardwareOperations);
        m_HardwareRundownCompleted = FALSE;
    }
    return NT_SUCCESS(closeStatus) ? failureStatus : closeStatus;
}

NTSTATUS VioGpuDod::StartDevice(_In_ DXGK_START_INFO *pDxgkStartInfo,
                                _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                                _Out_ ULONG *pNumberOfViews,
                                _Out_ ULONG *pNumberOfChildren)
{
    PAGED_CODE();

    NTSTATUS Status;

    VIOGPU_ASSERT(pDxgkStartInfo != NULL);
    VIOGPU_ASSERT(pDxgkInterface != NULL);
    VIOGPU_ASSERT(pNumberOfViews != NULL);
    VIOGPU_ASSERT(pNumberOfChildren != NULL);

    VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartEntered, STATUS_PENDING, VioGpuNativeStartDetailNone);

    if (IsDriverActive())
    {
        VIOGPU_RECORD_NATIVE_START(this,
                                   VioGpuNativeStartPreconditions,
                                   STATUS_ALREADY_INITIALIZED,
                                   VioGpuNativeStartDetailNone);
        return STATUS_ALREADY_INITIALIZED;
    }
    // A non-active retained adapter means a previous unwind could not prove
    // teardown.  Preserve the DXGK interface and mode state that own it.
    if (m_pHWDevice != NULL)
    {
        VIOGPU_RECORD_NATIVE_START(this,
                                   VioGpuNativeStartPreconditions,
                                   STATUS_DEVICE_NOT_READY,
                                   VioGpuNativeStartDetailNone);
        return STATUS_DEVICE_NOT_READY;
    }
    LONG startResetState = InterlockedCompareExchange(&m_HardwareResetState,
                                                      VioGpuHardwareRecovering,
                                                      VioGpuHardwareActive);
    if (startResetState == VioGpuHardwareResetRequested)
    {
        startResetState = InterlockedCompareExchange(&m_HardwareResetState,
                                                     VioGpuHardwareRecovering,
                                                     VioGpuHardwareResetRequested);
    }
    if (startResetState != VioGpuHardwareActive && startResetState != VioGpuHardwareResetRequested)
    {
        VIOGPU_RECORD_NATIVE_START(this,
                                   VioGpuNativeStartPreconditions,
                                   STATUS_DEVICE_NOT_READY,
                                   static_cast<DWORD>(startResetState));
        return STATUS_DEVICE_NOT_READY;
    }
    VIOGPU_RECORD_NATIVE_START(this,
                               VioGpuNativeStartPreconditions,
                               STATUS_SUCCESS,
                               static_cast<DWORD>(startResetState));
#if defined(VIOGPU_NATIVE_CONTEXT)
    ResetNativeFenceTracker();
#endif

    if (pDxgkInterface->Size > sizeof(m_DxgkInterface))
    {
        RtlCopyMemory(&m_DxgkInterface, pDxgkInterface, sizeof(m_DxgkInterface));
        m_DxgkInterface.Version = DXGKDDI_INTERFACE_VERSION;
        m_DxgkInterface.Size = sizeof(m_DxgkInterface);
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("VIOGPU: Provided interface version cannot be used by Viogpudo (version %u, size %u), degrading to "
                  "version %u, size %u)\n",
                  pDxgkInterface->Version,
                  pDxgkInterface->Size,
                  m_DxgkInterface.Version,
                  m_DxgkInterface.Size));
    }
    else
    {
        RtlCopyMemory(&m_DxgkInterface, pDxgkInterface, pDxgkInterface->Size);
    }

    RtlZeroMemory(&m_CurrentMode, sizeof(m_CurrentMode));
    m_CurrentMode.DispInfo.TargetId = D3DDDI_ID_UNINITIALIZED;

    VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartDeviceInformation, STATUS_PENDING, VioGpuNativeStartDetailNone);
    Status = m_DxgkInterface.DxgkCbGetDeviceInformation(m_DxgkInterface.DeviceHandle, &m_DeviceInfo);
    if (!NT_SUCCESS(Status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu StartDevice: DxgkCbGetDeviceInformation failed, status=0x%08X\n",
                   Status);
        VIOGPU_LOG_ASSERTION1("DxgkCbGetDeviceInformation failed with status 0x%X\n", Status);
        VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartDeviceInformation, Status, VioGpuNativeStartDetailNone);
        InterlockedCompareExchange(&m_HardwareResetState, startResetState, VioGpuHardwareRecovering);
        return Status;
    }

    VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartHardwareIdentity, STATUS_PENDING, VioGpuNativeStartDetailNone);
    if (!CheckHardware())
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "viogpu StartDevice: CheckHardware failed\n");
        VIOGPU_RECORD_NATIVE_START(this,
                                   VioGpuNativeStartHardwareIdentity,
                                   STATUS_GRAPHICS_DRIVER_MISMATCH,
                                   VioGpuNativeStartDetailNone);
        InterlockedCompareExchange(&m_HardwareResetState, startResetState, VioGpuHardwareRecovering);
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    }
    VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartAdapterAllocation, STATUS_PENDING, VioGpuNativeStartDetailNone);
    m_pHWDevice = new (NonPagedPoolNx) VioGpuAdapter(this);
    if (!m_pHWDevice)
    {
        Status = STATUS_NO_MEMORY;
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu StartDevice: adapter allocation failed, status=0x%08X\n",
                   Status);
        DbgPrint(TRACE_LEVEL_ERROR, ("StartDevice failed to allocate memory\n"));
        VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartAdapterAllocation, Status, VioGpuNativeStartDetailNone);
        InterlockedCompareExchange(&m_HardwareResetState, startResetState, VioGpuHardwareRecovering);
        return Status;
    }

    VIOGPU_RECORD_NATIVE_START(this,
                               VioGpuNativeStartRegistryConfiguration,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    Status = GetRegisterInfo();
    if (!NT_SUCCESS(Status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_WARNING_LEVEL,
                   "viogpu StartDevice: GetRegisterInfo warning, status=0x%08X\n",
                   Status);
        DbgPrint(TRACE_LEVEL_WARNING, ("GetRegisterInfo failed with status 0x%X\n", Status));
    }
    VIOGPU_RECORD_NATIVE_START(this,
                               VioGpuNativeStartRegistryConfiguration,
                               STATUS_SUCCESS,
                               static_cast<DWORD>(Status));

    Status = m_pHWDevice->HWInit(m_DeviceInfo.TranslatedResourceList, &m_CurrentMode.DispInfo);
    if (!NT_SUCCESS(Status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "viogpu StartDevice: HWInit failed, status=0x%08X\n", Status);
        DbgPrint(TRACE_LEVEL_ERROR, ("HWInit failed with status 0x%X\n", Status));
        return UnwindFailedStart(Status);
    }

    m_CurrentMode.RamFrameBuffer = m_pHWDevice->GetPciResources()[0].GetMappedAddress(0, 0);
    if (!m_CurrentMode.RamFrameBuffer)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("Failed to map RamFrameBuffer for VGA mode"));
    }

    VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartHardwareInformation, STATUS_PENDING, VioGpuNativeStartDetailNone);
    Status = SetRegisterInfo(m_pHWDevice->GetInstanceId(), 0);
    if (!NT_SUCCESS(Status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu StartDevice: SetRegisterInfo failed, status=0x%08X\n",
                   Status);
        VIOGPU_LOG_ASSERTION1("RegisterHWInfo failed with status 0x%X\n", Status);
        VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartHardwareInformation, Status, VioGpuNativeStartDetailNone);
        return UnwindFailedStart(Status);
    }

    VIOGPU_RECORD_NATIVE_START(this,
                               VioGpuNativeStartPostDisplayOwnership,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    if (IsVgaDevice() && m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership)
    {
        Status = m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership(m_DxgkInterface.DeviceHandle, &m_SystemDisplayInfo);
    }

    if (!NT_SUCCESS(Status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu StartDevice: DxgkCbAcquirePostDisplayOwnership failed, status=0x%08X\n",
                   Status);
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("DxgkCbAcquirePostDisplayOwnership failed with status 0x%X Width = %d\n",
                  Status,
                  m_SystemDisplayInfo.Width));
        VioGpuDbgBreak();
        VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartPostDisplayOwnership, Status, VioGpuNativeStartDetailNone);
        return UnwindFailedStart(STATUS_UNSUCCESSFUL);
    }
    DbgPrint(TRACE_LEVEL_FATAL,
             ("DxgkCbAcquirePostDisplayOwnership Width = %d Height = %d Pitch = %d ColorFormat = %d\n",
              m_SystemDisplayInfo.Width,
              m_SystemDisplayInfo.Height,
              m_SystemDisplayInfo.Pitch,
              m_SystemDisplayInfo.ColorFormat));

    if (m_SystemDisplayInfo.Width == 0)
    {
        m_SystemDisplayInfo.Width = NOM_WIDTH_SIZE;
        m_SystemDisplayInfo.Height = NOM_HEIGHT_SIZE;
        m_SystemDisplayInfo.ColorFormat = D3DDDIFMT_X8R8G8B8;
        m_SystemDisplayInfo.Pitch = (BPPFromPixelFormat(m_SystemDisplayInfo.ColorFormat) / BITS_PER_BYTE) *
                                    m_SystemDisplayInfo.Width;
        m_SystemDisplayInfo.TargetId = 0;
        if (m_SystemDisplayInfo.PhysicAddress.QuadPart == 0LL)
        {
            m_SystemDisplayInfo.PhysicAddress = m_pHWDevice->GetFrameBufferPA();
        }
    }

    m_CurrentMode.DispInfo.Width = max(MIN_WIDTH_SIZE, m_SystemDisplayInfo.Width);
    m_CurrentMode.DispInfo.Height = max(MIN_HEIGHT_SIZE, m_SystemDisplayInfo.Height);
    m_CurrentMode.DispInfo.ColorFormat = D3DDDIFMT_X8R8G8B8;
    m_CurrentMode.DispInfo.Pitch = (BPPFromPixelFormat(m_CurrentMode.DispInfo.ColorFormat) / BITS_PER_BYTE) *
                                   m_CurrentMode.DispInfo.Width;
    m_CurrentMode.DispInfo.TargetId = 0;
    if (m_CurrentMode.DispInfo.PhysicAddress.QuadPart == 0LL && m_SystemDisplayInfo.PhysicAddress.QuadPart != 0LL)
    {
        m_CurrentMode.DispInfo.PhysicAddress = m_SystemDisplayInfo.PhysicAddress;
    }

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s ColorFormat = %d\n", __FUNCTION__, m_CurrentMode.DispInfo.ColorFormat));

    VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartFinalState, STATUS_PENDING, VioGpuNativeStartDetailNone);
    *pNumberOfViews = MAX_VIEWS;
    *pNumberOfChildren = MAX_CHILDREN;
#if defined(VIOGPU_NATIVE_CONTEXT)
    InterlockedExchange(&m_HardwareResetCallerRva, 0);
#endif
    if (InterlockedCompareExchange(&m_HardwareResetState, VioGpuHardwareActive, VioGpuHardwareRecovering) !=
        VioGpuHardwareRecovering)
    {
        VIOGPU_RECORD_NATIVE_START(this,
                                   VioGpuNativeStartFinalState,
                                   STATUS_DEVICE_NOT_READY,
                                   VioGpuNativeStartDetailNone);
        return UnwindFailedStart(STATUS_DEVICE_NOT_READY);
    }
#if defined(VIOGPU_NATIVE_CONTEXT)
    if (!OpenNativePassiveQueue() || !OpenWddmPresentTransactions())
    {
        RequestWddmSubmissionDrainAtAnyIrql();
        WaitForWddmSubmissionDrain();
        VIOGPU_RECORD_NATIVE_START(this,
                                   VioGpuNativeStartFinalState,
                                   STATUS_DEVICE_NOT_READY,
                                   VioGpuNativeStartDetailNone);
        return UnwindFailedStart(STATUS_DEVICE_NOT_READY);
    }
#endif
    m_Flags.DriverStarted = TRUE;
    VIOGPU_RECORD_NATIVE_START(this, VioGpuNativeStartComplete, STATUS_SUCCESS, VioGpuNativeStartDetailNone);
    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu StartDevice: success, dxgk version=0x%08X size=%lu views=%lu children=%lu\n",
               pDxgkInterface->Version,
               pDxgkInterface->Size,
               *pNumberOfViews,
               *pNumberOfChildren);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::StopDevice(VOID)
{
    PAGED_CODE();

    InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
#if defined(VIOGPU_NATIVE_CONTEXT)
    RequestWddmSubmissionDrainAtAnyIrql();
    if (!WaitForWddmSubmissionDrain())
    {
        return STATUS_DEVICE_NOT_READY;
    }
#endif
    if (!m_HardwareRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_HardwareOperations);
        ExRundownCompleted(&m_HardwareOperations);
        m_HardwareRundownCompleted = TRUE;
    }
    NTSTATUS status = STATUS_SUCCESS;
    if (m_pHWDevice != NULL)
    {
        status = m_pHWDevice->HWClose();
        if (NT_SUCCESS(status))
        {
            delete m_pHWDevice;
            m_pHWDevice = NULL;
        }
    }
    if (NT_SUCCESS(status))
    {
        m_Flags.DriverStarted = FALSE;
    }
    if (NT_SUCCESS(status))
    {
        ExReInitializeRundownProtection(&m_HardwareOperations);
        m_HardwareRundownCompleted = FALSE;
    }
    return status;
}

#if defined(VIOGPU_NATIVE_CONTEXT)
#pragma code_seg(push)
#pragma code_seg()
PGPU_VBUFFER VioGpuDod::PrepareNativeSubmit(_In_ UINT contextId, _In_ const void *command, _In_ UINT commandSize)
{
    if (command == NULL || commandSize == 0 || !ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return NULL;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    PGPU_VBUFFER buffer = !IsHardwareResetRequested() && adapter != NULL ? adapter->PrepareNativeSubmit(contextId,
                                                                                                        command,
                                                                                                        commandSize)
                                                                         : NULL;
    ExReleaseRundownProtection(&m_HardwareOperations);
    return buffer;
}

BOOLEAN VioGpuDod::RefreshNativeSubmit(_In_ PGPU_VBUFFER buffer, _In_ const void *command, _In_ UINT commandSize)
{
    if (buffer == NULL || command == NULL || commandSize == 0 || !ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return FALSE;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    BOOLEAN refreshed = !IsHardwareResetRequested() && adapter != NULL &&
                        adapter->RefreshNativeSubmit(buffer, command, commandSize);
    ExReleaseRundownProtection(&m_HardwareOperations);
    return refreshed;
}

int VioGpuDod::QueueNativeSubmit(_In_ PGPU_VBUFFER buffer, _In_ ULONGLONG fenceId)
{
    if (buffer == NULL || fenceId == 0 || !ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return -1;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    int result = !IsHardwareResetRequested() && adapter != NULL ? adapter->QueueNativeSubmit(buffer, fenceId) : -1;
    ExReleaseRundownProtection(&m_HardwareOperations);
    return result;
}

BOOLEAN VioGpuDod::ReleaseNativeSubmitBuffer(_In_ PGPU_VBUFFER buffer)
{
    if (buffer == NULL)
    {
        return FALSE;
    }
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        VioGpuCompleteVbufferTerminalCallbacks(buffer);
        return FALSE;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    BOOLEAN released = adapter != NULL;
    if (adapter != NULL)
    {
        adapter->ReleaseNativeSubmitBuffer(buffer);
    }
    ExReleaseRundownProtection(&m_HardwareOperations);
    if (!released)
    {
        VioGpuCompleteVbufferTerminalCallbacks(buffer);
    }
    return released;
}

BOOLEAN VioGpuDod::IsNativeContextGenerationCurrent(_In_ LONG generation, _In_ ULONGLONG resetGeneration) const
{
    if (generation <= 0 || resetGeneration == 0 || !ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return FALSE;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    BOOLEAN current = !IsHardwareResetRequested() && adapter != NULL &&
                      adapter->IsNativeContextGenerationCurrent(generation, resetGeneration);
    ExReleaseRundownProtection(&m_HardwareOperations);
    return current;
}

BOOLEAN VioGpuDod::IsNativeContextResetRetired(_In_ ULONGLONG resetGeneration) const
{
    if (resetGeneration == 0 || !ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return FALSE;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    /* A reset request deliberately leaves the adapter alive until HWClose;
     * the rundown protects this pointer while the retirement epoch is read. */
    BOOLEAN retired = adapter != NULL && adapter->IsNativeContextResetRetired(resetGeneration);
    ExReleaseRundownProtection(&m_HardwareOperations);
    return retired;
}

BOOLEAN VioGpuDod::AcquireNativeSubmissionOperation(void) const
{
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return FALSE;
    }
    VioGpuAdapter *adapter = m_pHWDevice;
    if (IsHardwareResetRequested() || adapter == NULL || !adapter->AcquireNativeSubmitOperation())
    {
        ExReleaseRundownProtection(&m_HardwareOperations);
        return FALSE;
    }
    return TRUE;
}

void VioGpuDod::ReleaseNativeSubmissionOperation(void) const
{
    VioGpuAdapter *adapter = m_pHWDevice;
    if (adapter != NULL)
    {
        adapter->ReleaseNativeSubmitOperation();
    }
    ExReleaseRundownProtection(&m_HardwareOperations);
}

BOOLEAN VioGpuDod::RecordNativeSubmissionFence(_In_ UINT fenceId)
{
    if (fenceId == 0)
    {
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeFenceLock, &oldIrql);
    UINT submitted = static_cast<UINT>(m_NativeSubmittedFence);
    BOOLEAN valid = m_NativeFenceCount < VioGpuNativeFenceTrackerCapacity &&
                    (submitted == 0 || static_cast<LONG>(fenceId - submitted) > 0);
    for (UINT offset = 0; valid && offset < m_NativeFenceCount; ++offset)
    {
        UINT index = (m_NativeFenceHead + offset) % VioGpuNativeFenceTrackerCapacity;
        if (m_NativeFences[index].State != VioGpuNativeFenceFree && m_NativeFences[index].FenceId == fenceId)
        {
            valid = FALSE;
        }
    }
    if (valid)
    {
        UINT tail = (m_NativeFenceHead + m_NativeFenceCount) % VioGpuNativeFenceTrackerCapacity;
        NT_ASSERT(m_NativeFences[tail].State == VioGpuNativeFenceFree);
        m_NativeFences[tail].FenceId = fenceId;
        m_NativeFences[tail].State = VioGpuNativeFencePending;
        ++m_NativeFenceCount;
        InterlockedExchange(&m_NativeSubmittedFence, static_cast<LONG>(fenceId));
    }
    KeReleaseSpinLock(&m_NativeFenceLock, oldIrql);
    return valid;
}

BOOLEAN VioGpuDod::RetireNativeSubmissionFence(_In_ UINT fenceId, _Out_ UINT *completedFence)
{
    if (fenceId == 0 || completedFence == NULL)
    {
        return FALSE;
    }
    *completedFence = 0;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeFenceLock, &oldIrql);
    VIOGPU_NATIVE_FENCE_ENTRY *match = NULL;
    for (UINT offset = 0; offset < m_NativeFenceCount; ++offset)
    {
        UINT index = (m_NativeFenceHead + offset) % VioGpuNativeFenceTrackerCapacity;
        if (m_NativeFences[index].State == VioGpuNativeFencePending && m_NativeFences[index].FenceId == fenceId)
        {
            match = &m_NativeFences[index];
            break;
        }
    }
    if (match != NULL)
    {
        match->State = VioGpuNativeFenceRetired;
        while (m_NativeFenceCount != 0 && m_NativeFences[m_NativeFenceHead].State == VioGpuNativeFenceRetired)
        {
            VIOGPU_NATIVE_FENCE_ENTRY *head = &m_NativeFences[m_NativeFenceHead];
            *completedFence = head->FenceId;
            head->FenceId = 0;
            head->State = VioGpuNativeFenceFree;
            m_NativeFenceHead = (m_NativeFenceHead + 1) % VioGpuNativeFenceTrackerCapacity;
            --m_NativeFenceCount;
        }
        if (*completedFence != 0)
        {
            InterlockedExchange(&m_NativeCompletedFence, static_cast<LONG>(*completedFence));
        }
    }
    KeReleaseSpinLock(&m_NativeFenceLock, oldIrql);
    return match != NULL;
}

BOOLEAN VioGpuDod::IsNativeFenceQueueEmpty(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeFenceLock, &oldIrql);
    BOOLEAN empty = m_NativeFenceCount == 0;
    KeReleaseSpinLock(&m_NativeFenceLock, oldIrql);
    return empty;
}

void VioGpuDod::ResetNativeFenceTracker(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeFenceLock, &oldIrql);
    m_NativeFenceHead = 0;
    m_NativeFenceCount = 0;
    RtlZeroMemory(m_NativeFences, sizeof(m_NativeFences));
    InterlockedExchange(&m_NativeSubmittedFence, 0);
    InterlockedExchange(&m_NativeCompletedFence, 0);
    KeReleaseSpinLock(&m_NativeFenceLock, oldIrql);
}

void VioGpuDod::InvalidateNativeFenceTracker(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeFenceLock, &oldIrql);
    m_NativeFenceHead = 0;
    m_NativeFenceCount = 0;
    RtlZeroMemory(m_NativeFences, sizeof(m_NativeFences));
    /* Keep the submitted/completed endpoints.  An adapter-wide reset must
     * later advance completed to the last submitted fence instead of making
     * the scheduler observe a backwards fence. */
    KeReleaseSpinLock(&m_NativeFenceLock, oldIrql);
}

void VioGpuDod::CompleteNativeFenceReset(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeFenceLock, &oldIrql);
    UINT submitted = static_cast<UINT>(InterlockedCompareExchange(&m_NativeSubmittedFence, 0, 0));
    m_NativeFenceHead = 0;
    m_NativeFenceCount = 0;
    RtlZeroMemory(m_NativeFences, sizeof(m_NativeFences));
    InterlockedExchange(&m_NativeCompletedFence, static_cast<LONG>(submitted));
    KeReleaseSpinLock(&m_NativeFenceLock, oldIrql);
}

BOOLEAN VioGpuDod::NotifyNativeSchedulerInterrupt(_In_ const DXGKARGCB_NOTIFY_INTERRUPT_DATA *notification,
                                                  _In_ BOOLEAN queueDpc)
{
    if (notification == NULL || !ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return FALSE;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    ULONG messageNumber = 0;
    if (adapter == NULL || m_DxgkInterface.DxgkCbSynchronizeExecution == NULL ||
        !adapter->QueryNativeSubmitInterruptMessage(&messageNumber))
    {
        ExReleaseRundownProtection(&m_HardwareOperations);
        return FALSE;
    }

    VIOGPU_NATIVE_SCHEDULER_NOTIFICATION synchronized = {};
    synchronized.Interface = &m_DxgkInterface;
    synchronized.Data = *notification;
    BOOLEAN notified = FALSE;
    NTSTATUS status = m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle,
                                                                 VioGpuNotifyNativeSchedulerAtDirql,
                                                                 &synchronized,
                                                                 messageNumber,
                                                                 &notified);
    if (!NT_SUCCESS(status) || !notified)
    {
        ExReleaseRundownProtection(&m_HardwareOperations);
        return FALSE;
    }
    if (queueDpc && m_DxgkInterface.DxgkCbQueueDpc != NULL)
    {
        m_DxgkInterface.DxgkCbQueueDpc(m_DxgkInterface.DeviceHandle);
    }
    ExReleaseRundownProtection(&m_HardwareOperations);
    return TRUE;
}

BOOLEAN VioGpuDod::NotifyNativeCompletedFence(_In_ UINT completedFence,
                                              _In_ UINT nodeOrdinal,
                                              _In_ UINT engineOrdinal,
                                              _In_ BOOLEAN queueDpc)
{
    if (nodeOrdinal != 0 || engineOrdinal != 0)
    {
        return FALSE;
    }
    if (completedFence == 0)
    {
        return TRUE;
    }

    DXGKARGCB_NOTIFY_INTERRUPT_DATA notify = {};
    notify.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    notify.DmaCompleted.SubmissionFenceId = completedFence;
    notify.DmaCompleted.NodeOrdinal = nodeOrdinal;
    notify.DmaCompleted.EngineOrdinal = engineOrdinal;
    if (!NotifyNativeSchedulerInterrupt(&notify, queueDpc))
    {
        RequestHardwareResetAtAnyIrql();
        if (ExAcquireRundownProtection(&m_HardwareOperations))
        {
            VioGpuAdapter *adapter = m_pHWDevice;
            if (adapter != NULL)
            {
                adapter->FailNativeContextAtAnyIrql();
            }
            ExReleaseRundownProtection(&m_HardwareOperations);
        }
        return FALSE;
    }
    return TRUE;
}

void VioGpuDod::NotifyNativeSubmissionCompletion(_In_ UINT fenceId,
                                                 _In_ UINT nodeOrdinal,
                                                 _In_ UINT engineOrdinal,
                                                 _In_ BOOLEAN queueDpc)
{
    UINT completedFence = 0;
    if (fenceId == 0 || nodeOrdinal != 0 || engineOrdinal != 0 ||
        !RetireNativeSubmissionFence(fenceId, &completedFence))
    {
        NotifyNativeSubmissionFault(fenceId,
                                    STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE,
                                    nodeOrdinal,
                                    engineOrdinal,
                                    queueDpc);
        return;
    }
    NotifyNativeCompletedFence(completedFence, nodeOrdinal, engineOrdinal, queueDpc);
}

__declspec(noinline) void VioGpuDod::NotifyNativeSubmissionFault(_In_ UINT fenceId,
                                                                 _In_ NTSTATUS status,
                                                                 _In_ UINT nodeOrdinal,
                                                                 _In_ UINT engineOrdinal,
                                                                 _In_ BOOLEAN queueDpc,
                                                                 _In_ DWORD presentSubmitStage,
                                                                 _In_ NTSTATUS presentSubmitStatus,
                                                                 _In_ DWORD presentSubmitDetail)
{
#if defined(VIOGPU_NATIVE_CONTEXT)
    if (InterlockedCompareExchange(&m_NativeSubmissionFaultDiagnosticRecorded, 1, 0) == 0)
    {
        ULONG_PTR imageBase = reinterpret_cast<ULONG_PTR>(&__ImageBase);
        ULONG_PTR returnAddress = reinterpret_cast<ULONG_PTR>(_ReturnAddress());
        ULONG_PTR callerRva = returnAddress >= imageBase ? returnAddress - imageBase : 0;
        LONG executionDiagnosticState = InterlockedCompareExchange(&m_NativePresentExecutionDiagnosticRecorded, 0, 0);
        InterlockedExchange(&m_NativeSubmissionFaultCallerRva,
                            callerRva <= MAXULONG ? static_cast<LONG>(callerRva) : 0);
        InterlockedExchange(&m_NativeSubmissionFaultExecutionDiagnosticState, executionDiagnosticState);
        InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitStage, static_cast<LONG>(presentSubmitStage));
        InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitStatus, static_cast<LONG>(presentSubmitStatus));
        InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitDetail, static_cast<LONG>(presentSubmitDetail));
        KeMemoryBarrier();
        InterlockedExchange(&m_NativeSubmissionFaultDiagnosticRecorded, 2);
    }
#endif
    BOOLEAN validIdentity = fenceId != 0 && nodeOrdinal == 0 && engineOrdinal == 0;
    RequestHardwareResetAtAnyIrql();
#if defined(VIOGPU_NATIVE_CONTEXT)
    /* A fault invalidates every pending fence in this transport generation.
     * Do not leave failed submissions occupying the bounded tracker while
     * the scheduler is being driven into reset.  Preserve the submitted
     * endpoint so ResetFromTimeout can publish the adapter-wide reset fence. */
    InvalidateNativeFenceTracker();
#endif
    if (ExAcquireRundownProtection(&m_HardwareOperations))
    {
        VioGpuAdapter *adapter = m_pHWDevice;
        if (adapter != NULL)
        {
            adapter->FailNativeContextAtAnyIrql();
        }
        ExReleaseRundownProtection(&m_HardwareOperations);
    }
    if (!validIdentity)
    {
        return;
    }

    DXGKARGCB_NOTIFY_INTERRUPT_DATA notify = {};
    notify.InterruptType = DXGK_INTERRUPT_DMA_FAULTED;
    notify.DmaFaulted.FaultedFenceId = fenceId;
    notify.DmaFaulted.Status = status;
    notify.DmaFaulted.NodeOrdinal = nodeOrdinal;
    notify.DmaFaulted.EngineOrdinal = engineOrdinal;
    NotifyNativeSchedulerInterrupt(&notify, queueDpc);
}

void VioGpuDod::NotifyNativeSoftwareCompletion(_In_ UINT fenceId, _In_ UINT nodeOrdinal, _In_ UINT engineOrdinal)
{
    NotifyNativeSubmissionCompletion(fenceId, nodeOrdinal, engineOrdinal, TRUE);
}

BOOLEAN VioGpuDod::CompleteNativeSoftwareSubmission(_In_ UINT fenceId, _In_ UINT nodeOrdinal, _In_ UINT engineOrdinal)
{
    if (fenceId == 0 || nodeOrdinal != 0 || engineOrdinal != 0)
    {
        return FALSE;
    }

    UINT completedFence = 0;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeFenceLock, &oldIrql);
    UINT submitted = static_cast<UINT>(m_NativeSubmittedFence);
    BOOLEAN valid = m_NativeFenceCount < VioGpuNativeFenceTrackerCapacity &&
                    (submitted == 0 || static_cast<LONG>(fenceId - submitted) > 0);
    for (UINT offset = 0; valid && offset < m_NativeFenceCount; ++offset)
    {
        UINT index = (m_NativeFenceHead + offset) % VioGpuNativeFenceTrackerCapacity;
        if (m_NativeFences[index].State != VioGpuNativeFenceFree && m_NativeFences[index].FenceId == fenceId)
        {
            valid = FALSE;
        }
    }
    if (valid)
    {
        UINT tail = (m_NativeFenceHead + m_NativeFenceCount) % VioGpuNativeFenceTrackerCapacity;
        NT_ASSERT(m_NativeFences[tail].State == VioGpuNativeFenceFree);
        m_NativeFences[tail].FenceId = fenceId;
        m_NativeFences[tail].State = VioGpuNativeFenceRetired;
        ++m_NativeFenceCount;
        InterlockedExchange(&m_NativeSubmittedFence, static_cast<LONG>(fenceId));
        while (m_NativeFenceCount != 0 && m_NativeFences[m_NativeFenceHead].State == VioGpuNativeFenceRetired)
        {
            VIOGPU_NATIVE_FENCE_ENTRY *head = &m_NativeFences[m_NativeFenceHead];
            completedFence = head->FenceId;
            head->FenceId = 0;
            head->State = VioGpuNativeFenceFree;
            m_NativeFenceHead = (m_NativeFenceHead + 1) % VioGpuNativeFenceTrackerCapacity;
            --m_NativeFenceCount;
        }
        if (completedFence != 0)
        {
            InterlockedExchange(&m_NativeCompletedFence, static_cast<LONG>(completedFence));
        }
    }
    KeReleaseSpinLock(&m_NativeFenceLock, oldIrql);

    return valid && NotifyNativeCompletedFence(completedFence, nodeOrdinal, engineOrdinal, TRUE);
}

BOOLEAN VioGpuDod::CompleteNativeSystemSubmission(_In_ UINT fenceId, _In_ UINT nodeOrdinal, _In_ UINT engineOrdinal)
{
    UINT completedFence = 0;
    if (fenceId == 0 || nodeOrdinal != 0 || engineOrdinal != 0 ||
        !RetireNativeSubmissionFence(fenceId, &completedFence))
    {
        return FALSE;
    }
    return NotifyNativeCompletedFence(completedFence, nodeOrdinal, engineOrdinal, TRUE);
}

BOOLEAN VioGpuDod::QueueNativePassiveWork(_Inout_ VIOGPU_NATIVE_PASSIVE_WORK *work, _In_ UINT fenceId)
{
    if (work == NULL || fenceId == 0 || work->Routine == NULL || work->CancelRoutine == NULL || work->Context == NULL ||
        work->CancelRequested == NULL || work->FenceId != 0 || work->Link.Flink != &work->Link ||
        work->Link.Blink != &work->Link ||
        InterlockedCompareExchange(&work->State, 0, 0) != VioGpuNativePassiveWorkIdle ||
        InterlockedCompareExchange(&work->Retired, 0, 0) != 0 ||
        InterlockedCompareExchange(work->CancelRequested, 0, 0) != 0)
    {
        return FALSE;
    }

    BOOLEAN queueWorker = FALSE;
    BOOLEAN inserted = FALSE;
    BOOLEAN releaseWorkerReference = FALSE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativePassiveLock, &oldIrql);
    if (!m_NativePassiveClosing && !IsHardwareResetRequested() && work->FenceId == 0 &&
        work->Link.Flink == &work->Link && work->Link.Blink == &work->Link &&
        InterlockedCompareExchange(&work->State, 0, 0) == VioGpuNativePassiveWorkIdle &&
        InterlockedCompareExchange(&work->Retired, 0, 0) == 0 &&
        InterlockedCompareExchange(work->CancelRequested, 0, 0) == 0)
    {
        BOOLEAN needsWorker = m_NativePassiveActiveWork == NULL && !m_NativePassiveWorkerQueued;
        BOOLEAN workerReference = !needsWorker || ExAcquireRundownProtection(&m_HardwareOperations);
        if (workerReference && RecordNativeSubmissionFence(fenceId))
        {
            KeClearEvent(&m_NativePassiveIdleEvent);
            work->FenceId = fenceId;
            InterlockedExchange(&work->State, VioGpuNativePassiveWorkQueued);
            InsertTailList(&m_NativePassiveQueue, &work->Link);
            inserted = TRUE;
            if (needsWorker)
            {
                m_NativePassiveWorkerQueued = TRUE;
                queueWorker = TRUE;
            }
        }
        else if (needsWorker && workerReference)
        {
            releaseWorkerReference = TRUE;
        }
    }
    KeReleaseSpinLock(&m_NativePassiveLock, oldIrql);

    if (releaseWorkerReference)
    {
        ExReleaseRundownProtection(&m_HardwareOperations);
    }
    if (queueWorker)
    {
        ExQueueWorkItem(&m_NativePassiveWorkItem, DelayedWorkQueue);
    }
    return inserted;
}

VOID VioGpuDod::CompleteNativePassiveWork(_Inout_ VIOGPU_NATIVE_PASSIVE_WORK *work)
{
    if (work == NULL)
    {
        return;
    }

    BOOLEAN queueWorker = FALSE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativePassiveLock, &oldIrql);
    LONG state = InterlockedCompareExchange(&work->State, 0, 0);
    if (m_NativePassiveActiveWork == work && state == VioGpuNativePassiveWorkWorkerOwned)
    {
        NT_ASSERT(work->Link.Flink == &work->Link && work->Link.Blink == &work->Link);
        InterlockedExchange(&work->Retired, 1);
        InterlockedExchange(&work->State, VioGpuNativePassiveWorkIdle);
        m_NativePassiveActiveWork = NULL;
        if (!m_NativePassiveClosing && !IsHardwareResetRequested() && !m_NativePassiveWorkerQueued &&
            !IsListEmpty(&m_NativePassiveQueue))
        {
            if (ExAcquireRundownProtection(&m_HardwareOperations))
            {
                m_NativePassiveWorkerQueued = TRUE;
                queueWorker = TRUE;
            }
            else
            {
                InterlockedExchange(&m_NativePassiveClosing, TRUE);
            }
        }
    }
    BOOLEAN idle = m_NativePassiveActiveWork == NULL && m_NativePassiveWorkerQueued == FALSE &&
                   IsListEmpty(&m_NativePassiveQueue);
    KeReleaseSpinLock(&m_NativePassiveLock, oldIrql);

    if (idle)
    {
        KeSetEvent(&m_NativePassiveIdleEvent, IO_NO_INCREMENT, FALSE);
    }

    if (queueWorker)
    {
        ExQueueWorkItem(&m_NativePassiveWorkItem, DelayedWorkQueue);
    }
}

VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP VioGpuDod::CancelNativePassiveWork(_Inout_ VIOGPU_NATIVE_PASSIVE_WORK *work)
{
    VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP ownership = VioGpuNativePassiveWorkNotQueued;
    if (work == NULL)
    {
        return ownership;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativePassiveLock, &oldIrql);
    InterlockedExchange(&work->Retired, 1);
    if (work->CancelRequested != NULL)
    {
        InterlockedExchange(work->CancelRequested, 1);
    }
    LONG state = InterlockedCompareExchange(&work->State, 0, 0);
    if (state == VioGpuNativePassiveWorkQueued)
    {
        NT_ASSERT(m_NativePassiveActiveWork != work);
        if (work->Link.Flink != &work->Link && work->Link.Blink != &work->Link)
        {
            RemoveEntryList(&work->Link);
            InitializeListHead(&work->Link);
            InterlockedExchange(&work->State, VioGpuNativePassiveWorkIdle);
            ownership = VioGpuNativePassiveWorkRemoved;
        }
    }
    else if (state == VioGpuNativePassiveWorkWorkerOwned && m_NativePassiveActiveWork == work)
    {
        ownership = VioGpuNativePassiveOwnershipWorkerOwned;
    }
    BOOLEAN idle = m_NativePassiveActiveWork == NULL && m_NativePassiveWorkerQueued == FALSE &&
                   IsListEmpty(&m_NativePassiveQueue);
    KeReleaseSpinLock(&m_NativePassiveLock, oldIrql);
    if (idle)
    {
        KeSetEvent(&m_NativePassiveIdleEvent, IO_NO_INCREMENT, FALSE);
    }
    return ownership;
}

VOID VioGpuDod::CloseNativePassiveQueue(void)
{
    LIST_ENTRY cancelled;
    InitializeListHead(&cancelled);
    InterlockedExchange(&m_NativePassiveClosing, TRUE);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativePassiveLock, &oldIrql);
    if (m_NativePassiveActiveWork != NULL)
    {
        VIOGPU_NATIVE_PASSIVE_WORK *active = m_NativePassiveActiveWork;
        InterlockedExchange(&active->Retired, 1);
        if (active->CancelRequested != NULL)
        {
            InterlockedExchange(active->CancelRequested, 1);
        }
    }
    while (!IsListEmpty(&m_NativePassiveQueue))
    {
        PLIST_ENTRY entry = RemoveHeadList(&m_NativePassiveQueue);
        VIOGPU_NATIVE_PASSIVE_WORK *work = CONTAINING_RECORD(entry, VIOGPU_NATIVE_PASSIVE_WORK, Link);
        InterlockedExchange(&work->Retired, 1);
        if (work->CancelRequested != NULL)
        {
            InterlockedExchange(work->CancelRequested, 1);
        }
        InterlockedExchange(&work->State, VioGpuNativePassiveWorkIdle);
        InsertTailList(&cancelled, entry);
    }
    BOOLEAN idle = m_NativePassiveActiveWork == NULL && m_NativePassiveWorkerQueued == FALSE &&
                   IsListEmpty(&m_NativePassiveQueue);
    KeReleaseSpinLock(&m_NativePassiveLock, oldIrql);

    if (idle)
    {
        KeSetEvent(&m_NativePassiveIdleEvent, IO_NO_INCREMENT, FALSE);
    }

    while (!IsListEmpty(&cancelled))
    {
        PLIST_ENTRY entry = RemoveHeadList(&cancelled);
        InitializeListHead(entry);
        VIOGPU_NATIVE_PASSIVE_WORK *work = CONTAINING_RECORD(entry, VIOGPU_NATIVE_PASSIVE_WORK, Link);
        VIOGPU_NATIVE_PASSIVE_ROUTINE cancelRoutine = work->CancelRoutine;
        PVOID context = work->Context;
        NT_ASSERT(cancelRoutine != NULL);
        if (cancelRoutine != NULL)
        {
            cancelRoutine(context);
        }
    }
}

BOOLEAN VioGpuDod::WaitForNativePassiveQueueIdle(void)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    if (KeWaitForSingleObject(&m_NativePassiveIdleEvent, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS)
    {
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativePassiveLock, &oldIrql);
    BOOLEAN idle = m_NativePassiveActiveWork == NULL && m_NativePassiveWorkerQueued == FALSE &&
                   IsListEmpty(&m_NativePassiveQueue);
    KeReleaseSpinLock(&m_NativePassiveLock, oldIrql);
    return idle;
}

BOOLEAN VioGpuDod::OpenNativePassiveQueue(void)
{
    BOOLEAN opened = FALSE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativePassiveLock, &oldIrql);
    if (!IsHardwareResetRequested() && IsHardwareInit() && m_pHWDevice != NULL &&
        InterlockedCompareExchange(&m_WddmDrainRequested, 0, 0) == 0 &&
        InterlockedCompareExchange(&m_WddmDrainWorkerQueued, 0, 0) == 0)
    {
        if (InterlockedCompareExchange(&m_NativePassiveClosing, 0, 0) == 0)
        {
            opened = TRUE;
        }
        else if (IsListEmpty(&m_NativePassiveQueue) && m_NativePassiveActiveWork == NULL &&
                 !m_NativePassiveWorkerQueued)
        {
            InterlockedExchange(&m_NativePassiveClosing, FALSE);
            opened = TRUE;
        }
    }
    KeReleaseSpinLock(&m_NativePassiveLock, oldIrql);
    return opened;
}

VOID VioGpuDod::RequestWddmSubmissionDrainAtAnyIrql(void)
{
    InterlockedExchange(&m_NativePassiveClosing, TRUE);
    InterlockedExchange(&m_WddmPresentClosing, TRUE);
    InterlockedExchange(&m_WddmDrainRequested, TRUE);

    if (KeGetCurrentIrql() <= DISPATCH_LEVEL)
    {
        QueueWddmSubmissionDrainWorker();
    }
    else if (m_DxgkInterface.DxgkCbQueueDpc != NULL)
    {
        m_DxgkInterface.DxgkCbQueueDpc(m_DxgkInterface.DeviceHandle);
    }
}

VOID VioGpuDod::QueueWddmSubmissionDrainWorker(void)
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL ||
        InterlockedCompareExchange(&m_WddmDrainWorkerQueued, TRUE, FALSE) != FALSE)
    {
        return;
    }

    KeClearEvent(&m_WddmDrainIdleEvent);
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        InterlockedExchange(&m_WddmDrainRequested, FALSE);
        InterlockedExchange(&m_WddmDrainWorkerQueued, FALSE);
        KeSetEvent(&m_WddmDrainIdleEvent, IO_NO_INCREMENT, FALSE);
        return;
    }
    ExQueueWorkItem(&m_WddmDrainWorkItem, DelayedWorkQueue);
}

_Use_decl_annotations_ VOID VioGpuDod::WddmSubmissionDrainWorker(PVOID context)
{
    VioGpuDod *adapter = static_cast<VioGpuDod *>(context);
    if (adapter != NULL)
    {
        adapter->RunWddmSubmissionDrainWorker();
    }
}

VOID VioGpuDod::RunWddmSubmissionDrainWorker(void)
{
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    for (;;)
    {
        InterlockedExchange(&m_WddmDrainRequested, FALSE);
        CloseNativePassiveQueue();
        VioGpuWddmDrainPresentTransactions(this);
        if (IsHardwareResetRequested())
        {
            InvalidateNativeFenceTracker();
        }
        if (InterlockedCompareExchange(&m_WddmDrainRequested, 0, 0) != 0)
        {
            continue;
        }

        KeSetEvent(&m_WddmDrainIdleEvent, IO_NO_INCREMENT, FALSE);
        InterlockedExchange(&m_WddmDrainWorkerQueued, FALSE);
        if (InterlockedCompareExchange(&m_WddmDrainRequested, 0, 0) == 0)
        {
            break;
        }
        if (InterlockedCompareExchange(&m_WddmDrainWorkerQueued, TRUE, FALSE) == FALSE)
        {
            KeClearEvent(&m_WddmDrainIdleEvent);
            continue;
        }
        /* A requester already acquired a new rundown reference and queued the
         * shared work item after observing WorkerQueued == FALSE. */
        break;
    }
    ExReleaseRundownProtection(&m_HardwareOperations);
}

BOOLEAN VioGpuDod::WaitForWddmSubmissionDrain(void)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }

    for (;;)
    {
        if (InterlockedCompareExchange(&m_WddmDrainRequested, 0, 0) != 0 &&
            InterlockedCompareExchange(&m_WddmDrainWorkerQueued, 0, 0) == 0)
        {
            QueueWddmSubmissionDrainWorker();
        }
        if (InterlockedCompareExchange(&m_WddmDrainRequested, 0, 0) == 0 &&
            InterlockedCompareExchange(&m_WddmDrainWorkerQueued, 0, 0) == 0)
        {
            return TRUE;
        }
        KeWaitForSingleObject(&m_WddmDrainIdleEvent, Executive, KernelMode, FALSE, NULL);
    }
}

VOID VioGpuDod::CloseWddmPresentTransactions(void)
{
    InterlockedExchange(&m_WddmPresentClosing, TRUE);
}

BOOLEAN VioGpuDod::OpenWddmPresentTransactions(void)
{
    BOOLEAN opened = FALSE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_WddmPresentLock, &oldIrql);
    if (!IsHardwareResetRequested() && InterlockedCompareExchange(&m_WddmDrainRequested, 0, 0) == 0 &&
        InterlockedCompareExchange(&m_WddmDrainWorkerQueued, 0, 0) == 0)
    {
        if (InterlockedCompareExchange(&m_WddmPresentClosing, 0, 0) == 0)
        {
            opened = TRUE;
        }
        else if (IsListEmpty(&m_WddmPresentTransactions))
        {
            InterlockedExchange(&m_WddmPresentClosing, FALSE);
            opened = TRUE;
        }
    }
    KeReleaseSpinLock(&m_WddmPresentLock, oldIrql);
    return opened;
}

BOOLEAN VioGpuDod::RegisterWddmPresentTransaction(_Inout_ LIST_ENTRY *link)
{
    if (link == NULL || link->Flink != link || link->Blink != link)
    {
        return FALSE;
    }

    BOOLEAN registered = FALSE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_WddmPresentLock, &oldIrql);
    if (InterlockedCompareExchange(&m_WddmPresentClosing, 0, 0) == 0 && !IsHardwareResetRequested() &&
        link->Flink == link && link->Blink == link)
    {
        InsertTailList(&m_WddmPresentTransactions, link);
        registered = TRUE;
    }
    KeReleaseSpinLock(&m_WddmPresentLock, oldIrql);
    return registered;
}

BOOLEAN VioGpuDod::UnregisterWddmPresentTransaction(_Inout_ LIST_ENTRY *link)
{
    if (link == NULL)
    {
        return FALSE;
    }

    BOOLEAN removed = FALSE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_WddmPresentLock, &oldIrql);
    if (link->Flink != link && link->Blink != link)
    {
        RemoveEntryList(link);
        InitializeListHead(link);
        removed = TRUE;
    }
    KeReleaseSpinLock(&m_WddmPresentLock, oldIrql);
    return removed;
}

PLIST_ENTRY VioGpuDod::PopWddmPresentTransactionForReset(void)
{
    PLIST_ENTRY link = NULL;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_WddmPresentLock, &oldIrql);
    if (!IsListEmpty(&m_WddmPresentTransactions))
    {
        link = RemoveHeadList(&m_WddmPresentTransactions);
        InitializeListHead(link);
    }
    KeReleaseSpinLock(&m_WddmPresentLock, oldIrql);
    return link;
}

_Use_decl_annotations_ VOID VioGpuDod::NativePassiveWorker(PVOID context)
{
    VioGpuDod *adapter = static_cast<VioGpuDod *>(context);
    if (adapter != NULL)
    {
        adapter->RunNativePassiveWorker();
    }
}

VOID VioGpuDod::RunNativePassiveWorker(void)
{
    VIOGPU_NATIVE_PASSIVE_WORK *work = NULL;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativePassiveLock, &oldIrql);
    NT_ASSERT(m_NativePassiveWorkerQueued);
    m_NativePassiveWorkerQueued = FALSE;
    if (!m_NativePassiveClosing && !IsHardwareResetRequested() && m_NativePassiveActiveWork == NULL &&
        !IsListEmpty(&m_NativePassiveQueue))
    {
        PLIST_ENTRY entry = RemoveHeadList(&m_NativePassiveQueue);
        InitializeListHead(entry);
        work = CONTAINING_RECORD(entry, VIOGPU_NATIVE_PASSIVE_WORK, Link);
        m_NativePassiveActiveWork = work;
        InterlockedExchange(&work->State, VioGpuNativePassiveWorkWorkerOwned);
    }
    BOOLEAN idle = work == NULL && m_NativePassiveActiveWork == NULL && m_NativePassiveWorkerQueued == FALSE &&
                   IsListEmpty(&m_NativePassiveQueue);
    KeReleaseSpinLock(&m_NativePassiveLock, oldIrql);

    if (idle)
    {
        KeSetEvent(&m_NativePassiveIdleEvent, IO_NO_INCREMENT, FALSE);
    }

    if (work != NULL)
    {
        work->Routine(work->Context);
    }
    ExReleaseRundownProtection(&m_HardwareOperations);
}

UINT VioGpuDod::Allocate2DResourceId(void)
{
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return 0;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    UINT resourceId = !IsHardwareResetRequested() && adapter != NULL ? adapter->Allocate2DResourceId() : 0;
    ExReleaseRundownProtection(&m_HardwareOperations);
    return resourceId;
}

BOOLEAN VioGpuDod::Release2DResourceId(_In_ UINT resourceId)
{
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return FALSE;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    BOOLEAN released = adapter != NULL && adapter->Release2DResourceId(resourceId);
    ExReleaseRundownProtection(&m_HardwareOperations);
    return released;
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuDod::Create2DResourceBacking(_In_ UINT resourceId,
                                                              _In_ UINT format,
                                                              _In_ UINT width,
                                                              _In_ UINT height,
                                                              _In_ SIZE_T backingSize,
                                                              _In_reads_(entryCount) const GPU_MEM_ENTRY *entries,
                                                              _In_ UINT entryCount,
                                                              _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                              _Inout_ ULONGLONG *resourceResetGeneration)
{
    if (!AcquireNativeSubmissionOperation())
    {
        return VioGpuHostContextNotSubmitted;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    VIOGPU_HOST_CONTEXT_RESULT result = adapter != NULL ? adapter->Create2DResourceBacking(resourceId,
                                                                                           format,
                                                                                           width,
                                                                                           height,
                                                                                           backingSize,
                                                                                           entries,
                                                                                           entryCount,
                                                                                           resourceState,
                                                                                           resourceResetGeneration)
                                                        : VioGpuHostContextNotSubmitted;
    ReleaseNativeSubmissionOperation();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuDod::Destroy2DResource(_In_ UINT resourceId,
                                                        _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                        _Inout_ ULONGLONG *resourceResetGeneration,
                                                        _Out_ BOOLEAN *released)
{
    if (released == NULL)
    {
        return VioGpuHostContextNotSubmitted;
    }
    *released = FALSE;
    if (!AcquireNativeSubmissionOperation())
    {
        return VioGpuHostContextNotSubmitted;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    VIOGPU_HOST_CONTEXT_RESULT result = adapter != NULL ? adapter->Destroy2DResource(resourceId,
                                                                                     resourceState,
                                                                                     resourceResetGeneration,
                                                                                     released)
                                                        : VioGpuHostContextNotSubmitted;
    ReleaseNativeSubmissionOperation();
    return result;
}

BOOLEAN VioGpuDod::Reconcile2DResourceAfterReset(_Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                 _Inout_ ULONGLONG *resourceResetGeneration,
                                                 _Out_ BOOLEAN *retired)
{
    if (retired == NULL)
    {
        return FALSE;
    }
    *retired = FALSE;
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return FALSE;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    BOOLEAN valid = adapter != NULL &&
                    adapter->Reconcile2DResourceAfterReset(resourceState, resourceResetGeneration, retired);
    ExReleaseRundownProtection(&m_HardwareOperations);
    return valid;
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuDod::Present2DResource(_In_ UINT resourceId,
                                                        _In_ ULONGLONG offset,
                                                        _In_ UINT width,
                                                        _In_ UINT height,
                                                        _In_ UINT x,
                                                        _In_ UINT y,
                                                        _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                        _Inout_ ULONGLONG *resourceResetGeneration)
{
    if (!AcquireNativeSubmissionOperation())
    {
        return VioGpuHostContextNotSubmitted;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    VIOGPU_HOST_CONTEXT_RESULT result = adapter != NULL ? adapter->Present2DResource(resourceId,
                                                                                     offset,
                                                                                     width,
                                                                                     height,
                                                                                     x,
                                                                                     y,
                                                                                     resourceState,
                                                                                     resourceResetGeneration)
                                                        : VioGpuHostContextNotSubmitted;
    ReleaseNativeSubmissionOperation();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuDod::Set2DScanout(_In_ UINT scanoutId,
                                                   _In_ UINT resourceId,
                                                   _In_ UINT width,
                                                   _In_ UINT height,
                                                   _Out_ UINT *previousResourceId)
{
    if (previousResourceId == NULL)
    {
        return VioGpuHostContextNotSubmitted;
    }
    *previousResourceId = 0;
    if (!AcquireNativeSubmissionOperation())
    {
        return VioGpuHostContextNotSubmitted;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    VIOGPU_HOST_CONTEXT_RESULT result = adapter != NULL ? adapter->Set2DScanout(scanoutId,
                                                                                resourceId,
                                                                                width,
                                                                                height,
                                                                                previousResourceId)
                                                        : VioGpuHostContextNotSubmitted;
    ReleaseNativeSubmissionOperation();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuDod::Detach2DScanoutResource(_In_ UINT resourceId, _Out_ BOOLEAN *detached)
{
    if (detached == NULL)
    {
        return VioGpuHostContextNotSubmitted;
    }
    *detached = FALSE;
    if (!AcquireNativeSubmissionOperation())
    {
        return VioGpuHostContextNotSubmitted;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    VIOGPU_HOST_CONTEXT_RESULT result = adapter != NULL ? adapter->Detach2DScanoutResource(resourceId, detached)
                                                        : VioGpuHostContextNotSubmitted;
    ReleaseNativeSubmissionOperation();
    return result;
}

BOOLEAN VioGpuDod::Query2DScanoutResource(_In_ UINT resourceId, _Out_ BOOLEAN *active)
{
    if (active == NULL)
    {
        return FALSE;
    }
    *active = FALSE;
    if (!AcquireNativeSubmissionOperation())
    {
        return FALSE;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    BOOLEAN valid = adapter != NULL && adapter->Query2DScanoutResource(resourceId, active);
    ReleaseNativeSubmissionOperation();
    return valid;
}

UINT VioGpuDod::AllocateNativeResourceId(_In_ ULONGLONG expectedResetGeneration)
{
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return 0;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    UINT resourceId = !IsHardwareResetRequested() && adapter != NULL ? adapter->AllocateNativeResourceId(expectedResetGeneration)
                                                                     : 0;
    ExReleaseRundownProtection(&m_HardwareOperations);
    return resourceId;
}
#pragma code_seg(pop)

BOOLEAN VioGpuDod::AcquireNativeContextSnapshotForAllocation(_In_ ULONGLONG requestedIova,
                                                             _In_ SIZE_T backingSize,
                                                             _In_ ULONGLONG expectedResetGeneration,
                                                             _In_ UINT expectedContextId,
                                                             _Out_ VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot) const
{
    if (snapshot == NULL || !ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return FALSE;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    BOOLEAN acquired = !IsHardwareResetRequested() && adapter != NULL &&
                       adapter->AcquireNativeContextSnapshotForAllocation(requestedIova,
                                                                          backingSize,
                                                                          expectedResetGeneration,
                                                                          expectedContextId,
                                                                          snapshot);
    ExReleaseRundownProtection(&m_HardwareOperations);
    return acquired;
}
#endif

#pragma code_seg(push)
#pragma code_seg()
_IRQL_requires_max_(DISPATCH_LEVEL) BOOLEAN VioGpuDod::QueryNativeContextReadiness(_Out_ PGPU_CAPSET_DRM capset,
                                                                                   _Out_opt_ UINT *capsetVersion,
                                                                                   _Out_opt_ UINT *capsetSize,
                                                                                   _Out_opt_ ULONGLONG *resetGeneration)
{
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return FALSE;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    BOOLEAN ready = !IsHardwareResetRequested() && adapter != NULL &&
                    adapter->QueryNativeContextReadiness(capset, capsetVersion, capsetSize, resetGeneration);
    ExReleaseRundownProtection(&m_HardwareOperations);
    return ready;
}
#pragma code_seg(pop)

NTSTATUS VioGpuDod::CreateNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                        _In_ ULONGLONG expectedResetGeneration)
{
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    NTSTATUS status = !IsHardwareResetRequested() && adapter != NULL ? adapter->CreateNativeContext(context,
                                                                                                    expectedResetGeneration)
                                                                     : STATUS_DEVICE_NOT_READY;
    ExReleaseRundownProtection(&m_HardwareOperations);
    return status;
}

NTSTATUS VioGpuDod::DestroyNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context, _Out_ BOOLEAN *released)
{
    if (released == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *released = FALSE;
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    NTSTATUS status = !IsHardwareResetRequested() && adapter != NULL ? adapter->DestroyNativeContext(context, released)
                                                                     : STATUS_DEVICE_NOT_READY;
    ExReleaseRundownProtection(&m_HardwareOperations);
    return status;
}

NTSTATUS VioGpuDod::DispatchIoRequest(_In_ ULONG VidPnSourceId, _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(pVideoRequestPacket);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

PCHAR
DbgDevicePowerString(__in DEVICE_POWER_STATE Type)
{
    PAGED_CODE();

    switch (Type)
    {
        case PowerDeviceUnspecified:
            return "PowerDeviceUnspecified";
        case PowerDeviceD0:
            return "PowerDeviceD0";
        case PowerDeviceD1:
            return "PowerDeviceD1";
        case PowerDeviceD2:
            return "PowerDeviceD2";
        case PowerDeviceD3:
            return "PowerDeviceD3";
        case PowerDeviceMaximum:
            return "PowerDeviceMaximum";
        default:
            return "UnKnown Device Power State";
    }
}

PCHAR
DbgPowerActionString(__in POWER_ACTION Type)
{
    PAGED_CODE();

    switch (Type)
    {
        case PowerActionNone:
            return "PowerActionNone";
        case PowerActionReserved:
            return "PowerActionReserved";
        case PowerActionSleep:
            return "PowerActionSleep";
        case PowerActionHibernate:
            return "PowerActionHibernate";
        case PowerActionShutdown:
            return "PowerActionShutdown";
        case PowerActionShutdownReset:
            return "PowerActionShutdownReset";
        case PowerActionShutdownOff:
            return "PowerActionShutdownOff";
        case PowerActionWarmEject:
            return "PowerActionWarmEject";
        default:
            return "UnKnown Device Power State";
    }
}

NTSTATUS VioGpuDod::SetPowerState(_In_ ULONG HardwareUid,
                                  _In_ DEVICE_POWER_STATE DevicePowerState,
                                  _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ActionType);
    NTSTATUS Status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_FATAL,
             ("---> %s HardwareUid = 0x%x ActionType = %s DevicePowerState = %s AdapterPowerState = %s\n",
              __FUNCTION__,
              HardwareUid,
              DbgPowerActionString(ActionType),
              DbgDevicePowerString(DevicePowerState),
              DbgDevicePowerString(m_AdapterPowerState)));

    if (HardwareUid == DISPLAY_ADAPTER_HW_ID)
    {
        BOOLEAN resetRecovery = FALSE;
        if (DevicePowerState == PowerDeviceD0)
        {
            if (m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership)
            {
                Status = m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership(m_DxgkInterface.DeviceHandle,
                                                                           &m_SystemDisplayInfo);
            }

            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_FATAL,
                         ("DxgkCbAcquirePostDisplayOwnership failed with status 0x%X Width = %d\n",
                          Status,
                          m_SystemDisplayInfo.Width));
                VioGpuDbgBreak();
                return Status;
            }

            if (m_AdapterPowerState == PowerDeviceD3)
            {
                DXGKARG_SETVIDPNSOURCEVISIBILITY Visibility;
                Visibility.VidPnSourceId = D3DDDI_ID_ALL;
                Visibility.Visible = FALSE;
                SetVidPnSourceVisibility(&Visibility);
            }

            LONG resetState = InterlockedCompareExchange(&m_HardwareResetState,
                                                         VioGpuHardwareRecovering,
                                                         VioGpuHardwareResetRequested);
            if (resetState == VioGpuHardwareResetRequested)
            {
                resetRecovery = TRUE;
                // Consume the any-IRQL reset notification after owning the
                // recovery state. A concurrent reset changes it back to
                // ResetRequested and prevents the final publish below.
#if defined(VIOGPU_NATIVE_CONTEXT)
                RequestWddmSubmissionDrainAtAnyIrql();
                if (!WaitForWddmSubmissionDrain())
                {
                    InterlockedCompareExchange(&m_HardwareResetState,
                                               VioGpuHardwareResetRequested,
                                               VioGpuHardwareRecovering);
                    return STATUS_DEVICE_NOT_READY;
                }
#endif
                m_pHWDevice->ResetDevice();
            }
            else if (resetState != VioGpuHardwareActive)
            {
                return STATUS_DEVICE_NOT_READY;
            }
        }

#if defined(VIOGPU_NATIVE_CONTEXT)
        if (DevicePowerState == PowerDeviceD1 || DevicePowerState == PowerDeviceD2 || DevicePowerState == PowerDeviceD3)
        {
            RequestHardwareResetAtAnyIrql();
            if (!WaitForWddmSubmissionDrain())
            {
                return STATUS_DEVICE_NOT_READY;
            }
        }
#endif

        Status = m_pHWDevice->SetPowerState(&m_DeviceInfo, DevicePowerState, &m_CurrentMode);
        if (!NT_SUCCESS(Status) && resetRecovery)
        {
            InterlockedCompareExchange(&m_HardwareResetState, VioGpuHardwareResetRequested, VioGpuHardwareRecovering);
        }
#if defined(VIOGPU_NATIVE_CONTEXT)
        if (!NT_SUCCESS(Status) && (DevicePowerState == PowerDeviceD1 || DevicePowerState == PowerDeviceD2 ||
                                    DevicePowerState == PowerDeviceD3))
        {
            /* Present publication was closed before the fallible transport
             * transition.  Keep the outer gate closed until D0 recovery can
             * rebuild one coherent hardware epoch. */
            RequestHardwareResetAtAnyIrql();
        }
#endif
        if (NT_SUCCESS(Status) && DevicePowerState == PowerDeviceD0)
        {
            if (resetRecovery)
            {
#if defined(VIOGPU_NATIVE_CONTEXT)
                if (!WaitForNativePassiveQueueIdle())
                {
                    InterlockedCompareExchange(&m_HardwareResetState,
                                               VioGpuHardwareResetRequested,
                                               VioGpuHardwareRecovering);
                    return STATUS_DEVICE_NOT_READY;
                }
                InterlockedExchange(&m_HardwareResetCallerRva, 0);
#endif
                if (InterlockedCompareExchange(&m_HardwareResetState, VioGpuHardwareActive, VioGpuHardwareRecovering) !=
                    VioGpuHardwareRecovering)
                {
                    return STATUS_DEVICE_NOT_READY;
                }
            }
            else if (InterlockedCompareExchange(&m_HardwareResetState, VioGpuHardwareActive, VioGpuHardwareActive) !=
                     VioGpuHardwareActive)
            {
                return STATUS_DEVICE_NOT_READY;
            }
#if defined(VIOGPU_NATIVE_CONTEXT)
            if (resetRecovery)
            {
                /* Both publication gates remain closed here, so no new node
                 * fence can enter between reset retirement and queue reopen. */
                CompleteNativeFenceReset();
            }
            if (!OpenNativePassiveQueue() || !OpenWddmPresentTransactions())
            {
                RequestWddmSubmissionDrainAtAnyIrql();
                RequestHardwareResetAtAnyIrql();
                return STATUS_DEVICE_NOT_READY;
            }
#endif
        }
        if (NT_SUCCESS(Status))
        {
            m_AdapterPowerState = DevicePowerState;
        }
        return Status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::ResetFromTimeout(void)
{
    PAGED_CODE();

    /* TDR is an adapter-wide barrier.  Close the native transport before
     * publishing any new fence endpoint; StopNativeContextTransport also
     * invalidates every context/allocation generation and drains the queue. */
    InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
#if defined(VIOGPU_NATIVE_CONTEXT)
    RequestWddmSubmissionDrainAtAnyIrql();
    if (!WaitForWddmSubmissionDrain())
    {
        return STATUS_DEVICE_NOT_READY;
    }
#endif
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    VioGpuAdapter *adapter = m_pHWDevice;
    if (adapter == NULL)
    {
        ExReleaseRundownProtection(&m_HardwareOperations);
        return STATUS_DEVICE_NOT_READY;
    }

    adapter->ResetDevice();
    NTSTATUS status = adapter->SetPowerState(&m_DeviceInfo, PowerDeviceD3, &m_CurrentMode);
#if defined(VIOGPU_NATIVE_CONTEXT)
    /* StopNativeContextTransport has now drained every submitter that could
     * append to the tracker.  Discard pending entries only after that barrier. */
    InvalidateNativeFenceTracker();
#endif
    if (NT_SUCCESS(status))
    {
        m_AdapterPowerState = PowerDeviceD3;
#if defined(VIOGPU_NATIVE_CONTEXT)
        /* Only a completed hardware reset may advance the scheduler fence. */
        CompleteNativeFenceReset();
#endif
    }
    ExReleaseRundownProtection(&m_HardwareOperations);
    return status;
}

NTSTATUS VioGpuDod::RestartFromTimeout(void)
{
    PAGED_CODE();

    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (m_pHWDevice == NULL)
    {
        ExReleaseRundownProtection(&m_HardwareOperations);
        return STATUS_DEVICE_NOT_READY;
    }

    NTSTATUS status = SetPowerState(DISPLAY_ADAPTER_HW_ID, PowerDeviceD0, PowerActionNone);
    if (!NT_SUCCESS(status))
    {
        /* Keep all later DDI entry points fail-closed so a partial restart
         * cannot be mistaken for an Active transport generation. */
        InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
    }
    ExReleaseRundownProtection(&m_HardwareOperations);
    return status;
}

NTSTATUS VioGpuDod::QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                        _In_ ULONG ChildRelationsSize)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    VIOGPU_ASSERT(pChildRelations != NULL);

    ULONG ChildRelationsCount = (ChildRelationsSize / sizeof(DXGK_CHILD_DESCRIPTOR)) - 1;
    VIOGPU_ASSERT(ChildRelationsCount <= MAX_CHILDREN);

    for (UINT ChildIndex = 0; ChildIndex < ChildRelationsCount; ++ChildIndex)
    {
        pChildRelations[ChildIndex].ChildDeviceType = TypeVideoOutput;
        pChildRelations[ChildIndex].ChildCapabilities.HpdAwareness = IsVgaDevice() ? HpdAwarenessAlwaysConnected
                                                                                   : HpdAwarenessInterruptible;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = IsVgaDevice() ? D3DKMDT_VOT_INTERNAL
                                                                                                           : D3DKMDT_VOT_HD15;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
        pChildRelations[ChildIndex].AcpiUid = 0;
        pChildRelations[ChildIndex].ChildUid = ChildIndex;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::QueryChildStatus(_Inout_ DXGK_CHILD_STATUS *pChildStatus, _In_ BOOLEAN NonDestructiveOnly)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    VIOGPU_ASSERT(pChildStatus != NULL);
    VIOGPU_ASSERT(pChildStatus->ChildUid < MAX_CHILDREN);

    switch (pChildStatus->Type)
    {
        case StatusConnection:
            {
                pChildStatus->HotPlug.Connected = IsDriverActive();
                return STATUS_SUCCESS;
            }

        case StatusRotation:
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("Child status being queried for StatusRotation even though D3DKMDT_MOA_NONE was reported"));
                return STATUS_INVALID_PARAMETER;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_WARNING, ("Unknown pChildStatus->Type (0x%I64x) requested.", pChildStatus->Type));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

NTSTATUS VioGpuDod::QueryDeviceDescriptor(_In_ ULONG ChildUid, _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    VIOGPU_ASSERT(pDeviceDescriptor != NULL);
    VIOGPU_ASSERT(ChildUid < MAX_CHILDREN);
    PBYTE edid = NULL;

    edid = m_pHWDevice->GetEdidData();

    if (!edid)
    {
        return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
    }
    else if (pDeviceDescriptor->DescriptorOffset < EDID_RAW_BLOCK_SIZE)
    {
        ULONG len = min(pDeviceDescriptor->DescriptorLength,
                        (EDID_RAW_BLOCK_SIZE - pDeviceDescriptor->DescriptorOffset));
        RtlCopyMemory(pDeviceDescriptor->DescriptorBuffer, (edid + pDeviceDescriptor->DescriptorOffset), len);
        pDeviceDescriptor->DescriptorLength = len;
        return STATUS_SUCCESS;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
}

NTSTATUS VioGpuDod::GetScanLine(_Inout_ DXGKARG_GETSCANLINE *pGetScanLine)
{
    PAGED_CODE();

    if (pGetScanLine == NULL || pGetScanLine->VidPnTargetId != 0 || !IsDriverActive() || !IsHardwareInit() ||
        !m_CurrentMode.Flags.FrameBufferIsActive || m_CurrentMode.Flags.SourceNotVisible)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    UINT height = m_CurrentMode.DispInfo.Height;
    if (height == 0)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    LARGE_INTEGER frequency;
    LARGE_INTEGER counter = KeQueryPerformanceCounter(&frequency);
    ULONGLONG frequencyTicks = frequency.QuadPart > 0 ? static_cast<ULONGLONG>(frequency.QuadPart) : 0;
    if (frequencyTicks == 0 || counter.QuadPart < 0)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    /* VirtIO-GPU exposes no scanline register. Emulate a progressive 60 Hz
     * timing domain from the active VidPN mode without claiming hardware
     * vblank interrupts. */
    const UINT blankingLines = max(1U, height / 20U);
    const ULONGLONG totalLines = static_cast<ULONGLONG>(height) + blankingLines;
    const ULONGLONG frameTicks = max(1ULL, frequencyTicks / VIOGPU_SCANLINE_REFRESH_HZ);
    const ULONGLONG framePosition = static_cast<ULONGLONG>(counter.QuadPart) % frameTicks;
    const ULONGLONG scanLine = (framePosition * totalLines) / frameTicks;

    pGetScanLine->InVerticalBlank = scanLine >= height;
    pGetScanLine->ScanLine = static_cast<ULONG>(min(scanLine, totalLines - 1));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::ControlInterrupt(_In_ DXGK_INTERRUPT_TYPE interruptType, _In_ BOOLEAN enableInterrupt)
{
    PAGED_CODE();

    switch (interruptType)
    {
        case DXGK_INTERRUPT_DMA_COMPLETED:
        case DXGK_INTERRUPT_DMA_PREEMPTED:
        case DXGK_INTERRUPT_DMA_FAULTED:
            break;
        default:
            return STATUS_NOT_SUPPORTED;
    }

    if (!IsDriverActive() || !IsHardwareInit() || m_pHWDevice == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    return m_pHWDevice->ControlInterrupt(enableInterrupt);
}

#if defined(VIOGPU_NATIVE_CONTEXT)
static NTSTATUS VioGpuQueryWin7DriverCaps(_In_ CONST DXGKARG_QUERYADAPTERINFO *queryAdapterInfo,
                                          _In_ BOOLEAN pointerEnabled)
{
    if (queryAdapterInfo->OutputDataSize < VIOGPU_WIN7_DRIVERCAPS_SIZE)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("QueryAdapterInfo output size (0x%u) is smaller than the Win7 DXGK_DRIVERCAPS prefix (0x%u)\n",
                  queryAdapterInfo->OutputDataSize,
                  VIOGPU_WIN7_DRIVERCAPS_SIZE));
        return STATUS_BUFFER_TOO_SMALL;
    }

    DXGK_DRIVERCAPS *driverCaps = static_cast<DXGK_DRIVERCAPS *>(queryAdapterInfo->pOutputData);
    RtlZeroMemory(driverCaps, VIOGPU_WIN7_DRIVERCAPS_SIZE);
    driverCaps->WDDMVersion = DXGKDDI_WDDMv1;
    driverCaps->HighestAcceptableAddress.QuadPart = (ULONG64)-1;

    if (pointerEnabled)
    {
        driverCaps->MaxPointerWidth = POINTER_SIZE;
        driverCaps->MaxPointerHeight = POINTER_SIZE;
        driverCaps->PointerCaps.Color = 1;
        driverCaps->PointerCaps.MaskedColor = 1;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu Win7 DriverCaps: size=%u wddm=0x%04X pointerCaps=0x%08X\n",
               VIOGPU_WIN7_DRIVERCAPS_SIZE,
               driverCaps->WDDMVersion,
               driverCaps->PointerCaps.Value);
    return STATUS_SUCCESS;
}
#endif

NTSTATUS VioGpuDod::QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pQueryAdapterInfo != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    NTSTATUS status;

    switch (pQueryAdapterInfo->Type)
    {
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_6)
        case DXGKQAITYPE_WDDMDEVICECAPS:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_WDDMDEVICECAPS))
                {
                    status = STATUS_BUFFER_TOO_SMALL;
                    break;
                }

                DXGK_WDDMDEVICECAPS *pWddmDeviceCaps = (DXGK_WDDMDEVICECAPS *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pWddmDeviceCaps, pQueryAdapterInfo->OutputDataSize);
                /* The Native Context target is compiled against the Win8 DDI
                 * table, but deliberately reports the legacy WDDM profile
                 * until the WDDM 1.2 mandatory feature set is implemented. */
#if defined(VIOGPU_NATIVE_CONTEXT)
                pWddmDeviceCaps->WDDMVersion = DXGKDDI_WDDMv1;
#else
                pWddmDeviceCaps->WDDMVersion = DXGKDDI_WDDMv1_2;
#endif
                DbgPrintEx(DPFLTR_DEFAULT_ID,
                           DPFLTR_INFO_LEVEL,
                           "viogpu WddmDeviceCaps: wddm=0x%04X\n",
                           pWddmDeviceCaps->WDDMVersion);
                status = STATUS_SUCCESS;
                break;
            }
#endif

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_9)
        case DXGKQAITYPE_PHYSICAL_MEMORY_CAPS:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_PHYSICAL_MEMORY_CAPS))
                {
                    status = STATUS_BUFFER_TOO_SMALL;
                    break;
                }

                DXGK_PHYSICAL_MEMORY_CAPS *pPhysicalMemoryCaps = (DXGK_PHYSICAL_MEMORY_CAPS *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pPhysicalMemoryCaps, pQueryAdapterInfo->OutputDataSize);
                pPhysicalMemoryCaps->HighestVisibleAddress.QuadPart = -1LL;
                status = STATUS_SUCCESS;
                break;
            }

        case DXGKQAITYPE_IOMMU_CAPS:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_IOMMU_CAPS))
                {
                    status = STATUS_BUFFER_TOO_SMALL;
                    break;
                }

                DXGK_IOMMU_CAPS *pIommuCaps = (DXGK_IOMMU_CAPS *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pIommuCaps, pQueryAdapterInfo->OutputDataSize);
                status = STATUS_SUCCESS;
                break;
            }
#endif

        case DXGKQAITYPE_DRIVERCAPS:
            {
#if defined(VIOGPU_NATIVE_CONTEXT)
                status = VioGpuQueryWin7DriverCaps(pQueryAdapterInfo, IsPointerEnabled());
#else
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_DRIVERCAPS))
                {
                    status = STATUS_BUFFER_TOO_SMALL;
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(DXGK_DRIVERCAPS) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              sizeof(DXGK_DRIVERCAPS)));
                    break;
                }

                DXGK_DRIVERCAPS *pDriverCaps = (DXGK_DRIVERCAPS *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pDriverCaps, pQueryAdapterInfo->OutputDataSize);
                pDriverCaps->WDDMVersion = DXGKDDI_WDDMv1_2;
                pDriverCaps->HighestAcceptableAddress.QuadPart = (ULONG64)-1;

                if (IsPointerEnabled())
                {
                    pDriverCaps->MaxPointerWidth = POINTER_SIZE;
                    pDriverCaps->MaxPointerHeight = POINTER_SIZE;
                    pDriverCaps->PointerCaps.Color = 1;
                    pDriverCaps->PointerCaps.MaskedColor = 1;
                }
                pDriverCaps->SupportNonVGA = TRUE;
                pDriverCaps->SupportSmoothRotation = TRUE;
                DbgPrintEx(DPFLTR_DEFAULT_ID,
                           DPFLTR_INFO_LEVEL,
                           "viogpu DriverCaps: wddm=0x%04X vga=%u nonVga=%u smoothRotation=%u pointerCaps=0x%08X\n",
                           pDriverCaps->WDDMVersion,
                           IsVgaDevice(),
                           pDriverCaps->SupportNonVGA,
                           pDriverCaps->SupportSmoothRotation,
                           pDriverCaps->PointerCaps.Value);
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s 1\n", __FUNCTION__));
                status = STATUS_SUCCESS;
#endif
                break;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
                status = STATUS_NOT_SUPPORTED;
                break;
            }
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               NT_SUCCESS(status) ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "viogpu QueryAdapterInfo: type=%u output=%u driverCaps=%u status=0x%08X\n",
               pQueryAdapterInfo->Type,
               pQueryAdapterInfo->OutputDataSize,
#if defined(VIOGPU_NATIVE_CONTEXT)
               VIOGPU_WIN7_DRIVERCAPS_SIZE,
#else
               (ULONG)sizeof(DXGK_DRIVERCAPS),
#endif
               status);
    return status;
}

NTSTATUS VioGpuDod::SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pSetPointerPosition != NULL);
    VIOGPU_ASSERT(pSetPointerPosition->VidPnSourceId < MAX_VIEWS);
    if (IsPointerEnabled() && pSetPointerPosition->VidPnSourceId == 0)
    {
        return m_pHWDevice->SetPointerPosition(pSetPointerPosition, &m_CurrentMode);
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pSetPointerShape != NULL);

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<---> %s Height = %d, Width = %d, XHot= %d, YHot = %d SourceId = %d\n",
              __FUNCTION__,
              pSetPointerShape->Height,
              pSetPointerShape->Width,
              pSetPointerShape->XHot,
              pSetPointerShape->YHot,
              pSetPointerShape->VidPnSourceId));
    if (IsPointerEnabled() && pSetPointerShape->VidPnSourceId == 0)
    {
        return m_pHWDevice->SetPointerShape(pSetPointerShape, &m_CurrentMode);
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::Escape(_In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pEscape != NULL);

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s Flags = %d\n", __FUNCTION__, pEscape->Flags.Value));

    return m_pHWDevice->Escape(pEscape);
}

NTSTATUS VioGpuDod::PresentDisplayOnly(_In_ CONST DXGKARG_PRESENT_DISPLAYONLY *pPresentDisplayOnly)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    VIOGPU_ASSERT(pPresentDisplayOnly != NULL);
    VIOGPU_ASSERT(pPresentDisplayOnly->VidPnSourceId < MAX_VIEWS);

    if (pPresentDisplayOnly->BytesPerPixel < 4 || pPresentDisplayOnly->VidPnSourceId != 0)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pPresentDisplayOnly->BytesPerPixel is 0x%d, which is lower than the allowed.\n",
                  pPresentDisplayOnly->BytesPerPixel));
        return STATUS_INVALID_PARAMETER;
    }

    if ((m_MonitorPowerState > PowerDeviceD0) || (m_CurrentMode.Flags.SourceNotVisible))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s Source is not visiable\n", __FUNCTION__));
        return STATUS_SUCCESS;
    }

    if (!m_CurrentMode.Flags.FrameBufferIsActive)
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("<--- %s Frame Buffer is Not active\n", __FUNCTION__));
        return STATUS_UNSUCCESSFUL;
    }

    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION RotationNeededByFb = pPresentDisplayOnly->Flags.Rotate ? m_CurrentMode.Rotation
                                                                                               : D3DKMDT_VPPR_IDENTITY;
    BYTE *pDst = (BYTE *)m_CurrentMode.FrameBuffer;
    UINT DstBitPerPixel = BPPFromPixelFormat(m_CurrentMode.DispInfo.ColorFormat);
    if (m_CurrentMode.Scaling == D3DKMDT_VPPS_CENTERED)
    {
        UINT CenterShift = (m_CurrentMode.DispInfo.Height - m_CurrentMode.SrcModeHeight) * m_CurrentMode.DispInfo.Pitch;
        CenterShift += (m_CurrentMode.DispInfo.Width - m_CurrentMode.SrcModeWidth) * DstBitPerPixel / 8;
        pDst += (int)CenterShift / 2;
    }
    Status = m_pHWDevice->ExecutePresentDisplayOnly(pDst,
                                                    DstBitPerPixel,
                                                    (BYTE *)pPresentDisplayOnly->pSource,
                                                    pPresentDisplayOnly->BytesPerPixel,
                                                    pPresentDisplayOnly->Pitch,
                                                    pPresentDisplayOnly->NumMoves,
                                                    pPresentDisplayOnly->pMoves,
                                                    pPresentDisplayOnly->NumDirtyRects,
                                                    pPresentDisplayOnly->pDirtyRect,
                                                    RotationNeededByFb,
                                                    &m_CurrentMode);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::QueryInterface(_In_ CONST PQUERY_INTERFACE pQueryInterface)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pQueryInterface != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s Version = %d\n", __FUNCTION__, pQueryInterface->Version));

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS VioGpuDod::StopDeviceAndReleasePostDisplayOwnership(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                             _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo)
{
    PAGED_CODE();

    VIOGPU_ASSERT(TargetId < MAX_CHILDREN);

    // FIXME!!!
    if (m_MonitorPowerState > PowerDeviceD0)
    {
        SetPowerState(TargetId, PowerDeviceD0, PowerActionNone);
    }

    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return STATUS_DEVICE_NOT_READY;
    }
    VioGpuAdapter *adapter = m_pHWDevice;
    if (IsHardwareResetRequested() || adapter == NULL)
    {
        ExReleaseRundownProtection(&m_HardwareOperations);
        return STATUS_DEVICE_NOT_READY;
    }
    adapter->BlackOutScreen(&m_CurrentMode);
    ExReleaseRundownProtection(&m_HardwareOperations);
    DbgPrint(TRACE_LEVEL_FATAL,
             ("StopDeviceAndReleasePostDisplayOwnership Width = %d Height = %d Pitch = %d ColorFormat = %dn",
              m_SystemDisplayInfo.Width,
              m_SystemDisplayInfo.Height,
              m_SystemDisplayInfo.Pitch,
              m_SystemDisplayInfo.ColorFormat));

    *pDisplayInfo = m_SystemDisplayInfo;
    pDisplayInfo->TargetId = TargetId;
    pDisplayInfo->AcpiId = m_CurrentMode.DispInfo.AcpiId;
    return StopDevice();
}

NTSTATUS VioGpuDod::QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pVidPnHWCaps != NULL);
    VIOGPU_ASSERT(pVidPnHWCaps->SourceId < MAX_VIEWS);
    VIOGPU_ASSERT(pVidPnHWCaps->TargetId < MAX_CHILDREN);

#if defined(VIOGPU_NATIVE_CONTEXT)
    pVidPnHWCaps->VidPnHWCaps.DriverRotation = 0;
#else
    pVidPnHWCaps->VidPnHWCaps.DriverRotation = 1;
#endif
    pVidPnHWCaps->VidPnHWCaps.DriverScaling = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverCloning = 0;
#if defined(VIOGPU_NATIVE_CONTEXT)
    pVidPnHWCaps->VidPnHWCaps.DriverColorConvert = 0;
#else
    pVidPnHWCaps->VidPnHWCaps.DriverColorConvert = 1;
#endif
    pVidPnHWCaps->VidPnHWCaps.DriverLinkedAdapaterOutput = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverRemoteDisplay = 0;
    pVidPnHWCaps->VidPnHWCaps.Reserved = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pIsSupportedVidPn != NULL);

    if (pIsSupportedVidPn->hDesiredVidPn == 0)
    {
        pIsSupportedVidPn->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }

    pIsSupportedVidPn->IsVidPnSupported = FALSE;

    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface;
    NTSTATUS Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pIsSupportedVidPn->hDesiredVidPn,
                                                                DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                                &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hDesiredVidPn = %llu\n",
                  Status,
                  LONG_PTR(pIsSupportedVidPn->hDesiredVidPn)));
        return Status;
    }

    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface;
    Status = pVidPnInterface->pfnGetTopology(pIsSupportedVidPn->hDesiredVidPn,
                                             &hVidPnTopology,
                                             &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hDesiredVidPn = %llu\n",
                  Status,
                  LONG_PTR(pIsSupportedVidPn->hDesiredVidPn)));
        return Status;
    }

    for (D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId = 0; SourceId < MAX_VIEWS; ++SourceId)
    {
        SIZE_T NumPathsFromSource = 0;
        Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology, SourceId, &NumPathsFromSource);
        if (Status == STATUS_GRAPHICS_SOURCE_NOT_IN_TOPOLOGY)
        {
            continue;
        }
        else if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnGetNumPathsFromSource failed with Status = 0x%X hVidPnTopology = %llu, SourceId = %llu",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      LONG_PTR(SourceId)));
            return Status;
        }
        else if (NumPathsFromSource > MAX_CHILDREN)
        {
            return STATUS_SUCCESS;
        }

        // Check if resolution exceeds framebuffer segment capacity
        if (NumPathsFromSource == 0)
        {
            continue;
        }

        D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet;
        CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface;
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pIsSupportedVidPn->hDesiredVidPn,
                                                          SourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            if (Status == STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE)
            {
                continue;
            }
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--- %s pfnAcquireSourceModeSet failed with Status = 0x%X, SourceId = %llu\n",
                      __FUNCTION__,
                      Status,
                      LONG_PTR(SourceId)));
            return Status;
        }

        CONST D3DKMDT_VIDPN_SOURCE_MODE *pPinnedVidPnSourceModeInfo = NULL;
        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet,
                                                                        &pPinnedVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            pVidPnInterface->pfnReleaseSourceModeSet(pIsSupportedVidPn->hDesiredVidPn, hVidPnSourceModeSet);
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--- %s pfnAcquirePinnedModeInfo failed with Status = 0x%X\n", __FUNCTION__, Status));
            return Status;
        }

        BOOLEAN bReject = FALSE;
        if (pPinnedVidPnSourceModeInfo != NULL)
        {
            SIZE_T RequiredSize = (SIZE_T)pPinnedVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cx *
                                  pPinnedVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cy *
                                  (BPPFromPixelFormat(pPinnedVidPnSourceModeInfo->Format.Graphics.PixelFormat) /
                                   BITS_PER_BYTE);
            SIZE_T SegmentSize = m_pHWDevice->GetFrameSegmentSize();
            if (SegmentSize > 0 && RequiredSize > SegmentSize)
            {
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("<--- %s Resolution requires %llu bytes, segment size is %llu\n",
                          __FUNCTION__,
                          (ULONGLONG)RequiredSize,
                          (ULONGLONG)SegmentSize));
                bReject = TRUE;
            }
            pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pPinnedVidPnSourceModeInfo);
        }
        pVidPnInterface->pfnReleaseSourceModeSet(pIsSupportedVidPn->hDesiredVidPn, hVidPnSourceModeSet);

        if (bReject)
        {
            return STATUS_NO_MEMORY;
        }
    }

    pIsSupportedVidPn->IsVidPnSupported = TRUE;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpuDod::RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pRecommendFunctionalVidPn == NULL);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS VioGpuDod::RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pRecommendVidPnTopology == NULL);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS VioGpuDod::RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return AddSingleMonitorMode(pRecommendMonitorModes);
}

NTSTATUS VioGpuDod::AddSingleSourceMode(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface,
                                        D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                        D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(SourceId);

    for (ULONG idx = 0; idx < m_pHWDevice->GetModeCount(); ++idx)
    {
        D3DKMDT_VIDPN_SOURCE_MODE *pVidPnSourceModeInfo = NULL;
        PVIDEO_MODE_INFORMATION pModeInfo = m_pHWDevice->GetModeInfo(idx);
        NTSTATUS Status = pVidPnSourceModeSetInterface->pfnCreateNewModeInfo(hVidPnSourceModeSet,
                                                                             &pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = %llu",
                      Status,
                      LONG_PTR(hVidPnSourceModeSet)));
            return Status;
        }

        pVidPnSourceModeInfo->Type = D3DKMDT_RMT_GRAPHICS;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cx = pModeInfo->VisScreenWidth;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cy = pModeInfo->VisScreenHeight;
        pVidPnSourceModeInfo->Format.Graphics.VisibleRegionSize = pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize;
        pVidPnSourceModeInfo->Format.Graphics.Stride = pModeInfo->ScreenStride;
        pVidPnSourceModeInfo->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
        pVidPnSourceModeInfo->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
        pVidPnSourceModeInfo->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;

        Status = pVidPnSourceModeSetInterface->pfnAddMode(hVidPnSourceModeSet, pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            NTSTATUS TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet,
                                                                                   pVidPnSourceModeInfo);
            UNREFERENCED_PARAMETER(TempStatus);
            NT_ASSERT(NT_SUCCESS(TempStatus));

            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode failed with Status = 0x%X, hVidPnSourceModeSet = %llu, pVidPnSourceModeInfo = %p",
                          Status,
                          LONG_PTR(hVidPnSourceModeSet),
                          pVidPnSourceModeInfo));
                return Status;
            }
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

VOID VioGpuDod::BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *pVideoSignalInfo, PVIDEO_MODE_INFORMATION pModeInfo)
{
    PAGED_CODE();

    static const UINT VIOGPU_DEFAULT_REFRESH_HZ = 60U;

    pVideoSignalInfo->VideoStandard = D3DKMDT_VSS_OTHER;
    pVideoSignalInfo->TotalSize.cx = pModeInfo->VisScreenWidth;
    pVideoSignalInfo->TotalSize.cy = pModeInfo->VisScreenHeight;
    pVideoSignalInfo->ActiveSize = pVideoSignalInfo->TotalSize;

    pVideoSignalInfo->VSyncFreq.Numerator = VIOGPU_DEFAULT_REFRESH_HZ;
    pVideoSignalInfo->VSyncFreq.Denominator = 1U;
    pVideoSignalInfo->HSyncFreq.Numerator = pModeInfo->VisScreenHeight * VIOGPU_DEFAULT_REFRESH_HZ;
    pVideoSignalInfo->HSyncFreq.Denominator = 1U;
    pVideoSignalInfo->PixelRate = static_cast<UINT64>(pModeInfo->VisScreenWidth) * pModeInfo->VisScreenHeight *
                                  VIOGPU_DEFAULT_REFRESH_HZ;
    pVideoSignalInfo->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

NTSTATUS VioGpuDod::AddSingleTargetMode(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface,
                                        D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                        _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo,
                                        D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(SourceId);

    if (pVidPnPinnedSourceModeInfo != NULL && pVidPnPinnedSourceModeInfo->Type != D3DKMDT_RMT_GRAPHICS)
    {
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }

    PVIDEO_MODE_INFORMATION pModeInfo = NULL;
    if (pVidPnPinnedSourceModeInfo != NULL)
    {
        for (UINT ModeIndex = 0; ModeIndex < m_pHWDevice->GetModeCount(); ++ModeIndex)
        {
            PVIDEO_MODE_INFORMATION candidate = m_pHWDevice->GetModeInfo(ModeIndex);
            if (candidate->VisScreenWidth == pVidPnPinnedSourceModeInfo->Format.Graphics.VisibleRegionSize.cx &&
                candidate->VisScreenHeight == pVidPnPinnedSourceModeInfo->Format.Graphics.VisibleRegionSize.cy)
            {
                pModeInfo = candidate;
                break;
            }
        }
        if (pModeInfo == NULL)
        {
            return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
        }
    }
    else
    {
        pModeInfo = m_pHWDevice->GetModeInfo(m_pHWDevice->GetCurrentModeIndex());
    }

    D3DKMDT_VIDPN_TARGET_MODE *pVidPnTargetModeInfo = NULL;
    NTSTATUS Status = pVidPnTargetModeSetInterface->pfnCreateNewModeInfo(hVidPnTargetModeSet, &pVidPnTargetModeInfo);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnCreateNewModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = %llu",
                  Status,
                  LONG_PTR(hVidPnTargetModeSet)));
        return Status;
    }

    BuildVideoSignalInfo(&pVidPnTargetModeInfo->VideoSignalInfo, pModeInfo);
    pVidPnTargetModeInfo->Preference = D3DKMDT_MP_PREFERRED;

    Status = pVidPnTargetModeSetInterface->pfnAddMode(hVidPnTargetModeSet, pVidPnTargetModeInfo);
    if (!NT_SUCCESS(Status))
    {
        NTSTATUS AddStatus = Status;
        if (AddStatus != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAddMode failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu, "
                      "pVidPnTargetModeInfo = %p\n",
                      AddStatus,
                      LONG_PTR(hVidPnTargetModeSet),
                      pVidPnTargetModeInfo));
        }

        Status = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnTargetModeInfo);
        NT_ASSERT(NT_SUCCESS(Status));
        return AddStatus == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET ? STATUS_SUCCESS : AddStatus;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::AddSingleMonitorMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    D3DKMDT_MONITOR_SOURCE_MODE *pMonitorSourceMode = NULL;
    PVIDEO_MODE_INFORMATION pVbeModeInfo = NULL;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                          &pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnCreateNewModeInfo failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu\n",
                  Status,
                  LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet)));
        return Status;
    }

    pVbeModeInfo = m_pHWDevice->GetModeInfo(m_pHWDevice->GetCurrentModeIndex());

    BuildVideoSignalInfo(&pMonitorSourceMode->VideoSignalInfo, pVbeModeInfo);

    pMonitorSourceMode->Origin = D3DKMDT_MCO_DRIVER;
    pMonitorSourceMode->Preference = D3DKMDT_MP_PREFERRED;
    pMonitorSourceMode->ColorBasis = D3DKMDT_CB_SRGB;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FirstChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.SecondChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAddMode failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu, pMonitorSourceMode = "
                      "0x%p\n",
                      Status,
                      LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet),
                      pMonitorSourceMode));
        }
        else
        {
            Status = STATUS_SUCCESS;
        }

        NTSTATUS TempStatus = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                                         pMonitorSourceMode);
        UNREFERENCED_PARAMETER(TempStatus);
        NT_ASSERT(NT_SUCCESS(TempStatus));
        return Status;
    }

    for (UINT Idx = 0; Idx < m_pHWDevice->GetModeCount(); ++Idx)
    {
        pVbeModeInfo = m_pHWDevice->GetModeInfo(Idx);

        Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                              &pMonitorSourceMode);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu\n",
                      Status,
                      LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet)));
            return Status;
        }

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s: add pref mode, dimensions %ux%u, taken from DxgkCbAcquirePostDisplayOwnership at StartDevice\n",
                  __FUNCTION__,
                  pVbeModeInfo->VisScreenWidth,
                  pVbeModeInfo->VisScreenHeight));

        BuildVideoSignalInfo(&pMonitorSourceMode->VideoSignalInfo, pVbeModeInfo);

        pMonitorSourceMode->Origin = D3DKMDT_MCO_DRIVER;
        pMonitorSourceMode->ColorBasis = D3DKMDT_CB_SRGB;
        pMonitorSourceMode->ColorCoeffDynamicRanges.FirstChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.SecondChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;
        if (Idx == m_pHWDevice->GetCurrentModeIndex())
        {
            pMonitorSourceMode->Preference = D3DKMDT_MP_PREFERRED;
        }
        else
        {
            pMonitorSourceMode->Preference = D3DKMDT_MP_NOTPREFERRED;
        }

        Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                    pMonitorSourceMode);
        if (!NT_SUCCESS(Status))
        {
            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu, pMonitorSourceMode = "
                          "0x%p\n",
                          Status,
                          LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet),
                          pMonitorSourceMode));
            }

            Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                                pMonitorSourceMode);
            NT_ASSERT(NT_SUCCESS(Status));
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pEnumCofuncModality != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet = 0;
    D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface = NULL;
    CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPathTemp = NULL;
    CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo = NULL;
    CONST D3DKMDT_VIDPN_TARGET_MODE *pVidPnPinnedTargetModeInfo = NULL;

    NTSTATUS Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pEnumCofuncModality->hConstrainingVidPn,
                                                                DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                                &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
        return Status;
    }

    Status = pVidPnInterface->pfnGetTopology(pEnumCofuncModality->hConstrainingVidPn,
                                             &hVidPnTopology,
                                             &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
        return Status;
    }

    Status = pVidPnTopologyInterface->pfnAcquireFirstPathInfo(hVidPnTopology, &pVidPnPresentPath);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnAcquireFirstPathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        return Status;
    }

    while (Status != STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                          pVidPnPresentPath->VidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, SourceId = "
                      "0x%llu\n",
                      Status,
                      LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                      LONG_PTR(pVidPnPresentPath->VidPnSourceId)));
            break;
        }

        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet,
                                                                        &pVidPnPinnedSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = 0x%llu\n",
                      Status,
                      LONG_PTR(hVidPnSourceModeSet)));
            break;
        }

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId)))
        {
            if (pVidPnPinnedSourceModeInfo == NULL)
            {
                Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnSourceModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnSourceModeSet)));
                    break;
                }
                hVidPnSourceModeSet = 0;

                Status = pVidPnInterface->pfnCreateNewSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnSourceId,
                                                                    &hVidPnSourceModeSet,
                                                                    &pVidPnSourceModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnCreateNewSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "SourceId = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnSourceId)));
                    break;
                }

                {
                    Status = AddSingleSourceMode(pVidPnSourceModeSetInterface,
                                                 hVidPnSourceModeSet,
                                                 pVidPnPresentPath->VidPnSourceId);
                }

                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("AddSingleSourceMode failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
                    break;
                }

                Status = pVidPnInterface->pfnAssignSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                 pVidPnPresentPath->VidPnSourceId,
                                                                 hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnAssignSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, SourceId "
                              "= 0x%llu, hVidPnSourceModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnSourceId),
                              LONG_PTR(hVidPnSourceModeSet)));
                    break;
                }
                hVidPnSourceModeSet = 0;
            }
        }

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            Status = pVidPnInterface->pfnAcquireTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              pVidPnPresentPath->VidPnTargetId,
                                                              &hVidPnTargetModeSet,
                                                              &pVidPnTargetModeSetInterface);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAcquireTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, TargetId = "
                          "0x%llu\n",
                          Status,
                          LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                          LONG_PTR(pVidPnPresentPath->VidPnTargetId)));
                break;
            }

            Status = pVidPnTargetModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnTargetModeSet,
                                                                            &pVidPnPinnedTargetModeInfo);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu\n",
                          Status,
                          LONG_PTR(hVidPnTargetModeSet)));
                break;
            }

            if (pVidPnPinnedTargetModeInfo == NULL)
            {
                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;

                Status = pVidPnInterface->pfnCreateNewTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnTargetId,
                                                                    &hVidPnTargetModeSet,
                                                                    &pVidPnTargetModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnCreateNewTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "TargetId = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnTargetId)));
                    break;
                }

                Status = AddSingleTargetMode(pVidPnTargetModeSetInterface,
                                             hVidPnTargetModeSet,
                                             pVidPnPinnedSourceModeInfo,
                                             pVidPnPresentPath->VidPnSourceId);

                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("AddSingleTargetMode failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
                    break;
                }

                Status = pVidPnInterface->pfnAssignTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                 pVidPnPresentPath->VidPnTargetId,
                                                                 hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnAssignTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, TargetId "
                              "= 0x%llu, hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnTargetId),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;
            }
            else
            {
                Status = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet,
                                                                          pVidPnPinnedTargetModeInfo);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu, "
                              "pVidPnPinnedTargetModeInfo = %p\n",
                              Status,
                              LONG_PTR(hVidPnTargetModeSet),
                              pVidPnPinnedTargetModeInfo));
                    break;
                }
                pVidPnPinnedTargetModeInfo = NULL;

                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;
            }
        }

        if (pVidPnPinnedSourceModeInfo != NULL)
        {
            Status = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnReleaseModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = 0x%llu, "
                          "pVidPnPinnedSourceModeInfo = %p\n",
                          Status,
                          LONG_PTR(hVidPnSourceModeSet),
                          pVidPnPinnedSourceModeInfo));
                break;
            }
            pVidPnPinnedSourceModeInfo = NULL;
        }

        if (hVidPnSourceModeSet != 0)
        {
            Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnSourceModeSet);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnReleaseSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                          "hVidPnSourceModeSet = 0x%llu\n",
                          Status,
                          LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                          LONG_PTR(hVidPnSourceModeSet)));
                break;
            }
            hVidPnSourceModeSet = 0;
        }

        D3DKMDT_VIDPN_PRESENT_PATH LocalVidPnPresentPath = *pVidPnPresentPath;
        BOOLEAN SupportFieldsModified = FALSE;

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_SCALING) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            if (pVidPnPresentPath->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED)
            {
                RtlZeroMemory(&(LocalVidPnPresentPath.ContentTransformation.ScalingSupport),
                              sizeof(D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT));
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Identity = 1;
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Centered = 1;
                SupportFieldsModified = TRUE;
            }
        }

        if (!((pEnumCofuncModality->EnumPivotType != D3DKMDT_EPT_ROTATION) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            if (pVidPnPresentPath->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)
            {
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Identity = 1;
#if defined(VIOGPU_NATIVE_CONTEXT)
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate90 = 0;
#else
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate90 = 1;
#endif
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate180 = 0;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate270 = 0;
                SupportFieldsModified = TRUE;
            }
        }

        if (SupportFieldsModified)
        {
            Status = pVidPnTopologyInterface->pfnUpdatePathSupportInfo(hVidPnTopology, &LocalVidPnPresentPath);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnUpdatePathSupportInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                          Status,
                          LONG_PTR(hVidPnTopology)));
                break;
            }
        }

        pVidPnPresentPathTemp = pVidPnPresentPath;
        Status = pVidPnTopologyInterface->pfnAcquireNextPathInfo(hVidPnTopology,
                                                                 pVidPnPresentPathTemp,
                                                                 &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireNextPathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, "
                      "pVidPnPresentPathTemp = %p\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPathTemp));
            break;
        }

        NTSTATUS TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        if (!NT_SUCCESS(TempStatus))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnReleasePathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, pVidPnPresentPathTemp = "
                      "%p\n",
                      TempStatus,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPathTemp));
            Status = TempStatus;
            break;
        }
        pVidPnPresentPathTemp = NULL;
    }

    if (Status == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        Status = STATUS_SUCCESS;
    }

    NTSTATUS TempStatus = STATUS_NOT_FOUND;

    if ((pVidPnSourceModeSetInterface != NULL) && (pVidPnPinnedSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTargetModeSetInterface != NULL) && (pVidPnPinnedTargetModeInfo != NULL))
    {
        TempStatus = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnPinnedTargetModeInfo);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPath != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPathTemp != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnSourceModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnSourceModeSet);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnTargetModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnTargetModeSet);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    VIOGPU_ASSERT_CHK(TempStatus == STATUS_NOT_FOUND || Status != STATUS_SUCCESS);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pSetVidPnSourceVisibility != NULL);
    VIOGPU_ASSERT((pSetVidPnSourceVisibility->VidPnSourceId < MAX_VIEWS) ||
                  (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL));

    if (pSetVidPnSourceVisibility->Visible)
    {
        m_CurrentMode.Flags.FullscreenPresent = TRUE;
    }
    else
    {
#if !defined(VIOGPU_NATIVE_CONTEXT)
        m_pHWDevice->BlackOutScreen(&m_CurrentMode);
#endif
    }

    m_CurrentMode.Flags.SourceNotVisible = !(pSetVidPnSourceVisibility->Visible);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pCommitVidPn != NULL);
    VIOGPU_ASSERT(pCommitVidPn->AffectedVidPnSourceId < MAX_VIEWS);

    NTSTATUS Status;
    SIZE_T NumPaths = 0;
    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_SOURCE_MODE *pPinnedVidPnSourceModeInfo = NULL;

    if (pCommitVidPn->Flags.PathPoweredOff)
    {
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pCommitVidPn->hFunctionalVidPn,
                                                       DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                       &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
        goto CommitVidPnExit;
    }

    Status = pVidPnInterface->pfnGetTopology(pCommitVidPn->hFunctionalVidPn, &hVidPnTopology, &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
        goto CommitVidPnExit;
    }

    Status = pVidPnTopologyInterface->pfnGetNumPaths(hVidPnTopology, &NumPaths);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetNumPaths failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        goto CommitVidPnExit;
    }

    if (NumPaths != 0)
    {
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pCommitVidPn->hFunctionalVidPn,
                                                          pCommitVidPn->AffectedVidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireSourceModeSet failed with Status = 0x%X, hFunctionalVidPn = 0x%llu, SourceId = "
                      "0x%I64x\n",
                      Status,
                      LONG_PTR(pCommitVidPn->hFunctionalVidPn),
                      pCommitVidPn->AffectedVidPnSourceId));
            goto CommitVidPnExit;
        }

        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet,
                                                                        &pPinnedVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                      Status,
                      LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
            goto CommitVidPnExit;
        }
    }
    else
    {
        pPinnedVidPnSourceModeInfo = NULL;
    }

    if (pPinnedVidPnSourceModeInfo == NULL)
    {
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    Status = IsVidPnSourceModeFieldsValid(pPinnedVidPnSourceModeInfo);
    if (!NT_SUCCESS(Status))
    {
        goto CommitVidPnExit;
    }

    SIZE_T NumPathsFromSource = 0;
    Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology,
                                                               pCommitVidPn->AffectedVidPnSourceId,
                                                               &NumPathsFromSource);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetNumPathsFromSource failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        goto CommitVidPnExit;
    }

    for (SIZE_T PathIndex = 0; PathIndex < NumPathsFromSource; ++PathIndex)
    {
        D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId = D3DDDI_ID_UNINITIALIZED;
        Status = pVidPnTopologyInterface->pfnEnumPathTargetsFromSource(hVidPnTopology,
                                                                       pCommitVidPn->AffectedVidPnSourceId,
                                                                       PathIndex,
                                                                       &TargetId);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnEnumPathTargetsFromSource failed with Status = 0x%X, hVidPnTopology = 0x%llu, SourceId = "
                      "0x%I64x, PathIndex = 0x%I64x\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pCommitVidPn->AffectedVidPnSourceId,
                      PathIndex));
            goto CommitVidPnExit;
        }

        Status = pVidPnTopologyInterface->pfnAcquirePathInfo(hVidPnTopology,
                                                             pCommitVidPn->AffectedVidPnSourceId,
                                                             TargetId,
                                                             &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, SourceId = 0x%I64x, "
                      "TargetId = 0x%I64x\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pCommitVidPn->AffectedVidPnSourceId,
                      TargetId));
            goto CommitVidPnExit;
        }

        Status = IsVidPnPathFieldsValid(pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = SetSourceModeAndPath(pPinnedVidPnSourceModeInfo, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnReleasePathInfo failed with Status = 0x%X, hVidPnTopoogy = 0x%llu, pVidPnPresentPath = %p\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPath));
            goto CommitVidPnExit;
        }
        pVidPnPresentPath = NULL;
    }

CommitVidPnExit:

    NTSTATUS TempStatus = STATUS_SUCCESS;

    if ((pVidPnSourceModeSetInterface != NULL) && (hVidPnSourceModeSet != 0) && (pPinnedVidPnSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pPinnedVidPnSourceModeInfo);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnInterface != NULL) && (pCommitVidPn->hFunctionalVidPn != 0) && (hVidPnSourceModeSet != 0))
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pCommitVidPn->hFunctionalVidPn, hVidPnSourceModeSet);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTopologyInterface != NULL) && (hVidPnTopology != 0) && (pVidPnPresentPath != NULL))
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return Status;
}

NTSTATUS VioGpuDod::SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode,
                                         CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath)
{
    PAGED_CODE();
    VIOGPU_ASSERT(pPath->VidPnSourceId < MAX_VIEWS);

    NTSTATUS Status = STATUS_SUCCESS;

    CURRENT_MODE *pCurrentMode = &m_CurrentMode;
    DbgPrint(TRACE_LEVEL_FATAL,
             ("---> %s (%dx%d)\n",
              __FUNCTION__,
              pSourceMode->Format.Graphics.VisibleRegionSize.cx,
              pSourceMode->Format.Graphics.VisibleRegionSize.cy));
    pCurrentMode->Scaling = pPath->ContentTransformation.Scaling;
    pCurrentMode->SrcModeWidth = pSourceMode->Format.Graphics.VisibleRegionSize.cx;
    pCurrentMode->SrcModeHeight = pSourceMode->Format.Graphics.VisibleRegionSize.cy;
    pCurrentMode->Rotation = pPath->ContentTransformation.Rotation;

    pCurrentMode->DispInfo.Width = pSourceMode->Format.Graphics.PrimSurfSize.cx;
    pCurrentMode->DispInfo.Height = pSourceMode->Format.Graphics.PrimSurfSize.cy;
    pCurrentMode->DispInfo.Pitch = pSourceMode->Format.Graphics.PrimSurfSize.cx *
                                   BPPFromPixelFormat(pCurrentMode->DispInfo.ColorFormat) / BITS_PER_BYTE;

    if (NT_SUCCESS(Status))
    {
        pCurrentMode->Flags.FullscreenPresent = TRUE;
        for (USHORT ModeIndex = 0; ModeIndex < m_pHWDevice->GetModeCount(); ++ModeIndex)
        {
            PVIDEO_MODE_INFORMATION pModeInfo = m_pHWDevice->GetModeInfo(ModeIndex);
            if (pCurrentMode->DispInfo.Width == pModeInfo->VisScreenWidth &&
                pCurrentMode->DispInfo.Height == pModeInfo->VisScreenHeight)
            {
                Status = m_pHWDevice->SetCurrentMode(m_pHWDevice->GetModeNumber(ModeIndex), pCurrentMode);
                if (NT_SUCCESS(Status))
                {
                    m_pHWDevice->SetCurrentModeIndex(ModeIndex);
                }
                break;
            }
        }
    }

    return Status;
}

NTSTATUS VioGpuDod::IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath) const
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pPath->VidPnSourceId >= MAX_VIEWS)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("VidPnSourceId is 0x%I64x is too high (MAX_VIEWS is 0x%I64x)", pPath->VidPnSourceId, MAX_VIEWS));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }
    else if (pPath->VidPnTargetId >= MAX_CHILDREN)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("VidPnTargetId is 0x%I64x is too high (MAX_CHILDREN is 0x%I64x)",
                  pPath->VidPnTargetId,
                  MAX_CHILDREN));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }
    else if (pPath->GammaRamp.Type != D3DDDI_GAMMARAMP_DEFAULT)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pPath contains a gamma ramp (0x%I64x)", pPath->GammaRamp.Type));
        return STATUS_GRAPHICS_GAMMA_RAMP_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_IDENTITY) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_CENTERED) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_NOTSPECIFIED) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pPath contains a non-identity scaling (0x%I64x)", pPath->ContentTransformation.Scaling));
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_IDENTITY) &&
#if !defined(VIOGPU_NATIVE_CONTEXT)
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_ROTATE90) &&
#endif
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_NOTSPECIFIED) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pPath contains a not-supported rotation (0x%I64x)", pPath->ContentTransformation.Rotation));
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->VidPnTargetColorBasis != D3DKMDT_CB_SCRGB) &&
             (pPath->VidPnTargetColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pPath has a non-linear RGB color basis (0x%I64x)", pPath->VidPnTargetColorBasis));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode) const
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pSourceMode->Type != D3DKMDT_RMT_GRAPHICS)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pSourceMode is a non-graphics mode (0x%I64x)", pSourceMode->Type));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if ((pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_SCRGB) &&
             (pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pSourceMode has a non-linear RGB color basis (0x%I64x)", pSourceMode->Format.Graphics.ColorBasis));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if (pSourceMode->Format.Graphics.PixelValueAccessMode != D3DKMDT_PVAM_DIRECT)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pSourceMode has a palettized access mode (0x%I64x)",
                  pSourceMode->Format.Graphics.PixelValueAccessMode));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else
    {
        if (pSourceMode->Format.Graphics.PixelFormat == D3DDDIFMT_A8R8G8B8)
        {
            return STATUS_SUCCESS;
        }
    }

    DbgPrint(TRACE_LEVEL_ERROR,
             ("pSourceMode has an unknown pixel format (0x%I64x)", pSourceMode->Format.Graphics.PixelFormat));

    return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
}

NTSTATUS
VioGpuDod::UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pUpdateActiveVidPnPresentPath != NULL);
    VIOGPU_ASSERT(pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.VidPnSourceId == 0);

    NTSTATUS Status = IsVidPnPathFieldsValid(&(pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    m_CurrentMode.Flags.FullscreenPresent = TRUE;

    m_CurrentMode.Rotation = pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.ContentTransformation.Rotation;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

//
// Non-Paged Code
//
#pragma code_seg(push)
#pragma code_seg()

#if defined(VIOGPU_NATIVE_CONTEXT)
static ULONG VioGpuReadSharedU32(_In_ const volatile ULONG *value)
{
    // The control blob is reached through a PCI BAR.  ARM64 exclusive atomics are not valid for
    // every MMIO memory type, so use one volatile access plus an explicit ordering barrier.
    ULONG result = *value;
    KeMemoryBarrier();
    return result;
}

static VOID VioGpuWriteSharedU32(_Out_ volatile ULONG *value, _In_ ULONG newValue)
{
    KeMemoryBarrier();
    *value = newValue;
    KeMemoryBarrier();
}

static BOOLEAN VioGpuResolveNativeControlWindow(_In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner,
                                                _Out_ PMSM_SHMEM *shmem,
                                                _Out_ PUCHAR *response,
                                                _Out_ PULONG responseCapacity,
                                                _Inout_opt_ PVIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC diagnostic = NULL)
{
    if (diagnostic != NULL)
    {
        diagnostic->WindowStatus = VioGpuNativeContextParameterWindowNotChecked;
        if (owner != NULL)
        {
            diagnostic->ControlBarOffset = owner->ControlBarOffset;
            diagnostic->ControlAddress = reinterpret_cast<ULONG_PTR>(owner->ControlAddress);
            diagnostic->ControlBlobSize = owner->ControlBlobSize;
        }
    }

    if (owner == NULL || shmem == NULL || response == NULL || responseCapacity == NULL)
    {
        if (diagnostic != NULL)
        {
            diagnostic->WindowStatus = VioGpuNativeContextParameterWindowInvalidOwner;
        }
        return FALSE;
    }
    if (!owner->ControlResourceCreated || !owner->ControlMapped || owner->ControlResourceId == 0)
    {
        if (diagnostic != NULL)
        {
            diagnostic->WindowStatus = VioGpuNativeContextParameterWindowInvalidResource;
        }
        return FALSE;
    }
    if (owner->ControlBlobSize != VIOGPU_NATIVE_CONTROL_BLOB_SIZE || owner->ControlBlobSize < sizeof(MSM_SHMEM))
    {
        if (diagnostic != NULL)
        {
            diagnostic->WindowStatus = VioGpuNativeContextParameterWindowInvalidSize;
        }
        return FALSE;
    }
    if ((owner->ControlBarOffset & (PAGE_SIZE - 1)) != 0)
    {
        if (diagnostic != NULL)
        {
            diagnostic->WindowStatus = VioGpuNativeContextParameterWindowInvalidOffset;
        }
        return FALSE;
    }
    if (owner->ControlAddress == NULL)
    {
        if (diagnostic != NULL)
        {
            diagnostic->WindowStatus = VioGpuNativeContextParameterWindowInvalidAddress;
        }
        return FALSE;
    }

    PUCHAR blob = static_cast<PUCHAR>(owner->ControlAddress);
    PMSM_SHMEM shared = reinterpret_cast<PMSM_SHMEM>(blob);
    ULONG responseOffset = VioGpuReadSharedU32(&shared->base.rsp_mem_offset);
    ULONG responseCapacityValue = owner->ControlBlobSize - sizeof(MSM_SHMEM);
    if (diagnostic != NULL)
    {
        diagnostic->ResponseOffset = responseOffset;
        diagnostic->ResponseCapacity = responseCapacityValue;
    }
    if (responseOffset != sizeof(MSM_SHMEM) || responseOffset > owner->ControlBlobSize)
    {
        if (diagnostic != NULL)
        {
            diagnostic->WindowStatus = VioGpuNativeContextParameterWindowInvalidResponseOffset;
        }
        return FALSE;
    }

    *shmem = shared;
    *response = blob + responseOffset;
    *responseCapacity = owner->ControlBlobSize - responseOffset;
    if (diagnostic != NULL)
    {
        diagnostic->ResponseCapacity = *responseCapacity;
        diagnostic->WindowStatus = VioGpuNativeContextParameterWindowReady;
    }
    return TRUE;
}

static BOOLEAN
VioGpuSeedNativeControlResponse(_In_ VioGpuAdapter *adapter,
                                _Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner,
                                _In_ ULONG sequence,
                                _In_ ULONG responseSize,
                                _Inout_opt_ PVIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC diagnostic = NULL)
{
    if (diagnostic != NULL)
    {
        diagnostic->SeedResult = VioGpuNativeContextParameterSeedNotAttempted;
    }
    if (adapter == NULL || owner == NULL || sequence == 0 || responseSize < sizeof(MSM_CCMD_IOCTL_SIMPLE_RSP) ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        if (diagnostic != NULL)
        {
            diagnostic->SeedResult = VioGpuNativeContextParameterSeedInvalidArguments;
        }
        return FALSE;
    }

    PMSM_SHMEM shmem = NULL;
    PUCHAR response = NULL;
    ULONG responseCapacity = 0;
    BOOLEAN resolved = VioGpuResolveNativeControlWindow(owner, &shmem, &response, &responseCapacity, diagnostic);
    if (!resolved)
    {
        if (diagnostic != NULL)
        {
            diagnostic->SeedResult = VioGpuNativeContextParameterSeedWindowUnavailable;
        }
        return FALSE;
    }
    ULONG sharedSeqno = VioGpuReadSharedU32(&shmem->base.seqno);
    ULONG sharedAsyncError = VioGpuReadSharedU32(&shmem->async_error);
    ULONG sharedGlobalFaults = VioGpuReadSharedU32(&shmem->global_faults);
    if (diagnostic != NULL)
    {
        diagnostic->SeedSharedSeqno = sharedSeqno;
        diagnostic->SharedSeqno = sharedSeqno;
        diagnostic->SharedAsyncError = sharedAsyncError;
        diagnostic->SharedGlobalFaults = sharedGlobalFaults;
    }
    if (responseSize > responseCapacity)
    {
        if (diagnostic != NULL)
        {
            diagnostic->SeedResult = VioGpuNativeContextParameterSeedResponseOutOfBounds;
        }
        return FALSE;
    }
    if (sharedSeqno == sequence)
    {
        if (diagnostic != NULL)
        {
            diagnostic->SeedResult = VioGpuNativeContextParameterSeedSequenceBusy;
        }
        return FALSE;
    }

    RtlZeroMemory(response, responseSize);
    PMSM_CCMD_IOCTL_SIMPLE_RSP responseHeader = reinterpret_cast<PMSM_CCMD_IOCTL_SIMPLE_RSP>(response);
    VioGpuWriteSharedU32(reinterpret_cast<volatile ULONG *>(&responseHeader->ret), MAXLONG);
    VioGpuWriteSharedU32(reinterpret_cast<volatile ULONG *>(&responseHeader->hdr.len), responseSize);
    if (diagnostic != NULL)
    {
        diagnostic->SeedResult = VioGpuNativeContextParameterSeedWritten;
    }
    return TRUE;
}

static BOOLEAN
VioGpuCopyNativeControlResponse(_In_ VioGpuAdapter *adapter,
                                _In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner,
                                _In_ ULONG sequence,
                                _Out_ PVOID response,
                                _In_ ULONG responseSize,
                                _Inout_opt_ PVIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC diagnostic = NULL)
{
    if (diagnostic != NULL)
    {
        diagnostic->CopyResult = VioGpuNativeContextParameterCopyNotAttempted;
    }
    if (adapter == NULL || owner == NULL || sequence == 0 || response == NULL ||
        responseSize < sizeof(MSM_CCMD_IOCTL_SIMPLE_RSP) || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        if (diagnostic != NULL)
        {
            diagnostic->CopyResult = VioGpuNativeContextParameterCopyInvalidArguments;
        }
        return FALSE;
    }
    RtlZeroMemory(response, responseSize);

    PMSM_SHMEM shmem = NULL;
    PUCHAR sharedResponse = NULL;
    ULONG responseCapacity = 0;
    BOOLEAN resolved = VioGpuResolveNativeControlWindow(owner, &shmem, &sharedResponse, &responseCapacity, diagnostic);
    if (!resolved)
    {
        if (diagnostic != NULL)
        {
            diagnostic->CopyResult = VioGpuNativeContextParameterCopyWindowUnavailable;
        }
        return FALSE;
    }
    ULONG sharedSeqno = VioGpuReadSharedU32(&shmem->base.seqno);
    ULONG sharedAsyncError = VioGpuReadSharedU32(&shmem->async_error);
    ULONG sharedGlobalFaults = VioGpuReadSharedU32(&shmem->global_faults);
    if (diagnostic != NULL)
    {
        diagnostic->SharedSeqno = sharedSeqno;
        diagnostic->SharedAsyncError = sharedAsyncError;
        diagnostic->SharedGlobalFaults = sharedGlobalFaults;
    }
    if (responseSize > responseCapacity)
    {
        if (diagnostic != NULL)
        {
            diagnostic->CopyResult = VioGpuNativeContextParameterCopyMalformedResponse;
        }
        return FALSE;
    }
    if (sharedSeqno != sequence)
    {
        if (diagnostic != NULL)
        {
            diagnostic->CopyResult = VioGpuNativeContextParameterCopySequenceMismatch;
        }
        return FALSE;
    }

    KeMemoryBarrier();
    RtlCopyMemory(response, sharedResponse, responseSize);
    PVDRM_CCMD_RSP responseHeader = static_cast<PVDRM_CCMD_RSP>(response);
    sharedAsyncError = VioGpuReadSharedU32(&shmem->async_error);
    sharedGlobalFaults = VioGpuReadSharedU32(&shmem->global_faults);
    if (diagnostic != NULL)
    {
        diagnostic->InnerResponseLength = responseHeader->len;
        diagnostic->SharedAsyncError = sharedAsyncError;
        diagnostic->SharedGlobalFaults = sharedGlobalFaults;
    }
    if (responseHeader->len != responseSize || sharedAsyncError != 0 || sharedGlobalFaults != 0)
    {
        if (diagnostic != NULL)
        {
            diagnostic->CopyResult = VioGpuNativeContextParameterCopyMalformedResponse;
        }
        return FALSE;
    }
    if (diagnostic != NULL)
    {
        diagnostic->CopyResult = VioGpuNativeContextParameterCopyCompleted;
    }
    return TRUE;
}

static BOOLEAN
VioGpuConsumeNativeControlResponse(_In_ VioGpuAdapter *adapter,
                                   _In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner,
                                   _In_ ULONG sequence,
                                   _In_ ULONG parameter,
                                   _Out_ PULONGLONG value,
                                   _Inout_opt_ PVIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC diagnostic = NULL)
{
    if (value == NULL)
    {
        return FALSE;
    }
    *value = 0;

    MSM_CCMD_IOCTL_SIMPLE_GET_PARAM_RSP response = {};
    BOOLEAN copied = VioGpuCopyNativeControlResponse(adapter, owner, sequence, &response, sizeof(response), diagnostic);
    if (diagnostic != NULL && copied)
    {
        diagnostic->InnerRet = static_cast<UINT>(response.ret);
        diagnostic->InnerPipe = response.param.pipe;
        diagnostic->InnerParameter = response.param.param;
        diagnostic->InnerValue = response.param.value;
        diagnostic->InnerValueLength = response.param.len;
        diagnostic->InnerPadding = response.param.pad;
    }
    if (!copied)
    {
        if (diagnostic != NULL)
        {
            diagnostic->Validation = VioGpuHostResponseMalformed;
        }
        return FALSE;
    }
    if (response.ret != 0)
    {
        if (diagnostic != NULL)
        {
            diagnostic->Validation = VioGpuHostResponseRejected;
        }
        return FALSE;
    }
    if (response.param.pipe != MSM_PIPE_3D0 || response.param.param != parameter || response.param.len != 0 ||
        response.param.pad != 0)
    {
        if (diagnostic != NULL)
        {
            diagnostic->Validation = VioGpuHostResponseMalformed;
        }
        return FALSE;
    }
    if (diagnostic != NULL)
    {
        diagnostic->Validation = VioGpuHostResponseConfirmed;
    }
    *value = response.param.value;
    return TRUE;
}

static BOOLEAN VioGpuNativeControlFaultsClear(_In_ VioGpuAdapter *adapter,
                                              _In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner)
{
    if (adapter == NULL || owner == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }

    PMSM_SHMEM shmem = NULL;
    PUCHAR response = NULL;
    ULONG responseCapacity = 0;
    BOOLEAN valid = VioGpuResolveNativeControlWindow(owner, &shmem, &response, &responseCapacity) &&
                    responseCapacity >= sizeof(MSM_CCMD_IOCTL_SIMPLE_RSP) &&
                    VioGpuReadSharedU32(&shmem->async_error) == 0 && VioGpuReadSharedU32(&shmem->global_faults) == 0;
    return valid;
}
#endif

void VioGpuAdapter::FailNativeContextAtAnyIrql(void)
{
    InterlockedIncrement(&m_NativeContextGeneration);
    InterlockedIncrement64(&m_NativeContextResetGeneration);
    InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
    LONG state = InterlockedCompareExchange(&m_NativeContextState,
                                            VioGpuNativeContextOffline,
                                            VioGpuNativeContextOffline);
    while (state != VioGpuNativeContextOffline && state != VioGpuNativeContextFailed)
    {
        LONG observed = InterlockedCompareExchange(&m_NativeContextState, VioGpuNativeContextFailed, state);
        if (observed == state)
        {
            state = VioGpuNativeContextFailed;
            break;
        }
        state = observed;
    }
    if (state != VioGpuNativeContextOffline)
    {
        m_CtrlQueue.PoisonSynchronousRequests();
    }
    if (m_pVioGpuDod != NULL)
    {
        m_pVioGpuDod->RequestHardwareResetAtAnyIrql();
    }
}

__declspec(noinline) VOID VioGpuDod::RequestHardwareResetAtAnyIrql(void)
{
#if defined(VIOGPU_NATIVE_CONTEXT)
    ULONG_PTR imageBase = reinterpret_cast<ULONG_PTR>(&__ImageBase);
    ULONG_PTR returnAddress = reinterpret_cast<ULONG_PTR>(_ReturnAddress());
    ULONG_PTR callerRva = returnAddress >= imageBase ? returnAddress - imageBase : 0;
#endif
    LONG previousState = InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
#if defined(VIOGPU_NATIVE_CONTEXT)
    if (previousState == VioGpuHardwareActive && callerRva != 0 && callerRva <= MAXULONG)
    {
        InterlockedCompareExchange(&m_HardwareResetCallerRva, static_cast<LONG>(callerRva), 0);
    }
    RequestWddmSubmissionDrainAtAnyIrql();
#else
    UNREFERENCED_PARAMETER(previousState);
#endif
}

VOID VioGpuDod::DpcRoutine(VOID)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    if (ExAcquireRundownProtection(&m_HardwareOperations))
    {
        VioGpuAdapter *adapter = m_pHWDevice;
        if (IsHardwareInterruptDispatchAllowed() && adapter != NULL)
        {
            adapter->DpcRoutine(&m_DxgkInterface);
        }
        ExReleaseRundownProtection(&m_HardwareOperations);
    }
#if defined(VIOGPU_NATIVE_CONTEXT)
    if (InterlockedCompareExchange(&m_WddmDrainRequested, 0, 0) != 0)
    {
        QueueWddmSubmissionDrainWorker();
    }
#endif
    m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuDod::InterruptRoutine(_In_ ULONG MessageNumber)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));
    VioGpuAdapter *adapter = m_pHWDevice;
    return IsHardwareInterruptDispatchAllowed() && adapter != NULL ? adapter->InterruptRoutine(&m_DxgkInterface,
                                                                                               MessageNumber)
                                                                   : FALSE;
}

VOID VioGpuDod::ResetDevice(VOID)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
    InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
#if defined(VIOGPU_NATIVE_CONTEXT)
    RequestWddmSubmissionDrainAtAnyIrql();
#endif
    if (KeGetCurrentIrql() <= DISPATCH_LEVEL && ExAcquireRundownProtection(&m_HardwareOperations))
    {
        VioGpuAdapter *adapter = m_pHWDevice;
        if (adapter != NULL)
        {
            adapter->ResetDevice();
            InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
        }
        ExReleaseRundownProtection(&m_HardwareOperations);
    }
}

NTSTATUS VioGpuDod::SystemDisplayEnable(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                        _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                        _Out_ UINT *pWidth,
                                        _Out_ UINT *pHeight,
                                        _Out_ D3DDDIFORMAT *pColorFormat)
{
    UNREFERENCED_PARAMETER(Flags);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
#if defined(VIOGPU_NATIVE_CONTEXT)
    RequestWddmSubmissionDrainAtAnyIrql();
#endif

    VIOGPU_ASSERT((TargetId < MAX_CHILDREN) || (TargetId == D3DDDI_ID_UNINITIALIZED));

    if (!IsVgaDevice())
    {
        return STATUS_UNSUCCESSFUL;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_DEVICE_NOT_READY;
    }
#if defined(VIOGPU_NATIVE_CONTEXT)
    if (!WaitForWddmSubmissionDrain())
    {
        return STATUS_DEVICE_NOT_READY;
    }
#endif
    if (!ExAcquireRundownProtection(&m_HardwareOperations))
    {
        return STATUS_DEVICE_NOT_READY;
    }
    VioGpuAdapter *adapter = m_pHWDevice;
    BOOLEAN reset = adapter != NULL && adapter->ResetToVgaMode();
    InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
    ExReleaseRundownProtection(&m_HardwareOperations);
    if (!reset)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    if (m_CurrentMode.RamFrameBuffer == nullptr)
    {
        return STATUS_UNSUCCESSFUL;
    }

    m_CurrentMode.FrameBuffer = m_CurrentMode.RamFrameBuffer;
    m_CurrentMode.Flags.FrameBufferIsActive = TRUE;
    m_CurrentMode.Rotation = D3DKMDT_VPPR_IDENTITY;
    *pWidth = m_CurrentMode.DispInfo.Width = m_SystemDisplayInfo.Width;
    *pHeight = m_CurrentMode.DispInfo.Height = m_SystemDisplayInfo.Height;
    *pColorFormat = m_CurrentMode.DispInfo.ColorFormat = m_SystemDisplayInfo.ColorFormat;
    m_CurrentMode.DispInfo.Pitch = m_SystemDisplayInfo.Pitch = (BPPFromPixelFormat(m_SystemDisplayInfo.ColorFormat) /
                                                                BITS_PER_BYTE) *
                                                               m_SystemDisplayInfo.Width;

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s (%dx%dx%d)\n", __FUNCTION__, *pWidth, *pHeight, *pColorFormat));

    return STATUS_SUCCESS;
}

VOID VioGpuDod::SystemDisplayWrite(_In_reads_bytes_(SourceHeight *SourceStride) VOID *pSource,
                                   _In_ UINT SourceWidth,
                                   _In_ UINT SourceHeight,
                                   _In_ UINT SourceStride,
                                   _In_ INT PositionX,
                                   _In_ INT PositionY)
{
    if (m_CurrentMode.Flags.FrameBufferIsActive)
    {
        RECT Rect = {0};
        BLT_INFO SrcBltInfo = {0};
        BLT_INFO DstBltInfo = {0};

        Rect.left = PositionX;
        Rect.top = PositionY;
        Rect.right = Rect.left + SourceWidth;
        Rect.bottom = Rect.top + SourceHeight;

        DstBltInfo.pBits = m_CurrentMode.FrameBuffer;
        DstBltInfo.Pitch = m_CurrentMode.DispInfo.Pitch;
        DstBltInfo.BitsPerPel = BPPFromPixelFormat(m_CurrentMode.DispInfo.ColorFormat);
        DstBltInfo.Offset.x = 0;
        DstBltInfo.Offset.y = 0;
        DstBltInfo.Rotation = m_CurrentMode.Rotation;
        DstBltInfo.Width = m_CurrentMode.DispInfo.Width;
        DstBltInfo.Height = m_CurrentMode.DispInfo.Height;

        SrcBltInfo.pBits = pSource;
        SrcBltInfo.Pitch = SourceStride;
        SrcBltInfo.BitsPerPel = BPPFromPixelFormat(D3DDDIFMT_A8R8G8B8);
        SrcBltInfo.Offset.x = -PositionX;
        SrcBltInfo.Offset.y = -PositionY;
        SrcBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
        SrcBltInfo.Width = SourceWidth;
        SrcBltInfo.Height = SourceHeight;

        BltBits(&DstBltInfo, &SrcBltInfo, &Rect);
    }
}

#pragma code_seg(pop) // End Non-Paged Code

PAGED_CODE_SEG_BEGIN
NTSTATUS VioGpuDod::WriteRegistryString(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PCSTR pszValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    ANSI_STRING AnsiStrValue;
    UNICODE_STRING UnicodeStrValue;
    UNICODE_STRING UnicodeStrValueName;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    RtlInitAnsiString(&AnsiStrValue, pszValue);
    Status = RtlAnsiStringToUnicodeString(&UnicodeStrValue, &AnsiStrValue, TRUE);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("RtlAnsiStringToUnicodeString failed with Status: 0x%X\n", Status));
        return Status;
    }

    Status = ZwSetValueKey(DevInstRegKeyHandle,
                           &UnicodeStrValueName,
                           0,
                           REG_SZ,
                           UnicodeStrValue.Buffer,
                           UnicodeStrValue.MaximumLength);

    RtlFreeUnicodeString(&UnicodeStrValue);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwSetValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::WriteRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PDWORD pdwValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeStrValueName;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    Status = ZwSetValueKey(DevInstRegKeyHandle, &UnicodeStrValueName, 0, REG_DWORD, pdwValue, sizeof(DWORD));

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwSetValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

#if defined(VIOGPU_NATIVE_CONTEXT)
VOID VioGpuDod::RecordNativeStartDiagnostic(_In_ VIOGPU_NATIVE_START_STAGE stage,
                                            _In_ NTSTATUS status,
                                            _In_ DWORD detail)
{
    PAGED_CODE();

    if (stage == VioGpuNativeStartEntered)
    {
        InterlockedExchange(&m_NativePresentDiagnosticRecorded, 2);
        InterlockedExchange(&m_NativePresentExecutionDiagnosticRecorded, 2);
        InterlockedExchange(&m_NativePresentCopyProbeState, 3);
        InterlockedExchange(&m_NativeSubmissionFaultDiagnosticRecorded, 2);
    }

    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = IoOpenDeviceRegistryKey(m_pPhysicalDevice,
                                                  PLUGPLAY_REGKEY_DRIVER,
                                                  KEY_QUERY_VALUE | KEY_SET_VALUE,
                                                  &deviceKey);
    if (!NT_SUCCESS(openStatus))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native start diagnostic: registry open failed, stage=0x%04X status=0x%08X\n",
                   static_cast<DWORD>(stage),
                   openStatus);
        return;
    }

    NTSTATUS presentReasonWrite = STATUS_SUCCESS;
    NTSTATUS presentExecuteStageWrite = STATUS_SUCCESS;
    NTSTATUS presentEpochInvalidateWrite = STATUS_SUCCESS;
    NTSTATUS presentEpochCommitWrite = STATUS_SUCCESS;
    NTSTATUS parameterPhaseInvalidateWrite = STATUS_SUCCESS;
    NTSTATUS parameterWriteStatusInvalidateWrite = STATUS_SUCCESS;
    if (stage == VioGpuNativeStartEntered)
    {
        DWORD parameterPhaseZero = 0;
        parameterPhaseInvalidateWrite = WriteRegistryDWORD(deviceKey, L"NativeContextGetParamPhase", &parameterPhaseZero);
        DWORD parameterWriteStatus = static_cast<DWORD>(parameterPhaseInvalidateWrite);
        parameterWriteStatusInvalidateWrite = WriteRegistryDWORD(deviceKey,
                                                                 L"NativeContextGetParamWriteStatus",
                                                                 &parameterWriteStatus);

        DWORD previousPresentEpoch = 0;
        NTSTATUS presentEpochReadStatus = ReadRegistryDWORD(deviceKey,
                                                            L"NativePresentDiagnosticEpoch",
                                                            &previousPresentEpoch);
        BOOLEAN presentEpochReadAvailable = NT_SUCCESS(presentEpochReadStatus) ||
                                            presentEpochReadStatus == STATUS_OBJECT_NAME_NOT_FOUND;
        DWORD previousCommittedPresentEpoch = previousPresentEpoch & ~1UL;
        BOOLEAN presentEpochAvailable = presentEpochReadAvailable && previousCommittedPresentEpoch <= MAXULONG - 2;
        DWORD presentEpochInvalid = presentEpochAvailable ? previousCommittedPresentEpoch + 1 : MAXULONG;
        DWORD presentReason = 0;
        DWORD presentExecuteStage = 0;
        DWORD presentCopyProbeSequence = 0;
        DWORD presentEpochCommitted = presentEpochAvailable ? previousCommittedPresentEpoch + 2 : 0;
        presentEpochInvalidateWrite = presentEpochAvailable ? WriteRegistryDWORD(deviceKey,
                                                                                 L"NativePresentDiagnosticEpoch",
                                                                                 &presentEpochInvalid)
                                                            : STATUS_INTEGER_OVERFLOW;
        presentReasonWrite = presentEpochInvalidateWrite;
        if (NT_SUCCESS(presentReasonWrite))
        {
            presentReasonWrite = WriteRegistryDWORD(deviceKey, L"NativePresentReason", &presentReason);
        }
        presentExecuteStageWrite = presentReasonWrite;
        if (NT_SUCCESS(presentExecuteStageWrite))
        {
            presentExecuteStageWrite = WriteRegistryDWORD(deviceKey,
                                                          L"NativePresentExecuteStage",
                                                          &presentExecuteStage);
        }
        presentEpochCommitWrite = presentExecuteStageWrite;
        if (NT_SUCCESS(presentExecuteStageWrite))
        {
            presentEpochCommitWrite = WriteRegistryDWORD(deviceKey,
                                                         L"NativePresentCopyProbeSequence",
                                                         &presentCopyProbeSequence);
        }
        if (NT_SUCCESS(presentEpochCommitWrite))
        {
            presentEpochCommitWrite = WriteRegistryDWORD(deviceKey,
                                                         L"NativePresentDiagnosticEpoch",
                                                         &presentEpochCommitted);
        }
        if (NT_SUCCESS(presentEpochCommitWrite))
        {
            InterlockedExchange(&m_NativeSubmissionFaultCallerRva, 0);
            InterlockedExchange(&m_NativeSubmissionFaultExecutionDiagnosticState, 0);
            InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitStage, 0);
            InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitStatus, 0);
            InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitDetail, 0);
            InterlockedExchange(&m_NativePresentDiagnosticRecorded, 0);
            InterlockedExchange(&m_NativePresentExecutionDiagnosticRecorded, 0);
            InterlockedExchange(&m_NativePresentCopyProbeSequence, 0);
            InterlockedExchange(&m_NativePresentCopyProbeState, 0);
            InterlockedExchange(&m_NativeSubmissionFaultDiagnosticRecorded, 0);
        }
    }

    DWORD statusValue = static_cast<DWORD>(status);
    DWORD stageValue = static_cast<DWORD>(stage);
    NTSTATUS statusWrite = WriteRegistryDWORD(deviceKey, L"NativeStartStatus", &statusValue);
    NTSTATUS detailWrite = WriteRegistryDWORD(deviceKey, L"NativeStartDetail", &detail);
    // Stage is the commit marker for the status/detail pair.
    NTSTATUS stageWrite = WriteRegistryDWORD(deviceKey, L"NativeStartStage", &stageValue);
    ZwClose(deviceKey);

    if (!NT_SUCCESS(parameterPhaseInvalidateWrite) || !NT_SUCCESS(parameterWriteStatusInvalidateWrite) ||
        !NT_SUCCESS(presentEpochInvalidateWrite) || !NT_SUCCESS(presentReasonWrite) ||
        !NT_SUCCESS(presentExecuteStageWrite) || !NT_SUCCESS(presentEpochCommitWrite) || !NT_SUCCESS(statusWrite) ||
        !NT_SUCCESS(detailWrite) || !NT_SUCCESS(stageWrite))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native start diagnostic: write failed, stage=0x%04X status=0x%08X "
                   "writes=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X\n",
                   stageValue,
                   statusValue,
                   parameterPhaseInvalidateWrite,
                   parameterWriteStatusInvalidateWrite,
                   presentEpochInvalidateWrite,
                   presentReasonWrite,
                   presentExecuteStageWrite,
                   presentEpochCommitWrite,
                   statusWrite,
                   detailWrite,
                   stageWrite);
        return;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu native start diagnostic: stage=0x%04X status=0x%08X detail=0x%08X\n",
               stageValue,
               statusValue,
               detail);
}

VOID VioGpuDod::RecordNativeContextCreateDiagnostic(_In_ VIOGPU_NATIVE_CONTEXT_CREATE_STAGE stage,
                                                    _In_ NTSTATUS status,
                                                    _In_ DWORD detail)
{
    PAGED_CODE();

    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &deviceKey);
    if (!NT_SUCCESS(openStatus))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native context create diagnostic: registry open failed, stage=0x%04X "
                   "status=0x%08X open=0x%08X\n",
                   static_cast<DWORD>(stage),
                   status,
                   openStatus);
        return;
    }

    DWORD statusValue = static_cast<DWORD>(status);
    DWORD detailValue = detail;
    DWORD stageValue = static_cast<DWORD>(stage);
    NTSTATUS statusWrite = WriteRegistryDWORD(deviceKey, L"NativeContextCreateStatus", &statusValue);
    NTSTATUS detailWrite = WriteRegistryDWORD(deviceKey, L"NativeContextCreateDetail", &detailValue);
    /* Keep the first failing transport stage separate from the final Current
     * commit marker.  The latter is intentionally written during rollback and
     * otherwise hides whether blob creation, BAR mapping, VA discovery, or
     * submit-queue setup was the first rejected operation. */
    NTSTATUS failureWrite = STATUS_SUCCESS;
    if (stage == VioGpuNativeContextCreateEntered)
    {
        DWORD zero = 0;
        failureWrite = WriteRegistryDWORD(deviceKey, L"NativeContextCreateFailureStage", &zero);
        if (NT_SUCCESS(failureWrite))
        {
            failureWrite = WriteRegistryDWORD(deviceKey, L"NativeContextCreateFailureStatus", &zero);
        }
        if (NT_SUCCESS(failureWrite))
        {
            failureWrite = WriteRegistryDWORD(deviceKey, L"NativeContextCreateFailureDetail", &zero);
        }
    }
    else if (stage != VioGpuNativeContextCreateCurrent && stage != VioGpuNativeContextCreateComplete &&
             status != STATUS_SUCCESS && status != STATUS_PENDING)
    {
        DWORD failureStageValue = static_cast<DWORD>(stage);
        DWORD failureStatusValue = static_cast<DWORD>(status);
        failureWrite = WriteRegistryDWORD(deviceKey, L"NativeContextCreateFailureStage", &failureStageValue);
        if (NT_SUCCESS(failureWrite))
        {
            failureWrite = WriteRegistryDWORD(deviceKey, L"NativeContextCreateFailureStatus", &failureStatusValue);
        }
        if (NT_SUCCESS(failureWrite))
        {
            failureWrite = WriteRegistryDWORD(deviceKey, L"NativeContextCreateFailureDetail", &detailValue);
        }
    }
    /* Stage is the commit marker for the status/detail pair. */
    NTSTATUS stageWrite = WriteRegistryDWORD(deviceKey, L"NativeContextCreateStage", &stageValue);
    ZwClose(deviceKey);

    if (!NT_SUCCESS(statusWrite) || !NT_SUCCESS(detailWrite) || !NT_SUCCESS(failureWrite) || !NT_SUCCESS(stageWrite))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native context create diagnostic: write failed, stage=0x%04X "
                   "status=0x%08X writes=%08X/%08X/%08X/%08X\n",
                   stageValue,
                   statusValue,
                   statusWrite,
                   detailWrite,
                   failureWrite,
                   stageWrite);
        return;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu native context create diagnostic: stage=0x%04X status=0x%08X detail=0x%08X\n",
               stageValue,
               statusValue,
               detailValue);
}

VOID VioGpuDod::RecordNativeContextCreateResponseDiagnostic(_In_ const VIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC *diagnostic)
{
    PAGED_CODE();

    if (diagnostic == NULL)
    {
        return;
    }

    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &deviceKey);
    if (!NT_SUCCESS(openStatus))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native context response diagnostic: registry open failed, status=0x%08X\n",
                   openStatus);
        return;
    }

    DWORD responseSize = diagnostic->ResponseSize;
    DWORD type = diagnostic->Type;
    DWORD flags = diagnostic->Flags;
    DWORD fenceLow = static_cast<DWORD>(diagnostic->FenceId & 0xffffffffULL);
    DWORD fenceHigh = static_cast<DWORD>(diagnostic->FenceId >> 32);
    DWORD contextId = diagnostic->ContextId;
    DWORD ringIndex = diagnostic->RingIndex;
    DWORD padding = static_cast<DWORD>(diagnostic->Padding[0]) | (static_cast<DWORD>(diagnostic->Padding[1]) << 8) |
                    (static_cast<DWORD>(diagnostic->Padding[2]) << 16);
    DWORD submitted = diagnostic->Submitted ? 1U : 0U;
    DWORD completed = diagnostic->Completed ? 1U : 0U;
    DWORD validation = diagnostic->Validation;

    NTSTATUS writes[11] = {};
    writes[0] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponseSize", &responseSize);
    writes[1] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponseType", &type);
    writes[2] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponseFlags", &flags);
    writes[3] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponseFenceLow", &fenceLow);
    writes[4] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponseFenceHigh", &fenceHigh);
    writes[5] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponseContextId", &contextId);
    writes[6] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponseRingIndex", &ringIndex);
    writes[7] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponsePadding", &padding);
    writes[8] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponseSubmitted", &submitted);
    writes[9] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponseCompleted", &completed);
    writes[10] = WriteRegistryDWORD(deviceKey, L"NativeContextCreateResponseValidation", &validation);
    ZwClose(deviceKey);

    for (const NTSTATUS write : writes)
    {
        if (!NT_SUCCESS(write))
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu native context response diagnostic: write failed, status=0x%08X\n",
                       write);
            return;
        }
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu native context response diagnostic: size=%u type=0x%08X flags=0x%08X "
               "fence=0x%016llX ctx=%u ring=%u padding=0x%06X submitted=%u completed=%u validation=%u\n",
               responseSize,
               type,
               flags,
               diagnostic->FenceId,
               contextId,
               ringIndex,
               padding,
               submitted,
               completed,
               validation);
}

VOID VioGpuDod::RecordNativeContextMapResponseDiagnostic(_In_ const VIOGPU_NATIVE_MAP_RESPONSE_DIAGNOSTIC *diagnostic)
{
    PAGED_CODE();

    if (diagnostic == NULL)
    {
        return;
    }

    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &deviceKey);
    if (!NT_SUCCESS(openStatus))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native context map response diagnostic: registry open failed, status=0x%08X\n",
                   openStatus);
        return;
    }

    DWORD responseSize = diagnostic->ResponseSize;
    DWORD type = diagnostic->Type;
    DWORD flags = diagnostic->Flags;
    DWORD fenceLow = static_cast<DWORD>(diagnostic->FenceId & 0xffffffffULL);
    DWORD fenceHigh = static_cast<DWORD>(diagnostic->FenceId >> 32);
    DWORD contextId = diagnostic->ContextId;
    DWORD ringIndex = diagnostic->RingIndex;
    DWORD headerPadding = static_cast<DWORD>(diagnostic->HeaderPadding[0]) |
                          (static_cast<DWORD>(diagnostic->HeaderPadding[1]) << 8) |
                          (static_cast<DWORD>(diagnostic->HeaderPadding[2]) << 16);
    DWORD mapInfo = diagnostic->MapInfo;
    DWORD mapPadding = diagnostic->MapPadding;
    DWORD submitted = diagnostic->Submitted ? 1U : 0U;
    DWORD completed = diagnostic->Completed ? 1U : 0U;
    DWORD validation = diagnostic->Validation;

    struct DIAGNOSTIC_VALUE
    {
        PCWSTR Name;
        DWORD Value;
    };
    // The validation value is the final commit marker for this response snapshot.
    // Keep the registry names as individual literals so offline readers and
    // setup diagnostics can discover them without evaluating concatenations.
    // clang-format off
    const DIAGNOSTIC_VALUE values[] = {
        {L"NativeContextCreateMapResponseSize", responseSize},
        {L"NativeContextCreateMapResponseType", type},
        {L"NativeContextCreateMapResponseFlags", flags},
        {L"NativeContextCreateMapResponseFenceLow", fenceLow},
        {L"NativeContextCreateMapResponseFenceHigh", fenceHigh},
        {L"NativeContextCreateMapResponseContextId", contextId},
        {L"NativeContextCreateMapResponseRingIndex", ringIndex},
        {L"NativeContextCreateMapResponseHeaderPadding", headerPadding},
        {L"NativeContextCreateMapResponseMapInfo", mapInfo},
        {L"NativeContextCreateMapResponseMapPadding", mapPadding},
        {L"NativeContextCreateMapResponseSubmitted", submitted},
        {L"NativeContextCreateMapResponseCompleted", completed},
        {L"NativeContextCreateMapResponseValidation", validation},
    };

    // clang-format on

    // Invalidate the previous snapshot before publishing a new one.  Validation
    // is written last and acts as the commit marker for all preceding fields.
    DWORD zero = 0;
    NTSTATUS markerClear = WriteRegistryDWORD(deviceKey, L"NativeContextCreateMapResponseValidation", &zero);
    NTSTATUS writeStatus = markerClear;
    for (UINT index = 0; index < ARRAYSIZE(values); ++index)
    {
        if (!NT_SUCCESS(writeStatus))
        {
            break;
        }
        DWORD value = values[index].Value;
        NTSTATUS currentStatus = WriteRegistryDWORD(deviceKey, values[index].Name, &value);
        if (!NT_SUCCESS(currentStatus))
        {
            writeStatus = currentStatus;
            break;
        }
    }
    ZwClose(deviceKey);

    if (!NT_SUCCESS(writeStatus))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native context map response diagnostic: write failed, status=0x%08X\n",
                   writeStatus);
        return;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu native context map response diagnostic: size=%u type=0x%08X flags=0x%08X "
               "fence=0x%016llX ctx=%u ring=%u header_padding=0x%06X map_info=0x%08X "
               "map_padding=0x%08X submitted=%u completed=%u validation=%u\n",
               responseSize,
               type,
               flags,
               diagnostic->FenceId,
               contextId,
               ringIndex,
               headerPadding,
               mapInfo,
               mapPadding,
               submitted,
               completed,
               validation);
}

VOID VioGpuDod::RecordNativeContextParameterDiagnostic(_Inout_ PVIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC diagnostic)
{
    PAGED_CODE();

    if (diagnostic == NULL)
    {
        return;
    }

    const BOOLEAN physicalDeviceValid = m_pPhysicalDevice != NULL;
    diagnostic->PhysicalDeviceValid = physicalDeviceValid ? 1U : 0U;
    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = physicalDeviceValid
                               ? IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &deviceKey)
                               : STATUS_INVALID_DEVICE_STATE;
    diagnostic->RegistryOpenStatus = static_cast<UINT>(openStatus);
    if (!NT_SUCCESS(openStatus))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native context GET_PARAM diagnostic: registry open failed, physical_device=%u status=0x%08X\n",
                   physicalDeviceValid ? 1U : 0U,
                   openStatus);
        return;
    }

    DWORD contextId = diagnostic->ContextId;
    DWORD parameter = diagnostic->Parameter;
    DWORD sequence = diagnostic->Sequence;
    DWORD physicalDevice = physicalDeviceValid ? 1U : 0U;
    DWORD registryOpenStatus = static_cast<DWORD>(openStatus);
    DWORD windowStatus = diagnostic->WindowStatus;
    DWORD controlBarOffsetLow = static_cast<DWORD>(diagnostic->ControlBarOffset & 0xffffffffULL);
    DWORD controlBarOffsetHigh = static_cast<DWORD>(diagnostic->ControlBarOffset >> 32);
    DWORD controlAddressLow = static_cast<DWORD>(diagnostic->ControlAddress & 0xffffffffULL);
    DWORD controlAddressHigh = static_cast<DWORD>(diagnostic->ControlAddress >> 32);
    DWORD controlBlobSize = diagnostic->ControlBlobSize;
    DWORD responseOffset = diagnostic->ResponseOffset;
    DWORD responseCapacity = diagnostic->ResponseCapacity;
    DWORD seedResult = diagnostic->SeedResult;
    DWORD seedSharedSeqno = diagnostic->SeedSharedSeqno;
    DWORD sharedSeqno = diagnostic->SharedSeqno;
    DWORD sharedAsyncError = diagnostic->SharedAsyncError;
    DWORD sharedGlobalFaults = diagnostic->SharedGlobalFaults;
    DWORD outerResponseSize = diagnostic->OuterResponseSize;
    DWORD outerType = diagnostic->OuterType;
    DWORD outerSubmitted = diagnostic->OuterSubmitted;
    DWORD outerCompleted = diagnostic->OuterCompleted;
    DWORD outerValidation = diagnostic->OuterValidation;
    DWORD submitResult = diagnostic->SubmitResult;
    DWORD copyResult = diagnostic->CopyResult;
    DWORD innerResponseLength = diagnostic->InnerResponseLength;
    DWORD innerRet = diagnostic->InnerRet;
    DWORD innerPipe = diagnostic->InnerPipe;
    DWORD innerParameter = diagnostic->InnerParameter;
    DWORD innerValueLow = static_cast<DWORD>(diagnostic->InnerValue & 0xffffffffULL);
    DWORD innerValueHigh = static_cast<DWORD>(diagnostic->InnerValue >> 32);
    DWORD innerValueLength = diagnostic->InnerValueLength;
    DWORD innerPadding = diagnostic->InnerPadding;
    DWORD validation = diagnostic->Validation;
    DWORD result = diagnostic->Result;

    struct DIAGNOSTIC_VALUE
    {
        PCWSTR Name;
        DWORD Value;
    };
    // Keep the snapshot bounded and make the phase value the final commit marker.
    const DIAGNOSTIC_VALUE values[] = {
        {L"NativeContextGetParamContextId", contextId},
        {L"NativeContextGetParamParameter", parameter},
        {L"NativeContextGetParamSequence", sequence},
        {L"NativeContextGetParamPhysicalDevice", physicalDevice},
        {L"NativeContextGetParamRegistryOpenStatus", registryOpenStatus},
        {L"NativeContextGetParamWindowStatus", windowStatus},
        {L"NativeContextGetParamControlBarOffsetLow", controlBarOffsetLow},
        {L"NativeContextGetParamControlBarOffsetHigh", controlBarOffsetHigh},
        {L"NativeContextGetParamControlAddressLow", controlAddressLow},
        {L"NativeContextGetParamControlAddressHigh", controlAddressHigh},
        {L"NativeContextGetParamControlBlobSize", controlBlobSize},
        {L"NativeContextGetParamResponseOffset", responseOffset},
        {L"NativeContextGetParamResponseCapacity", responseCapacity},
        {L"NativeContextGetParamSeedResult", seedResult},
        {L"NativeContextGetParamSeedSharedSeqno", seedSharedSeqno},
        {L"NativeContextGetParamSharedSeqno", sharedSeqno},
        {L"NativeContextGetParamSharedAsyncError", sharedAsyncError},
        {L"NativeContextGetParamSharedGlobalFaults", sharedGlobalFaults},
        {L"NativeContextGetParamOuterResponseSize", outerResponseSize},
        {L"NativeContextGetParamOuterType", outerType},
        {L"NativeContextGetParamOuterSubmitted", outerSubmitted},
        {L"NativeContextGetParamOuterCompleted", outerCompleted},
        {L"NativeContextGetParamOuterValidation", outerValidation},
        {L"NativeContextGetParamSubmitResult", submitResult},
        {L"NativeContextGetParamCopyResult", copyResult},
        {L"NativeContextGetParamInnerResponseLength", innerResponseLength},
        {L"NativeContextGetParamInnerRet", innerRet},
        {L"NativeContextGetParamInnerPipe", innerPipe},
        {L"NativeContextGetParamInnerParameter", innerParameter},
        {L"NativeContextGetParamInnerValueLow", innerValueLow},
        {L"NativeContextGetParamInnerValueHigh", innerValueHigh},
        {L"NativeContextGetParamInnerValueLength", innerValueLength},
        {L"NativeContextGetParamInnerPadding", innerPadding},
        {L"NativeContextGetParamValidation", validation},
        {L"NativeContextGetParamResult", result},
    };

    DWORD zero = 0;
    NTSTATUS markerClear = WriteRegistryDWORD(deviceKey, L"NativeContextGetParamPhase", &zero);
    NTSTATUS writeStatus = markerClear;
    for (UINT index = 0; index < ARRAYSIZE(values); ++index)
    {
        if (!NT_SUCCESS(writeStatus))
        {
            break;
        }
        DWORD value = values[index].Value;
        writeStatus = WriteRegistryDWORD(deviceKey, values[index].Name, &value);
    }

    DWORD registryWriteStatus = NT_SUCCESS(writeStatus) ? static_cast<DWORD>(STATUS_SUCCESS)
                                                        : static_cast<DWORD>(writeStatus);
    diagnostic->RegistryWriteStatus = registryWriteStatus;
    NTSTATUS statusWrite = WriteRegistryDWORD(deviceKey, L"NativeContextGetParamWriteStatus", &registryWriteStatus);

    NTSTATUS markerWrite = STATUS_SUCCESS;
    if (NT_SUCCESS(writeStatus) && NT_SUCCESS(statusWrite))
    {
        DWORD phase = diagnostic->Phase;
        markerWrite = WriteRegistryDWORD(deviceKey, L"NativeContextGetParamPhase", &phase);
    }
    ZwClose(deviceKey);

    if (!NT_SUCCESS(writeStatus) || !NT_SUCCESS(statusWrite) || !NT_SUCCESS(markerWrite))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native context GET_PARAM diagnostic: write failed, phase=%u data=0x%08X status=0x%08X marker=0x%08X\n",
                   diagnostic->Phase,
                   writeStatus,
                   statusWrite,
                   markerWrite);
        return;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu native context GET_PARAM diagnostic: ctx=%u param=0x%X seq=%u phase=%u "
               "window=%u rsp=0x%X/%u seed=%u seq=%u/%u outer=%u/%u type=0x%X submit=%u "
               "copy=%u inner_len=%u inner_ret=0x%X value=0x%016llX validation=%u result=%u\n",
               contextId,
               parameter,
               sequence,
               diagnostic->Phase,
               windowStatus,
               responseOffset,
               responseCapacity,
               seedResult,
               seedSharedSeqno,
               sharedSeqno,
               outerSubmitted,
               outerCompleted,
               outerType,
               submitResult,
               copyResult,
               innerResponseLength,
               innerRet,
               diagnostic->InnerValue,
               validation,
               result);
}
VOID VioGpuDod::RecordNativeContextMapMemoryDiagnostic(_In_ NTSTATUS status,
                                                       _In_ ULONGLONG physicalAddress,
                                                       _In_ ULONGLONG length,
                                                       _In_ UINT bar,
                                                       _In_ ULONGLONG regionOffset,
                                                       _In_ BOOLEAN attempted,
                                                       _In_ BOOLEAN mapped)
{
    PAGED_CODE();

    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &deviceKey);
    if (!NT_SUCCESS(openStatus))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native context map memory diagnostic: registry open failed, status=0x%08X\n",
                   openStatus);
        return;
    }

    DWORD statusValue = static_cast<DWORD>(status);
    DWORD physicalLow = static_cast<DWORD>(physicalAddress & 0xffffffffULL);
    DWORD physicalHigh = static_cast<DWORD>(physicalAddress >> 32);
    DWORD lengthLow = static_cast<DWORD>(length & 0xffffffffULL);
    DWORD lengthHigh = static_cast<DWORD>(length >> 32);
    DWORD barValue = bar;
    DWORD offsetLow = static_cast<DWORD>(regionOffset & 0xffffffffULL);
    DWORD offsetHigh = static_cast<DWORD>(regionOffset >> 32);
    DWORD mappedValue = mapped ? 1U : 0U;
    DWORD attemptedValue = attempted ? 1U : 0U;

    // Clear the commit marker before writing the request/result fields.  A reader must not
    // combine a new status with stale physical-address data from a previous context attempt.
    DWORD zero = 0;
    NTSTATUS markerClear = WriteRegistryDWORD(deviceKey, L"NativeContextCreateMapMemoryAttempted", &zero);
    NTSTATUS writes[] = {
                                                                                                        WriteRegistryDWORD(deviceKey,
                                                                                                                           L"NativeContextCreateMapMemoryStatus",
                                                                                                                           &statusValue),
                                                                                                        WriteRegistryDWORD(deviceKey,
                                                                                                                           L"NativeContextCreateMapMemoryPhysicalLow",
                                                                                                                           &physicalLow),
                                                                                                        WriteRegistryDWORD(deviceKey,
                                                                                                                           L"NativeContextCreateMapMemoryPhysicalHigh",
                                                                                                                           &physicalHigh),
                                                                                                        WriteRegistryDWORD(deviceKey,
                                                                                                                           L"NativeContextCreateMapMemoryLengthLow",
                                                                                                                           &lengthLow),
                                                                                                        WriteRegistryDWORD(deviceKey,
                                                                                                                           L"NativeContextCreateMapMemoryLengthHigh",
                                                                                                                           &lengthHigh),
                                                                                                        WriteRegistryDWORD(deviceKey,
                                                                                                                           L"NativeContextCreateMapMemoryBar",
                                                                                                                           &barValue),
                                                                                                        WriteRegistryDWORD(deviceKey,
                                                                                                                           L"NativeContextCreateMapMemoryRegionOffsetLow",
                                                                                                                           &offsetLow),
                                                                                                        WriteRegistryDWORD(deviceKey,
                                                                                                                           L"NativeContextCreateMapMemoryRegionOffsetHigh",
                                                                                                                           &offsetHigh),
                                                                                                        WriteRegistryDWORD(deviceKey,
                                                                                                                           L"NativeContextCreateMapMemoryMapped",
                                                                                                                           &mappedValue),
    };
    NTSTATUS markerWrite = STATUS_SUCCESS;
    NTSTATUS writeFailure = markerClear;
    BOOLEAN writesSucceeded = NT_SUCCESS(markerClear);
    for (const NTSTATUS write : writes)
    {
        if (!NT_SUCCESS(write))
        {
            writesSucceeded = FALSE;
            writeFailure = write;
            break;
        }
    }
    if (writesSucceeded)
    {
        markerWrite = WriteRegistryDWORD(deviceKey, L"NativeContextCreateMapMemoryAttempted", &attemptedValue);
    }
    ZwClose(deviceKey);

    if (!writesSucceeded || !NT_SUCCESS(markerWrite))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu native context map memory diagnostic: write failed, status=0x%08X marker=0x%08X\n",
                   status,
                   !writesSucceeded ? writeFailure : markerWrite);
        return;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu native context map memory diagnostic: attempted=%u status=0x%08X "
               "physical=0x%016llX length=0x%016llX bar=%u region_offset=0x%016llX mapped=%u\n",
               attemptedValue,
               statusValue,
               physicalAddress,
               length,
               barValue,
               regionOffset,
               mappedValue);
}

VOID VioGpuDod::RecordNativeQueryAdapterInfoDiagnostic(_In_ UINT type,
                                                       _In_ NTSTATUS status,
                                                       _In_ UINT inputDataSize,
                                                       _In_ UINT outputDataSize)
{
    PAGED_CODE();

    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &deviceKey);
    if (!NT_SUCCESS(openStatus))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu QueryAdapterInfo diagnostic: registry open failed, type=%u status=0x%08X open=0x%08X\n",
                   type,
                   status,
                   openStatus);
        return;
    }

    DWORD statusValue = static_cast<DWORD>(status);
    DWORD inputSizeValue = inputDataSize;
    DWORD outputSizeValue = outputDataSize;
    DWORD typeValue = type;
    NTSTATUS statusWrite = WriteRegistryDWORD(deviceKey, L"NativeQueryAdapterInfoStatus", &statusValue);
    NTSTATUS inputWrite = WriteRegistryDWORD(deviceKey, L"NativeQueryAdapterInfoInputSize", &inputSizeValue);
    NTSTATUS outputWrite = WriteRegistryDWORD(deviceKey, L"NativeQueryAdapterInfoOutputSize", &outputSizeValue);
    // Type is the commit marker for the preceding status and size fields.
    NTSTATUS typeWrite = WriteRegistryDWORD(deviceKey, L"NativeQueryAdapterInfoType", &typeValue);
    ZwClose(deviceKey);

    if (!NT_SUCCESS(statusWrite) || !NT_SUCCESS(inputWrite) || !NT_SUCCESS(outputWrite) || !NT_SUCCESS(typeWrite))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu QueryAdapterInfo diagnostic: write failed, type=%u status=0x%08X "
                   "writes=%08X/%08X/%08X/%08X\n",
                   type,
                   statusValue,
                   statusWrite,
                   inputWrite,
                   outputWrite,
                   typeWrite);
    }
}

VOID VioGpuDod::RecordNativePresentDiagnostic(_In_ DWORD reason,
                                              _In_ NTSTATUS status,
                                              _In_ const VIOGPU_NATIVE_PRESENT_DIAGNOSTIC *diagnostic)
{
    PAGED_CODE();

    if (reason == 0 || diagnostic == NULL || InterlockedCompareExchange(&m_NativePresentDiagnosticRecorded, 1, 0) != 0)
    {
        return;
    }

    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &deviceKey);
    if (!NT_SUCCESS(openStatus))
    {
        InterlockedExchange(&m_NativePresentDiagnosticRecorded, 2);
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu Present diagnostic: registry open failed, reason=%u status=0x%08X open=0x%08X\n",
                   reason,
                   status,
                   openStatus);
        return;
    }

    struct DIAGNOSTIC_VALUE
    {
        PCWSTR Name;
        DWORD Value;
    };
    DWORD statusValue = static_cast<DWORD>(status);
    DWORD hardwareResetState = static_cast<DWORD>(QueryHardwareResetState());
    DWORD hardwareResetCallerRva = static_cast<DWORD>(InterlockedCompareExchange(&m_HardwareResetCallerRva, 0, 0));
    DWORD submissionFaultProvenanceValid = InterlockedCompareExchange(&m_NativeSubmissionFaultDiagnosticRecorded, 2, 2) == 2 ? 1
                                                                                                                             : 0;
    DWORD submissionFaultCallerRva = submissionFaultProvenanceValid != 0 ? static_cast<DWORD>(InterlockedCompareExchange(&m_NativeSubmissionFaultCallerRva,
                                                                                                                         0,
                                                                                                                         0))
                                                                         : 0;
    DWORD submissionFaultExecutionDiagnosticState = submissionFaultProvenanceValid != 0 ? static_cast<DWORD>(InterlockedCompareExchange(&m_NativeSubmissionFaultExecutionDiagnosticState,
                                                                                                                                        0,
                                                                                                                                        0))
                                                                                        : 0;
    DWORD submissionFaultPresentSubmitStage = submissionFaultProvenanceValid != 0 ? static_cast<DWORD>(InterlockedCompareExchange(&m_NativeSubmissionFaultPresentSubmitStage,
                                                                                                                                  0,
                                                                                                                                  0))
                                                                                  : 0;
    DWORD submissionFaultPresentSubmitStatus = submissionFaultProvenanceValid != 0 ? static_cast<DWORD>(InterlockedCompareExchange(&m_NativeSubmissionFaultPresentSubmitStatus,
                                                                                                                                   0,
                                                                                                                                   0))
                                                                                   : 0;
    DWORD submissionFaultPresentSubmitDetail = submissionFaultProvenanceValid != 0 ? static_cast<DWORD>(InterlockedCompareExchange(&m_NativeSubmissionFaultPresentSubmitDetail,
                                                                                                                                   0,
                                                                                                                                   0))
                                                                                   : 0;
    // clang-format off
    const DIAGNOSTIC_VALUE values[] = {
        {L"NativePresentStatus", statusValue},
        {L"NativePresentHardwareResetState", hardwareResetState},
        {L"NativePresentHardwareResetCallerRva", hardwareResetCallerRva},
        {L"NativePresentSubmissionFaultProvenanceValid", submissionFaultProvenanceValid},
        {L"NativePresentSubmissionFaultCallerRva", submissionFaultCallerRva},
        {L"NativePresentSubmissionFaultExecutionDiagnosticState", submissionFaultExecutionDiagnosticState},
        {L"NativePresentSubmissionFaultPresentSubmitStage", submissionFaultPresentSubmitStage},
        {L"NativePresentSubmissionFaultPresentSubmitStatus", submissionFaultPresentSubmitStatus},
        {L"NativePresentSubmissionFaultPresentSubmitDetail", submissionFaultPresentSubmitDetail},
        {L"NativePresentContextType", diagnostic->ContextType},
        {L"NativePresentFlags", diagnostic->PresentFlags},
        {L"NativePresentSubRectCount", diagnostic->SubRectCount},
        {L"NativePresentMultipassOffset", diagnostic->MultipassOffset},
        {L"NativePresentSourceFlags", diagnostic->SourceFlags},
        {L"NativePresentDestinationFlags", diagnostic->DestinationFlags},
        {L"NativePresentSourceHostState", diagnostic->SourceHostState},
        {L"NativePresentDestinationHostState", diagnostic->DestinationHostState},
        {L"NativePresentSourceResource2DState", diagnostic->SourceResource2DState},
        {L"NativePresentDestinationResource2DState", diagnostic->DestinationResource2DState},
        {L"NativePresentSourcePlacementState", diagnostic->SourcePlacementState},
        {L"NativePresentDestinationPlacementState", diagnostic->DestinationPlacementState},
        {L"NativePresentSourceFormat", diagnostic->SourceFormat},
        {L"NativePresentDestinationFormat", diagnostic->DestinationFormat},
        {L"NativePresentSourceWidth", diagnostic->SourceWidth},
        {L"NativePresentSourceHeight", diagnostic->SourceHeight},
        {L"NativePresentSourcePitch", diagnostic->SourcePitch},
        {L"NativePresentDestinationWidth", diagnostic->DestinationWidth},
        {L"NativePresentDestinationHeight", diagnostic->DestinationHeight},
        {L"NativePresentDestinationPitch", diagnostic->DestinationPitch},
        {L"NativePresentSourceAllocationListValue", diagnostic->SourceAllocationListValue},
        {L"NativePresentDestinationAllocationListValue", diagnostic->DestinationAllocationListValue},
        {L"NativePresentSourceResourceId", diagnostic->SourceResourceId},
        {L"NativePresentDestinationResourceId", diagnostic->DestinationResourceId},
        {L"NativePresentSourceRectLeft", diagnostic->SourceRectLeft},
        {L"NativePresentSourceRectTop", diagnostic->SourceRectTop},
        {L"NativePresentSourceRectRight", diagnostic->SourceRectRight},
        {L"NativePresentSourceRectBottom", diagnostic->SourceRectBottom},
        {L"NativePresentDestinationRectLeft", diagnostic->DestinationRectLeft},
        {L"NativePresentDestinationRectTop", diagnostic->DestinationRectTop},
        {L"NativePresentDestinationRectRight", diagnostic->DestinationRectRight},
        {L"NativePresentDestinationRectBottom", diagnostic->DestinationRectBottom},
    };
    // clang-format on

    DWORD emptyReason = 0;
    NTSTATUS invalidateStatus = WriteRegistryDWORD(deviceKey, L"NativePresentReason", &emptyReason);
    NTSTATUS writeStatus = invalidateStatus;
    for (UINT index = 0; NT_SUCCESS(writeStatus) && index < ARRAYSIZE(values); ++index)
    {
        DWORD value = values[index].Value;
        NTSTATUS currentStatus = WriteRegistryDWORD(deviceKey, values[index].Name, &value);
        if (!NT_SUCCESS(currentStatus) && NT_SUCCESS(writeStatus))
        {
            writeStatus = currentStatus;
        }
    }

    DWORD reasonValue = reason;
    NTSTATUS reasonWrite = NT_SUCCESS(writeStatus) ? WriteRegistryDWORD(deviceKey, L"NativePresentReason", &reasonValue)
                                                   : writeStatus;
    ZwClose(deviceKey);

    if (!NT_SUCCESS(writeStatus) || !NT_SUCCESS(reasonWrite))
    {
        InterlockedExchange(&m_NativePresentDiagnosticRecorded, 2);
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu Present diagnostic: write failed, reason=%u status=0x%08X writes=%08X/%08X/%08X\n",
                   reason,
                   statusValue,
                   invalidateStatus,
                   writeStatus,
                   reasonWrite);
        return;
    }

    InterlockedExchange(&m_NativePresentDiagnosticRecorded, 2);
    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu Present diagnostic: reason=%u status=0x%08X context=%u flags=0x%08X\n",
               reason,
               statusValue,
               diagnostic->ContextType,
               diagnostic->PresentFlags);
}

BOOLEAN VioGpuDod::ClaimNativePresentExecutionDiagnostic(void)
{
    PAGED_CODE();

    return InterlockedCompareExchange(&m_NativePresentExecutionDiagnosticRecorded, 1, 0) == 0;
}

VOID VioGpuDod::RecordNativePresentExecutionDiagnostic(_In_ const VIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC *diagnostic)
{
    PAGED_CODE();

    if (diagnostic == NULL || diagnostic->Stage == 0 || diagnostic->Stage == 0x0FFF ||
        InterlockedCompareExchange(&m_NativePresentExecutionDiagnosticRecorded, 1, 1) != 1)
    {
        return;
    }

    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &deviceKey);
    if (!NT_SUCCESS(openStatus))
    {
        InterlockedExchange(&m_NativePresentExecutionDiagnosticRecorded, 2);
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu Present execution diagnostic: registry open failed, stage=%u status=0x%08X open=0x%08X\n",
                   diagnostic->Stage,
                   diagnostic->Status,
                   openStatus);
        return;
    }

    struct DIAGNOSTIC_VALUE
    {
        PCWSTR Name;
        DWORD Value;
    };
    // clang-format off
    const DIAGNOSTIC_VALUE values[] = {
        {L"NativePresentExecuteStatus", diagnostic->Status},
        {L"NativePresentExecuteDetail", diagnostic->Detail},
        {L"NativePresentExecuteResetProvenanceValid", 0},
        {L"NativePresentExecuteFenceId", diagnostic->FenceId},
        {L"NativePresentExecuteTransactionState", diagnostic->TransactionState},
        {L"NativePresentExecuteContextType", diagnostic->ContextType},
        {L"NativePresentExecuteSourceResourceId", diagnostic->SourceResourceId},
        {L"NativePresentExecuteDestinationResourceId", diagnostic->DestinationResourceId},
        {L"NativePresentExecuteSourcePlacementState", diagnostic->SourcePlacementState},
        {L"NativePresentExecuteDestinationPlacementState", diagnostic->DestinationPlacementState},
        {L"NativePresentExecuteSourceResource2DState", diagnostic->SourceResource2DState},
        {L"NativePresentExecuteDestinationResource2DState", diagnostic->DestinationResource2DState},
        {L"NativePresentExecuteSourcePlacementOffsetLow", diagnostic->SourcePlacementOffsetLow},
        {L"NativePresentExecuteSourcePlacementOffsetHigh", diagnostic->SourcePlacementOffsetHigh},
        {L"NativePresentExecuteDestinationPlacementOffsetLow", diagnostic->DestinationPlacementOffsetLow},
        {L"NativePresentExecuteDestinationPlacementOffsetHigh", diagnostic->DestinationPlacementOffsetHigh},
        {L"NativePresentExecuteTransactionSourcePlacementOffsetLow", diagnostic->TransactionSourcePlacementOffsetLow},
        {L"NativePresentExecuteTransactionSourcePlacementOffsetHigh", diagnostic->TransactionSourcePlacementOffsetHigh},
        {L"NativePresentExecuteTransactionDestinationPlacementOffsetLow", diagnostic->TransactionDestinationPlacementOffsetLow},
        {L"NativePresentExecuteTransactionDestinationPlacementOffsetHigh", diagnostic->TransactionDestinationPlacementOffsetHigh},
        {L"NativePresentExecuteSourceResetGenerationLow", diagnostic->SourceResetGenerationLow},
        {L"NativePresentExecuteSourceResetGenerationHigh", diagnostic->SourceResetGenerationHigh},
        {L"NativePresentExecuteDestinationResetGenerationLow", diagnostic->DestinationResetGenerationLow},
        {L"NativePresentExecuteDestinationResetGenerationHigh", diagnostic->DestinationResetGenerationHigh},
        {L"NativePresentExecuteTransactionDestinationResetGenerationLow", diagnostic->TransactionDestinationResetGenerationLow},
        {L"NativePresentExecuteTransactionDestinationResetGenerationHigh", diagnostic->TransactionDestinationResetGenerationHigh},
    };
    // clang-format on

    DWORD emptyStage = 0;
    NTSTATUS invalidateStatus = WriteRegistryDWORD(deviceKey, L"NativePresentExecuteStage", &emptyStage);
    NTSTATUS writeStatus = invalidateStatus;
    for (UINT index = 0; NT_SUCCESS(writeStatus) && index < ARRAYSIZE(values); ++index)
    {
        DWORD value = values[index].Value;
        NTSTATUS currentStatus = WriteRegistryDWORD(deviceKey, values[index].Name, &value);
        if (!NT_SUCCESS(currentStatus) && NT_SUCCESS(writeStatus))
        {
            writeStatus = currentStatus;
        }
    }

    DWORD stageValue = diagnostic->Stage;
    NTSTATUS stageWrite = NT_SUCCESS(writeStatus) ? WriteRegistryDWORD(deviceKey,
                                                                       L"NativePresentExecuteStage",
                                                                       &stageValue)
                                                  : writeStatus;
    ZwClose(deviceKey);

    if (!NT_SUCCESS(writeStatus) || !NT_SUCCESS(stageWrite))
    {
        InterlockedExchange(&m_NativePresentExecutionDiagnosticRecorded, 2);
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu Present execution diagnostic: write failed, stage=%u status=0x%08X "
                   "writes=%08X/%08X/%08X\n",
                   diagnostic->Stage,
                   diagnostic->Status,
                   invalidateStatus,
                   writeStatus,
                   stageWrite);
        return;
    }

    InterlockedExchange(&m_NativePresentExecutionDiagnosticRecorded, 2);
    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu Present execution diagnostic: stage=%u status=0x%08X detail=0x%08X fence=%u\n",
               diagnostic->Stage,
               diagnostic->Status,
               diagnostic->Detail,
               diagnostic->FenceId);
}

VOID VioGpuDod::RecordNativePresentExecutionResetProvenance(void)
{
    PAGED_CODE();

    if (InterlockedCompareExchange(&m_NativePresentExecutionDiagnosticRecorded, 2, 2) != 2)
    {
        return;
    }

    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &deviceKey);
    if (!NT_SUCCESS(openStatus))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu Present execution reset provenance: registry open failed, status=0x%08X\n",
                   openStatus);
        return;
    }

    DWORD unavailable = 0;
    DWORD hardwareResetState = static_cast<DWORD>(QueryHardwareResetState());
    DWORD hardwareResetCallerRva = static_cast<DWORD>(InterlockedCompareExchange(&m_HardwareResetCallerRva, 0, 0));
    DWORD available = 1;
    NTSTATUS invalidateStatus = WriteRegistryDWORD(deviceKey,
                                                   L"NativePresentExecuteResetProvenanceValid",
                                                   &unavailable);
    NTSTATUS stateWrite = NT_SUCCESS(invalidateStatus) ? WriteRegistryDWORD(deviceKey,
                                                                            L"NativePresentExecuteHardwareResetState",
                                                                            &hardwareResetState)
                                                       : invalidateStatus;
    NTSTATUS callerWrite = NT_SUCCESS(stateWrite) ? WriteRegistryDWORD(deviceKey,
                                                                       L"NativePresentExecuteHardwareResetCallerRva",
                                                                       &hardwareResetCallerRva)
                                                  : stateWrite;
    NTSTATUS commitWrite = NT_SUCCESS(callerWrite) ? WriteRegistryDWORD(deviceKey,
                                                                        L"NativePresentExecuteResetProvenanceValid",
                                                                        &available)
                                                   : callerWrite;
    ZwClose(deviceKey);

    if (!NT_SUCCESS(invalidateStatus) || !NT_SUCCESS(stateWrite) || !NT_SUCCESS(callerWrite) ||
        !NT_SUCCESS(commitWrite))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu Present execution reset provenance: write failed, writes=%08X/%08X/%08X/%08X\n",
                   invalidateStatus,
                   stateWrite,
                   callerWrite,
                   commitWrite);
    }
}

VOID VioGpuDod::RecordNativePresentCopyProbe(_In_ const VIOGPU_NATIVE_PRESENT_COPY_PROBE *probe)
{
    PAGED_CODE();

    if (probe == NULL || probe->SampleCount == 0 || probe->HostPresentCount == 0)
    {
        return;
    }

    LONG desiredState = probe->SourceRgbNonzero == 0 ? 1 : 2;
    LONG previousState = InterlockedCompareExchange(&m_NativePresentCopyProbeState, desiredState, 0);
    if (previousState != 0 && !(desiredState == 2 && previousState == 1 &&
                                InterlockedCompareExchange(&m_NativePresentCopyProbeState, desiredState, 1) == 1))
    {
        return;
    }

    HANDLE deviceKey = NULL;
    NTSTATUS openStatus = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &deviceKey);
    if (!NT_SUCCESS(openStatus))
    {
        InterlockedExchange(&m_NativePresentCopyProbeState, 3);
        return;
    }

    struct PROBE_VALUE
    {
        PCWSTR Name;
        DWORD Value;
    };
    DWORD sequence = static_cast<DWORD>(InterlockedIncrement(&m_NativePresentCopyProbeSequence));
    // clang-format off
    const PROBE_VALUE values[] = {
        {L"NativePresentCopyProbeFenceId", probe->FenceId},
        {L"NativePresentCopyProbeSampleCount", probe->SampleCount},
        {L"NativePresentCopyProbeSourceRgbNonzero", probe->SourceRgbNonzero},
        {L"NativePresentCopyProbeDestinationRgbNonzero", probe->DestinationRgbNonzero},
        {L"NativePresentCopyProbeSourceHash", probe->SourceHash},
        {L"NativePresentCopyProbeDestinationHash", probe->DestinationHash},
        {L"NativePresentCopyProbeSourceFirstPixel", probe->SourceFirstPixel},
        {L"NativePresentCopyProbeDestinationFirstPixel", probe->DestinationFirstPixel},
        {L"NativePresentCopyProbeSourceResourceId", probe->SourceResourceId},
        {L"NativePresentCopyProbeDestinationResourceId", probe->DestinationResourceId},
        {L"NativePresentCopyProbeRectCount", probe->RectCount},
        {L"NativePresentCopyProbeHostPresentCount", probe->HostPresentCount},
        {L"NativePresentCopyProbeHostPresentResult", probe->HostPresentResult},
        {L"NativePresentCopyProbeState", static_cast<DWORD>(desiredState)},
    };
    // clang-format on

    DWORD invalidSequence = 0;
    NTSTATUS writeStatus = WriteRegistryDWORD(deviceKey, L"NativePresentCopyProbeSequence", &invalidSequence);
    for (UINT index = 0; NT_SUCCESS(writeStatus) && index < ARRAYSIZE(values); ++index)
    {
        DWORD value = values[index].Value;
        writeStatus = WriteRegistryDWORD(deviceKey, values[index].Name, &value);
    }
    if (NT_SUCCESS(writeStatus))
    {
        writeStatus = WriteRegistryDWORD(deviceKey, L"NativePresentCopyProbeSequence", &sequence);
    }
    ZwClose(deviceKey);

    if (!NT_SUCCESS(writeStatus))
    {
        InterlockedExchange(&m_NativePresentCopyProbeState, 3);
    }
}
#endif

NTSTATUS VioGpuDod::ReadRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle,
                                      _In_ PCWSTR pszwValueName,
                                      _Inout_ PDWORD pdwValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeStrValueName;
    ULONG ulRes;
    UCHAR Buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(DWORD)];
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    Status = ZwQueryValueKey(DevInstRegKeyHandle,
                             &UnicodeStrValueName,
                             KeyValuePartialInformation,
                             Buf,
                             sizeof(Buf),
                             &ulRes);

    if (Status == STATUS_SUCCESS)
    {
        if (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Type == REG_DWORD &&
            (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->DataLength == sizeof(DWORD)))
        {
            *pdwValue = *((PDWORD) & (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Data));
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
            VioGpuDbgBreak();
        }
    }

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwQueryValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::SetRegisterInfo(_In_ ULONG Id, _In_ DWORD MemSize)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PCSTR StrHWInfoChipType = "QEMU VIRTIO GPU";
    PCSTR StrHWInfoDacType = "VIRTIO GPU";
    PCSTR StrHWInfoAdapterString = "VIRTIO GPU";
    PCSTR StrHWInfoBiosString = "SEABIOS VIRTIO GPU";

    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    do
    {
        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.ChipType", StrHWInfoChipType);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for ChipType with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.DacType", StrHWInfoDacType);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed DacType with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.AdapterString", StrHWInfoAdapterString);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for AdapterString with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.BiosString", StrHWInfoBiosString);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for BiosString with Status: 0x%X", Status));
            break;
        }

        DWORD MemorySize = MemSize;
        Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"HardwareInformation.MemorySize", &MemorySize);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryDWORD failed for MemorySize with Status: 0x%X", Status));
            break;
        }

        DWORD DeviceId = Id;
        Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"VioGpuAdapterID", &DeviceId);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryDWORD failed for VioGpuAdapterID with Status: 0x%X", Status));
        }
    } while (0);

    ZwClose(DevInstRegKeyHandle);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::SetRegisterConfigInfo()
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DWORD value = 0;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    do
    {
        if (IsPersistentDispMode0Set())
        {
            value = GetPersistentDispMode0Width();
            Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"PersistentDispMode0Width", &value);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("WriteRegistryDWORD failed for PersistentDispMode0Width with Status: 0x%X", Status));
                break;
            }

            value = GetPersistentDispMode0Height();
            Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"PersistentDispMode0Height", &value);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("WriteRegistryDWORD failed for PersistentDispMode0Height with Status: 0x%X", Status));
                break;
            }
        }
    } while (0);

    ZwClose(DevInstRegKeyHandle);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::GetRegisterInfo(void)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS StatusOptional = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    DWORD value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"HWCursor", &value);
    if (NT_SUCCESS(Status))
    {
        SetPointerEnabled(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"FlexResolution", &value);
    if (NT_SUCCESS(Status))
    {
        SetFlexResolution(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"UsePhysicalMemory", &value);
    if (NT_SUCCESS(Status))
    {
        SetUsePhysicalMemory(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"UsePresentProgress", &value);
    if (NT_SUCCESS(Status))
    {
        SetUsePresentProgress(!!value);
    }

    // The following keys are optional and no need to report error if them are missing
    value = 0;
    StatusOptional = ReadRegistryDWORD(DevInstRegKeyHandle, L"PersistentDispMode0Width", &value);
    if (NT_SUCCESS(StatusOptional))
    {
        SetPersistentDispMode0Width((USHORT)value);
    }

    value = 0;
    StatusOptional = ReadRegistryDWORD(DevInstRegKeyHandle, L"PersistentDispMode0Height", &value);
    if (NT_SUCCESS(StatusOptional))
    {
        SetPersistentDispMode0Height((USHORT)value);
    }

    ZwClose(DevInstRegKeyHandle);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

VioGpuAdapter::VioGpuAdapter(_In_ VioGpuDod *pVioGpuDod)
{
    PAGED_CODE();
    RtlZeroMemory(&m_VioDev, sizeof(m_VioDev));
    m_pVioGpuDod = pVioGpuDod;
    m_CurrentModeIndex = 0;
    m_CustomModeIndex = 0;
    RtlZeroMemory(m_EDIDs, sizeof(m_EDIDs));
    m_bEDID = FALSE;
    m_ModeInfo = NULL;
    m_ModeCount = 0;
    m_Id = g_InstanceId++;
    m_pFrameBuf = NULL;
    m_pCursorBuf = NULL;
    m_PendingWorks = 0;
    m_bStopWorkThread = FALSE;
    m_pWorkThread = NULL;
    m_ResolutionEvent = NULL;
    m_ResolutionEventHandle = NULL;
    m_u64HostFeatures = 0;
    m_u64GuestFeatures = 0;
    m_u32NumCapsets = 0;
    m_u32NumScanouts = 0;
    KeInitializeSpinLock(&m_NativeContextReadinessLock);
    RtlZeroMemory(&m_NativeContextReadiness, sizeof(m_NativeContextReadiness));
    KeInitializeMutex(&m_NativeContextLifecycleMutex, 0);
    InitializeListHead(&m_NativeContextRegistry);
    ExInitializeRundownProtection(&m_NativeContextReferences);
    m_NextNativeContextId = 1;
#if defined(VIOGPU_NATIVE_CONTEXT)
    m_NextNativeResourceId = VIOGPU_NATIVE_RESOURCE_ID_START;
    KeInitializeMutex(&m_2DScanoutMutex, 0);
    m_2DResourceIdsInitialized = FALSE;
    m_2DScanoutResourceId = 0;
    m_2DScanoutUnknown = FALSE;
    m_2DScanoutResetGeneration = 0;
    m_2DRetiredResetGeneration = 0;
    KeInitializeSpinLock(&m_NativeSubmitRundownLock);
    ExInitializeRundownProtection(&m_NativeSubmitRundown);
    m_NativeSubmitClosing = FALSE;
    m_NativeSubmitRundownCompleted = FALSE;
#endif
    m_NativeContextState = VioGpuNativeContextOffline;
    m_NativeContextGeneration = 0;
    m_NativeContextResetGeneration = 0;
    m_InterruptDispatchEnabled = FALSE;
    m_bVirtioInitialized = FALSE;
    m_bQueuesInitialized = FALSE;
    m_WorkThreadHandle = NULL;

    KeInitializeEvent(&m_ConfigUpdateEvent, SynchronizationEvent, FALSE);
}

VioGpuAdapter::~VioGpuAdapter(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s 0x%p\n", __FUNCTION__, this));
    LONG state = InterlockedCompareExchange(&m_NativeContextState,
                                            VioGpuNativeContextOffline,
                                            VioGpuNativeContextOffline);
    if (state != VioGpuNativeContextOffline || !IsListEmpty(&m_NativeContextRegistry) || m_bVirtioInitialized ||
        m_bQueuesInitialized || m_pWorkThread != NULL || m_WorkThreadHandle != NULL || m_GpuBuf.HasAllocationOwner() ||
        m_FrameSegment.GetSize() != 0 || m_CursorSegment.GetSize() != 0)
    {
        NTSTATUS status = StopNativeContextTransport();
        if (!NT_SUCCESS(status))
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu adapter destructor: StopNativeContextTransport failed, status=0x%08X\n",
                       status);
        }
        NT_ASSERT(NT_SUCCESS(status));
    }
    NT_ASSERT(InterlockedCompareExchange(&m_NativeContextState,
                                         VioGpuNativeContextOffline,
                                         VioGpuNativeContextOffline) == VioGpuNativeContextOffline);
    NT_ASSERT(IsListEmpty(&m_NativeContextRegistry));
    NT_ASSERT(!m_bVirtioInitialized && !m_bQueuesInitialized);
    NT_ASSERT(m_pWorkThread == NULL && m_WorkThreadHandle == NULL);
    NT_ASSERT(!m_GpuBuf.HasAllocationOwner());
    NT_ASSERT(m_FrameSegment.GetSize() == 0 && m_CursorSegment.GetSize() == 0);
    ExWaitForRundownProtectionRelease(&m_NativeContextReferences);
    ExRundownCompleted(&m_NativeContextReferences);
    CloseResolutionEvent();
    delete[] m_ModeInfo;
    m_ModeInfo = NULL;
    m_CurrentModeIndex = 0;
    m_ModeCount = 0;
    m_Id = 0;
    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
}

NTSTATUS VioGpuAdapter::SetCurrentMode(ULONG Mode, CURRENT_MODE *pCurrentMode)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s - %d: Mode = %d\n", __FUNCTION__, m_Id, Mode));
    for (ULONG idx = 0; idx < GetModeCount(); idx++)
    {
        if (Mode == m_ModeInfo[idx].ModeIndex /*m_ModeNumbers[idx]*/)
        {
            if (pCurrentMode->Flags.FrameBufferIsActive)
            {
                DestroyFrameBufferObj(FALSE, FALSE);
                pCurrentMode->Flags.FrameBufferIsActive = FALSE;
            }
            if (CreateFrameBufferObj(&m_ModeInfo[idx], pCurrentMode))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s device %d: setting current mode %d (%d x %d)\n",
                          __FUNCTION__,
                          m_Id,
                          Mode,
                          m_ModeInfo[idx].VisScreenWidth,
                          m_ModeInfo[idx].VisScreenHeight));
                return STATUS_SUCCESS;
            }
        }
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed\n", __FUNCTION__));
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS VioGpuAdapter::VioGpuAdapterInit(DXGK_DISPLAY_INFORMATION *pDispInfo)
{
    PAGED_CODE();
    NTSTATUS status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(pDispInfo);
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartVirtioPreconditions,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    if (m_pVioGpuDod->IsHardwareInit())
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Already Initialized\n"));
        VioGpuDbgBreak();
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartVirtioPreconditions,
                                   STATUS_ALREADY_INITIALIZED,
                                   VioGpuNativeStartDetailNone);
        return STATUS_ALREADY_INITIALIZED;
    }
    if (InterlockedCompareExchange(&m_NativeContextState,
                                   VioGpuNativeContextStarting,
                                   VioGpuNativeContextStarting) != VioGpuNativeContextStarting ||
        m_bVirtioInitialized || m_bQueuesInitialized)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu init: refusing to replace retained transport storage\n");
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartVirtioPreconditions,
                                   STATUS_DEVICE_NOT_READY,
                                   VioGpuNativeStartDetailNone);
        return STATUS_DEVICE_NOT_READY;
    }
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartVirtioDevice,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    status = VirtIoDeviceInit();
    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu HWInit: VirtIoDeviceInit failed, status=0x%08X\n",
                   status);
        DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio device, error %x\n", status));
        VioGpuDbgBreak();
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartVirtioDevice, status, VioGpuNativeStartDetailNone);
        return status;
    }
    m_bVirtioInitialized = TRUE;

    m_u64HostFeatures = virtio_get_features(&m_VioDev);
    m_u64GuestFeatures = 0;
    do
    {
        struct virtqueue *vqs[2];
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartVirtioVersion,
                                   STATUS_PENDING,
                                   VioGpuNativeStartDetailNone);
        if (!AckFeature(VIRTIO_F_VERSION_1))
        {
            status = STATUS_UNSUCCESSFUL;
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu HWInit: VIRTIO_F_VERSION_1 unavailable, status=0x%08X\n",
                       status);
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                       VioGpuNativeStartVirtioVersion,
                                       status,
                                       VioGpuNativeStartDetailNone);
            break;
        }

        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartVirtioNativeFeatures,
                                   STATUS_PENDING,
                                   VioGpuNativeStartDetailNone);
        status = NegotiateNativeContextFeatures();
        if (!NT_SUCCESS(status))
        {
            DWORD missingFeatures = VioGpuNativeStartDetailNone;
            if (!virtio_is_feature_enabled(m_u64HostFeatures, VIRTIO_GPU_F_VIRGL))
            {
                missingFeatures |= VioGpuNativeStartDetailMissingVirgl;
            }
            if (!virtio_is_feature_enabled(m_u64HostFeatures, VIRTIO_GPU_F_RESOURCE_BLOB))
            {
                missingFeatures |= VioGpuNativeStartDetailMissingResourceBlob;
            }
            if (!virtio_is_feature_enabled(m_u64HostFeatures, VIRTIO_GPU_F_CONTEXT_INIT))
            {
                missingFeatures |= VioGpuNativeStartDetailMissingContextInit;
            }
            if (!virtio_is_feature_enabled(m_u64HostFeatures, VIRTIO_GPU_F_CREATE_GUEST_HANDLE))
            {
                missingFeatures |= VioGpuNativeStartDetailMissingGuestHandle;
            }
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu native context: required VirtIO-GPU features unavailable, status=0x%08X\n",
                       status);
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartVirtioNativeFeatures, status, missingFeatures);
            break;
        }

        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartVirtioSetFeatures,
                                   STATUS_PENDING,
                                   VioGpuNativeStartDetailNone);
        status = virtio_set_features(&m_VioDev, m_u64GuestFeatures);
        if (!NT_SUCCESS(status))
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu HWInit: virtio_set_features failed, status=0x%08X\n",
                       status);
            DbgPrint(TRACE_LEVEL_FATAL, ("%s virtio_set_features failed with %x\n", __FUNCTION__, status));
            VioGpuDbgBreak();
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                       VioGpuNativeStartVirtioSetFeatures,
                                       status,
                                       VioGpuNativeStartDetailNone);
            break;
        }

        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartVirtioFindQueues,
                                   STATUS_PENDING,
                                   VioGpuNativeStartDetailNone);
        status = virtio_find_queues(&m_VioDev, 2, vqs);
        if (!NT_SUCCESS(status))
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu HWInit: virtio_find_queues failed, status=0x%08X\n",
                       status);
            DbgPrint(TRACE_LEVEL_FATAL, ("virtio_find_queues failed with error %x\n", status));
            VioGpuDbgBreak();
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                       VioGpuNativeStartVirtioFindQueues,
                                       status,
                                       VioGpuNativeStartDetailNone);
            break;
        }
        m_bQueuesInitialized = TRUE;

        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartVirtioQueueObjects,
                                   STATUS_PENDING,
                                   VioGpuNativeStartDetailNone);
        if (!m_CtrlQueue.Init(&m_VioDev, vqs[0], 0) || !m_CursorQueue.Init(&m_VioDev, vqs[1], 1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio queues\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu HWInit: queue initialization failed, status=0x%08X\n",
                       status);
            VioGpuDbgBreak();
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                       VioGpuNativeStartVirtioQueueObjects,
                                       status,
                                       VioGpuNativeStartDetailNone);
            break;
        }
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartVirtioQueueBacklog,
                                   STATUS_PENDING,
                                   VioGpuNativeStartDetailNone);
        if (!m_CtrlQueue.ResetNativeSubmitBacklog())
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu native submit: stale backlog survived transport teardown\n");
            status = STATUS_DEVICE_NOT_READY;
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                       VioGpuNativeStartVirtioQueueBacklog,
                                       status,
                                       VioGpuNativeStartDetailNone);
            break;
        }

        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartVirtioConfig,
                                   STATUS_PENDING,
                                   VioGpuNativeStartDetailNone);
        virtio_get_config(&m_VioDev,
                          FIELD_OFFSET(GPU_CONFIG, num_scanouts),
                          &m_u32NumScanouts,
                          sizeof(m_u32NumScanouts));

        if (m_u32NumScanouts > VIRTIO_GPU_MAX_SCANOUTS)
        {
            status = STATUS_NOT_SUPPORTED;
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu init: invalid scanout count %u, maximum %u\n",
                       m_u32NumScanouts,
                       VIRTIO_GPU_MAX_SCANOUTS);
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartVirtioConfig, status, m_u32NumScanouts);
            break;
        }

        virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, num_capsets), &m_u32NumCapsets, sizeof(m_u32NumCapsets));
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartVirtioConfig,
                                   STATUS_SUCCESS,
                                   ((m_u32NumCapsets & 0xFFFFU) << 16) | (m_u32NumScanouts & 0xFFFFU));
    } while (0);
    if (!NT_SUCCESS(status))
    {
        virtio_add_status(&m_VioDev, VIRTIO_CONFIG_S_FAILED);
        VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return status;
}

NTSTATUS VioGpuAdapter::SetPowerState(DXGK_DEVICE_INFO *pDeviceInfo,
                                      DEVICE_POWER_STATE DevicePowerState,
                                      CURRENT_MODE *pCurrentMode)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s DevicePowerState = %d\n", __FUNCTION__, DevicePowerState));
    UNREFERENCED_PARAMETER(pDeviceInfo);

    switch (DevicePowerState)
    {
        case PowerDeviceUnspecified:
        case PowerDeviceD0:
            {
                if (pCurrentMode == NULL)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                LONG state = InterlockedCompareExchange(&m_NativeContextState,
                                                        VioGpuNativeContextOffline,
                                                        VioGpuNativeContextOffline);
                if (state == VioGpuNativeContextReady)
                {
                    GPU_CAPSET_DRM capset = {};
                    if (!QueryNativeContextReadiness(&capset, NULL, NULL, NULL))
                    {
                        FailNativeContextAtAnyIrql();
                        NTSTATUS closeStatus = StopNativeContextTransport();
                        return NT_SUCCESS(closeStatus) ? STATUS_DEVICE_NOT_READY : closeStatus;
                    }
                    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                               VioGpuNativeStartComplete,
                                               STATUS_SUCCESS,
                                               VioGpuNativeStartDetailNone);
                    return STATUS_SUCCESS;
                }

                if (state != VioGpuNativeContextOffline)
                {
                    NTSTATUS closeStatus = StopNativeContextTransport();
                    if (!NT_SUCCESS(closeStatus))
                    {
                        return closeStatus;
                    }
                }
                if (!BeginNativeContextInitialization())
                {
                    return STATUS_DEVICE_NOT_READY;
                }
                NTSTATUS status = StartNativeContextTransport(&pCurrentMode->DispInfo);
                if (NT_SUCCESS(status))
                {
                    status = StartWorkThread();
                }
                if (!NT_SUCCESS(status))
                {
                    return FailNativeContextInitialization(status);
                }
                if (!CompleteNativeContextInitialization())
                {
                    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                               VioGpuNativeStartCompleteInitialization,
                                               STATUS_DEVICE_NOT_READY,
                                               VioGpuNativeStartDetailNone);
                    return FailNativeContextInitialization(STATUS_DEVICE_NOT_READY);
                }
                VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                           VioGpuNativeStartComplete,
                                           STATUS_SUCCESS,
                                           VioGpuNativeStartDetailNone);
                return STATUS_SUCCESS;
            }
            break;
        case PowerDeviceD1:
        case PowerDeviceD2:
        case PowerDeviceD3:
            {
                NTSTATUS status = StopNativeContextTransport();
                if (!NT_SUCCESS(status))
                {
                    return status;
                }
                pCurrentMode->Flags.FrameBufferIsActive = FALSE;
                pCurrentMode->FrameBuffer = NULL;
                return STATUS_SUCCESS;
            }
            break;
    }
    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

#if defined(VIOGPU_NATIVE_CONTEXT)
__declspec(code_seg(".text")) BOOLEAN VioGpuAdapter::AcquireNativeSubmitOperation(void) const
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeSubmitRundownLock, &oldIrql);
    BOOLEAN acquired = !m_NativeSubmitClosing && ExAcquireRundownProtection(&m_NativeSubmitRundown);
    KeReleaseSpinLock(&m_NativeSubmitRundownLock, oldIrql);
    return acquired;
}

void VioGpuAdapter::ReleaseNativeSubmitOperation(void) const
{
    ExReleaseRundownProtection(&m_NativeSubmitRundown);
}

__declspec(code_seg(".text")) void VioGpuAdapter::CompleteNativeSubmitRundown(void)
{
    PAGED_CODE();

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeSubmitRundownLock, &oldIrql);
    if (m_NativeSubmitRundownCompleted)
    {
        KeReleaseSpinLock(&m_NativeSubmitRundownLock, oldIrql);
        return;
    }
    m_NativeSubmitClosing = TRUE;
    KeReleaseSpinLock(&m_NativeSubmitRundownLock, oldIrql);

    ExWaitForRundownProtectionRelease(&m_NativeSubmitRundown);
    ExRundownCompleted(&m_NativeSubmitRundown);

    KeAcquireSpinLock(&m_NativeSubmitRundownLock, &oldIrql);
    m_NativeSubmitRundownCompleted = TRUE;
    KeReleaseSpinLock(&m_NativeSubmitRundownLock, oldIrql);
}

__declspec(code_seg(".text")) BOOLEAN VioGpuAdapter::ReinitializeNativeSubmitRundown(void)
{
    PAGED_CODE();

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeSubmitRundownLock, &oldIrql);
    BOOLEAN completed = m_NativeSubmitRundownCompleted;
    BOOLEAN closing = m_NativeSubmitClosing;
    KeReleaseSpinLock(&m_NativeSubmitRundownLock, oldIrql);

    if (!completed)
    {
        return !closing;
    }

    /* The previous generation has drained and called ExRundownCompleted;
     * reinitialization is therefore legal.  Keep the closing bit set until
     * the new rundown object is ready so no submitter can enter the gap. */
    ExReInitializeRundownProtection(&m_NativeSubmitRundown);
    KeAcquireSpinLock(&m_NativeSubmitRundownLock, &oldIrql);
    m_NativeSubmitRundownCompleted = FALSE;
    m_NativeSubmitClosing = FALSE;
    KeReleaseSpinLock(&m_NativeSubmitRundownLock, oldIrql);
    return TRUE;
}
#endif

BOOLEAN VioGpuAdapter::AckFeature(UINT64 Feature)
{
    PAGED_CODE();

    if (virtio_is_feature_enabled(m_u64HostFeatures, Feature))
    {
        virtio_feature_enable(m_u64GuestFeatures, Feature);
        return TRUE;
    }
    return FALSE;
}

BOOLEAN VioGpuAdapter::BeginNativeContextInitialization(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }
    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_NativeContextLifecycleMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        FailNativeContextAtAnyIrql();
        return FALSE;
    }

    if (!IsListEmpty(&m_NativeContextRegistry) || m_bVirtioInitialized || m_bQueuesInitialized ||
        m_pWorkThread != NULL || m_WorkThreadHandle != NULL || m_GpuBuf.HasAllocationOwner() ||
        m_FrameSegment.GetSize() != 0 || m_CursorSegment.GetSize() != 0 ||
        InterlockedCompareExchange(&m_NativeContextState,
                                   VioGpuNativeContextStarting,
                                   VioGpuNativeContextOffline) != VioGpuNativeContextOffline)
    {
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        return FALSE;
    }

#if defined(VIOGPU_NATIVE_CONTEXT)
    if (!ReinitializeNativeSubmitRundown())
    {
        InterlockedCompareExchange(&m_NativeContextState, VioGpuNativeContextOffline, VioGpuNativeContextStarting);
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        return FALSE;
    }
#endif

    ClearNativeContextReadiness();
    InterlockedIncrement(&m_NativeContextGeneration);
    InterlockedIncrement64(&m_NativeContextResetGeneration);
    InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
    return TRUE;
}

/*
 * The readiness spin lock raises IRQL while this function is executing.
 * Keep the whole routine resident so the instruction immediately after
 * KeAcquireSpinLock cannot fault while the lock is held.
 */
__declspec(code_seg(".text")) BOOLEAN VioGpuAdapter::CompleteNativeContextInitialization(void)
{
    PAGED_CODE();

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeContextReadinessLock, &oldIrql);
    LONG generation = InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0);
    ULONGLONG resetGeneration = (ULONGLONG)InterlockedCompareExchange64(&m_NativeContextResetGeneration, 0, 0);
    BOOLEAN ready = m_bVirtioInitialized && m_bQueuesInitialized && m_pVioGpuDod->IsHardwareInit() &&
                    InterlockedCompareExchange(&m_InterruptDispatchEnabled, FALSE, FALSE) != FALSE &&
                    m_pWorkThread != NULL && m_WorkThreadHandle == NULL && m_NativeContextReadiness.Ready &&
                    m_NativeContextReadiness.Generation == generation &&
                    m_NativeContextReadiness.ResetGeneration == resetGeneration && resetGeneration != 0 &&
                    m_CtrlQueue.IsSynchronousRequestsHealthy() &&
                    InterlockedCompareExchange(&m_NativeContextState,
                                               VioGpuNativeContextReady,
                                               VioGpuNativeContextStarting) == VioGpuNativeContextStarting;
    if (!ready)
    {
        RtlZeroMemory(&m_NativeContextReadiness, sizeof(m_NativeContextReadiness));
        KeReleaseSpinLock(&m_NativeContextReadinessLock, oldIrql);
        return FALSE;
    }
    KeReleaseSpinLock(&m_NativeContextReadinessLock, oldIrql);
    KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
    return TRUE;
}

NTSTATUS VioGpuAdapter::NegotiateNativeContextFeatures(void)
{
    PAGED_CODE();

    if (!AckFeature(VIRTIO_GPU_F_VIRGL) || !AckFeature(VIRTIO_GPU_F_RESOURCE_BLOB) ||
        !AckFeature(VIRTIO_GPU_F_CONTEXT_INIT) || !AckFeature(VIRTIO_GPU_F_CREATE_GUEST_HANDLE))
    {
        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::FailNativeContextInitialization(NTSTATUS status)
{
    PAGED_CODE();

    NTSTATUS closeStatus = StopNativeContextTransportLocked();
    KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
    return NT_SUCCESS(closeStatus) ? status : closeStatus;
}

#pragma code_seg(push)
#pragma code_seg()
_IRQL_requires_max_(DISPATCH_LEVEL) BOOLEAN VioGpuAdapter::QueryNativeContextReadiness(_Out_ PGPU_CAPSET_DRM capset,
                                                                                       _Out_opt_ UINT *capsetVersion,
                                                                                       _Out_opt_ UINT *capsetSize,
                                                                                       _Out_opt_ ULONGLONG *resetGeneration)
{
    if (capset == NULL)
    {
        return FALSE;
    }

    RtlZeroMemory(capset, sizeof(*capset));
    if (capsetVersion != NULL)
    {
        *capsetVersion = 0;
    }
    if (capsetSize != NULL)
    {
        *capsetSize = 0;
    }
    if (resetGeneration != NULL)
    {
        *resetGeneration = 0;
    }

    BOOLEAN ready = FALSE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeContextReadinessLock, &oldIrql);
    LONG generation = InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0);
    ULONGLONG currentResetGeneration = (ULONGLONG)InterlockedCompareExchange64(&m_NativeContextResetGeneration, 0, 0);
    ready = InterlockedCompareExchange(&m_NativeContextState,
                                       VioGpuNativeContextOffline,
                                       VioGpuNativeContextOffline) == VioGpuNativeContextReady &&
            m_NativeContextReadiness.Ready && m_NativeContextReadiness.Generation == generation &&
            m_NativeContextReadiness.ResetGeneration == currentResetGeneration && currentResetGeneration != 0 &&
            m_CtrlQueue.IsSynchronousRequestsHealthy();
    if (ready)
    {
        *capset = m_NativeContextReadiness.Capset;
        if (capsetVersion != NULL)
        {
            *capsetVersion = m_NativeContextReadiness.CapsetVersion;
        }
        if (capsetSize != NULL)
        {
            *capsetSize = m_NativeContextReadiness.CapsetSize;
        }
        if (resetGeneration != NULL)
        {
            *resetGeneration = m_NativeContextReadiness.ResetGeneration;
        }
        ready = InterlockedCompareExchange(&m_NativeContextState,
                                           VioGpuNativeContextOffline,
                                           VioGpuNativeContextOffline) == VioGpuNativeContextReady &&
                InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0) == generation &&
                (ULONGLONG)InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                        0,
                                                        0) == currentResetGeneration &&
                m_CtrlQueue.IsSynchronousRequestsHealthy();
        if (!ready)
        {
            RtlZeroMemory(capset, sizeof(*capset));
            if (capsetVersion != NULL)
            {
                *capsetVersion = 0;
            }
            if (capsetSize != NULL)
            {
                *capsetSize = 0;
            }
            if (resetGeneration != NULL)
            {
                *resetGeneration = 0;
            }
        }
    }
    KeReleaseSpinLock(&m_NativeContextReadinessLock, oldIrql);
    return ready;
}
#pragma code_seg(pop)

UINT VioGpuAdapter::AllocateNativeContextIdLocked(void)
{
    PAGED_CODE();

    UINT contextId = m_NextNativeContextId;
    if (contextId == 0 || contextId == MAXUINT)
    {
        return 0;
    }
    ++m_NextNativeContextId;
    return contextId;
}

#if defined(VIOGPU_NATIVE_CONTEXT)
PAGED_CODE_SEG_END

void VioGpuAdapter::Reconcile2DScanoutAfterResetLocked(void)
{
    ULONGLONG retiredGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_2DRetiredResetGeneration,
                                                                                      0,
                                                                                      0));
    if (m_2DScanoutResetGeneration != 0 && m_2DScanoutResetGeneration <= retiredGeneration)
    {
        m_2DScanoutResourceId = 0;
        m_2DScanoutUnknown = FALSE;
        m_2DScanoutResetGeneration = 0;
    }
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::Set2DScanout(_In_ UINT scanoutId,
                                                       _In_ UINT resourceId,
                                                       _In_ UINT width,
                                                       _In_ UINT height,
                                                       _Out_ UINT *previousResourceId)
{
    if (previousResourceId == NULL || scanoutId >= VIRTIO_GPU_MAX_SCANOUTS ||
        (resourceId == 0 ? width != 0 || height != 0
                         : resourceId >= VIOGPU_NATIVE_RESOURCE_ID_START || width == 0 || height == 0) ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return VioGpuHostContextNotSubmitted;
    }
    *previousResourceId = 0;

    LARGE_INTEGER timeout;
    timeout.QuadPart = -5LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_2DScanoutMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }
    Reconcile2DScanoutAfterResetLocked();
    *previousResourceId = m_2DScanoutResourceId;
    if (m_2DScanoutUnknown)
    {
        KeReleaseMutex(&m_2DScanoutMutex, FALSE);
        return VioGpuHostContextUnknown;
    }

    ULONGLONG operationGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                                                        0,
                                                                                        0));
    if (operationGeneration == 0)
    {
        KeReleaseMutex(&m_2DScanoutMutex, FALSE);
        return VioGpuHostContextNotSubmitted;
    }

    VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.SetScanoutSynchronous(scanoutId, resourceId, width, height, 0, 0);
    if (result == VioGpuHostContextConfirmed)
    {
        m_2DScanoutResourceId = resourceId;
        m_2DScanoutResetGeneration = resourceId == 0 ? 0 : operationGeneration;
    }
    else if (result == VioGpuHostContextUnknown)
    {
        m_2DScanoutUnknown = TRUE;
        m_2DScanoutResetGeneration = operationGeneration;
        FailNativeContextAtAnyIrql();
    }
    KeReleaseMutex(&m_2DScanoutMutex, FALSE);
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::Detach2DScanoutResource(_In_ UINT resourceId, _Out_ BOOLEAN *detached)
{
    if (detached == NULL || resourceId == 0 || resourceId >= VIOGPU_NATIVE_RESOURCE_ID_START ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return VioGpuHostContextNotSubmitted;
    }
    *detached = FALSE;

    LARGE_INTEGER timeout;
    timeout.QuadPart = -5LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_2DScanoutMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }
    Reconcile2DScanoutAfterResetLocked();
    if (m_2DScanoutUnknown)
    {
        KeReleaseMutex(&m_2DScanoutMutex, FALSE);
        return VioGpuHostContextUnknown;
    }
    if (m_2DScanoutResourceId != resourceId)
    {
        *detached = TRUE;
        KeReleaseMutex(&m_2DScanoutMutex, FALSE);
        return VioGpuHostContextConfirmed;
    }

    ULONGLONG operationGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                                                        0,
                                                                                        0));
    if (operationGeneration == 0)
    {
        KeReleaseMutex(&m_2DScanoutMutex, FALSE);
        return VioGpuHostContextNotSubmitted;
    }

    VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.SetScanoutSynchronous(0, 0, 0, 0, 0, 0);
    if (result == VioGpuHostContextConfirmed)
    {
        m_2DScanoutResourceId = 0;
        m_2DScanoutResetGeneration = 0;
        *detached = TRUE;
    }
    else if (result == VioGpuHostContextUnknown)
    {
        m_2DScanoutUnknown = TRUE;
        m_2DScanoutResetGeneration = operationGeneration;
        FailNativeContextAtAnyIrql();
    }
    KeReleaseMutex(&m_2DScanoutMutex, FALSE);
    return result;
}

BOOLEAN VioGpuAdapter::Query2DScanoutResource(_In_ UINT resourceId, _Out_ BOOLEAN *active)
{
    if (active == NULL || resourceId == 0 || resourceId >= VIOGPU_NATIVE_RESOURCE_ID_START ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }
    *active = FALSE;

    LARGE_INTEGER timeout;
    timeout.QuadPart = -5LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_2DScanoutMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        FailNativeContextAtAnyIrql();
        return FALSE;
    }
    Reconcile2DScanoutAfterResetLocked();
    BOOLEAN valid = !m_2DScanoutUnknown;
    if (valid)
    {
        *active = m_2DScanoutResourceId == resourceId;
    }
    KeReleaseMutex(&m_2DScanoutMutex, FALSE);
    return valid;
}

PAGED_CODE_SEG_BEGIN

UINT VioGpuAdapter::Allocate2DResourceId(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !m_CtrlQueue.IsSynchronousRequestsHealthy())
    {
        return 0;
    }
    UINT resourceId = m_Idr.GetId();
    return resourceId < VIOGPU_NATIVE_RESOURCE_ID_START ? resourceId : 0;
}

BOOLEAN VioGpuAdapter::Release2DResourceId(_In_ UINT resourceId)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || resourceId == 0 || resourceId >= VIOGPU_NATIVE_RESOURCE_ID_START)
    {
        return FALSE;
    }
    m_Idr.PutId(resourceId);
    return TRUE;
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::Create2DResourceBacking(_In_ UINT resourceId,
                                                                  _In_ UINT format,
                                                                  _In_ UINT width,
                                                                  _In_ UINT height,
                                                                  _In_ SIZE_T backingSize,
                                                                  _In_reads_(entryCount) const GPU_MEM_ENTRY *entries,
                                                                  _In_ UINT entryCount,
                                                                  _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                                  _Inout_ ULONGLONG *resourceResetGeneration)
{
    PAGED_CODE();

    if (resourceState == NULL || resourceResetGeneration == NULL || resourceId == 0 ||
        resourceId >= VIOGPU_NATIVE_RESOURCE_ID_START || width == 0 || height == 0 || backingSize == 0 ||
        backingSize > MAXULONG || (backingSize & (PAGE_SIZE - 1)) != 0 || entries == NULL || entryCount == 0 ||
        (*resourceState != VioGpu2DResourceNone && *resourceState != VioGpu2DResourceCreated) ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return VioGpuHostContextNotSubmitted;
    }

    ULONGLONG operationGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                                                        0,
                                                                                        0));
    if (operationGeneration == 0 ||
        (*resourceState == VioGpu2DResourceNone ? *resourceResetGeneration != 0
                                                : *resourceResetGeneration != operationGeneration))
    {
        return VioGpuHostContextNotSubmitted;
    }

    VIOGPU_HOST_CONTEXT_RESULT result = VioGpuHostContextConfirmed;
    if (*resourceState == VioGpu2DResourceNone)
    {
        result = m_CtrlQueue.CreateResource2DSynchronous(resourceId, format, width, height);
        if (result != VioGpuHostContextConfirmed)
        {
            if (result == VioGpuHostContextUnknown)
            {
                *resourceState = VioGpu2DResourceUnknown;
                *resourceResetGeneration = operationGeneration;
                FailNativeContextAtAnyIrql();
            }
            return result;
        }
        *resourceState = VioGpu2DResourceCreated;
        *resourceResetGeneration = operationGeneration;
    }

    result = m_CtrlQueue.AttachBackingSynchronous(resourceId, entries, entryCount);
    if (result != VioGpuHostContextConfirmed)
    {
        if (result != VioGpuHostContextUnknown)
        {
            VIOGPU_HOST_CONTEXT_RESULT rollback = m_CtrlQueue.UnrefResourceSynchronous(resourceId);
            if (rollback == VioGpuHostContextConfirmed)
            {
                *resourceState = VioGpu2DResourceNone;
                *resourceResetGeneration = 0;
                return result;
            }
            if (rollback == VioGpuHostContextNotSubmitted)
            {
                return result;
            }
        }
        *resourceState = VioGpu2DResourceUnknown;
        *resourceResetGeneration = operationGeneration;
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }
    *resourceState = VioGpu2DResourceBackingAttached;
    *resourceResetGeneration = operationGeneration;

    ULONGLONG completedGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                                                        0,
                                                                                        0));
    if (completedGeneration != operationGeneration)
    {
        *resourceState = VioGpu2DResourceUnknown;
        *resourceResetGeneration = operationGeneration;
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }
    return VioGpuHostContextConfirmed;
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::Destroy2DResource(_In_ UINT resourceId,
                                                            _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                            _Inout_ ULONGLONG *resourceResetGeneration,
                                                            _Out_ BOOLEAN *released)
{
    PAGED_CODE();

    if (released == NULL)
    {
        return VioGpuHostContextNotSubmitted;
    }
    *released = FALSE;
    if (resourceState == NULL || resourceResetGeneration == NULL || resourceId == 0 ||
        resourceId >= VIOGPU_NATIVE_RESOURCE_ID_START || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return VioGpuHostContextNotSubmitted;
    }
    BOOLEAN retired = FALSE;
    if (!Reconcile2DResourceAfterReset(resourceState, resourceResetGeneration, &retired))
    {
        return VioGpuHostContextNotSubmitted;
    }
    if (*resourceState == VioGpu2DResourceNone)
    {
        *released = TRUE;
        return VioGpuHostContextConfirmed;
    }
    if (*resourceState == VioGpu2DResourceUnknown ||
        (*resourceState != VioGpu2DResourceCreated && *resourceState != VioGpu2DResourceBackingAttached))
    {
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }

    ULONGLONG operationGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                                                        0,
                                                                                        0));
    if (operationGeneration == 0 || *resourceResetGeneration != operationGeneration)
    {
        *resourceState = VioGpu2DResourceUnknown;
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }

    VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.UnrefResourceSynchronous(resourceId);
    if (result == VioGpuHostContextConfirmed)
    {
        *resourceState = VioGpu2DResourceNone;
        *resourceResetGeneration = 0;
        *released = TRUE;
    }
    else if (result == VioGpuHostContextUnknown || result == VioGpuHostContextRejected)
    {
        *resourceState = VioGpu2DResourceUnknown;
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }
    return result;
}

BOOLEAN VioGpuAdapter::Reconcile2DResourceAfterReset(_Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                     _Inout_ ULONGLONG *resourceResetGeneration,
                                                     _Out_ BOOLEAN *retired)
{
    PAGED_CODE();

    if (resourceState == NULL || resourceResetGeneration == NULL || retired == NULL ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }
    *retired = FALSE;
    if (*resourceState == VioGpu2DResourceNone)
    {
        return *resourceResetGeneration == 0;
    }
    if (*resourceResetGeneration == 0)
    {
        return FALSE;
    }

    ULONGLONG retiredGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_2DRetiredResetGeneration,
                                                                                      0,
                                                                                      0));
    if (*resourceResetGeneration <= retiredGeneration)
    {
        *resourceState = VioGpu2DResourceNone;
        *resourceResetGeneration = 0;
        *retired = TRUE;
    }
    return TRUE;
}

BOOLEAN VioGpuAdapter::IsNativeContextResetRetired(_In_ ULONGLONG resetGeneration)
{
    PAGED_CODE();

    if (resetGeneration == 0 || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }

    ULONGLONG retiredGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_2DRetiredResetGeneration,
                                                                                      0,
                                                                                      0));
    return retiredGeneration != 0 && resetGeneration <= retiredGeneration;
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::Present2DResource(_In_ UINT resourceId,
                                                            _In_ ULONGLONG offset,
                                                            _In_ UINT width,
                                                            _In_ UINT height,
                                                            _In_ UINT x,
                                                            _In_ UINT y,
                                                            _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                            _Inout_ ULONGLONG *resourceResetGeneration)
{
    PAGED_CODE();

    if (resourceState == NULL || resourceResetGeneration == NULL || resourceId == 0 ||
        resourceId >= VIOGPU_NATIVE_RESOURCE_ID_START || width == 0 || height == 0 || x > MAXUINT - width ||
        y > MAXUINT - height || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return VioGpuHostContextNotSubmitted;
    }

    BOOLEAN retired = FALSE;
    if (!Reconcile2DResourceAfterReset(resourceState, resourceResetGeneration, &retired) ||
        *resourceState != VioGpu2DResourceBackingAttached)
    {
        return VioGpuHostContextNotSubmitted;
    }

    ULONGLONG operationGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                                                        0,
                                                                                        0));
    if (operationGeneration == 0 || *resourceResetGeneration != operationGeneration)
    {
        return VioGpuHostContextNotSubmitted;
    }

    VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.TransferToHost2DSynchronous(resourceId,
                                                                                offset,
                                                                                width,
                                                                                height,
                                                                                x,
                                                                                y);
    if (result == VioGpuHostContextConfirmed)
    {
        result = m_CtrlQueue.FlushResourceSynchronous(resourceId, width, height, x, y);
    }
    if (result == VioGpuHostContextUnknown)
    {
        *resourceState = VioGpu2DResourceUnknown;
        FailNativeContextAtAnyIrql();
    }
    return result;
}

UINT VioGpuAdapter::AllocateNativeResourceIdLocked(void)
{
    PAGED_CODE();

    UINT resourceId = m_NextNativeResourceId;
    if (resourceId < VIOGPU_NATIVE_RESOURCE_ID_START || resourceId == MAXUINT)
    {
        return 0;
    }
    ++m_NextNativeResourceId;
    return resourceId;
}

UINT VioGpuAdapter::AllocateNativeResourceId(_In_ ULONGLONG expectedResetGeneration)
{
    PAGED_CODE();

    if (expectedResetGeneration == 0 || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return 0;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_NativeContextLifecycleMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        FailNativeContextAtAnyIrql();
        return 0;
    }

    GPU_CAPSET_DRM capset = {};
    ULONGLONG resetGeneration = 0;
    UINT resourceId = 0;
    if (QueryNativeContextReadiness(&capset, NULL, NULL, &resetGeneration) &&
        resetGeneration == expectedResetGeneration)
    {
        resourceId = AllocateNativeResourceIdLocked();
    }
    KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
    return resourceId;
}

VIOGPU_HOST_CONTEXT_RESULT
VioGpuAdapter::CreateNativeGuestAllocation(_In_ const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                                           _In_ UINT resourceId,
                                           _In_ UINT blobId,
                                           _In_ ULONGLONG logicalSize,
                                           _In_ SIZE_T backingSize,
                                           _In_ ULONGLONG requestedIova,
                                           _In_reads_(entryCount) const GPU_MEM_ENTRY *entries,
                                           _In_ UINT entryCount,
                                           _In_ UINT msmFlags,
                                           _In_ UINT blobFlags,
                                           _Out_ BOOLEAN *ownershipRetained)
{
    PAGED_CODE();

    const UINT validMsmFlags = MSM_BO_CACHED_COHERENT | MSM_BO_GPU_READONLY;
    const UINT validBlobFlags = VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE | VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
    if (ownershipRetained == NULL)
    {
        return VioGpuHostContextNotSubmitted;
    }
    *ownershipRetained = FALSE;
    if (logicalSize == 0 || logicalSize > MAXULONGLONG - (PAGE_SIZE - 1))
    {
        return VioGpuHostContextNotSubmitted;
    }
    const ULONGLONG logicalAlignedSize = (logicalSize + PAGE_SIZE - 1) & ~((ULONGLONG)PAGE_SIZE - 1);
    GPU_CAPSET_DRM capset = {};
    ULONGLONG readyResetGeneration = 0;
    /* drm2kgsl rounds GEM_NEW.size to pages before matching the subsequent
     * HOST3D_GUEST blob.  Reject a mismatched logical extent before taking
     * host ownership; this path deliberately pairs the KMD resource and blob
     * identities so rollback cannot target an unrelated resource. */
    if (snapshot == NULL || snapshot->Adapter != this || snapshot->Owner == NULL || snapshot->Registration == NULL ||
        snapshot->Owner->Registration != snapshot->Registration || snapshot->ContextId == 0 ||
        snapshot->Generation <= 0 || snapshot->Owner->State != VioGpuNativeContextOwnerLive ||
        snapshot->Owner->Generation != snapshot->Generation ||
        snapshot->Owner->ResetGeneration != snapshot->ResetGeneration ||
        snapshot->Owner->ContextId != snapshot->ContextId || ReadNativeAllocationCount(snapshot->Owner) == MAXLONG ||
        snapshot->ResetGeneration == 0 || snapshot->VaStart == 0 || snapshot->VaSize == 0 ||
        resourceId < VIOGPU_NATIVE_RESOURCE_ID_START || resourceId == MAXUINT || blobId == 0 || resourceId != blobId ||
        backingSize == 0 || logicalSize > backingSize || backingSize < PAGE_SIZE || backingSize > MAXULONG ||
        (backingSize & (PAGE_SIZE - 1)) != 0 || logicalAlignedSize != (ULONGLONG)backingSize ||
        (ULONGLONG)backingSize > snapshot->VaSize || requestedIova == 0 || (requestedIova & (PAGE_SIZE - 1)) != 0 ||
        requestedIova > MAXULONGLONG - (backingSize - 1) || entries == NULL || entryCount == 0 ||
        (msmFlags & MSM_BO_CACHED_COHERENT) == 0 || (msmFlags & ~validMsmFlags) != 0 ||
        (blobFlags & VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE) == 0 || (blobFlags & ~validBlobFlags) != 0 ||
        requestedIova < snapshot->VaStart || snapshot->VaStart > MAXULONGLONG - (snapshot->VaSize - 1) ||
        requestedIova > snapshot->VaStart + snapshot->VaSize - (ULONGLONG)backingSize ||
        !QueryNativeContextReadiness(&capset, NULL, NULL, &readyResetGeneration) ||
        readyResetGeneration != snapshot->ResetGeneration || capset.msm.has_cached_coherent == 0 ||
        !IsNativeContextGenerationCurrent(snapshot->Generation, snapshot->ResetGeneration) ||
        !VioGpuNativeControlFaultsClear(this, snapshot->Owner))
    {
        return VioGpuHostContextNotSubmitted;
    }

    if (!IsNativeContextGenerationCurrent(snapshot->Generation, snapshot->ResetGeneration))
    {
        return VioGpuHostContextNotSubmitted;
    }

    MSM_CCMD_GEM_NEW_REQ request = {};
    request.hdr.cmd = MSM_CCMD_GEM_NEW;
    request.hdr.len = sizeof(request);
    request.iova = requestedIova;
    request.size = logicalSize;
    request.flags = msmFlags | MSM_BO_GUEST_ALLOC;
    request.blob_id = blobId;

    if (!TryReferenceNativeAllocationCount(snapshot->Owner))
    {
        return VioGpuHostContextNotSubmitted;
    }

    VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.SubmitNativeControl(snapshot->ContextId, &request, sizeof(request));
    if (result == VioGpuHostContextUnknown)
    {
        *ownershipRetained = TRUE;
        FailNativeContextAtAnyIrql();
        return result;
    }
    if (result != VioGpuHostContextConfirmed)
    {
        BOOLEAN released = ReleaseNativeAllocationCount(snapshot->Owner);
        NT_ASSERT(released);
        UNREFERENCED_PARAMETER(released);
        if (result == VioGpuHostContextRejected)
        {
            FailNativeContextAtAnyIrql();
        }
        return result;
    }
    *ownershipRetained = TRUE;
    if (!IsNativeContextGenerationCurrent(snapshot->Generation, snapshot->ResetGeneration) ||
        !VioGpuNativeControlFaultsClear(this, snapshot->Owner))
    {
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }

    result = m_CtrlQueue.CreateNativeGuestBlob(snapshot->ContextId,
                                               resourceId,
                                               blobId,
                                               backingSize,
                                               blobFlags,
                                               entries,
                                               entryCount);
    if (result != VioGpuHostContextConfirmed)
    {
        /* GEM_NEW owns a host object before RESOURCE_CREATE_BLOB runs.  If
         * blob creation was rejected or never submitted, release that object
         * while the context is still live; otherwise retain the ownership
         * count and quarantine the transport because the host result is
         * unknowable. */
        if (result != VioGpuHostContextUnknown)
        {
            VIOGPU_HOST_CONTEXT_RESULT rollback = m_CtrlQueue.UnrefNativeResource(resourceId);
            /* INVALID_RESOURCE_ID cannot prove that the GEM_NEW blob-table
             * object was released: RESOURCE_CREATE_BLOB may fail before it
             * attaches that object to resourceId.  Only a confirmed UNREF is
             * a transactional rollback proof. */
            if (rollback == VioGpuHostContextConfirmed && ReleaseNativeAllocationCount(snapshot->Owner))
            {
                *ownershipRetained = FALSE;
                return result;
            }
        }
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }

    if (!IsNativeContextGenerationCurrent(snapshot->Generation, snapshot->ResetGeneration))
    {
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }
    return VioGpuHostContextConfirmed;
}

VIOGPU_HOST_CONTEXT_RESULT
VioGpuAdapter::DestroyNativeGuestAllocation(_In_ const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                                            _In_ UINT resourceId,
                                            _Out_ BOOLEAN *released)
{
    PAGED_CODE();

    if (released == NULL)
    {
        return VioGpuHostContextNotSubmitted;
    }
    *released = FALSE;
    if (snapshot == NULL || snapshot->Adapter != this || snapshot->Owner == NULL || snapshot->Registration == NULL ||
        snapshot->Owner->Registration != snapshot->Registration ||
        snapshot->Owner->State != VioGpuNativeContextOwnerLive || snapshot->Owner->Generation != snapshot->Generation ||
        snapshot->Owner->ResetGeneration != snapshot->ResetGeneration ||
        snapshot->Owner->ContextId != snapshot->ContextId || ReadNativeAllocationCount(snapshot->Owner) == 0 ||
        resourceId < VIOGPU_NATIVE_RESOURCE_ID_START || resourceId == MAXUINT || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !IsNativeContextGenerationCurrent(snapshot->Generation, snapshot->ResetGeneration))
    {
        return VioGpuHostContextNotSubmitted;
    }

    VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.UnrefNativeResource(resourceId);
    if (result == VioGpuHostContextConfirmed)
    {
        BOOLEAN countReleased = ReleaseNativeAllocationCount(snapshot->Owner);
        if (!countReleased)
        {
            FailNativeContextAtAnyIrql();
            return VioGpuHostContextUnknown;
        }
        *released = TRUE;
    }
    else if (result == VioGpuHostContextUnknown || result == VioGpuHostContextRejected)
    {
        FailNativeContextAtAnyIrql();
        return VioGpuHostContextUnknown;
    }
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::QueryNativeContextParameterLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner,
                                                                            _In_ ULONG parameter,
                                                                            _Out_ PULONGLONG value)
{
    PAGED_CODE();

    VIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC diagnostic = {};
    diagnostic.Parameter = parameter;
    diagnostic.Validation = VioGpuHostResponseUnclassified;
    diagnostic.Result = VioGpuHostContextNotSubmitted;
    if (owner != NULL)
    {
        diagnostic.ContextId = owner->ContextId;
        diagnostic.ControlBarOffset = owner->ControlBarOffset;
        diagnostic.ControlAddress = reinterpret_cast<ULONG_PTR>(owner->ControlAddress);
        diagnostic.ControlBlobSize = owner->ControlBlobSize;
    }
    if (m_pVioGpuDod != NULL)
    {
        diagnostic.Phase = VioGpuNativeContextParameterPreconditions;
        m_pVioGpuDod->RecordNativeContextParameterDiagnostic(&diagnostic);
    }

    if (owner == NULL || value == NULL || owner->ContextId == 0 || !owner->ControlResourceCreated ||
        !owner->ControlMapped || (parameter != MSM_PARAM_VA_START && parameter != MSM_PARAM_VA_SIZE) ||
        owner->LastControlSeqno == MAXULONG)
    {
        diagnostic.Result = VioGpuHostContextNotSubmitted;
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextParameterDiagnostic(&diagnostic);
        }
        return VioGpuHostContextNotSubmitted;
    }
    *value = 0;

    ULONG sequence = owner->LastControlSeqno + 1;
    diagnostic.ContextId = owner->ContextId;
    diagnostic.Sequence = sequence;

    if (m_pVioGpuDod != NULL)
    {
        diagnostic.Phase = VioGpuNativeContextParameterSeed;
        m_pVioGpuDod->RecordNativeContextParameterDiagnostic(&diagnostic);
    }
    if (!VioGpuSeedNativeControlResponse(this,
                                         owner,
                                         sequence,
                                         sizeof(MSM_CCMD_IOCTL_SIMPLE_GET_PARAM_RSP),
                                         &diagnostic))
    {
        diagnostic.Result = VioGpuHostContextUnknown;
        m_CtrlQueue.PoisonSynchronousRequests();
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextParameterDiagnostic(&diagnostic);
        }
        return VioGpuHostContextUnknown;
    }
    owner->LastControlSeqno = sequence;

    MSM_CCMD_IOCTL_SIMPLE_GET_PARAM_REQ request = {};
    request.hdr.cmd = MSM_CCMD_IOCTL_SIMPLE;
    request.hdr.len = sizeof(request);
    request.hdr.seqno = sequence;
    request.hdr.rsp_off = 0;
    request.ioctl_cmd = DRM_IOCTL_MSM_GET_PARAM;
    request.param.pipe = MSM_PIPE_3D0;
    request.param.param = parameter;

    if (m_pVioGpuDod != NULL)
    {
        diagnostic.Phase = VioGpuNativeContextParameterSubmitted;
        m_pVioGpuDod->RecordNativeContextParameterDiagnostic(&diagnostic);
    }
    VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.SubmitNativeControl(owner->ContextId,
                                                                        &request,
                                                                        sizeof(request),
                                                                        &diagnostic);
    if (result != VioGpuHostContextConfirmed)
    {
        diagnostic.Result = result;
        diagnostic.Validation = static_cast<viogpu_host_context_response_validation>(diagnostic.OuterValidation);
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextParameterDiagnostic(&diagnostic);
        }
        return result;
    }
    if (m_pVioGpuDod != NULL)
    {
        diagnostic.Phase = VioGpuNativeContextParameterCopy;
        m_pVioGpuDod->RecordNativeContextParameterDiagnostic(&diagnostic);
    }
    if (!VioGpuConsumeNativeControlResponse(this, owner, sequence, parameter, value, &diagnostic))
    {
        diagnostic.Result = VioGpuHostContextUnknown;
        m_CtrlQueue.PoisonSynchronousRequests();
        if (m_pVioGpuDod != NULL)
        {
            diagnostic.Phase = VioGpuNativeContextParameterValidated;
            m_pVioGpuDod->RecordNativeContextParameterDiagnostic(&diagnostic);
        }
        return VioGpuHostContextUnknown;
    }
    diagnostic.Result = VioGpuHostContextConfirmed;
    diagnostic.Phase = VioGpuNativeContextParameterComplete;
    if (m_pVioGpuDod != NULL)
    {
        m_pVioGpuDod->RecordNativeContextParameterDiagnostic(&diagnostic);
    }
    return VioGpuHostContextConfirmed;
}

VIOGPU_HOST_CONTEXT_RESULT
VioGpuAdapter::CreateNativeSubmitQueueLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner, _Out_ PUINT queueId)
{
    PAGED_CODE();

    if (queueId == NULL)
    {
        return VioGpuHostContextNotSubmitted;
    }
    *queueId = 0;
    if (owner == NULL || owner->ContextId == 0 || !owner->ControlResourceCreated || !owner->ControlMapped ||
        owner->ControlAddress == NULL || owner->SubmitQueueCreated || owner->SubmitQueueId != 0 ||
        owner->LastControlSeqno == MAXULONG)
    {
        return VioGpuHostContextNotSubmitted;
    }

    ULONG sequence = owner->LastControlSeqno + 1;
    if (!VioGpuSeedNativeControlResponse(this, owner, sequence, sizeof(MSM_CCMD_IOCTL_SIMPLE_SUBMITQUEUE_NEW_RSP)))
    {
        m_CtrlQueue.PoisonSynchronousRequests();
        return VioGpuHostContextUnknown;
    }
    owner->LastControlSeqno = sequence;

    MSM_CCMD_IOCTL_SIMPLE_SUBMITQUEUE_NEW_REQ request = {};
    request.hdr.cmd = MSM_CCMD_IOCTL_SIMPLE;
    request.hdr.len = sizeof(request);
    request.hdr.seqno = sequence;
    request.hdr.rsp_off = 0;
    request.ioctl_cmd = DRM_IOCTL_MSM_SUBMITQUEUE_NEW;

    VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.SubmitNativeControl(owner->ContextId, &request, sizeof(request));
    if (result != VioGpuHostContextConfirmed)
    {
        return result;
    }

    MSM_CCMD_IOCTL_SIMPLE_SUBMITQUEUE_NEW_RSP response = {};
    if (!VioGpuCopyNativeControlResponse(this, owner, sequence, &response, sizeof(response)))
    {
        m_CtrlQueue.PoisonSynchronousRequests();
        return VioGpuHostContextUnknown;
    }
    if (response.ret != 0)
    {
        return VioGpuHostContextRejected;
    }
    if (response.submitqueue.flags != 0 || response.submitqueue.prio != 0 || response.submitqueue.id == 0)
    {
        m_CtrlQueue.PoisonSynchronousRequests();
        return VioGpuHostContextUnknown;
    }

    owner->SubmitQueueId = response.submitqueue.id;
    owner->SubmitQueueCreated = TRUE;
    *queueId = owner->SubmitQueueId;
    return VioGpuHostContextConfirmed;
}

VIOGPU_HOST_CONTEXT_RESULT
VioGpuAdapter::CloseNativeSubmitQueueLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner)
{
    PAGED_CODE();

    if (owner == NULL || owner->ContextId == 0 || !owner->ControlResourceCreated || !owner->ControlMapped ||
        owner->ControlAddress == NULL || !owner->SubmitQueueCreated || owner->SubmitQueueId == 0 ||
        owner->LastControlSeqno == MAXULONG)
    {
        return VioGpuHostContextNotSubmitted;
    }

    ULONG sequence = owner->LastControlSeqno + 1;
    if (!VioGpuSeedNativeControlResponse(this, owner, sequence, sizeof(MSM_CCMD_IOCTL_SIMPLE_SUBMITQUEUE_CLOSE_RSP)))
    {
        m_CtrlQueue.PoisonSynchronousRequests();
        return VioGpuHostContextUnknown;
    }
    owner->LastControlSeqno = sequence;

    MSM_CCMD_IOCTL_SIMPLE_SUBMITQUEUE_CLOSE_REQ request = {};
    request.hdr.cmd = MSM_CCMD_IOCTL_SIMPLE;
    request.hdr.len = sizeof(request);
    request.hdr.seqno = sequence;
    request.hdr.rsp_off = 0;
    request.ioctl_cmd = DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE;
    request.queue_id = owner->SubmitQueueId;

    VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.SubmitNativeControl(owner->ContextId, &request, sizeof(request));
    if (result != VioGpuHostContextConfirmed)
    {
        return result;
    }

    MSM_CCMD_IOCTL_SIMPLE_SUBMITQUEUE_CLOSE_RSP response = {};
    if (!VioGpuCopyNativeControlResponse(this, owner, sequence, &response, sizeof(response)) || response.ret != 0)
    {
        m_CtrlQueue.PoisonSynchronousRequests();
        return VioGpuHostContextUnknown;
    }

    owner->SubmitQueueCreated = FALSE;
    owner->SubmitQueueId = 0;
    return VioGpuHostContextConfirmed;
}

VIOGPU_HOST_CONTEXT_RESULT
VioGpuAdapter::DestroyNativeContextHostObjectsLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner)
{
    PAGED_CODE();

    if (owner == NULL || owner->ContextId == 0)
    {
        return VioGpuHostContextUnknown;
    }

    if (owner->SubmitQueueCreated)
    {
        VIOGPU_HOST_CONTEXT_RESULT result = CloseNativeSubmitQueueLocked(owner);
        if (result != VioGpuHostContextConfirmed)
        {
            return VioGpuHostContextUnknown;
        }
    }
    else if (owner->SubmitQueueId != 0)
    {
        return VioGpuHostContextUnknown;
    }

    if (owner->ControlMapped)
    {
        VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.UnmapNativeControlBlob(owner->ControlResourceId);
        if (result != VioGpuHostContextConfirmed && result != VioGpuHostContextRejected)
        {
            return VioGpuHostContextUnknown;
        }
        owner->ControlMapped = FALSE;
        if (result == VioGpuHostContextRejected)
        {
            owner->ControlResourceCreated = FALSE;
            owner->ControlResourceId = 0;
        }
    }

    if (owner->ControlAddress != NULL)
    {
        NTSTATUS status = m_PciResources.UnmapHostVisibleAddress(owner->ControlAddress);
        if (!NT_SUCCESS(status))
        {
            return VioGpuHostContextUnknown;
        }
        owner->ControlBarOffset = 0;
        owner->ControlAddress = NULL;
    }

    if (owner->ControlResourceCreated)
    {
        VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.UnrefNativeResource(owner->ControlResourceId);
        if (result != VioGpuHostContextConfirmed && result != VioGpuHostContextRejected)
        {
            return VioGpuHostContextUnknown;
        }
        owner->ControlResourceCreated = FALSE;
        owner->ControlResourceId = 0;
    }

    VIOGPU_HOST_CONTEXT_RESULT result = m_CtrlQueue.DestroyNativeContext(owner->ContextId);
    if (result != VioGpuHostContextConfirmed && result != VioGpuHostContextRejected)
    {
        return VioGpuHostContextUnknown;
    }
    owner->ContextId = 0;
    return VioGpuHostContextConfirmed;
}

BOOLEAN VioGpuAdapter::AllocateNativeControlSlotLocked(_Out_ PULONGLONG offset, _Out_ PVOID *address)
{
    PAGED_CODE();

    if (offset == NULL || address == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }
    *offset = 0;
    *address = NULL;

    UINT bar = 0;
    ULONGLONG regionOffset = 0;
    ULONGLONG regionSize = 0;
    if (!m_PciResources.QueryHostVisibleRegion(&bar, &regionOffset, &regionSize) ||
        regionSize <= VIOGPU_NATIVE_CONTROL_BAR_GUARD_SIZE ||
        regionSize - VIOGPU_NATIVE_CONTROL_BAR_GUARD_SIZE < VIOGPU_NATIVE_CONTROL_BLOB_SIZE)
    {
        return FALSE;
    }

    ULONGLONG slotCount = (regionSize - VIOGPU_NATIVE_CONTROL_BAR_GUARD_SIZE) / VIOGPU_NATIVE_CONTROL_BLOB_SIZE;
    for (ULONGLONG slot = 0; slot < slotCount; ++slot)
    {
        ULONGLONG candidate = VIOGPU_NATIVE_CONTROL_BAR_GUARD_SIZE + slot * VIOGPU_NATIVE_CONTROL_BLOB_SIZE;
        BOOLEAN inUse = FALSE;
        for (PLIST_ENTRY link = m_NativeContextRegistry.Flink; link != &m_NativeContextRegistry; link = link->Flink)
        {
            VIOGPU_NATIVE_CONTEXT_OWNER *owner = CONTAINING_RECORD(link, VIOGPU_NATIVE_CONTEXT_OWNER, AdapterLink);
            if (owner->ControlAddress != NULL && owner->ControlBarOffset == candidate)
            {
                inUse = TRUE;
                break;
            }
        }
        if (inUse)
        {
            continue;
        }

        // Do not touch the BAR until RESOURCE_MAP_BLOB has installed the
        // backing at this guest offset.  On Gunyah the initial BAR mapping
        // can otherwise retain an unmapped stage-2 entry and fault on the
        // first CPU read even though the later host map succeeds.
        *offset = candidate;
        return TRUE;
    }
    return FALSE;
}
#endif

__declspec(code_seg(".text")) NTSTATUS VioGpuAdapter::CreateNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                                                          _In_ ULONGLONG expectedResetGeneration)
{
    PAGED_CODE();

    if (m_pVioGpuDod != NULL)
    {
        m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateEntered, STATUS_PENDING, 0);
    }

    if (context == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        InterlockedCompareExchange(&context->State,
                                   VioGpuNativeContextAllocated,
                                   VioGpuNativeContextAllocated) != VioGpuNativeContextAllocated ||
        context->Adapter != NULL || context->Owner != NULL || context->Registered || context->VaStart != 0 ||
        context->VaSize != 0 || context->SubmitQueueId != 0 || context->AllocationReferences != 0 ||
        !IsListEmpty(&context->AllocationRanges))
    {
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreatePreconditions,
                                                              STATUS_INVALID_PARAMETER,
                                                              0);
        }
        return STATUS_INVALID_PARAMETER;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_NativeContextLifecycleMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        FailNativeContextAtAnyIrql();
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateMutex, status, 0);
        }
        return status;
    }

    LONG generation = InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0);
    ULONGLONG resetGeneration = 0;
    GPU_CAPSET_DRM capset = {};
    if (InterlockedCompareExchange(&m_NativeContextState,
                                   VioGpuNativeContextOffline,
                                   VioGpuNativeContextOffline) != VioGpuNativeContextReady ||
        expectedResetGeneration == 0 || !QueryNativeContextReadiness(&capset, NULL, NULL, &resetGeneration) ||
        resetGeneration != expectedResetGeneration)
    {
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateReadiness,
                                                              STATUS_DEVICE_NOT_READY,
                                                              static_cast<DWORD>(expectedResetGeneration));
        }
        return STATUS_DEVICE_NOT_READY;
    }

    UINT contextId = AllocateNativeContextIdLocked();
    if (contextId == 0)
    {
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateIds, STATUS_NO_MEMORY, 0);
        }
        return STATUS_NO_MEMORY;
    }
#if defined(VIOGPU_NATIVE_CONTEXT)
    UINT resourceId = AllocateNativeResourceIdLocked();
    if (resourceId == 0)
    {
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateIds,
                                                              STATUS_NO_MEMORY,
                                                              contextId);
        }
        return STATUS_NO_MEMORY;
    }
#endif

    VIOGPU_NATIVE_CONTEXT_OWNER *owner = new (NonPagedPoolNx) VIOGPU_NATIVE_CONTEXT_OWNER;
    if (owner == NULL)
    {
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateOwner,
                                                              STATUS_NO_MEMORY,
                                                              contextId);
        }
        return STATUS_NO_MEMORY;
    }
    RtlZeroMemory(owner, sizeof(*owner));
    InitializeListHead(&owner->AdapterLink);
    owner->Registration = context;
    owner->State = VioGpuNativeContextOwnerCreating;
    owner->Generation = generation;
    owner->ResetGeneration = resetGeneration;
    owner->ContextId = contextId;
#if defined(VIOGPU_NATIVE_CONTEXT)
    owner->ControlResourceId = resourceId;
    owner->ControlBlobSize = VIOGPU_NATIVE_CONTROL_BLOB_SIZE;
#endif
    InsertTailList(&m_NativeContextRegistry, &owner->AdapterLink);

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->BindingLock, &oldIrql);
    context->Adapter = this;
    context->Owner = owner;
    context->Generation = generation;
    context->ResetGeneration = resetGeneration;
    context->ContextId = contextId;
    InterlockedExchange(&context->State, VioGpuNativeContextCreating);
    KeReleaseSpinLock(&context->BindingLock, oldIrql);

    VIOGPU_HOST_CONTEXT_RESULT createResult = m_CtrlQueue.CreateNativeContext(contextId);
    VIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC hostResponse = {};
    m_CtrlQueue.GetLastNativeContextResponseDiagnostic(&hostResponse);
    if (m_pVioGpuDod != NULL)
    {
        m_pVioGpuDod->RecordNativeContextCreateResponseDiagnostic(&hostResponse);
    }
    VIOGPU_HOST_CONTEXT_RESULT stageResult = createResult;
    BOOLEAN hostContextCreated = createResult == VioGpuHostContextConfirmed;
    if (m_pVioGpuDod != NULL)
    {
        m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateHostContext,
                                                          createResult == VioGpuHostContextConfirmed ? STATUS_SUCCESS
                                                                                                     : STATUS_DEVICE_NOT_READY,
                                                          static_cast<DWORD>(createResult));
    }
#if defined(VIOGPU_NATIVE_CONTEXT)
    ULONGLONG vaStart = 0;
    ULONGLONG vaSize = 0;
    UINT submitQueueId = 0;
    if (stageResult == VioGpuHostContextConfirmed)
    {
        stageResult = m_CtrlQueue.CreateNativeControlBlob(contextId, resourceId);
        if (stageResult == VioGpuHostContextConfirmed)
        {
            owner->ControlResourceCreated = TRUE;
        }
        else if (stageResult == VioGpuHostContextNotSubmitted || stageResult == VioGpuHostContextRejected)
        {
            owner->ControlResourceId = 0;
        }
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateControlBlob,
                                                              stageResult == VioGpuHostContextConfirmed ? STATUS_SUCCESS
                                                                                                        : STATUS_DEVICE_NOT_READY,
                                                              static_cast<DWORD>(stageResult));
        }
    }
    if (stageResult == VioGpuHostContextConfirmed)
    {
        ULONGLONG controlOffset = 0;
        PVOID controlAddress = NULL;
        if (!AllocateNativeControlSlotLocked(&controlOffset, &controlAddress))
        {
            stageResult = VioGpuHostContextNotSubmitted;
            if (m_pVioGpuDod != NULL)
            {
                m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateControlMap,
                                                                  STATUS_DEVICE_NOT_READY,
                                                                  static_cast<DWORD>(stageResult));
            }
        }
        else
        {
            owner->ControlBarOffset = controlOffset;
            UINT hostVisibleBar = MAXUINT;
            ULONGLONG hostVisibleOffset = 0;
            ULONGLONG hostVisibleSize = 0;
            if (m_PciResources.QueryHostVisibleRegion(&hostVisibleBar, &hostVisibleOffset, &hostVisibleSize))
            {
                owner->ControlHostVisibleBar = hostVisibleBar;
                owner->ControlHostVisibleOffset = hostVisibleOffset;
                owner->ControlHostVisibleSize = hostVisibleSize;
            }
            stageResult = m_CtrlQueue.MapNativeControlBlob(resourceId, controlOffset);
            VIOGPU_NATIVE_MAP_RESPONSE_DIAGNOSTIC mapResponse = {};
            m_CtrlQueue.GetLastNativeMapResponseDiagnostic(&mapResponse);
            if (m_pVioGpuDod != NULL)
            {
                m_pVioGpuDod->RecordNativeContextMapResponseDiagnostic(&mapResponse);
            }
            DWORD controlMapDetail = static_cast<DWORD>(stageResult);
            if (stageResult == VioGpuHostContextConfirmed)
            {
                owner->ControlMapped = TRUE;
                NTSTATUS mapStatus = m_PciResources.MapHostVisibleAddress(controlOffset,
                                                                          VIOGPU_NATIVE_CONTROL_BLOB_SIZE,
                                                                          &controlAddress);
                owner->ControlMapStatus = static_cast<ULONG>(mapStatus);
                PHYSICAL_ADDRESS mappedPhysicalAddress = {};
                ULONGLONG mappedRegionOffset = 0;
                ULONGLONG mappedLength = 0;
                BOOLEAN mappingKnown = NT_SUCCESS(mapStatus) && controlAddress != NULL &&
                                       m_PciResources.QueryHostVisibleMapping(&mappedPhysicalAddress,
                                                                              &mappedRegionOffset,
                                                                              &mappedLength);
                if (m_pVioGpuDod != NULL)
                {
                    m_pVioGpuDod->RecordNativeContextMapMemoryDiagnostic(mapStatus,
                                                                         mappingKnown ? static_cast<ULONGLONG>(mappedPhysicalAddress.QuadPart)
                                                                                      : 0,
                                                                         mappingKnown ? mappedLength : 0,
                                                                         hostVisibleBar,
                                                                         mappingKnown ? mappedRegionOffset
                                                                                      : controlOffset,
                                                                         TRUE,
                                                                         NT_SUCCESS(mapStatus) && controlAddress != NULL);
                }
                if (!NT_SUCCESS(mapStatus) || controlAddress == NULL)
                {
                    controlMapDetail = static_cast<DWORD>(mapStatus);
                    stageResult = VioGpuHostContextNotSubmitted;
                }
                else
                {
                    owner->ControlAddress = controlAddress;
                }
            }
            else if (m_pVioGpuDod != NULL)
            {
                // A rejected host map never attempted DxgkCbMapMemory.  Clear the
                // commit marker so a reader cannot reuse an earlier local BAR map.
                m_pVioGpuDod->RecordNativeContextMapMemoryDiagnostic(STATUS_DEVICE_NOT_READY,
                                                                     0,
                                                                     0,
                                                                     hostVisibleBar,
                                                                     controlOffset,
                                                                     FALSE,
                                                                     FALSE);
            }
            if (m_pVioGpuDod != NULL)
            {
                m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateControlMap,
                                                                  stageResult == VioGpuHostContextConfirmed ? STATUS_SUCCESS
                                                                                                            : STATUS_DEVICE_NOT_READY,
                                                                  stageResult == VioGpuHostContextConfirmed ? static_cast<DWORD>(stageResult)
                                                                                                            : controlMapDetail);
            }
        }
    }
    if (stageResult == VioGpuHostContextConfirmed)
    {
        stageResult = QueryNativeContextParameterLocked(owner, MSM_PARAM_VA_START, &vaStart);
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateVaStart,
                                                              stageResult == VioGpuHostContextConfirmed ? STATUS_SUCCESS
                                                                                                        : STATUS_DEVICE_NOT_READY,
                                                              static_cast<DWORD>(stageResult));
        }
    }
    if (stageResult == VioGpuHostContextConfirmed)
    {
        stageResult = QueryNativeContextParameterLocked(owner, MSM_PARAM_VA_SIZE, &vaSize);
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateVaSize,
                                                              stageResult == VioGpuHostContextConfirmed ? STATUS_SUCCESS
                                                                                                        : STATUS_DEVICE_NOT_READY,
                                                              static_cast<DWORD>(stageResult));
        }
    }
    if (stageResult == VioGpuHostContextConfirmed &&
        (vaStart == 0 || vaSize == 0 || (vaStart & (PAGE_SIZE - 1)) != 0 || (vaSize & (PAGE_SIZE - 1)) != 0 ||
         vaStart > MAXULONGLONG - vaSize))
    {
        m_CtrlQueue.PoisonSynchronousRequests();
        stageResult = VioGpuHostContextUnknown;
    }
    if (stageResult == VioGpuHostContextConfirmed)
    {
        stageResult = CreateNativeSubmitQueueLocked(owner, &submitQueueId);
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateSubmitQueue,
                                                              stageResult == VioGpuHostContextConfirmed ? STATUS_SUCCESS
                                                                                                        : STATUS_DEVICE_NOT_READY,
                                                              static_cast<DWORD>(stageResult));
        }
    }
#endif

    BOOLEAN stillCurrent = stageResult == VioGpuHostContextConfirmed && m_CtrlQueue.IsSynchronousRequestsHealthy() &&
                           InterlockedCompareExchange(&m_NativeContextState,
                                                      VioGpuNativeContextOffline,
                                                      VioGpuNativeContextOffline) == VioGpuNativeContextReady &&
                           InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0) == generation &&
                           (ULONGLONG)InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                                   0,
                                                                   0) == resetGeneration;
    if (!stillCurrent)
    {
        if (m_pVioGpuDod != NULL)
        {
            m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateCurrent,
                                                              STATUS_DEVICE_NOT_READY,
                                                              static_cast<DWORD>(stageResult));
        }
        BOOLEAN generationCurrent = InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0) == generation &&
                                    (ULONGLONG)InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                                            0,
                                                                            0) == resetGeneration &&
                                    InterlockedCompareExchange(&m_NativeContextState,
                                                               VioGpuNativeContextOffline,
                                                               VioGpuNativeContextOffline) == VioGpuNativeContextReady;
        BOOLEAN ownerRetired = FALSE;
        if (!hostContextCreated &&
            (createResult == VioGpuHostContextNotSubmitted || createResult == VioGpuHostContextRejected))
        {
            RetireNativeContextOwnerLocked(owner);
            ownerRetired = TRUE;
        }
        else if (hostContextCreated && stageResult != VioGpuHostContextUnknown &&
                 m_CtrlQueue.IsSynchronousRequestsHealthy() && generationCurrent)
        {
#if defined(VIOGPU_NATIVE_CONTEXT)
            VIOGPU_HOST_CONTEXT_RESULT cleanupResult = DestroyNativeContextHostObjectsLocked(owner);
#else
            VIOGPU_HOST_CONTEXT_RESULT cleanupResult = m_CtrlQueue.DestroyNativeContext(contextId);
#endif
            if (cleanupResult == VioGpuHostContextConfirmed || cleanupResult == VioGpuHostContextRejected)
            {
                RetireNativeContextOwnerLocked(owner);
                ownerRetired = TRUE;
            }
        }
        if (!ownerRetired)
        {
            owner->Registration = NULL;
            FailNativeContextAtAnyIrql();
        }
        KeAcquireSpinLock(&context->BindingLock, &oldIrql);
        context->Adapter = NULL;
        context->Owner = NULL;
        context->Generation = 0;
        context->ResetGeneration = 0;
        context->ContextId = 0;
        context->VaStart = 0;
        context->VaSize = 0;
        context->SubmitQueueId = 0;
        InterlockedExchange(&context->State, VioGpuNativeContextDead);
        KeReleaseSpinLock(&context->BindingLock, oldIrql);
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        return STATUS_DEVICE_NOT_READY;
    }

    owner->State = VioGpuNativeContextOwnerLive;
    KeAcquireSpinLock(&context->BindingLock, &oldIrql);
#if defined(VIOGPU_NATIVE_CONTEXT)
    context->VaStart = vaStart;
    context->VaSize = vaSize;
    context->SubmitQueueId = submitQueueId;
#endif
    context->Registered = TRUE;
    InterlockedExchange(&context->State, VioGpuNativeContextLive);
    KeReleaseSpinLock(&context->BindingLock, oldIrql);
    KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
    if (m_pVioGpuDod != NULL)
    {
        m_pVioGpuDod->RecordNativeContextCreateDiagnostic(VioGpuNativeContextCreateComplete, STATUS_SUCCESS, contextId);
    }
    return STATUS_SUCCESS;
}

__declspec(code_seg(".text")) NTSTATUS VioGpuAdapter::DestroyNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                                                           _Out_ BOOLEAN *released)
{
    PAGED_CODE();

    if (context == NULL || released == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *released = FALSE;

    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_NativeContextLifecycleMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        FailNativeContextAtAnyIrql();
        return status;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->BindingLock, &oldIrql);
    LONG objectState = InterlockedCompareExchange(&context->State, VioGpuNativeContextDead, VioGpuNativeContextDead);
    if (objectState == VioGpuNativeContextDead && context->Adapter == NULL && !context->Registered)
    {
        if (context->AllocationReferences != 0 || !IsListEmpty(&context->AllocationRanges))
        {
            KeReleaseSpinLock(&context->BindingLock, oldIrql);
            KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
            return STATUS_DEVICE_BUSY;
        }
        if (context->Owner != NULL || context->Generation != 0 || context->ResetGeneration != 0 ||
            context->ContextId != 0 || context->VaStart != 0 || context->VaSize != 0 || context->SubmitQueueId != 0)
        {
            KeReleaseSpinLock(&context->BindingLock, oldIrql);
            KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
            FailNativeContextAtAnyIrql();
            return STATUS_INVALID_DEVICE_STATE;
        }
        KeReleaseSpinLock(&context->BindingLock, oldIrql);
        *released = TRUE;
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        return STATUS_SUCCESS;
    }
    VIOGPU_NATIVE_CONTEXT_OWNER *owner = context->Owner;
    if (objectState != VioGpuNativeContextLive || context->Adapter != this || !context->Registered || owner == NULL ||
        owner->Registration != context || owner->State != VioGpuNativeContextOwnerLive ||
        owner->Generation != context->Generation || owner->ResetGeneration != context->ResetGeneration ||
        owner->ContextId != context->ContextId
#if defined(VIOGPU_NATIVE_CONTEXT)
        || !owner->ControlResourceCreated || !owner->ControlMapped || owner->ControlResourceId == 0 ||
        owner->ControlBlobSize != VIOGPU_NATIVE_CONTROL_BLOB_SIZE || owner->ControlAddress == NULL ||
        !owner->SubmitQueueCreated || owner->SubmitQueueId == 0 || context->SubmitQueueId != owner->SubmitQueueId ||
        context->VaStart == 0 || context->VaSize == 0
#endif
    )
    {
        KeReleaseSpinLock(&context->BindingLock, oldIrql);
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        return STATUS_INVALID_HANDLE;
    }
    if (ReadNativeAllocationCount(owner) != 0 || context->AllocationReferences != 0 ||
        !IsListEmpty(&context->AllocationRanges))
    {
        KeReleaseSpinLock(&context->BindingLock, oldIrql);
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        return STATUS_DEVICE_BUSY;
    }

    LONG contextGeneration = context->Generation;
    ULONGLONG contextResetGeneration = context->ResetGeneration;
#if !defined(VIOGPU_NATIVE_CONTEXT)
    UINT contextId = context->ContextId;
#endif
    InterlockedExchange(&context->State, VioGpuNativeContextDestroying);
    context->Registered = FALSE;
    owner->State = VioGpuNativeContextOwnerDestroying;
    KeReleaseSpinLock(&context->BindingLock, oldIrql);

    LONG generation = InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0);
    BOOLEAN current = contextGeneration == generation && contextResetGeneration != 0 &&
                      contextResetGeneration == (ULONGLONG)InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                                                        0,
                                                                                        0) &&
                      InterlockedCompareExchange(&m_NativeContextState,
                                                 VioGpuNativeContextOffline,
                                                 VioGpuNativeContextOffline) == VioGpuNativeContextReady &&
                      m_CtrlQueue.IsSynchronousRequestsHealthy();

    VIOGPU_HOST_CONTEXT_RESULT destroyResult = VioGpuHostContextUnknown;
    if (current)
    {
#if defined(VIOGPU_NATIVE_CONTEXT)
        destroyResult = DestroyNativeContextHostObjectsLocked(owner);
#else
        destroyResult = m_CtrlQueue.DestroyNativeContext(contextId);
#endif
    }
    if (destroyResult == VioGpuHostContextConfirmed || destroyResult == VioGpuHostContextRejected)
    {
        RetireNativeContextOwnerLocked(owner);
    }
    else
    {
        owner->Registration = NULL;
    }
    KeAcquireSpinLock(&context->BindingLock, &oldIrql);
    context->Adapter = NULL;
    context->Owner = NULL;
    context->Generation = 0;
    context->ResetGeneration = 0;
    context->ContextId = 0;
    context->VaStart = 0;
    context->VaSize = 0;
    context->SubmitQueueId = 0;
    InterlockedExchange(&context->State, VioGpuNativeContextDead);
    KeReleaseSpinLock(&context->BindingLock, oldIrql);
    *released = TRUE;

    if (destroyResult != VioGpuHostContextConfirmed && destroyResult != VioGpuHostContextRejected)
    {
        FailNativeContextAtAnyIrql();
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu context destroy: Host ownership uncertain; transport failed closed\n");
    }
    status = STATUS_SUCCESS;
    KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
    return status;
}

__declspec(code_seg(".text")) BOOLEAN VioGpuAdapter::AcquireNativeContextSnapshot(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                                                                  _Out_ VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot)
{
    if (context == NULL || snapshot == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }

    RtlZeroMemory(snapshot, sizeof(*snapshot));
    VioGpuAdapter *adapter = ReferenceNativeContextAdapter(context);
    if (adapter == NULL)
    {
        return FALSE;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&adapter->m_NativeContextLifecycleMutex,
                                            Executive,
                                            KernelMode,
                                            FALSE,
                                            &timeout);
    if (status != STATUS_SUCCESS)
    {
        adapter->FailNativeContextAtAnyIrql();
        DereferenceNativeContextAdapter(adapter);
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->BindingLock, &oldIrql);
    BOOLEAN acquired = context->Adapter == adapter && context->Owner != NULL && context->Registered &&
                       context->Generation != 0 && context->ResetGeneration != 0 && context->ContextId != 0 &&
#if defined(VIOGPU_NATIVE_CONTEXT)
                       context->SubmitQueueId != 0 && context->Owner->SubmitQueueCreated &&
                       context->Owner->SubmitQueueId == context->SubmitQueueId &&
#endif
                       InterlockedCompareExchange(&context->State,
                                                  VioGpuNativeContextDead,
                                                  VioGpuNativeContextDead) == VioGpuNativeContextLive;
    if (acquired)
    {
        snapshot->Adapter = adapter;
        snapshot->Owner = context->Owner;
        snapshot->Registration = context;
        snapshot->Generation = context->Generation;
        snapshot->ResetGeneration = context->ResetGeneration;
        snapshot->ContextId = context->ContextId;
        snapshot->VaStart = context->VaStart;
        snapshot->VaSize = context->VaSize;
        snapshot->SubmitQueueId = context->SubmitQueueId;
    }
    KeReleaseSpinLock(&context->BindingLock, oldIrql);

    if (acquired && !adapter->IsNativeContextGenerationCurrent(snapshot->Generation, snapshot->ResetGeneration))
    {
        RtlZeroMemory(snapshot, sizeof(*snapshot));
        acquired = FALSE;
    }
    if (!acquired)
    {
        KeReleaseMutex(&adapter->m_NativeContextLifecycleMutex, FALSE);
        DereferenceNativeContextAdapter(adapter);
    }
    return acquired;
}

__declspec(code_seg(".text")) BOOLEAN VioGpuAdapter::AcquireNativeContextSnapshotForAllocation(_In_ ULONGLONG requestedIova,
                                                                                               _In_ SIZE_T backingSize,
                                                                                               _In_ ULONGLONG expectedResetGeneration,
                                                                                               _In_ UINT expectedContextId,
                                                                                               _Out_ VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot)
{
    PAGED_CODE();

    if (snapshot == NULL || requestedIova == 0 || backingSize == 0 || expectedResetGeneration == 0 ||
        expectedContextId == 0 || (requestedIova & (PAGE_SIZE - 1)) != 0 || (backingSize & (PAGE_SIZE - 1)) != 0 ||
        requestedIova > MAXULONGLONG - ((ULONGLONG)backingSize - 1) || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }
    RtlZeroMemory(snapshot, sizeof(*snapshot));

    if (!ExAcquireRundownProtection(&m_NativeContextReferences))
    {
        return FALSE;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_NativeContextLifecycleMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        FailNativeContextAtAnyIrql();
        ExReleaseRundownProtection(&m_NativeContextReferences);
        return FALSE;
    }

    LONG generation = InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0);
    ULONGLONG resetGeneration = (ULONGLONG)InterlockedCompareExchange64(&m_NativeContextResetGeneration, 0, 0);
    BOOLEAN acquired = FALSE;
    BOOLEAN duplicate = FALSE;
    if (resetGeneration == expectedResetGeneration && IsNativeContextGenerationCurrent(generation, resetGeneration))
    {
        for (PLIST_ENTRY entry = m_NativeContextRegistry.Flink; entry != &m_NativeContextRegistry; entry = entry->Flink)
        {
            VIOGPU_NATIVE_CONTEXT_OWNER *owner = CONTAINING_RECORD(entry, VIOGPU_NATIVE_CONTEXT_OWNER, AdapterLink);
            VIOGPU_NATIVE_CONTEXT_REGISTRATION *context = owner->Registration;
            if (context == NULL || owner->State != VioGpuNativeContextOwnerLive || owner->Generation != generation ||
                owner->ResetGeneration != resetGeneration || owner->ContextId == 0 ||
                owner->ContextId != expectedContextId)
            {
                continue;
            }

            KIRQL oldIrql;
            KeAcquireSpinLock(&context->BindingLock, &oldIrql);
            BOOLEAN matches = context->Adapter == this && context->Owner == owner && context->Registered &&
                              context->Generation == generation && context->ResetGeneration == resetGeneration &&
                              context->ContextId == owner->ContextId && context->VaStart != 0 && context->VaSize != 0 &&
#if defined(VIOGPU_NATIVE_CONTEXT)
                              context->SubmitQueueId != 0 && owner->SubmitQueueCreated &&
                              owner->SubmitQueueId == context->SubmitQueueId &&
#endif
                              (ULONGLONG)backingSize <= context->VaSize && requestedIova >= context->VaStart &&
                              context->VaStart <= MAXULONGLONG - (context->VaSize - 1) &&
                              requestedIova <= context->VaStart + context->VaSize - (ULONGLONG)backingSize &&
                              InterlockedCompareExchange(&context->State,
                                                         VioGpuNativeContextDead,
                                                         VioGpuNativeContextDead) == VioGpuNativeContextLive;
            if (matches && !acquired)
            {
                snapshot->Adapter = this;
                snapshot->Owner = owner;
                snapshot->Registration = context;
                snapshot->Generation = generation;
                snapshot->ResetGeneration = resetGeneration;
                snapshot->ContextId = context->ContextId;
                snapshot->VaStart = context->VaStart;
                snapshot->VaSize = context->VaSize;
                snapshot->SubmitQueueId = context->SubmitQueueId;
                acquired = TRUE;
            }
            else if (matches)
            {
                duplicate = TRUE;
            }
            KeReleaseSpinLock(&context->BindingLock, oldIrql);
            if (duplicate)
            {
                break;
            }
        }
    }

    if (!acquired || duplicate)
    {
        RtlZeroMemory(snapshot, sizeof(*snapshot));
        KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
        ExReleaseRundownProtection(&m_NativeContextReferences);
        if (duplicate)
        {
            FailNativeContextAtAnyIrql();
        }
        return FALSE;
    }
    return TRUE;
}

#pragma code_seg(pop)

BOOLEAN VioGpuAdapter::ReferenceNativeContextAllocation(_In_ const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                                                        _Out_ VIOGPU_NATIVE_CONTEXT_REGISTRATION **registration)
{
    if (registration == NULL)
    {
        return FALSE;
    }
    *registration = NULL;
    if (snapshot == NULL || snapshot->Adapter == NULL || snapshot->Owner == NULL || snapshot->Registration == NULL ||
        snapshot->Generation <= 0 || snapshot->ResetGeneration == 0 || snapshot->ContextId == 0 ||
        snapshot->SubmitQueueId == 0)
    {
        return FALSE;
    }

    VIOGPU_NATIVE_CONTEXT_REGISTRATION *context = snapshot->Registration;
    KIRQL oldIrql;
    KeAcquireSpinLock(&context->BindingLock, &oldIrql);
    BOOLEAN referenced = context->Adapter == snapshot->Adapter && context->Owner == snapshot->Owner &&
                         snapshot->Owner->Registration == context && context->Registered &&
                         context->Generation == snapshot->Generation &&
                         context->ResetGeneration == snapshot->ResetGeneration &&
                         context->ContextId == snapshot->ContextId &&
                         context->SubmitQueueId == snapshot->SubmitQueueId &&
                         context->AllocationReferences != MAXULONG &&
                         InterlockedCompareExchange(&context->State,
                                                    VioGpuNativeContextDead,
                                                    VioGpuNativeContextDead) == VioGpuNativeContextLive;
    if (referenced)
    {
        ++context->AllocationReferences;
        *registration = context;
    }
    KeReleaseSpinLock(&context->BindingLock, oldIrql);
    return referenced;
}

BOOLEAN VioGpuAdapter::DereferenceNativeContextAllocation(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration)
{
    if (registration == NULL)
    {
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&registration->BindingLock, &oldIrql);
    BOOLEAN dereferenced = registration->AllocationReferences != 0;
    if (dereferenced)
    {
        --registration->AllocationReferences;
    }
    KeReleaseSpinLock(&registration->BindingLock, oldIrql);
    return dereferenced;
}

BOOLEAN VioGpuAdapter::IsNativeContextAllocationBindingRetired(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration)
{
    if (registration == NULL)
    {
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&registration->BindingLock, &oldIrql);
    BOOLEAN retired = registration->Adapter == NULL && registration->Owner == NULL && !registration->Registered &&
                      registration->Generation == 0 && registration->ResetGeneration == 0 &&
                      registration->ContextId == 0 && registration->VaStart == 0 && registration->VaSize == 0 &&
                      registration->SubmitQueueId == 0 &&
                      InterlockedCompareExchange(&registration->State,
                                                 VioGpuNativeContextDead,
                                                 VioGpuNativeContextDead) == VioGpuNativeContextDead;
    KeReleaseSpinLock(&registration->BindingLock, oldIrql);
    return retired;
}

void VioGpuAdapter::ReleaseNativeContextSnapshot(_Inout_ VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot)
{
    if (snapshot == NULL || snapshot->Adapter == NULL)
    {
        return;
    }
    VioGpuAdapter *adapter = snapshot->Adapter;
    RtlZeroMemory(snapshot, sizeof(*snapshot));
    KeReleaseMutex(&adapter->m_NativeContextLifecycleMutex, FALSE);
    DereferenceNativeContextAdapter(adapter);
}

VioGpuAdapter *VioGpuAdapter::ReferenceNativeContextAdapter(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context)
{
    if (context == NULL)
    {
        return NULL;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->BindingLock, &oldIrql);
    VioGpuAdapter *adapter = context->Adapter;
    if (adapter == NULL || context->Owner == NULL || !context->Registered || context->Generation == 0 ||
        context->ResetGeneration == 0 || context->ContextId == 0 ||
#if defined(VIOGPU_NATIVE_CONTEXT)
        context->SubmitQueueId == 0 || !context->Owner->SubmitQueueCreated ||
        context->Owner->SubmitQueueId != context->SubmitQueueId ||
#endif
        InterlockedCompareExchange(&context->State,
                                   VioGpuNativeContextDead,
                                   VioGpuNativeContextDead) != VioGpuNativeContextLive ||
        !ExAcquireRundownProtection(&adapter->m_NativeContextReferences))
    {
        adapter = NULL;
    }
    KeReleaseSpinLock(&context->BindingLock, oldIrql);
    return adapter;
}

void VioGpuAdapter::DereferenceNativeContextAdapter(_In_ VioGpuAdapter *adapter)
{
    ExReleaseRundownProtection(&adapter->m_NativeContextReferences);
}

BOOLEAN VioGpuAdapter::IsNativeContextReleased(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context)
{
    if (context == NULL)
    {
        return FALSE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->BindingLock, &oldIrql);
    BOOLEAN released = context->Adapter == NULL && context->Owner == NULL && !context->Registered &&
                       context->Generation == 0 && context->ResetGeneration == 0 && context->ContextId == 0 &&
                       context->VaStart == 0 && context->VaSize == 0 && context->SubmitQueueId == 0 &&
                       context->AllocationReferences == 0 && IsListEmpty(&context->AllocationRanges) &&
                       InterlockedCompareExchange(&context->State,
                                                  VioGpuNativeContextDead,
                                                  VioGpuNativeContextDead) == VioGpuNativeContextDead;
    KeReleaseSpinLock(&context->BindingLock, oldIrql);
    return released;
}

BOOLEAN VioGpuAdapter::IsNativeContextGenerationCurrent(_In_ LONG generation, _In_ ULONGLONG resetGeneration)
{
    return !m_pVioGpuDod->IsHardwareResetRequested() &&
           generation == InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0) && resetGeneration != 0 &&
           resetGeneration == (ULONGLONG)InterlockedCompareExchange64(&m_NativeContextResetGeneration, 0, 0) &&
           InterlockedCompareExchange(&m_NativeContextState,
                                      VioGpuNativeContextOffline,
                                      VioGpuNativeContextOffline) == VioGpuNativeContextReady &&
           m_CtrlQueue.IsSynchronousRequestsHealthy();
}

#pragma code_seg(push)
#pragma code_seg("PAGE")

__declspec(code_seg(".text")) void VioGpuAdapter::InvalidateNativeContextRegistrationsLocked(void)
{
    PAGED_CODE();

    for (PLIST_ENTRY entry = m_NativeContextRegistry.Flink; entry != &m_NativeContextRegistry; entry = entry->Flink)
    {
        VIOGPU_NATIVE_CONTEXT_OWNER *owner = CONTAINING_RECORD(entry, VIOGPU_NATIVE_CONTEXT_OWNER, AdapterLink);
        VIOGPU_NATIVE_CONTEXT_REGISTRATION *context = owner->Registration;
        if (context == NULL)
        {
            continue;
        }

        KIRQL oldIrql;
        KeAcquireSpinLock(&context->BindingLock, &oldIrql);
        if (context->Adapter == this && context->Owner == owner)
        {
            InterlockedExchange(&context->State, VioGpuNativeContextDestroying);
            context->Registered = FALSE;
            context->Adapter = NULL;
            context->Owner = NULL;
            context->Generation = 0;
            context->ResetGeneration = 0;
            context->ContextId = 0;
            context->VaStart = 0;
            context->VaSize = 0;
            context->SubmitQueueId = 0;
            InterlockedExchange(&context->State, VioGpuNativeContextDead);
        }
        KeReleaseSpinLock(&context->BindingLock, oldIrql);
        owner->Registration = NULL;
        owner->State = VioGpuNativeContextOwnerDestroying;
    }
}

NTSTATUS VioGpuAdapter::RetireAllNativeContextOwnersLocked(void)
{
    PAGED_CODE();

    while (!IsListEmpty(&m_NativeContextRegistry))
    {
        PLIST_ENTRY entry = m_NativeContextRegistry.Flink;
        VIOGPU_NATIVE_CONTEXT_OWNER *owner = CONTAINING_RECORD(entry, VIOGPU_NATIVE_CONTEXT_OWNER, AdapterLink);
#if defined(VIOGPU_NATIVE_CONTEXT)
        if (owner->ControlAddress != NULL)
        {
            NTSTATUS status = m_PciResources.UnmapHostVisibleAddress(owner->ControlAddress);
            if (!NT_SUCCESS(status))
            {
                return status;
            }
            owner->ControlAddress = NULL;
            owner->ControlBarOffset = 0;
        }
#endif
        RemoveEntryList(entry);
        InitializeListHead(&owner->AdapterLink);
        delete owner;
    }
    return STATUS_SUCCESS;
}

void VioGpuAdapter::Publish2DResetRetirementLocked(void)
{
    PAGED_CODE();

#if defined(VIOGPU_NATIVE_CONTEXT)
    ULONGLONG resetGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_NativeContextResetGeneration,
                                                                                    0,
                                                                                    0));
    ULONGLONG retiredGeneration = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_2DRetiredResetGeneration,
                                                                                      0,
                                                                                      0));
    if (resetGeneration == 0 || resetGeneration < retiredGeneration)
    {
        FailNativeContextAtAnyIrql();
        return;
    }
    InterlockedExchange64(&m_2DRetiredResetGeneration, static_cast<LONG64>(resetGeneration));

    NTSTATUS status = KeWaitForSingleObject(&m_2DScanoutMutex, Executive, KernelMode, FALSE, NULL);
    if (status == STATUS_SUCCESS)
    {
        Reconcile2DScanoutAfterResetLocked();
        KeReleaseMutex(&m_2DScanoutMutex, FALSE);
    }
    else
    {
        FailNativeContextAtAnyIrql();
    }
#endif
}

void VioGpuAdapter::RetireNativeContextOwnerLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner)
{
    PAGED_CODE();

    RemoveEntryList(&owner->AdapterLink);
    InitializeListHead(&owner->AdapterLink);
    delete owner;
}

__declspec(code_seg(".text")) NTSTATUS VioGpuAdapter::ProbeNativeContextReadiness(void)
{
    PAGED_CODE();

    ClearNativeContextReadiness();
    LONG generation = InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0);
    ULONGLONG resetGeneration = (ULONGLONG)InterlockedCompareExchange64(&m_NativeContextResetGeneration, 0, 0);

    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartCapsetFeatureState,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    DWORD missingFeatures = VioGpuNativeStartDetailNone;
    if (!virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_VIRGL))
    {
        missingFeatures |= VioGpuNativeStartDetailMissingVirgl;
    }
    if (!virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_RESOURCE_BLOB))
    {
        missingFeatures |= VioGpuNativeStartDetailMissingResourceBlob;
    }
    if (!virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_CONTEXT_INIT))
    {
        missingFeatures |= VioGpuNativeStartDetailMissingContextInit;
    }
    if (!virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_CREATE_GUEST_HANDLE))
    {
        missingFeatures |= VioGpuNativeStartDetailMissingGuestHandle;
    }
    if (missingFeatures != VioGpuNativeStartDetailNone)
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartCapsetFeatureState,
                                   STATUS_NOT_SUPPORTED,
                                   missingFeatures);
        return STATUS_NOT_SUPPORTED;
    }

    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartCapsetCount, STATUS_PENDING, m_u32NumCapsets);
    if (m_u32NumCapsets == 0 || m_u32NumCapsets > VIOGPU_MAX_CAPSETS)
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartCapsetCount, STATUS_NOT_SUPPORTED, m_u32NumCapsets);
        return STATUS_NOT_SUPPORTED;
    }

    GPU_RESP_CAPSET_INFO selectedInfo = {};
    BOOLEAN found = FALSE;
    for (UINT capsetIndex = 0; capsetIndex < m_u32NumCapsets; ++capsetIndex)
    {
        GPU_RESP_CAPSET_INFO info = {};
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartCapsetInfoQuery, STATUS_PENDING, capsetIndex);
        if (!m_CtrlQueue.QueryCapsetInfo(capsetIndex, &info))
        {
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                       VioGpuNativeStartCapsetInfoQuery,
                                       STATUS_DEVICE_NOT_READY,
                                       capsetIndex);
            return STATUS_DEVICE_NOT_READY;
        }
        if (info.capset_id != VIRTIO_GPU_CAPSET_DRM)
        {
            continue;
        }
        if (found)
        {
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                       VioGpuNativeStartCapsetInfoUnique,
                                       STATUS_NOT_SUPPORTED,
                                       capsetIndex);
            return STATUS_NOT_SUPPORTED;
        }
        selectedInfo = info;
        found = TRUE;
    }

    if (!found)
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartCapsetInfoUnique,
                                   STATUS_NOT_SUPPORTED,
                                   m_u32NumCapsets);
        return STATUS_NOT_SUPPORTED;
    }
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartCapsetInfoLayout,
                               STATUS_PENDING,
                               selectedInfo.capset_max_size);
    if (selectedInfo.capset_max_size < sizeof(GPU_CAPSET_DRM) ||
        selectedInfo.capset_max_size > PAGE_SIZE - sizeof(GPU_CTRL_HDR))
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartCapsetInfoLayout,
                                   STATUS_NOT_SUPPORTED,
                                   selectedInfo.capset_max_size);
        return STATUS_NOT_SUPPORTED;
    }

    GPU_CAPSET_DRM capset = {};
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartCapsetPayloadQuery,
                               STATUS_PENDING,
                               selectedInfo.capset_max_version);
    if (!m_CtrlQueue.QueryCapset(VIRTIO_GPU_CAPSET_DRM,
                                 selectedInfo.capset_max_version,
                                 selectedInfo.capset_max_size,
                                 &capset))
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartCapsetPayloadQuery,
                                   STATUS_DEVICE_NOT_READY,
                                   selectedInfo.capset_max_version);
        return STATUS_DEVICE_NOT_READY;
    }

    ULONGLONG vaEnd = capset.msm.va_start + capset.msm.va_size;
    DWORD invalidCapset = VioGpuNativeStartDetailNone;
    if (capset.wire_format_version != VIRTIO_GPU_DRM_WIRE_FORMAT_VERSION)
    {
        invalidCapset |= VioGpuNativeStartDetailInvalidWireVersion;
    }
    if (capset.context_type != VIRTIO_GPU_DRM_CONTEXT_MSM)
    {
        invalidCapset |= VioGpuNativeStartDetailInvalidContextType;
    }
    if (capset.padding != 0)
    {
        invalidCapset |= VioGpuNativeStartDetailInvalidPadding;
    }
    if (capset.version_major != 1 || capset.version_minor < VIOGPU_MINIMUM_MSM_VERSION_MINOR)
    {
        invalidCapset |= VioGpuNativeStartDetailInvalidMsmVersion;
    }
    if (capset.msm.priorities == 0)
    {
        invalidCapset |= VioGpuNativeStartDetailInvalidPriorities;
    }
    if (capset.msm.va_start == 0 || (capset.msm.va_start & (PAGE_SIZE - 1)) != 0)
    {
        invalidCapset |= VioGpuNativeStartDetailInvalidVaStart;
    }
    if (capset.msm.va_size == 0 || (capset.msm.va_size & (PAGE_SIZE - 1)) != 0)
    {
        invalidCapset |= VioGpuNativeStartDetailInvalidVaSize;
    }
    if (vaEnd < capset.msm.va_start)
    {
        invalidCapset |= VioGpuNativeStartDetailInvalidVaRange;
    }
    if (invalidCapset != VioGpuNativeStartDetailNone)
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartCapsetPayloadValidation,
                                   STATUS_NOT_SUPPORTED,
                                   invalidCapset);
        return STATUS_NOT_SUPPORTED;
    }
    if (!m_CtrlQueue.IsSynchronousRequestsHealthy())
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartCapsetPublish,
                                   STATUS_DEVICE_NOT_READY,
                                   VioGpuNativeStartDetailNone);
        return STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartCapsetPublish,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeContextReadinessLock, &oldIrql);
    if (!m_CtrlQueue.IsSynchronousRequestsHealthy() ||
        InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0) != generation ||
        (ULONGLONG)InterlockedCompareExchange64(&m_NativeContextResetGeneration, 0, 0) != resetGeneration ||
        resetGeneration == 0 ||
        InterlockedCompareExchange(&m_NativeContextState,
                                   VioGpuNativeContextStarting,
                                   VioGpuNativeContextStarting) != VioGpuNativeContextStarting)
    {
        KeReleaseSpinLock(&m_NativeContextReadinessLock, oldIrql);
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartCapsetPublish,
                                   STATUS_DEVICE_NOT_READY,
                                   VioGpuNativeStartDetailNone);
        return STATUS_DEVICE_NOT_READY;
    }
    m_NativeContextReadiness.Generation = generation;
    m_NativeContextReadiness.ResetGeneration = resetGeneration;
    m_NativeContextReadiness.CapsetVersion = selectedInfo.capset_max_version;
    m_NativeContextReadiness.CapsetSize = selectedInfo.capset_max_size;
    m_NativeContextReadiness.Capset = capset;
    m_NativeContextReadiness.Ready = TRUE;
    KeReleaseSpinLock(&m_NativeContextReadinessLock, oldIrql);
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartCapsetPublish,
                               STATUS_SUCCESS,
                               VioGpuNativeStartDetailNone);

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu native context: capset 6 ready, wire=%u msm=%u.%u va=0x%I64X+0x%I64X\n",
               capset.wire_format_version,
               capset.version_major,
               capset.version_minor,
               capset.msm.va_start,
               capset.msm.va_size);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::VirtIoDeviceInit()
{
    PAGED_CODE();

    return virtio_device_initialize(&m_VioDev,
                                    &VioGpuSystemOps,
                                    reinterpret_cast<IVioGpuPCI *>(this),
                                    m_PciResources.IsMSIEnabled());
}

NTSTATUS VioGpuAdapter::StartNativeContextTransport(DXGK_DISPLAY_INFORMATION *pDispInfo)
{
    PAGED_CODE();

    if (pDispInfo == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartBeginInitialization,
                                   STATUS_INVALID_PARAMETER,
                                   static_cast<DWORD>(KeGetCurrentIrql()));
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status = STATUS_SUCCESS;

    status = VioGpuAdapterInit(pDispInfo);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

#if defined(VIOGPU_NATIVE_CONTEXT)
    UINT hostVisibleBar = 0;
    ULONGLONG hostVisibleOffset = 0;
    ULONGLONG hostVisibleSize = 0;
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartHostVisibleRegion,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    BOOLEAN hasHostVisibleRegion = m_PciResources.QueryHostVisibleRegion(&hostVisibleBar,
                                                                         &hostVisibleOffset,
                                                                         &hostVisibleSize);
    if (!hasHostVisibleRegion || hostVisibleSize < VIOGPU_NATIVE_CONTROL_BLOB_SIZE)
    {
        DWORD detail = hasHostVisibleRegion ? static_cast<DWORD>(min(hostVisibleSize, static_cast<ULONGLONG>(MAXULONG)))
                                            : VioGpuNativeStartDetailNone;
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartHostVisibleRegion, STATUS_DEVICE_NOT_READY, detail);
        return STATUS_DEVICE_NOT_READY;
    }
#endif

    UINT allocation = m_CtrlQueue.QueryAllocation() + m_CursorQueue.QueryAllocation();
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartQueueBuffer, STATUS_PENDING, allocation);
    if (allocation == 0 || !m_GpuBuf.Init(allocation))
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartQueueBuffer,
                                   STATUS_INSUFFICIENT_RESOURCES,
                                   allocation);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m_CtrlQueue.SetGpuBuf(&m_GpuBuf);
    m_CursorQueue.SetGpuBuf(&m_GpuBuf);

    BOOLEAN initializeResourceIds = TRUE;
#if defined(VIOGPU_NATIVE_CONTEXT)
    initializeResourceIds = !m_2DResourceIdsInitialized;
#endif
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartResourceIds, STATUS_PENDING, initializeResourceIds);
    if (initializeResourceIds && !m_Idr.Init(1, VIOGPU_NATIVE_RESOURCE_ID_START))
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartResourceIds,
                                   STATUS_INSUFFICIENT_RESOURCES,
                                   initializeResourceIds);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
#if defined(VIOGPU_NATIVE_CONTEXT)
    m_2DResourceIdsInitialized = TRUE;
#endif

    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartQueueInterrupts,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    if (!m_CtrlQueue.EnableInterrupt() || !m_CursorQueue.EnableInterrupt())
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartQueueInterrupts,
                                   STATUS_DEVICE_NOT_READY,
                                   VioGpuNativeStartDetailNone);
        return STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartDriverReady, STATUS_PENDING, VioGpuNativeStartDetailNone);
    virtio_device_ready(&m_VioDev);
    if ((virtio_get_status(&m_VioDev) & VIRTIO_CONFIG_S_DRIVER_OK) == 0)
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartDriverReady,
                                   STATUS_DEVICE_NOT_READY,
                                   virtio_get_status(&m_VioDev));
        return STATUS_DEVICE_NOT_READY;
    }
    m_pVioGpuDod->SetHardwareInit(TRUE);
    InterlockedExchange(&m_InterruptDispatchEnabled, TRUE);

    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartSynchronousRequests,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    if (!m_CtrlQueue.EnableSynchronousRequests())
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartSynchronousRequests,
                                   STATUS_DEVICE_NOT_READY,
                                   VioGpuNativeStartDetailNone);
        return STATUS_DEVICE_NOT_READY;
    }
    status = ProbeNativeContextReadiness();
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartModeList, STATUS_PENDING, VioGpuNativeStartDetailNone);
    status = BuildModeList(pDispInfo);
    if (NT_SUCCESS(status) && !m_CtrlQueue.IsSynchronousRequestsHealthy())
    {
        status = STATUS_IO_TIMEOUT;
    }
    if (!NT_SUCCESS(status))
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartModeList, status, m_ModeCount);
        return status;
    }

    PHYSICAL_ADDRESS fbPa = m_PciResources.GetPciBar(0)->GetPA();
    UINT fbSize = (UINT)m_PciResources.GetPciBar(0)->GetSize();
    UINT reqSize = pDispInfo->Pitch * pDispInfo->Height;
    reqSize = max(reqSize, 0x1000000U);

    UINT maxResSize = MIN_WIDTH_SIZE * MIN_HEIGHT_SIZE;
    for (UINT idx = 0; idx < m_ModeCount; ++idx)
    {
        UINT resSize = m_ModeInfo[idx].VisScreenWidth * m_ModeInfo[idx].VisScreenHeight;
        maxResSize = max(maxResSize, resSize);
    }
    reqSize = max(reqSize, maxResSize * 4U);

    if (fbPa.QuadPart != 0)
    {
        pDispInfo->PhysicAddress = fbPa;
    }
    if (fbSize < reqSize)
    {
        m_pVioGpuDod->SetUsePhysicalMemory(FALSE);
    }
    if (!m_pVioGpuDod->IsUsePhysicalMemory() || fbPa.QuadPart == 0 || fbSize < reqSize)
    {
        fbPa.QuadPart = 0;
        fbSize = max(reqSize, fbSize);
    }

    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartFrameSegment, STATUS_PENDING, fbSize);
    if (!m_FrameSegment.Init(fbSize, &fbPa))
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartFrameSegment, STATUS_INSUFFICIENT_RESOURCES, fbSize);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartCursorSegment,
                               STATUS_PENDING,
                               POINTER_SIZE * POINTER_SIZE * 4);
    if (!m_CursorSegment.Init(POINTER_SIZE * POINTER_SIZE * 4, NULL))
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartCursorSegment,
                                   STATUS_INSUFFICIENT_RESOURCES,
                                   POINTER_SIZE * POINTER_SIZE * 4);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

PBYTE VioGpuAdapter::GetEdidData()
{
    PAGED_CODE();

    return m_bEDID ? m_EDIDs : (PBYTE)(g_gpu_edid);
}

PBYTE VioGpuAdapter::GetCTA861Data(void)
{
    PAGED_CODE();

    if (m_bEDID)
    {
        PEDID_DATA_V1 edid_data = (PEDID_DATA_V1)m_EDIDs;
        if (edid_data->ExtensionFlag[0])
        {
            PEDID_CTA_861 cta_data = (PEDID_CTA_861)(m_EDIDs + EDID_V1_BLOCK_SIZE);
            if (cta_data->ExtentionTag[0] >= 2 && cta_data->Revision[0] >= 3)
            {
                return (PBYTE)cta_data;
            }
        }
    }
    return NULL;
}

VOID VioGpuAdapter::CreateResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEvent != NULL && m_ResolutionEventHandle != NULL)
    {
        return;
    }
    DECLARE_UNICODE_STRING_SIZE(DeviceNumber, 10);
    DECLARE_UNICODE_STRING_SIZE(EventName, 256);

    RtlIntegerToUnicodeString(m_Id, 10, &DeviceNumber);
    NTSTATUS status = RtlUnicodeStringPrintf(&EventName,
                                             L"%ws%ws%ws",
                                             BASE_NAMED_OBJECTS,
                                             RESOLUTION_EVENT_NAME,
                                             DeviceNumber.Buffer);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("RtlUnicodeStringPrintf failed 0x%x\n", status));
        return;
    }
    m_ResolutionEvent = IoCreateNotificationEvent(&EventName, &m_ResolutionEventHandle);
    if (m_ResolutionEvent == NULL)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s\n", __FUNCTION__));
        return;
    }
    KeClearEvent(m_ResolutionEvent);
    ObReferenceObject(m_ResolutionEvent);
}

VOID VioGpuAdapter::NotifyResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEvent != NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("NotifyResolutionEvent\n"));
        KeSetEvent(m_ResolutionEvent, IO_NO_INCREMENT, FALSE);
        KeClearEvent(m_ResolutionEvent);
    }
}

VOID VioGpuAdapter::CloseResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEventHandle != NULL)
    {
        ZwClose(m_ResolutionEventHandle);
        m_ResolutionEventHandle = NULL;
    }

    if (m_ResolutionEvent != NULL)
    {
        ObDereferenceObject(m_ResolutionEvent);
        m_ResolutionEvent = NULL;
    }
}

NTSTATUS VioGpuAdapter::HWInit(PCM_RESOURCE_LIST pResList, DXGK_DISPLAY_INFORMATION *pDispInfo)
{
    PAGED_CODE();

    NTSTATUS status;
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartBeginInitialization,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    if (!BeginNativeContextInitialization())
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartBeginInitialization,
                                   STATUS_DEVICE_NOT_READY,
                                   VioGpuNativeStartDetailNone);
        return STATUS_DEVICE_NOT_READY;
    }
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));

    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                               VioGpuNativeStartPciResources,
                               STATUS_PENDING,
                               VioGpuNativeStartDetailNone);
    status = m_PciResources.Init(GetVioGpu()->GetDxgkInterface(), pResList) ? STATUS_SUCCESS
                                                                            : STATUS_INSUFFICIENT_RESOURCES;
    if (!NT_SUCCESS(status))
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartPciResources, status, VioGpuNativeStartDetailNone);
    }

    if (NT_SUCCESS(status))
    {
        status = StartNativeContextTransport(pDispInfo);
    }
    if (NT_SUCCESS(status))
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartWorkThread,
                                   STATUS_PENDING,
                                   VioGpuNativeStartDetailNone);
        status = StartWorkThread();
        if (!NT_SUCCESS(status))
        {
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartWorkThread, status, VioGpuNativeStartDetailNone);
        }
    }
    if (NT_SUCCESS(status))
    {
        VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                   VioGpuNativeStartCompleteInitialization,
                                   STATUS_PENDING,
                                   VioGpuNativeStartDetailNone);
        if (!CompleteNativeContextInitialization())
        {
            status = STATUS_DEVICE_NOT_READY;
            VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,
                                       VioGpuNativeStartCompleteInitialization,
                                       status,
                                       VioGpuNativeStartDetailNone);
        }
    }
    if (!NT_SUCCESS(status))
    {
        return FailNativeContextInitialization(status);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "viogpu HWInit: completed, modes=%u status=0x%08X\n",
               m_ModeCount,
               status);
    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod, VioGpuNativeStartCompleteInitialization, STATUS_SUCCESS, m_ModeCount);
    return status;
}

NTSTATUS VioGpuAdapter::StartWorkThread(void)
{
    PAGED_CODE();

    if (m_pWorkThread != NULL || m_WorkThreadHandle != NULL)
    {
        return m_bStopWorkThread ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
    }
    if (!m_CtrlQueue.IsSynchronousRequestsHealthy())
    {
        return STATUS_DEVICE_NOT_READY;
    }

    m_bStopWorkThread = FALSE;
    KeClearEvent(&m_ConfigUpdateEvent);

    HANDLE threadHandle = NULL;
    NTSTATUS status = PsCreateSystemThread(&threadHandle,
                                           SYNCHRONIZE,
                                           NULL,
                                           (HANDLE)0,
                                           NULL,
                                           VioGpuAdapter::ThreadWork,
                                           this);
    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu StartWorkThread: PsCreateSystemThread failed, status=0x%08X\n",
                   status);
        return status;
    }
    m_WorkThreadHandle = threadHandle;

    PETHREAD workThread = NULL;
    status = ObReferenceObjectByHandle(threadHandle,
                                       SYNCHRONIZE,
                                       *PsThreadType,
                                       KernelMode,
                                       reinterpret_cast<PVOID *>(&workThread),
                                       NULL);
    if (!NT_SUCCESS(status))
    {
        m_bStopWorkThread = TRUE;
        KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu StartWorkThread: retaining worker handle after reference failure, status=0x%08X\n",
                   status);
        return status;
    }

    m_pWorkThread = workThread;
    ZwClose(m_WorkThreadHandle);
    m_WorkThreadHandle = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::StopWorkThread(void)
{
    PAGED_CODE();

    if (m_pWorkThread == NULL && m_WorkThreadHandle == NULL)
    {
        m_bStopWorkThread = FALSE;
        return STATUS_SUCCESS;
    }

    m_bStopWorkThread = TRUE;
    KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    NTSTATUS status;
    if (m_pWorkThread == NULL)
    {
        PETHREAD workThread = NULL;
        status = ObReferenceObjectByHandle(m_WorkThreadHandle,
                                           SYNCHRONIZE,
                                           *PsThreadType,
                                           KernelMode,
                                           reinterpret_cast<PVOID *>(&workThread),
                                           NULL);
        if (!NT_SUCCESS(status))
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu StopWorkThread: thread reference failed, status=0x%08X\n",
                       status);
            return status;
        }
        m_pWorkThread = workThread;
    }
    status = KeWaitForSingleObject(m_pWorkThread, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "viogpu StopWorkThread: worker did not exit, status=0x%08X\n",
                   status);
        return status;
    }
    if (m_pWorkThread != NULL)
    {
        ObDereferenceObject(m_pWorkThread);
        m_pWorkThread = NULL;
    }
    if (m_WorkThreadHandle != NULL)
    {
        ZwClose(m_WorkThreadHandle);
        m_WorkThreadHandle = NULL;
    }
    m_bStopWorkThread = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::HWClose(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));

    NTSTATUS status = StopNativeContextTransport();
    if (NT_SUCCESS(status))
    {
        InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
        status = SynchronizeInterruptMessages();
    }
    if (NT_SUCCESS(status))
    {
        KeFlushQueuedDpcs();
        status = m_PciResources.Close();
    }
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s status=0x%08X\n", __FUNCTION__, status));
    return status;
}

BOOLEAN FindUpdateRect(_In_ ULONG NumMoves,
                       _In_ D3DKMT_MOVE_RECT *pMoves,
                       _In_ ULONG NumDirtyRects,
                       _In_ PRECT pDirtyRect,
                       _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                       _Out_ PRECT pUpdateRect)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Rotation);
    BOOLEAN updated = FALSE;

    if (pUpdateRect == NULL)
    {
        return FALSE;
    }

    if (NumMoves == 0 && NumDirtyRects == 0)
    {
        pUpdateRect->bottom = 0;
        pUpdateRect->left = 0;
        pUpdateRect->right = 0;
        pUpdateRect->top = 0;
    }

    for (ULONG i = 0; i < NumMoves; i++)
    {
        PRECT pRect = &pMoves[i].DestRect;
        if (!updated)
        {
            *pUpdateRect = *pRect;
            updated = TRUE;
        }
        else
        {
            pUpdateRect->bottom = max(pRect->bottom, pUpdateRect->bottom);
            pUpdateRect->left = min(pRect->left, pUpdateRect->left);
            pUpdateRect->right = max(pRect->right, pUpdateRect->right);
            pUpdateRect->top = min(pRect->top, pUpdateRect->top);
        }
    }
    for (ULONG i = 0; i < NumDirtyRects; i++)
    {
        PRECT pRect = &pDirtyRect[i];
        if (!updated)
        {
            *pUpdateRect = *pRect;
            updated = TRUE;
        }
        else
        {
            pUpdateRect->bottom = max(pRect->bottom, pUpdateRect->bottom);
            pUpdateRect->left = min(pRect->left, pUpdateRect->left);
            pUpdateRect->right = max(pRect->right, pUpdateRect->right);
            pUpdateRect->top = min(pRect->top, pUpdateRect->top);
        }
    }
    if (Rotation == D3DKMDT_VPPR_ROTATE90 || Rotation == D3DKMDT_VPPR_ROTATE270)
    {
    }
    return updated;
}

NTSTATUS VioGpuAdapter::ExecutePresentDisplayOnly(_In_ BYTE *DstAddr,
                                                  _In_ UINT DstBitPerPixel,
                                                  _In_ BYTE *SrcAddr,
                                                  _In_ UINT SrcBytesPerPixel,
                                                  _In_ LONG SrcPitch,
                                                  _In_ ULONG NumMoves,
                                                  _In_ D3DKMT_MOVE_RECT *pMoves,
                                                  _In_ ULONG NumDirtyRects,
                                                  _In_ RECT *pDirtyRect,
                                                  _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                                                  _In_ const CURRENT_MODE *pModeCur)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    BLT_INFO SrcBltInfo = {0};
    BLT_INFO DstBltInfo = {0};
    UINT resid = 0;
    RECT updrect = {0};
    ULONG offset = 0UL;

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("SrcBytesPerPixel = %d DstBitPerPixel = %d (%dx%d)\n",
              SrcBytesPerPixel,
              DstBitPerPixel,
              pModeCur->SrcModeWidth,
              pModeCur->SrcModeHeight));

    DstBltInfo.pBits = DstAddr;
    DstBltInfo.Pitch = pModeCur->DispInfo.Pitch;
    DstBltInfo.BitsPerPel = DstBitPerPixel;
    DstBltInfo.Offset.x = 0;
    DstBltInfo.Offset.y = 0;
    DstBltInfo.Rotation = Rotation;
    DstBltInfo.Width = pModeCur->SrcModeWidth;
    DstBltInfo.Height = pModeCur->SrcModeHeight;

    SrcBltInfo.pBits = SrcAddr;
    SrcBltInfo.Pitch = SrcPitch;
    SrcBltInfo.BitsPerPel = SrcBytesPerPixel * BITS_PER_BYTE;
    SrcBltInfo.Offset.x = 0;
    SrcBltInfo.Offset.y = 0;
    SrcBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
    if (Rotation == D3DKMDT_VPPR_ROTATE90 || Rotation == D3DKMDT_VPPR_ROTATE270)
    {
        SrcBltInfo.Width = DstBltInfo.Height;
        SrcBltInfo.Height = DstBltInfo.Width;
    }
    else
    {
        SrcBltInfo.Width = DstBltInfo.Width;
        SrcBltInfo.Height = DstBltInfo.Height;
    }

    for (UINT i = 0; i < NumMoves; i++)
    {
        RECT *pDestRect = &pMoves[i].DestRect;
        BltBits(&DstBltInfo, &SrcBltInfo, pDestRect);
    }

    for (UINT i = 0; i < NumDirtyRects; i++)
    {
        RECT *pRect = &pDirtyRect[i];
        BltBits(&DstBltInfo, &SrcBltInfo, pRect);
    }
    if (!FindUpdateRect(NumMoves, pMoves, NumDirtyRects, pDirtyRect, Rotation, &updrect))
    {
        updrect.top = 0;
        updrect.left = 0;
        updrect.bottom = pModeCur->SrcModeHeight;
        updrect.right = pModeCur->SrcModeWidth;
    }
    // FIXME!!! rotation
    offset = (updrect.top * pModeCur->DispInfo.Pitch) +
             (updrect.left * ((DstBitPerPixel + BITS_PER_BYTE - 1) / BITS_PER_BYTE));

    resid = m_pFrameBuf->GetId();
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("offset = %lu (XxYxWxH) (%dx%dx%dx%d) vs (%dx%dx%dx%d)\n",
              offset,
              updrect.left,
              updrect.top,
              updrect.right - updrect.left,
              updrect.bottom - updrect.top,
              0,
              0,
              pModeCur->SrcModeWidth,
              pModeCur->SrcModeHeight));

    if (!m_CtrlQueue.TransferToHost2D(resid,
                                      offset,
                                      updrect.right - updrect.left,
                                      updrect.bottom - updrect.top,
                                      updrect.left,
                                      updrect.top))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed to queue framebuffer transfer\n", __FUNCTION__));
        return STATUS_DEVICE_NOT_READY;
    }
    if (!m_CtrlQueue.ResFlush(resid,
                              updrect.right - updrect.left,
                              updrect.bottom - updrect.top,
                              updrect.left,
                              updrect.top))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed to queue framebuffer flush\n", __FUNCTION__));
        return STATUS_DEVICE_NOT_READY;
    }

    return STATUS_SUCCESS;
}

VOID VioGpuAdapter::BlackOutScreen(CURRENT_MODE *pCurrentMod)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));

    if (pCurrentMod->Flags.FrameBufferIsActive)
    {
        UINT ScreenHeight = pCurrentMod->DispInfo.Height;
        UINT ScreenPitch = pCurrentMod->DispInfo.Pitch;
        BYTE *pDst = (BYTE *)pCurrentMod->FrameBuffer;

        UINT resid = 0;

        if (pDst)
        {
            RtlZeroMemory(pDst, (ULONGLONG)ScreenHeight * ScreenPitch);
        }

        // FIXME!!! rotation

        resid = m_pFrameBuf->GetId();

        m_CtrlQueue.TransferToHost2D(resid, 0UL, pCurrentMod->DispInfo.Width, pCurrentMod->DispInfo.Height, 0, 0);
        m_CtrlQueue.ResFlush(resid, pCurrentMod->DispInfo.Width, pCurrentMod->DispInfo.Height, 0, 0);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

NTSTATUS VioGpuAdapter::SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape,
                                        _In_ CONST CURRENT_MODE *pModeCur)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--> %s flag = %d pitch = %d, pixels = %p, id = %d, w = %d, h = %d, x = %d, y = %d\n",
              __FUNCTION__,
              pSetPointerShape->Flags.Value,
              pSetPointerShape->Pitch,
              pSetPointerShape->pPixels,
              pSetPointerShape->VidPnSourceId,
              pSetPointerShape->Width,
              pSetPointerShape->Height,
              pSetPointerShape->XHot,
              pSetPointerShape->YHot));

    if (UpdateCursor(pSetPointerShape, pModeCur))
    {
        PGPU_UPDATE_CURSOR crsr;
        PGPU_VBUFFER vbuf;
        UINT ret = 0;
        crsr = (PGPU_UPDATE_CURSOR)m_CursorQueue.AllocCursor(&vbuf);
        RtlZeroMemory(crsr, sizeof(*crsr));

        crsr->hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
        crsr->resource_id = m_pCursorBuf->GetId();
        crsr->pos.x = 0;
        crsr->pos.y = 0;
        crsr->hot_x = pSetPointerShape->XHot;
        crsr->hot_y = pSetPointerShape->YHot;
        ret = m_CursorQueue.QueueCursor(vbuf);
        DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s vbuf = %p, ret = %d\n", __FUNCTION__, vbuf, ret));
        if (ret == 0)
        {
            return STATUS_SUCCESS;
        }
        VioGpuDbgBreak();
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s Failed to create cursor\n", __FUNCTION__));
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS VioGpuAdapter::SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition,
                                           _In_ CONST CURRENT_MODE *pModeCur)
{
    PAGED_CODE();
    if (m_pCursorBuf != NULL)
    {
        PGPU_UPDATE_CURSOR crsr;
        PGPU_VBUFFER vbuf;
        UINT ret = 0;
        crsr = (PGPU_UPDATE_CURSOR)m_CursorQueue.AllocCursor(&vbuf);
        RtlZeroMemory(crsr, sizeof(*crsr));

        crsr->hdr.type = VIRTIO_GPU_CMD_MOVE_CURSOR;
        crsr->resource_id = m_pCursorBuf->GetId();

        if (!pSetPointerPosition->Flags.Visible || (UINT)pSetPointerPosition->X > pModeCur->SrcModeWidth ||
            (UINT)pSetPointerPosition->Y > pModeCur->SrcModeHeight || pSetPointerPosition->X < 0 ||
            pSetPointerPosition->Y < 0)
        {
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("---> %s (%d - %d) Visiable = %d Value = %x VidPnSourceId = %d\n",
                      __FUNCTION__,
                      pSetPointerPosition->X,
                      pSetPointerPosition->Y,
                      pSetPointerPosition->Flags.Visible,
                      pSetPointerPosition->Flags.Value,
                      pSetPointerPosition->VidPnSourceId));
            crsr->pos.x = 0;
            crsr->pos.y = 0;
        }
        else
        {
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("---> %s (%d - %d) Visiable = %d Value = %x VidPnSourceId = %d posX = %d, psY = %d\n",
                      __FUNCTION__,
                      pSetPointerPosition->X,
                      pSetPointerPosition->Y,
                      pSetPointerPosition->Flags.Visible,
                      pSetPointerPosition->Flags.Value,
                      pSetPointerPosition->VidPnSourceId,
                      pSetPointerPosition->X,
                      pSetPointerPosition->Y));
            crsr->pos.x = pSetPointerPosition->X;
            crsr->pos.y = pSetPointerPosition->Y;
        }
        ret = m_CursorQueue.QueueCursor(vbuf);
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s vbuf = %p, ret = %d\n", __FUNCTION__, vbuf, ret));
        if (ret == 0)
        {
            return STATUS_SUCCESS;
        }
        VioGpuDbgBreak();
    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS VioGpuAdapter::Escape(_In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();
    PVIOGPU_ESCAPE pVioGpuEscape = (PVIOGPU_ESCAPE)pEscape->pPrivateDriverData;
    NTSTATUS status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UINT size = pEscape->PrivateDriverDataSize;
    if (size < sizeof(PVIOGPU_ESCAPE))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s buffer too small %d, should be at least %d\n",
                  __FUNCTION__,
                  pEscape->PrivateDriverDataSize,
                  size));
        return STATUS_INVALID_BUFFER_SIZE;
    }

    switch (pVioGpuEscape->Type)
    {
        case VIOGPU_GET_DEVICE_ID:
            {
                CreateResolutionEvent();
                size = sizeof(ULONG);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                pVioGpuEscape->Id = m_Id;
                break;
            }
        case VIOGPU_GET_CUSTOM_RESOLUTION:
            {
                size = sizeof(VIOGPU_DISP_MODE);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                pVioGpuEscape->Resolution.XResolution = (USHORT)m_ModeInfo[m_CustomModeIndex].VisScreenWidth;
                pVioGpuEscape->Resolution.YResolution = (USHORT)m_ModeInfo[m_CustomModeIndex].VisScreenHeight;
                break;
            }
        case VIOGPU_SET_CUSTOM_RESOLUTION:
            {
                size = sizeof(VIOGPU_DISP_MODE);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                if (pVioGpuEscape->Resolution.XResolution <= 0 || pVioGpuEscape->Resolution.YResolution <= 0)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s PersistentDispMode0 width %d and height %d should be > 0\n",
                              __FUNCTION__,
                              pVioGpuEscape->Resolution.XResolution,
                              pVioGpuEscape->Resolution.YResolution));
                    return STATUS_INVALID_PARAMETER;
                }

                DbgPrint(TRACE_LEVEL_INFORMATION,
                         ("%s PersistentDispMode0 width %d, height %d\n",
                          __FUNCTION__,
                          pVioGpuEscape->Resolution.XResolution,
                          pVioGpuEscape->Resolution.YResolution));

                m_pVioGpuDod->SetPersistentDispMode0Width(pVioGpuEscape->Resolution.XResolution);
                m_pVioGpuDod->SetPersistentDispMode0Height(pVioGpuEscape->Resolution.YResolution);
                m_pVioGpuDod->SetRegisterConfigInfo();
                SetCustomDisplay(pVioGpuEscape->Resolution.XResolution, pVioGpuEscape->Resolution.YResolution);
                SetCurrentModeIndex(m_CustomModeIndex);
                break;
            }
        default:
            DbgPrint(TRACE_LEVEL_ERROR, ("%s: invalid Escape type 0x%x\n", __FUNCTION__, pVioGpuEscape->Type));
            status = STATUS_INVALID_PARAMETER;
    }

    return status;
}

BOOLEAN VioGpuAdapter::GetDisplayInfo(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    ULONG xres = 0;
    ULONG yres = 0;
    for (UINT32 i = 0; i < m_u32NumScanouts; i++)
    {
        BOOLEAN success = m_CtrlQueue.QueryDisplayInfo(i, &xres, &yres);
        if (success)
        {
            if (xres && yres)
            {
                DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s (%dx%d)\n", __FUNCTION__, xres, yres));
                SetCustomDisplay((USHORT)xres, (USHORT)yres);
                SetCurrentModeIndex(m_CustomModeIndex);
                return TRUE;
            }
        }
    }
    xres = NOM_WIDTH_SIZE;
    yres = NOM_HEIGHT_SIZE;
    SetCustomDisplay((USHORT)xres, (USHORT)yres);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return FALSE;
}

int VioGpuAdapter::ProcessEdid(void)
{
    PAGED_CODE();

    if (virtio_is_feature_enabled(m_u64HostFeatures, VIRTIO_GPU_F_EDID))
    {
        GetEdids();
    }
    else
    {
        FixEdid();
    }

    return AddEdidModes();
}

void VioGpuAdapter::FixEdid(void)
{
    PAGED_CODE();

    UCHAR Sum = 0;
    PUCHAR buf = GetEdidData();
    ;
    PEDID_DATA_V1 pdata = (PEDID_DATA_V1)buf;
    pdata->MaximumHorizontalImageSize[0] = 0;
    pdata->MaximumVerticallImageSize[0] = 0;
    pdata->ExtensionFlag[0] = 0;
    pdata->Checksum[0] = 0;
    for (ULONG i = 0; i < EDID_V1_BLOCK_SIZE; i++)
    {
        Sum += buf[i];
    }
    pdata->Checksum[0] = -Sum;
}

BOOLEAN VioGpuAdapter::GetEdids(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    for (UINT32 i = 0; i < m_u32NumScanouts; i++)
    {
        if (m_CtrlQueue.QueryEdidInfo(i, m_EDIDs))
        {
            m_bEDID = TRUE;
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

BOOLEAN VioGpuAdapter::UpdateModes(USHORT xres, USHORT yres, int &cnt)
{
    int idx = 0;

    DbgPrint(TRACE_LEVEL_INFORMATION, (" x_res: %d, y_res: %d\n", xres, yres));
    if ((xres < MIN_WIDTH_SIZE) || (yres < MIN_HEIGHT_SIZE))
    {
        return FALSE;
    }

    for (; idx < cnt; idx++)
    {
        if ((gpu_disp_modes[idx].XResolution == xres) && (gpu_disp_modes[idx].YResolution == yres))
        {
            return FALSE;
        }
    }
    gpu_disp_modes[idx].XResolution = xres;
    gpu_disp_modes[idx].YResolution = yres;
    cnt++;
    return TRUE;
}

int VioGpuAdapter::AddEdidModes(void)
{
    PAGED_CODE();
    PEDID_DATA_V1 edid_data = (PEDID_DATA_V1)(GetEdidData());
    ESTABLISHED_TIMINGS_1_2 est_timing_1_2 = edid_data->EstablishedTimings;
    MANUFACTURER_TIMINGS manufact_timing = edid_data->ManufacturerTimings;
    int modecount = 0;

    DbgPrint(TRACE_LEVEL_INFORMATION, (" Default resolutions\n"));
    UpdateModes(MIN_WIDTH_SIZE, MIN_HEIGHT_SIZE, modecount);
    UpdateModes(NOM_WIDTH_SIZE, NOM_HEIGHT_SIZE, modecount);

    DbgPrint(TRACE_LEVEL_INFORMATION, (" Processing EDID's Established timings I and II\n"));
    if (est_timing_1_2.Timing_640x480_75 || est_timing_1_2.Timing_640x480_72 || est_timing_1_2.Timing_640x480_67 ||
        est_timing_1_2.Timing_640x480_60)
    {
        UpdateModes(640, 480, modecount);
    }

    if (est_timing_1_2.Timing_800x600_60 || est_timing_1_2.Timing_800x600_56 || est_timing_1_2.Timing_800x600_75 ||
        est_timing_1_2.Timing_800x600_72)
    {
        UpdateModes(800, 600, modecount);
    }

    if (est_timing_1_2.Timing_720x400_88 || est_timing_1_2.Timing_720x400_70)
    {
        UpdateModes(720, 400, modecount);
    }

    if (est_timing_1_2.Timing_832x624_75)
    {
        UpdateModes(832, 624, modecount);
    }

    if (est_timing_1_2.Timing_1024x768_75 || est_timing_1_2.Timing_1024x768_70 || est_timing_1_2.Timing_1024x768_60 ||
        est_timing_1_2.Timing_1024x768_87)
    {
        UpdateModes(1024, 768, modecount);
    }

    if (est_timing_1_2.Timing_1280x1024_75)
    {
        UpdateModes(1280, 1024, modecount);
    }

    if (manufact_timing.Timing_1152x870_75)
    {
        UpdateModes(1152, 870, modecount);
    }

    PSTANDARD_TIMING_DESCRIPTOR standard_timing = edid_data->StandardTimings;
    DbgPrint(TRACE_LEVEL_INFORMATION, (" Processing EDID's Standard timings\n"));
    for (int i = 0; i < 8; i++, standard_timing++)
    {
        VIOGPU_DISP_MODE mode{0};
        if (GetStandardTimingResolution(standard_timing, &mode))
        {
            UpdateModes(mode.XResolution, mode.YResolution, modecount);
        }
    }

    DbgPrint(TRACE_LEVEL_INFORMATION, (" Processing EDID's detailed timings (4 18-byte blocks)\n"));
    if (edid_data->Revision[0] == 4)
    {
        PEDID_DETAILED_DESCRIPTOR detailed_desc = edid_data->EDIDDetailedTimings;
        for (int i = 0; i < 4; i++, detailed_desc++)
        {
            if (detailed_desc->PixelClock == 0)
            {
                PEDID_DISPLAY_DESCRIPTOR disp = (PEDID_DISPLAY_DESCRIPTOR)detailed_desc;
                if (disp->Tag[3] == 0xF7 && disp->Revision == 0xA)
                {
                    PESTABLISHED_TIMINGS_3 est_timing_3 = (PESTABLISHED_TIMINGS_3)disp->Data;
                    if (est_timing_3->Timing_640x350_85)
                    {
                        UpdateModes(640, 350, modecount);
                    }

                    if (est_timing_3->Timing_640x400_85)
                    {
                        UpdateModes(640, 400, modecount);
                    }

                    if (est_timing_3->Timing_640x480_85)
                    {
                        UpdateModes(640, 480, modecount);
                    }

                    if (est_timing_3->Timing_720x400_85)
                    {
                        UpdateModes(720, 400, modecount);
                    }

                    if (est_timing_3->Timing_800x600_85)
                    {
                        UpdateModes(800, 600, modecount);
                    }

                    if (est_timing_3->Timing_848x480_60)
                    {
                        UpdateModes(848, 480, modecount);
                    }

                    if (est_timing_3->Timing_1024x768_85)
                    {
                        UpdateModes(1024, 768, modecount);
                    }

                    if (est_timing_3->Timing_1152x864_75)
                    {
                        UpdateModes(1152, 864, modecount);
                    }

                    if (est_timing_3->Timing_1280x768_60 || est_timing_3->Timing_1280x768_60_RB ||
                        est_timing_3->Timing_1280x768_75 || est_timing_3->Timing_1280x768_85)
                    {
                        UpdateModes(1280, 768, modecount);
                    }

                    if (est_timing_3->Timing_1280x960_60 || est_timing_3->Timing_1280x960_85)
                    {
                        UpdateModes(1280, 960, modecount);
                    }

                    if (est_timing_3->Timing_1280x1024_60 || est_timing_3->Timing_1280x1024_85)
                    {
                        UpdateModes(1280, 1024, modecount);
                    }

                    if (est_timing_3->Timing_1360x768_60)
                    {
                        UpdateModes(1360, 768, modecount);
                    }

                    if (est_timing_3->Timing_1400x1050_60 || est_timing_3->Timing_1400x1050_60_RB ||
                        est_timing_3->Timing_1400x1050_75 || est_timing_3->Timing_1400x1050_85)
                    {
                        UpdateModes(1400, 1050, modecount);
                    }

                    if (est_timing_3->Timing_1440x900_60 || est_timing_3->Timing_1440x900_60_RB ||
                        est_timing_3->Timing_1440x900_75 || est_timing_3->Timing_1440x900_85)
                    {
                        UpdateModes(1440, 900, modecount);
                    }

                    if (est_timing_3->Timing_1600x1200_60 || est_timing_3->Timing_1600x1200_65 ||
                        est_timing_3->Timing_1600x1200_70 || est_timing_3->Timing_1600x1200_75 ||
                        est_timing_3->Timing_1600x1200_85)
                    {
                        UpdateModes(1600, 1200, modecount);
                    }

                    if (est_timing_3->Timing_1680x1050_60 || est_timing_3->Timing_1680x1050_60_RB ||
                        est_timing_3->Timing_1680x1050_75 || est_timing_3->Timing_1680x1050_85)
                    {
                        UpdateModes(1680, 1050, modecount);
                    }

                    if (est_timing_3->Timing_1792x1344_60 || est_timing_3->Timing_1792x1344_75)
                    {
                        UpdateModes(1792, 1344, modecount);
                    }

                    if (est_timing_3->Timing_1856x1392_60 || est_timing_3->Timing_1856x1392_75)
                    {
                        UpdateModes(1856, 1392, modecount);
                    }

                    if (est_timing_3->Timing_1920x1200_60 || est_timing_3->Timing_1920x1200_60_RB ||
                        est_timing_3->Timing_1920x1200_75 || est_timing_3->Timing_1920x1200_85)
                    {
                        UpdateModes(1920, 1200, modecount);
                    }

                    if (est_timing_3->Timing_1920x1440_60 || est_timing_3->Timing_1920x1440_75)
                    {
                        UpdateModes(1920, 1440, modecount);
                    }
                }
            }
        }
    }

    DbgPrint(TRACE_LEVEL_INFORMATION, (" Processing CTA861 data\n"));
    PEDID_CTA_861 cta_data = (PEDID_CTA_861)GetCTA861Data();
    if (cta_data && cta_data->DTDBegin[0] > 4)
    {
        int vics = (cta_data->DTDBegin[0] - 1) - 4;
        for (int idx = 0; idx < vics; idx++)
        {
            VIOGPU_DISP_MODE mode{0};
            USHORT vic_num = cta_data->Data[idx];
            if (GetVICResolution(vic_num, &mode))
            {
                UpdateModes(mode.XResolution, mode.YResolution, modecount);
            }
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return modecount;
}

void VioGpuAdapter::SetVideoModeInfo(UINT Idx, PVIOGPU_DISP_MODE pModeInfo)
{
    PAGED_CODE();

    PVIDEO_MODE_INFORMATION pMode = NULL;

    pMode = &m_ModeInfo[Idx];
    pMode->Length = sizeof(VIDEO_MODE_INFORMATION);
    pMode->ModeIndex = Idx;
    pMode->VisScreenWidth = pModeInfo->XResolution;
    pMode->VisScreenHeight = pModeInfo->YResolution;
    pMode->ScreenStride = (pModeInfo->XResolution * 4 + 3) & ~0x3;
}

NTSTATUS VioGpuAdapter::UpdateChildStatus(BOOLEAN connect)
{
    PAGED_CODE();
    NTSTATUS Status(STATUS_SUCCESS);
    DXGK_CHILD_STATUS ChildStatus;
    PDXGKRNL_INTERFACE pDXGKInterface(m_pVioGpuDod->GetDxgkInterface());

    RtlZeroMemory(&ChildStatus, sizeof(ChildStatus));

    ChildStatus.Type = StatusConnection;
    ChildStatus.ChildUid = 0;
    ChildStatus.HotPlug.Connected = connect;
    Status = pDXGKInterface->DxgkCbIndicateChildStatus(pDXGKInterface->DeviceHandle, &ChildStatus);
    if (Status != STATUS_SUCCESS)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<--- %s DxgkCbIndicateChildStatus failed with status %x\n ", __FUNCTION__, Status));
    }
    return Status;
}

void VioGpuAdapter::SetCustomDisplay(_In_ USHORT xres, _In_ USHORT yres)
{
    PAGED_CODE();

    VIOGPU_DISP_MODE tmpModeInfo = {0};

    if (xres < MIN_WIDTH_SIZE || yres < MIN_HEIGHT_SIZE)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s: (%dx%d) less than (%dx%d)\n", __FUNCTION__, xres, yres, MIN_WIDTH_SIZE, MIN_HEIGHT_SIZE));
    }
    tmpModeInfo.XResolution = m_pVioGpuDod->IsFlexResolution() ? xres : max(MIN_WIDTH_SIZE, xres);
    tmpModeInfo.YResolution = m_pVioGpuDod->IsFlexResolution() ? yres : max(MIN_HEIGHT_SIZE, yres);

    DbgPrint(TRACE_LEVEL_FATAL,
             ("%s - %d (%dx%d)\n", __FUNCTION__, m_CustomModeIndex, tmpModeInfo.XResolution, tmpModeInfo.YResolution));

    SetVideoModeInfo(m_CustomModeIndex, &tmpModeInfo);
}

NTSTATUS VioGpuAdapter::BuildModeList(DXGK_DISPLAY_INFORMATION *pDispInfo)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    delete[] m_ModeInfo;
    m_ModeInfo = NULL;
    m_ModeCount = 0;

    m_ModeCount = ProcessEdid() + 1;

    m_ModeInfo = new (PagedPool) VIDEO_MODE_INFORMATION[m_ModeCount];
    if (!m_ModeInfo)
    {
        Status = STATUS_NO_MEMORY;
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpuAdapter::GetModeList failed to allocate m_ModeInfo memory\n"));
        return Status;
    }
    RtlZeroMemory(m_ModeInfo, sizeof(VIDEO_MODE_INFORMATION) * m_ModeCount);

    SetCurrentModeIndex(0);

    pDispInfo->Height = max(pDispInfo->Height, MIN_HEIGHT_SIZE);
    pDispInfo->Width = max(pDispInfo->Width, MIN_WIDTH_SIZE);
    pDispInfo->ColorFormat = D3DDDIFMT_X8R8G8B8;
    pDispInfo->Pitch = (BPPFromPixelFormat(pDispInfo->ColorFormat) / BITS_PER_BYTE) * pDispInfo->Width;

    for (USHORT indx = 0; indx < m_ModeCount - 1; indx++)
    {

        PVIOGPU_DISP_MODE pModeInfo = &gpu_disp_modes[indx];

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s: modes[%d] x_res = %d, y_res = %d\n",
                  __FUNCTION__,
                  indx,
                  pModeInfo->XResolution,
                  pModeInfo->YResolution));

        SetVideoModeInfo(indx, pModeInfo);
        if (pModeInfo->XResolution == NOM_WIDTH_SIZE && pModeInfo->YResolution == NOM_HEIGHT_SIZE)
        {
            SetCurrentModeIndex(indx);
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("%s: modes[%d] x_res = %d, y_res = %d\n",
                      __FUNCTION__,
                      m_CurrentModeIndex,
                      pModeInfo->XResolution,
                      pModeInfo->YResolution));
        }
    }

    m_CustomModeIndex = (USHORT)(m_ModeCount - 1);

    DbgPrint(TRACE_LEVEL_INFORMATION, ("ModeCount filtered %d\n", m_ModeCount));

    GetDisplayInfo();

    if (m_pVioGpuDod->IsPersistentDispMode0Set())
    {
        SetCustomDisplay(m_pVioGpuDod->GetPersistentDispMode0Width(), m_pVioGpuDod->GetPersistentDispMode0Height());
        SetCurrentModeIndex(m_CustomModeIndex);
    }

    for (UINT idx = 0; idx < m_ModeCount; idx++)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("index %d, XRes = %d, YRes = %d\n",
                  m_ModeInfo[idx].ModeIndex,
                  m_ModeInfo[idx].VisScreenWidth,
                  m_ModeInfo[idx].VisScreenHeight));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}
PAGED_CODE_SEG_END

void VioGpuAdapter::ClearNativeContextReadiness(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeContextReadinessLock, &oldIrql);
    RtlZeroMemory(&m_NativeContextReadiness, sizeof(m_NativeContextReadiness));
    KeReleaseSpinLock(&m_NativeContextReadinessLock, oldIrql);
}

BOOLEAN VioGpuAdapter::ResetToVgaMode(void)
{
    return NT_SUCCESS(StopNativeContextTransport());
}

void VioGpuAdapter::DestroyFrameBufferObj(BOOLEAN bReset, BOOLEAN bKeepBuffer)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UINT resid = 0;

    if (m_pFrameBuf != NULL)
    {
        resid = (UINT)m_pFrameBuf->GetId();
        if (m_CtrlQueue.IsSynchronousRequestsHealthy())
        {
            m_CtrlQueue.DetachBacking(resid);
            m_CtrlQueue.DestroyResource(resid);
            if (bReset == TRUE)
            {
                m_CtrlQueue.SetScanout(0, 0, 0, 0, 0, 0);
            }
        }

        if (bKeepBuffer)
        {
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("%s: Keeping frame buffer object. Don't use except in bugcheck flow!\n", __FUNCTION__));
        }
        else
        {
            delete m_pFrameBuf;
        }
        m_pFrameBuf = NULL;
        m_Idr.PutId(resid);
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

NTSTATUS VioGpuAdapter::StopNativeContextTransport(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        FailNativeContextAtAnyIrql();
        return STATUS_DEVICE_NOT_READY;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_NativeContextLifecycleMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        FailNativeContextAtAnyIrql();
        return status;
    }

    status = StopNativeContextTransportLocked();
    KeReleaseMutex(&m_NativeContextLifecycleMutex, FALSE);
    return status;
}

NTSTATUS VioGpuAdapter::SynchronizeInterruptMessages(void)
{
    PAGED_CODE();

    ULONG messageCount = m_PciResources.GetInterruptMessageCount();
    if (messageCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (!m_PciResources.HasKnownInterruptMessageCount())
    {
        return STATUS_DEVICE_NOT_READY;
    }
    PDXGKRNL_INTERFACE dxgkInterface = GetDxgkInterface();
    if (dxgkInterface == NULL || dxgkInterface->DxgkCbSynchronizeExecution == NULL)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    for (ULONG messageNumber = 0; messageNumber < messageCount; ++messageNumber)
    {
        BOOLEAN barrierResult = FALSE;
        NTSTATUS barrierStatus = dxgkInterface->DxgkCbSynchronizeExecution(dxgkInterface->DeviceHandle,
                                                                           VioGpuInterruptBarrier,
                                                                           this,
                                                                           messageNumber,
                                                                           &barrierResult);
        if (!NT_SUCCESS(barrierStatus) || !barrierResult)
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu close: interrupt barrier %lu failed, status=0x%08X result=%u\n",
                       messageNumber,
                       barrierStatus,
                       barrierResult);
            return NT_SUCCESS(barrierStatus) ? STATUS_DEVICE_NOT_READY : barrierStatus;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::StopNativeContextTransportLocked(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s\n", __FUNCTION__));

    LONG state = InterlockedCompareExchange(&m_NativeContextState,
                                            VioGpuNativeContextOffline,
                                            VioGpuNativeContextOffline);
    if (!IsListEmpty(&m_NativeContextRegistry) && !m_bVirtioInitialized)
    {
        InterlockedCompareExchange(&m_NativeContextState, VioGpuNativeContextFailed, VioGpuNativeContextOffline);
        FailNativeContextAtAnyIrql();
        return STATUS_DEVICE_NOT_READY;
    }
    if (state == VioGpuNativeContextOffline)
    {
        if (!IsListEmpty(&m_NativeContextRegistry) || m_pWorkThread != NULL || m_WorkThreadHandle != NULL ||
            m_bVirtioInitialized || m_bQueuesInitialized || m_GpuBuf.HasAllocationOwner() ||
            m_FrameSegment.GetSize() != 0 || m_CursorSegment.GetSize() != 0)
        {
            state = InterlockedCompareExchange(&m_NativeContextState,
                                               VioGpuNativeContextFailed,
                                               VioGpuNativeContextOffline);
            if (state == VioGpuNativeContextOffline)
            {
                state = VioGpuNativeContextFailed;
            }
        }
        else
        {
            ClearNativeContextReadiness();
            return STATUS_SUCCESS;
        }
    }
    if (state == VioGpuNativeContextFailed)
    {
        LONG observed = InterlockedCompareExchange(&m_NativeContextState,
                                                   VioGpuNativeContextQuiescing,
                                                   VioGpuNativeContextFailed);
        if (observed == VioGpuNativeContextFailed)
        {
            InterlockedIncrement(&m_NativeContextGeneration);
            InterlockedIncrement64(&m_NativeContextResetGeneration);
            state = VioGpuNativeContextQuiescing;
        }
        else
        {
            state = observed;
        }
    }
    if (state == VioGpuNativeContextOffline)
    {
        return STATUS_SUCCESS;
    }

    while (state != VioGpuNativeContextQuiescing)
    {
        if (state != VioGpuNativeContextStarting && state != VioGpuNativeContextReady)
        {
            FailNativeContextAtAnyIrql();
            return STATUS_DEVICE_NOT_READY;
        }
        LONG observed = InterlockedCompareExchange(&m_NativeContextState, VioGpuNativeContextQuiescing, state);
        if (observed == state)
        {
            state = VioGpuNativeContextQuiescing;
            InterlockedIncrement(&m_NativeContextGeneration);
            InterlockedIncrement64(&m_NativeContextResetGeneration);
            break;
        }
        state = observed;
    }

    /* D-state and partial-start teardown do not complete the outer adapter
     * rundown used by SubmitCommand.  Close this transport generation under
     * the same lock as QueueNativeSubmit before resetting or deleting its
     * virtqueues.  A submitter already in that critical section has left it
     * when this call returns; later submitters fail closed. */
    m_CtrlQueue.PoisonNativeSubmitBacklog();
#if defined(VIOGPU_NATIVE_CONTEXT)
    CompleteNativeSubmitRundown();
#endif
    InvalidateNativeContextRegistrationsLocked();

    NTSTATUS status = m_CtrlQueue.QuiesceSynchronousRequests();
    if (!NT_SUCCESS(status))
    {
        FailNativeContextAtAnyIrql();
        return status;
    }

    status = StopWorkThread();
    if (!NT_SUCCESS(status))
    {
        FailNativeContextAtAnyIrql();
        return status;
    }

    m_pVioGpuDod->SetHardwareInit(FALSE);

    if (m_bQueuesInitialized && !m_bVirtioInitialized)
    {
        FailNativeContextAtAnyIrql();
        return STATUS_DEVICE_NOT_READY;
    }
    InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
    if (m_bVirtioInitialized)
    {
        status = SynchronizeInterruptMessages();
        if (!NT_SUCCESS(status))
        {
            FailNativeContextAtAnyIrql();
            return status;
        }
        KeFlushQueuedDpcs();
    }
    if (m_bQueuesInitialized)
    {
        m_CtrlQueue.DisableInterrupt();
        m_CursorQueue.DisableInterrupt();
    }
    if (m_bVirtioInitialized)
    {
        // The ISR gate, per-message barriers, and DPC drain must precede any
        // queue or device mutation. A successful reset is the Host-owner proof.
        status = virtio_device_reset_checked(&m_VioDev);
        if (!NT_SUCCESS(status) || virtio_get_status(&m_VioDev) != 0)
        {
            FailNativeContextAtAnyIrql();
            return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;
        }
        /* A confirmed device reset is the Host-ownership boundary.  Publish
         * retirement before any later fallible teardown check so surviving
         * allocations never retain an unretirable pre-reset Host identity. */
        Publish2DResetRetirementLocked();
        status = RetireAllNativeContextOwnersLocked();
        if (!NT_SUCCESS(status))
        {
            FailNativeContextAtAnyIrql();
            return status;
        }
        status = SynchronizeInterruptMessages();
        if (!NT_SUCCESS(status))
        {
            FailNativeContextAtAnyIrql();
            return status;
        }
    }
    if (m_bQueuesInitialized)
    {
        InterlockedExchange((PLONG)&m_PendingWorks, 0);
        virtio_delete_queues(&m_VioDev);
        /* Backlog links are separate from VioGpuBuf's ownership lists.  Drop
         * them while every GPU_VBUFFER is still owned by VioGpuBuf.  Close()
         * invokes cancellation callbacks and may free those buffers, so doing
         * this afterwards would traverse LIST_ENTRY nodes embedded in freed
         * memory. */
        m_CtrlQueue.DetachNativeSubmitBacklog();
        m_CtrlQueue.Close();
        m_CursorQueue.Close();
        m_bQueuesInitialized = FALSE;
    }
    else if (m_bVirtioInitialized)
    {
        InterlockedExchange((PLONG)&m_PendingWorks, 0);
    }
    else
    {
        InterlockedExchange((PLONG)&m_PendingWorks, 0);
    }
    if (m_bVirtioInitialized)
    {
        virtio_device_shutdown(&m_VioDev);
        m_bVirtioInitialized = FALSE;
    }
    m_CtrlQueue.CompleteSynchronousRequestTeardown();
    DestroyCursor();
    DestroyFrameBufferObj(TRUE, FALSE);
    m_FrameSegment.Close();
    m_CursorSegment.Close();
    if (!m_GpuBuf.Close())
    {
        FailNativeContextAtAnyIrql();
        return STATUS_DEVICE_NOT_READY;
    }
    ClearNativeContextReadiness();
    m_u64HostFeatures = 0;
    m_u64GuestFeatures = 0;
    m_u32NumCapsets = 0;
    m_u32NumScanouts = 0;
    InterlockedExchange(&m_NativeContextState, VioGpuNativeContextOffline);
    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::ControlInterrupt(_In_ BOOLEAN enableInterrupt)
{
    PAGED_CODE();

    if (!m_bVirtioInitialized || !m_bQueuesInitialized || m_pVioGpuDod == NULL || !m_pVioGpuDod->IsHardwareInit())
    {
        return STATUS_DEVICE_NOT_READY;
    }

    if (enableInterrupt)
    {
        if (!m_CtrlQueue.EnableInterrupt() || !m_CursorQueue.EnableInterrupt())
        {
            m_CtrlQueue.DisableInterrupt();
            m_CursorQueue.DisableInterrupt();
            InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
            return STATUS_DEVICE_NOT_READY;
        }
        InterlockedExchange(&m_InterruptDispatchEnabled, TRUE);
        return STATUS_SUCCESS;
    }

    /* Match the transport teardown ordering: close the ISR publication gate,
     * synchronize every message, drain already queued DPCs, then mask queues. */
    InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
    NTSTATUS status = SynchronizeInterruptMessages();
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    KeFlushQueuedDpcs();
    m_CtrlQueue.DisableInterrupt();
    m_CursorQueue.DisableInterrupt();
    return STATUS_SUCCESS;
}

BOOLEAN VioGpuAdapter::InterruptRoutine(_In_ PDXGKRNL_INTERFACE pDxgkInterface, _In_ ULONG MessageNumber)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s MessageNumber = %d\n", __FUNCTION__, MessageNumber));
    if (InterlockedCompareExchange(&m_InterruptDispatchEnabled, FALSE, FALSE) == FALSE)
    {
        return FALSE;
    }
    ULONG intReason = 0;

    if (m_PciResources.IsMSIEnabled())
    {
        switch (MessageNumber)
        {
            case 0:
                intReason = ISR_REASON_CHANGE;
                break;
            case 1:
                intReason = ISR_REASON_DISPLAY;
                break;
            case 2:
                intReason = ISR_REASON_CURSOR;
                break;
            default:
                DbgPrint(TRACE_LEVEL_FATAL,
                         ("---> %s Unknown Interrupt Reason MessageNumber%d\n", __FUNCTION__, MessageNumber));
        }
    }
    else
    {
        UNREFERENCED_PARAMETER(MessageNumber);
        UCHAR isrstat = virtio_read_isr_status(&m_VioDev);

        if ((isrstat & 1U) != 0)
        {
            intReason |= ISR_REASON_DISPLAY | ISR_REASON_CURSOR;
        }
        if ((isrstat & VIRTIO_PCI_ISR_CONFIG) != 0)
        {
            intReason |= ISR_REASON_CHANGE;
        }
    }

    BOOLEAN serviced = intReason != 0;
    if (serviced)
    {
        if (m_pVioGpuDod->IsUsePresentProgress() && (intReason & ISR_REASON_DISPLAY) == ISR_REASON_DISPLAY)
        {
            DXGKARGCB_NOTIFY_INTERRUPT_DATA NotifyInterrupt = {};
            NotifyInterrupt.InterruptType = DXGK_INTERRUPT_DISPLAYONLY_PRESENT_PROGRESS;
            NotifyInterrupt.DisplayOnlyPresentProgress.VidPnSourceId = 0;

            NotifyInterrupt.DisplayOnlyPresentProgress.ProgressId = DXGK_PRESENT_DISPLAYONLY_PROGRESS_ID_COMPLETE;
            pDxgkInterface->DxgkCbNotifyInterrupt(pDxgkInterface->DeviceHandle, &NotifyInterrupt);
        }

        InterlockedOr((PLONG)&m_PendingWorks, intReason);
        pDxgkInterface->DxgkCbQueueDpc(pDxgkInterface->DeviceHandle);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return serviced;
}

void VioGpuAdapter::ThreadWork(_In_ PVOID Context)
{
    VioGpuAdapter *pdev = reinterpret_cast<VioGpuAdapter *>(Context);
    pdev->ThreadWorkRoutine();
}

void VioGpuAdapter::ThreadWorkRoutine(void)
{
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    for (;;)
    {
        KeWaitForSingleObject(&m_ConfigUpdateEvent, Executive, KernelMode, FALSE, NULL);

        if (m_bStopWorkThread)
        {
            PsTerminateSystemThread(STATUS_SUCCESS);
            break;
        }
        ConfigChanged();
        NotifyResolutionEvent();
    }
}

void VioGpuAdapter::ConfigChanged(void)
{
    DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s\n", __FUNCTION__));
    UINT32 events_read, events_clear = 0;
    virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_read), &events_read, sizeof(m_u32NumScanouts));
    if (events_read & VIRTIO_GPU_EVENT_DISPLAY)
    {
        GetDisplayInfo();
        if (!m_CtrlQueue.IsSynchronousRequestsHealthy())
        {
            FailNativeContextAtAnyIrql();
            ClearNativeContextReadiness();
            return;
        }
        events_clear |= VIRTIO_GPU_EVENT_DISPLAY;
        virtio_set_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_clear), &events_clear, sizeof(m_u32NumScanouts));
        //        UpdateChildStatus(FALSE);
        //        ProcessEdid();
        UpdateChildStatus(TRUE);
    }
}

VOID VioGpuAdapter::DpcRoutine(_In_ PDXGKRNL_INTERFACE pDxgkInterface)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(pDxgkInterface);
    PGPU_VBUFFER pvbuf = NULL;
    UINT len = 0;
    ULONG reason;
    while ((reason = InterlockedExchange((PLONG)&m_PendingWorks, 0)) != 0)
    {
        if ((reason & ISR_REASON_DISPLAY))
        {
            while ((pvbuf = m_CtrlQueue.DequeueBuffer(&len)) != NULL)
            {
                DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s m_CtrlQueue pvbuf = %p len = %d\n", __FUNCTION__, pvbuf, len));
                pvbuf->response_size = len;
                PGPU_CTRL_HDR pcmd = (PGPU_CTRL_HDR)pvbuf->buf;
                PGPU_CTRL_HDR resp = (PGPU_CTRL_HDR)pvbuf->resp_buf;

                if (len < sizeof(GPU_CTRL_HDR))
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s short response, bytes = %u\n", __FUNCTION__, len));
                }
                else if (resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
                {
                    DbgPrint(TRACE_LEVEL_FATAL, ("!!!!! Command failed %d", resp->type));
                }
                if (len >= sizeof(GPU_CTRL_HDR) && resp->type != VIRTIO_GPU_RESP_OK_NODATA)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s type = %xlu flags = %lu fence_id = %llu ctx_id = %lu cmd_type = %lu\n",
                              __FUNCTION__,
                              resp->type,
                              resp->flags,
                              resp->fence_id,
                              resp->ctx_id,
                              pcmd->type));
                }
                VIOGPU_VBUFFER_TERMINAL_CLAIM terminalClaim = VioGpuClaimVbufferTerminalCallbacks(pvbuf);
                if (terminalClaim == VioGpuVbufferTerminalClaimLost)
                {
                    continue;
                }
                void (*completeCallback)(void *) = pvbuf->complete_cb;
                void *completeContext = pvbuf->complete_ctx;
                if (completeCallback != NULL)
                {
                    VioGpuDetachVbufferTerminalCallbacks(pvbuf);
                    completeCallback(completeContext);
                    continue;
                }
                if (terminalClaim == VioGpuVbufferTerminalClaimWon)
                {
                    VioGpuDetachVbufferTerminalCallbacks(pvbuf);
                    m_CtrlQueue.ReleaseBuffer(pvbuf);
                    FailNativeContextAtAnyIrql();
                    continue;
                }
                if (pvbuf->auto_release)
                {
                    m_CtrlQueue.ReleaseBuffer(pvbuf);
                }
            };
            if (InterlockedCompareExchange(&m_InterruptDispatchEnabled, FALSE, FALSE) != FALSE &&
                !m_pVioGpuDod->IsHardwareResetRequested())
            {
                m_CtrlQueue.DrainNativeSubmitBacklog();
            }
        }
        if ((reason & ISR_REASON_CURSOR))
        {
            while ((pvbuf = m_CursorQueue.DequeueCursor(&len)) != NULL)
            {
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("---> %s m_CursorQueue pvbuf = %p len = %u\n", __FUNCTION__, pvbuf, len));
                m_CursorQueue.ReleaseBuffer(pvbuf);
            };
        }
        if (reason & ISR_REASON_CHANGE)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> %s ConfigChanged\n", __FUNCTION__));
            KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        }
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VOID VioGpuAdapter::ResetDevice(VOID)
{
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));
    FailNativeContextAtAnyIrql();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

UINT ColorFormat(UINT format)
{
    switch (format)
    {
        case D3DDDIFMT_A8R8G8B8:
            return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        case D3DDDIFMT_X8R8G8B8:
            return VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        case D3DDDIFMT_A8B8G8R8:
            return VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
        case D3DDDIFMT_X8B8G8R8:
            return VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Unsupported color format %d\n", __FUNCTION__, format));
    return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
}

PAGED_CODE_SEG_BEGIN
BOOLEAN VioGpuAdapter::CreateFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, CURRENT_MODE *pCurrentMode)
{
    UINT resid, format, size;
    VioGpuObj *obj;
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s - %d: (%d x %d)\n", __FUNCTION__, m_Id, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight));
    ASSERT(m_pFrameBuf == NULL);
    size = pModeInfo->ScreenStride * pModeInfo->VisScreenHeight;
    format = ColorFormat(pCurrentMode->DispInfo.ColorFormat);
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s - (%d -> %d)\n", __FUNCTION__, pCurrentMode->DispInfo.ColorFormat, format));
    resid = m_Idr.GetId();
    if (!m_CtrlQueue.CreateResource(resid, format, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight))
    {
        m_Idr.PutId(resid);
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to queue resource creation\n", __FUNCTION__));
        return FALSE;
    }
    obj = new (NonPagedPoolNx) VioGpuObj();
    if (!obj->Init(size, &m_FrameSegment))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to init obj size = %d\n", __FUNCTION__, size));
        m_CtrlQueue.DestroyResource(resid);
        m_Idr.PutId(resid);
        delete obj;
        return FALSE;
    }

    if (!GpuObjectAttach(resid, obj))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to attach gpu object\n", __FUNCTION__));
        m_CtrlQueue.DestroyResource(resid);
        m_Idr.PutId(resid);
        delete obj;
        return FALSE;
    }
    if (!m_CtrlQueue.SetScanout(0, resid, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight, 0, 0) ||
        !m_CtrlQueue.TransferToHost2D(resid, 0, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight, 0, 0) ||
        !m_CtrlQueue.ResFlush(resid, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight, 0, 0))
    {
        m_CtrlQueue.DetachBacking(resid);
        m_CtrlQueue.DestroyResource(resid);
        m_Idr.PutId(resid);
        delete obj;
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to queue initial scanout\n", __FUNCTION__));
        return FALSE;
    }
    m_pFrameBuf = obj;
    pCurrentMode->FrameBuffer = obj->GetVirtualAddress();
    pCurrentMode->Flags.FrameBufferIsActive = TRUE;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

BOOLEAN VioGpuAdapter::CreateCursor(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape,
                                    _In_ CONST CURRENT_MODE *pCurrentMode)
{
    UINT resid, format, size;
    VioGpuObj *obj;
    BOOLEAN status = TRUE;
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s - %d: (%d x %d - %d) (%d + %d)\n",
              __FUNCTION__,
              m_Id,
              pSetPointerShape->Width,
              pSetPointerShape->Height,
              pSetPointerShape->Pitch,
              pSetPointerShape->XHot,
              pSetPointerShape->YHot));

    size = POINTER_SIZE * POINTER_SIZE * 4;
    format = ColorFormat(D3DDDIFMT_A8R8G8B8);
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s - (%x -> %x)\n", __FUNCTION__, pCurrentMode->DispInfo.ColorFormat, format));
    resid = (UINT)m_Idr.GetId();
    if (!m_CtrlQueue.CreateResource(resid, format, POINTER_SIZE, POINTER_SIZE))
    {
        m_Idr.PutId(resid);
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to queue resource creation\n", __FUNCTION__));
        return FALSE;
    }
    obj = new (NonPagedPoolNx) VioGpuObj();
    if (!obj->Init(size, &m_CursorSegment))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to init obj size = %d\n", __FUNCTION__, size));
        status = FALSE;
    }
    else if (!GpuObjectAttach(resid, obj))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to attach gpu object\n", __FUNCTION__));
        status = FALSE;
    }
    if (status)
    {
        m_pCursorBuf = obj;
    }
    else
    {
        VioGpuDbgBreak();
        m_CtrlQueue.DestroyResource(resid);
        m_Idr.PutId(resid);
        delete obj;
    }
    return status;
}

BOOLEAN VioGpuAdapter::UpdateCursor(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape,
                                    _In_ CONST CURRENT_MODE *pCurrentMode)
{
    PAGED_CODE();

    if (pSetPointerShape == NULL || pCurrentMode == NULL || pSetPointerShape->pPixels == NULL ||
        pSetPointerShape->Width == 0 || pSetPointerShape->Height == 0 || pSetPointerShape->Width > POINTER_SIZE ||
        pSetPointerShape->Height > POINTER_SIZE)
    {
        return FALSE;
    }

    const UINT maskPitch = (pSetPointerShape->Width + 7U) / 8U;
    if (pSetPointerShape->Flags.Monochrome)
    {
        if (pSetPointerShape->Pitch < maskPitch ||
            static_cast<SIZE_T>(pSetPointerShape->Pitch) > (MAXULONG_PTR / 2U) / pSetPointerShape->Height)
        {
            return FALSE;
        }
    }
    else if (!pSetPointerShape->Flags.Color || pSetPointerShape->Pitch < pSetPointerShape->Width * 4U)
    {
        return FALSE;
    }

    RECT Rect;
    Rect.left = 0;
    Rect.top = 0;
    Rect.right = Rect.left + pSetPointerShape->Width;
    Rect.bottom = Rect.top + pSetPointerShape->Height;

    if ((m_pCursorBuf == NULL) && !CreateCursor(pSetPointerShape, pCurrentMode))
    {
        VioGpuDbgBreak();
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s Cannot create cursor\n", __FUNCTION__));
        return FALSE;
    }

    BLT_INFO DstBltInfo;
    DstBltInfo.pBits = m_pCursorBuf->GetVirtualAddress();
    DstBltInfo.Pitch = POINTER_SIZE * 4;
    DstBltInfo.BitsPerPel = BPPFromPixelFormat(D3DDDIFMT_A8R8G8B8);
    DstBltInfo.Offset.x = 0;
    DstBltInfo.Offset.y = 0;
    DstBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
    DstBltInfo.Width = POINTER_SIZE;
    DstBltInfo.Height = POINTER_SIZE;

    if (pSetPointerShape->Flags.Monochrome)
    {
        RtlZeroMemory(m_pCursorBuf->GetVirtualAddress(), POINTER_SIZE * POINTER_SIZE * sizeof(ULONG));
        const BYTE *andMask = static_cast<const BYTE *>(pSetPointerShape->pPixels);
        const BYTE *xorMask = andMask + static_cast<SIZE_T>(pSetPointerShape->Pitch) * pSetPointerShape->Height;
        ULONG *destination = static_cast<ULONG *>(m_pCursorBuf->GetVirtualAddress());
        for (UINT y = 0; y < pSetPointerShape->Height; ++y)
        {
            const BYTE *andRow = andMask + static_cast<SIZE_T>(y) * pSetPointerShape->Pitch;
            const BYTE *xorRow = xorMask + static_cast<SIZE_T>(y) * pSetPointerShape->Pitch;
            for (UINT x = 0; x < pSetPointerShape->Width; ++x)
            {
                const BYTE andBit = (andRow[x >> 3] >> (7U - (x & 7U))) & 1U;
                const BYTE xorBit = (xorRow[x >> 3] >> (7U - (x & 7U))) & 1U;
                destination[y * POINTER_SIZE + x] = andBit == 0 ? (xorBit != 0 ? 0xFFFFFFFFU : 0xFF000000U) : 0U;
            }
        }
    }
    else
    {
        BLT_INFO SrcBltInfo;
        SrcBltInfo.pBits = (PVOID)pSetPointerShape->pPixels;
        SrcBltInfo.Pitch = pSetPointerShape->Pitch;
        SrcBltInfo.BitsPerPel = BPPFromPixelFormat(D3DDDIFMT_A8R8G8B8);
        SrcBltInfo.Offset.x = 0;
        SrcBltInfo.Offset.y = 0;
        SrcBltInfo.Rotation = pCurrentMode->Rotation;
        SrcBltInfo.Width = pSetPointerShape->Width;
        SrcBltInfo.Height = pSetPointerShape->Height;

        BltBits(&DstBltInfo, &SrcBltInfo, &Rect);
    }

    if (!m_CtrlQueue.TransferToHost2D(m_pCursorBuf->GetId(),
                                      0,
                                      pSetPointerShape->Width,
                                      pSetPointerShape->Height,
                                      0,
                                      0))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed to queue cursor transfer\n", __FUNCTION__));
        return FALSE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

void VioGpuAdapter::DestroyCursor()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    if (m_pCursorBuf != NULL)
    {
        UINT id = (UINT)m_pCursorBuf->GetId();
        if (m_CtrlQueue.IsSynchronousRequestsHealthy())
        {
            m_CtrlQueue.DetachBacking(id);
            m_CtrlQueue.DestroyResource(id);
        }
        delete m_pCursorBuf;
        m_pCursorBuf = NULL;
        m_Idr.PutId(id);
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuAdapter::GpuObjectAttach(UINT res_id, VioGpuObj *obj)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_MEM_ENTRY ents = NULL;
    PSCATTER_GATHER_LIST sgl = NULL;
    UINT size = 0;
    if (obj == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s object is null\n", __FUNCTION__));
        return FALSE;
    }
    sgl = obj->GetSGList();
    if (sgl == NULL || sgl->NumberOfElements == 0)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s scatter-gather list is empty\n", __FUNCTION__));
        return FALSE;
    }
    size = sizeof(GPU_MEM_ENTRY) * sgl->NumberOfElements;
    ents = reinterpret_cast<PGPU_MEM_ENTRY>(m_GpuBuf.AllocateMemory(size));

    if (!ents)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("<--- %s cannot allocate memory %x bytes numberofentries = %d\n",
                  __FUNCTION__,
                  size,
                  sgl->NumberOfElements));
        return FALSE;
    }
    RtlZeroMemory(ents, size);

    for (UINT i = 0; i < sgl->NumberOfElements; i++)
    {
        ents[i].addr = sgl->Elements[i].Address.QuadPart;
        ents[i].length = sgl->Elements[i].Length;
        ents[i].padding = 0;
    }

    if (!m_CtrlQueue.AttachBacking(res_id, ents, sgl->NumberOfElements))
    {
        m_GpuBuf.FreeMemory(ents);
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s queue failed\n", __FUNCTION__));
        return FALSE;
    }
    obj->SetId(res_id);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}
PAGED_CODE_SEG_END

PDXGKRNL_INTERFACE VioGpuAdapter::GetDxgkInterface()
{
    return m_pVioGpuDod->GetDxgkInterface();
}
