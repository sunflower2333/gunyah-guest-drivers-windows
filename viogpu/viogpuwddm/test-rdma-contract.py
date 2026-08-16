#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKERS = (
    "viogpu/viogpuwddm/check-contract.py",
    "viogpu/viogpuwddm/check-named-pool.py",
    "droidvmpool/check-contract.py",
)
CHECKED_SUBTREES = (".github", "viogpu", "rdmapool", "droidvmpool", "VirtIO", "NetKVM", "viostor", "vioscsi")


@dataclass(frozen=True)
class Rewrite:
    name: str
    path: str
    old: str
    new: str
    accepted: bool = False


REWRITES = (
    Rewrite(
        "R01_remove_free_token",
        "rdmapool/rdmapool.c",
        """                                          inputValue.NumPages,
                                          inputValue.AllocationToken);""",
        """                                          inputValue.NumPages,
                                          1);""",
    ),
    Rewrite(
        "R02_ignore_file_owner",
        "rdmapool/dmapool.c",
        "Allocation == NULL || Allocation->Owner != Owner",
        "Allocation == NULL",
    ),
    Rewrite(
        "R03_free_wrong_extent",
        "rdmapool/dmapool.c",
        "Allocation->StartPage != StartPage || Allocation->NumPages != NumPages",
        "Allocation->StartPage != StartPage",
    ),
    Rewrite(
        "R04_reset_token_on_pnp",
        "rdmapool/dmapool.c",
        "gInitializingCount = 0;",
        "gNextAllocationToken = 0;\n    gInitializingCount = 0;",
    ),
    Rewrite(
        "R05_publish_before_zero",
        "rdmapool/dmapool.c",
        """    RtlZeroMemory(allocationVa, (SIZE_T)NumPages * PAGE_SIZE);

    KeAcquireSpinLock(&gPoolLock, &OldIrql);""",
        """    KeAcquireSpinLock(&gPoolLock, &OldIrql);""",
    ),
    Rewrite(
        "R06_skip_initializer_drain",
        "rdmapool/dmapool.c",
        """    gPoolReady = FALSE;
    for (Entry = gAllocationList.Flink; Entry != &gAllocationList; Entry = Entry->Flink)""",
        """    for (Entry = gAllocationList.Flink; Entry != &gAllocationList; Entry = Entry->Flink)""",
    ),
    Rewrite(
        "R07_skip_method_buffer_snapshot",
        "rdmapool/rdmapool.c",
        """                inputValue = *input;
                if (fileContext == NULL || inputValue.InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2)""",
        """                RtlZeroMemory(&inputValue, sizeof(inputValue));
                if (fileContext == NULL || inputValue.InterfaceVersion != RDMAPOOL_INTERFACE_VERSION_V2)""",
    ),
    Rewrite(
        "R08_publish_ready_before_zero",
        "viogpu/common/viogpu_rdma.cpp",
        """    RtlZeroMemory(m_BaseVA, m_Size);
    if (m_RundownCompleted)""",
        """    m_Ready = TRUE;
    RtlZeroMemory(m_BaseVA, m_Size);
    if (m_RundownCompleted)""",
    ),
    Rewrite(
        "R09_skip_suballocator_rundown",
        "viogpu/common/viogpu_rdma.cpp",
        """if (size == 0 || KeGetCurrentIrql() > DISPATCH_LEVEL || !ExAcquireRundownProtection(&m_Operations))""",
        """if (size == 0 || KeGetCurrentIrql() > DISPATCH_LEVEL || FALSE)""",
    ),
    Rewrite(
        "R10_clear_failed_free_owner",
        "viogpu/common/viogpu_rdma.cpp",
        """    if (!NT_SUCCESS(m_DisconnectStatus))
    {
        // Once submitted, the provider may have consumed the allocation even when completion reports failure.""",
        """    if (!NT_SUCCESS(m_DisconnectStatus))
    {
        ClearConnection();
        // Once submitted, the provider may have consumed the allocation even when completion reports failure.""",
    ),
    Rewrite(
        "R11_retry_submitted_free",
        "viogpu/common/viogpu_rdma.cpp",
        """    if (m_DisconnectAttempted)
    {
        return m_DisconnectStatus;
    }

    RDMAPOOL_FREE_INPUT input = {};""",
        """    RDMAPOOL_FREE_INPUT input = {};""",
    ),
    Rewrite(
        "R12_use_ready_as_owner",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (!IsListEmpty(&m_NativeContextRegistry) || m_bVirtioInitialized || m_bQueuesInitialized ||
        m_pWorkThread != NULL || m_WorkThreadHandle != NULL || m_GpuBuf.HasAllocationOwner() ||
        m_RdmaPool.HasArenaOwner() ||
#if defined(VIOGPU_WDDM_CI_ONLY)
        m_DrmHostPool.HasConnectionOwner() || m_GpuGuestPool.HasConnectionOwner() ||
#endif
        m_FrameSegment.GetSize() != 0 || m_CursorSegment.GetSize() != 0 ||
        InterlockedCompareExchange(&m_NativeContextState,""",
        """    if (!IsListEmpty(&m_NativeContextRegistry) || m_bVirtioInitialized || m_bQueuesInitialized ||
        m_pWorkThread != NULL || m_WorkThreadHandle != NULL || m_GpuBuf.HasAllocationOwner() ||
        m_RdmaPool.IsActive() ||
#if defined(VIOGPU_WDDM_CI_ONLY)
        m_DrmHostPool.HasConnectionOwner() || m_GpuGuestPool.HasConnectionOwner() ||
#endif
        m_FrameSegment.GetSize() != 0 || m_CursorSegment.GetSize() != 0 ||
        InterlockedCompareExchange(&m_NativeContextState,""",
    ),
    Rewrite(
        "R13_drop_information_check",
        "viogpu/common/viogpu_rdma.cpp",
        "ioctlResult.Information != sizeof(output) || ",
        "",
    ),
    Rewrite(
        "R14_make_free_unversioned",
        "viogpu/common/viogpu_rdma.cpp",
        """    RDMAPOOL_FREE_INPUT input = {};
    input.InterfaceVersion = RDMAPOOL_INTERFACE_VERSION_V2;
    input.VirtualAddress = m_BaseVA;""",
        """    RDMAPOOL_FREE_INPUT input = {};
    input.InterfaceVersion = 0;
    input.VirtualAddress = m_BaseVA;""",
    ),
    Rewrite(
        "R15_skip_close_initializer_drain",
        "rdmapool/dmapool.c",
        """    (void)KeWaitForSingleObject(&Owner->NoInitializersEvent, Executive, KernelMode, FALSE, NULL);
    return Released;""",
        """    (void)0;
    return Released;""",
    ),
    Rewrite(
        "R15b_reclaim_owner_at_file_cleanup",
        "rdmapool/rdmapool.c",
        "    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, RdmaPoolEvtDeviceFileCreate, RdmaPoolEvtFileClose, WDF_NO_EVENT_CALLBACK);",
        "    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, RdmaPoolEvtDeviceFileCreate, WDF_NO_EVENT_CALLBACK, RdmaPoolEvtFileClose);",
    ),
    Rewrite(
        "R15c_wait_for_all_file_initializers",
        "rdmapool/dmapool.c",
        """    (void)KeWaitForSingleObject(&Owner->NoInitializersEvent, Executive, KernelMode, FALSE, NULL);
    return Released;""",
        """    (void)KeWaitForSingleObject(&gNoInitializersEvent, Executive, KernelMode, FALSE, NULL);
    return Released;""",
    ),
    Rewrite(
        "R16_forget_submitted_free",
        "viogpu/common/viogpu_rdma.cpp",
        """    m_DisconnectAttempted = TRUE;
    m_DisconnectStatus = ioctlResult.Status;""",
        """    m_DisconnectAttempted = FALSE;
    m_DisconnectStatus = ioctlResult.Status;""",
    ),
    Rewrite(
        "R17_shared_overwrite_retained_owner",
        "rdmapool/rdmaclient.c",
        """        c->AllocationPages != 0 || c->AllocationToken != 0 || c->DisconnectAttempted)""",
        """        c->AllocationToken != 0 || c->DisconnectAttempted)""",
    ),
    Rewrite(
        "R18_shared_retry_submitted_free",
        "rdmapool/rdmaclient.c",
        """    if (c->DisconnectAttempted)
    {
        return c->DisconnectStatus;
    }

    if (c->AllocationToken != 0 || c->AllocationPages != 0 || c->BaseVA != NULL)""",
        """    if (c->AllocationToken != 0 || c->AllocationPages != 0 || c->BaseVA != NULL)""",
    ),
    Rewrite(
        "R19_shared_forget_submitted_free",
        "rdmapool/rdmaclient.c",
        """        c->DisconnectAttempted = TRUE;
        if (!NT_SUCCESS(status) || ioctlResult.Information != 0)""",
        """        c->DisconnectAttempted = FALSE;
        if (!NT_SUCCESS(status) || ioctlResult.Information != 0)""",
    ),
    Rewrite(
        "R20_shared_drop_malformed_owner",
        "rdmapool/rdmaclient.c",
        """            c->AllocationPages = allocOutput.NumPages;
            c->AllocationToken = allocOutput.AllocationToken;
            c->DisconnectAttempted = rollbackResult.Submitted;""",
        """            c->AllocationPages = 0;
            c->AllocationToken = 0;
            c->DisconnectAttempted = rollbackResult.Submitted;""",
    ),
    Rewrite(
        "R21_wdf_retry_submitted_free",
        "VirtIO/WDF/Dma.c",
        """    if (entry->FreeAttempted) {
        return entry->FreeStatus;
    }

    RtlZeroMemory(&freeInput, sizeof(freeInput));""",
        """    RtlZeroMemory(&freeInput, sizeof(freeInput));""",
    ),
    Rewrite(
        "R22_wdf_forget_submitted_free",
        "VirtIO/WDF/Dma.c",
        """    entry->FreeAttempted = TRUE;
    entry->FreeStatus = NT_SUCCESS(ioctlResult.Status)""",
        """    entry->FreeAttempted = FALSE;
    entry->FreeStatus = NT_SUCCESS(ioctlResult.Status)""",
    ),
    Rewrite(
        "R23_wdf_forget_unknown_owner",
        "VirtIO/WDF/Dma.c",
        """            pWdfDriver->RdmaPoolClosing = TRUE;
            pWdfDriver->RdmaPoolOwnerUnknown = TRUE;""",
        """            pWdfDriver->RdmaPoolClosing = TRUE;
            pWdfDriver->RdmaPoolOwnerUnknown = FALSE;""",
    ),
    Rewrite(
        "R24_wdf_close_before_quiesce",
        "VirtIO/WDF/VirtIOWdf.c",
        """    virtio_device_shutdown(&pWdfDriver->VIODevice);

    status = VirtIOWdfDisconnectRdmaPool(pWdfDriver);""",
        """    status = VirtIOWdfDisconnectRdmaPool(pWdfDriver);

    virtio_device_shutdown(&pWdfDriver->VIODevice);""",
    ),
    Rewrite(
        "R25_wdf_fallback_on_incompatible_pool",
        "VirtIO/WDF/VirtIOWdf.c",
        "        } else if (rdmaStatus == STATUS_NOT_FOUND &&",
        "        } else if (!NT_SUCCESS(rdmaStatus) &&",
    ),
    Rewrite(
        "R26_wdf_close_file_after_free_failure",
        "VirtIO/WDF/VirtIOWdf.c",
        """    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (pWdfDriver->RdmaPoolFileObject != NULL) {""",
        """    if (pWdfDriver->RdmaPoolFileObject != NULL) {""",
    ),
    Rewrite(
        "R26b_wdf_fallback_without_broker_in_pvm",
        "VirtIO/WDF/VirtIOWdf.c",
        """        } else if (rdmaStatus == STATUS_NOT_FOUND &&
                   !virtio_is_feature_enabled(VirtIOWdfGetDeviceFeatures(pWdfDriver),
                                              VIRTIO_F_ACCESS_PLATFORM)) {""",
        """        } else if (rdmaStatus == STATUS_NOT_FOUND) {""",
    ),
    Rewrite(
        "R26c_wdf_ack_access_platform_without_owner",
        "VirtIO/WDF/VirtIOWdf.c",
        """    if (virtio_is_feature_enabled(uDeviceFeatures, VIRTIO_F_ACCESS_PLATFORM)) {
        if (!pWdfDriver->RdmaPoolActive || pWdfDriver->RdmaPoolClosing ||
            pWdfDriver->RdmaPoolFileObject == NULL) {
            DPrintf(0, "%s(%s) FAILED: ACCESS_PLATFORM requires an active rdmapool owner\\n",
                    __FUNCTION__, drvTag);
            return STATUS_DEVICE_NOT_READY;
        }
        virtio_feature_enable(uFeatures, VIRTIO_F_ACCESS_PLATFORM);
    }""",
        """    if (virtio_is_feature_enabled(uDeviceFeatures, VIRTIO_F_ACCESS_PLATFORM)) {
        virtio_feature_enable(uFeatures, VIRTIO_F_ACCESS_PLATFORM);
    }""",
    ),
    Rewrite(
        "R27_viostor_remove_access_platform_gate",
        "viostor/virtio_stor.c",
        """    if (CHECKBIT(adaptExt->features, VIRTIO_F_ACCESS_PLATFORM))
    {
        RhelDbgPrint(TRACE_LEVEL_FATAL,
                     " VIRTIO_F_ACCESS_PLATFORM requires a restricted-DMA broker; physical StorPort is unsupported\\n");
        return SP_RETURN_ERROR;
    }
""",
        "",
    ),
    Rewrite(
        "R28_vioscsi_bypass_access_platform_gate",
        "vioscsi/vioscsi.c",
        "CHECKBIT(adaptExt->features, VIRTIO_F_ACCESS_PLATFORM)",
        "!CHECKBIT(adaptExt->features, VIRTIO_F_ACCESS_PLATFORM)",
    ),
    Rewrite(
        "R29_viostor_acknowledge_access_platform",
        "viostor/virtio_stor.c",
        """    if (CHECKBIT(adaptExt->features, VIRTIO_F_ANY_LAYOUT))
    {
        guestFeatures |= (1ULL << VIRTIO_F_ANY_LAYOUT);
    }
""",
        """    if (CHECKBIT(adaptExt->features, VIRTIO_F_ACCESS_PLATFORM))
    {
        guestFeatures |= (1ULL << VIRTIO_F_ACCESS_PLATFORM);
    }
    if (CHECKBIT(adaptExt->features, VIRTIO_F_ANY_LAYOUT))
    {
        guestFeatures |= (1ULL << VIRTIO_F_ANY_LAYOUT);
    }
""",
    ),
    Rewrite(
        "R30_vioscsi_gate_after_feature_ack",
        "vioscsi/vioscsi.c",
        """    if (CHECKBIT(adaptExt->features, VIRTIO_F_ACCESS_PLATFORM))
    {
        RhelDbgPrint(TRACE_LEVEL_FATAL,
                     " VIRTIO_F_ACCESS_PLATFORM requires a restricted-DMA broker; physical StorPort is unsupported\\n");
        return SP_RETURN_ERROR;
    }
    SetGuestFeatures(DeviceExtension);
""",
        """    SetGuestFeatures(DeviceExtension);
    if (CHECKBIT(adaptExt->features, VIRTIO_F_ACCESS_PLATFORM))
    {
        RhelDbgPrint(TRACE_LEVEL_FATAL,
                     " VIRTIO_F_ACCESS_PLATFORM requires a restricted-DMA broker; physical StorPort is unsupported\\n");
        return SP_RETURN_ERROR;
    }
""",
    ),
    Rewrite(
        "R31_viostor_compile_shared_rdma_client",
        "viostor/viostor.vcxproj",
        "    <ClCompile Include=\"virtio_stor_utils.c\" />",
        "    <ClCompile Include=\"..\\rdmapool\\rdmaclient.c\" />",
    ),
    Rewrite(
        "R32_vioscsi_call_prohibited_broker_ddi",
        "vioscsi/vioscsi.c",
        """    UNREFERENCED_PARAMETER(Again);

    ENTER_FN();

    adaptExt = (PADAPTER_EXTENSION)DeviceExtension;""",
        """    UNREFERENCED_PARAMETER(Again);

    ENTER_FN();
    IoGetDeviceInterfaces(NULL, NULL, 0, NULL);

    adaptExt = (PADAPTER_EXTENSION)DeviceExtension;""",
    ),
    Rewrite(
        "R33_viostor_remove_restart_access_platform_gate",
        "viostor/virtio_stor.c",
        """    if (CHECKBIT(adaptExt->features, VIRTIO_F_ACCESS_PLATFORM))
    {
        RhelDbgPrint(TRACE_LEVEL_FATAL,
                     " VIRTIO_F_ACCESS_PLATFORM requires a restricted-DMA broker; physical StorPort is unsupported\\n");
        return FALSE;
    }
""",
        "",
    ),
    Rewrite(
        "R33b_viostor_keep_stale_restart_features",
        "viostor/virtio_stor_hw_helper.c",
        "    adaptExt->features = virtio_get_features(&adaptExt->vdev);",
        "    (void)virtio_get_features(&adaptExt->vdev);",
    ),
    Rewrite(
        "R33c_viostor_restart_gate_after_feature_ack",
        "viostor/virtio_stor.c",
        """    if (CHECKBIT(adaptExt->features, VIRTIO_F_ACCESS_PLATFORM))
    {
        RhelDbgPrint(TRACE_LEVEL_FATAL,
                     " VIRTIO_F_ACCESS_PLATFORM requires a restricted-DMA broker; physical StorPort is unsupported\\n");
        return FALSE;
    }
    RhelSetGuestFeatures(DeviceExtension);
""",
        """    RhelSetGuestFeatures(DeviceExtension);
    if (CHECKBIT(adaptExt->features, VIRTIO_F_ACCESS_PLATFORM))
    {
        RhelDbgPrint(TRACE_LEVEL_FATAL,
                     " VIRTIO_F_ACCESS_PLATFORM requires a restricted-DMA broker; physical StorPort is unsupported\\n");
        return FALSE;
    }
""",
    ),
    Rewrite(
        "R34_netkvm_retry_cached_free",
        "NetKVM/Common/ParaNdis_RdmaPool.cpp",
        """    if (Allocation->FreeSubmitted)
    {
        return Allocation->FreeStatus;
    }
    if (pContext->RdmaPoolFileObject == NULL""",
        """    if (pContext->RdmaPoolFileObject == NULL""",
    ),
    Rewrite(
        "R35_netkvm_repeat_cached_cleanup",
        "NetKVM/Common/ParaNdis_Common.cpp",
        """    if (pContext->CleanupComplete)
    {
        return pContext->CleanupStatus;
    }

    pContext->guestAnnouncePackets.Clear();""",
        """    pContext->guestAnnouncePackets.Clear();""",
    ),
    Rewrite(
        "R36_netkvm_continue_after_first_free_failure",
        "NetKVM/Common/ParaNdis_RdmaPool.cpp",
        """    for (entry = pContext->RdmaPoolAutoDisconnect.m_Allocations.Flink;
         NT_SUCCESS(firstFailure) && entry != &pContext->RdmaPoolAutoDisconnect.m_Allocations;
         entry = entry->Flink)""",
        """    for (entry = pContext->RdmaPoolAutoDisconnect.m_Allocations.Flink;
         entry != &pContext->RdmaPoolAutoDisconnect.m_Allocations;
         entry = entry->Flink)""",
    ),
    Rewrite(
        "R37_netkvm_publish_tombstone_before_file_close",
        "NetKVM/Common/ParaNdis_RdmaPool.cpp",
        """        RdmaPoolCloseFileLocked(pContext);
        RdmaPoolPublishOwnerCleanupLocked(pContext);""",
        """        RdmaPoolPublishOwnerCleanupLocked(pContext);
        RdmaPoolCloseFileLocked(pContext);""",
    ),
    Rewrite(
        "R38_netkvm_reject_late_tombstone",
        "NetKVM/Common/ParaNdis_RdmaPool.cpp",
        """    if (pContext->RdmaPoolAutoDisconnect.m_DisconnectStarted && !allocation->FreeSubmitted)
    {
        RdmaPoolUnlock(pContext);
        return STATUS_INVALID_DEVICE_STATE;
    }""",
        """    if (pContext->RdmaPoolAutoDisconnect.m_DisconnectStarted)
    {
        RdmaPoolUnlock(pContext);
        return STATUS_INVALID_DEVICE_STATE;
    }""",
    ),
    Rewrite(
        "R38b_netkvm_leave_late_tombstone_tracked",
        "NetKVM/Common/ParaNdis_RdmaPool.cpp",
        """    RemoveEntryList(&allocation->ListEntry);
    RdmaPoolUnlock(pContext);
    NdisFreeMemoryWithTagPriority(pContext->MiniportHandle, allocation, NETKVM_RDMAPOOL_ALLOC_TAG);""",
        """    RdmaPoolUnlock(pContext);
    NdisFreeMemoryWithTagPriority(pContext->MiniportHandle, allocation, NETKVM_RDMAPOOL_ALLOC_TAG);""",
    ),
    Rewrite(
        "R39_netkvm_destroy_before_init_failure_cleanup",
        "NetKVM/wlh/ParaNdis6_Driver.cpp",
        """            NTSTATUS cleanupStatus = ParaNdis_CleanupContext(pContext);
            if (!NT_SUCCESS(cleanupStatus))
            {
                DPrintf(0, "ERROR: adapter cleanup used terminal file-owner recovery (%X)", cleanupStatus);
                status = NDIS_STATUS_DEVICE_FAILED;
            }
            pContext->Destroy(pContext, pContext->MiniportHandle);""",
        """            pContext->Destroy(pContext, pContext->MiniportHandle);
            NTSTATUS cleanupStatus = ParaNdis_CleanupContext(pContext);
            if (!NT_SUCCESS(cleanupStatus))
            {
                DPrintf(0, "ERROR: adapter cleanup used terminal file-owner recovery (%X)", cleanupStatus);
                status = NDIS_STATUS_DEVICE_FAILED;
            }""",
    ),
    Rewrite(
        "R40_netkvm_destroy_before_halt_cleanup",
        "NetKVM/wlh/ParaNdis6_Driver.cpp",
        """    NTSTATUS cleanupStatus = ParaNdis_CleanupContext(pContext);
    if (!NT_SUCCESS(cleanupStatus))
    {
        /* MiniportHaltEx returns VOID. The provider owner has still been
         * closed and local records are terminal tombstones. */
        DPrintf(0, "ERROR: adapter halt cleanup used terminal file-owner recovery (%X)", cleanupStatus);
    }
    pContext->Destroy(pContext, pContext->MiniportHandle);""",
        """    pContext->Destroy(pContext, pContext->MiniportHandle);
    NTSTATUS cleanupStatus = ParaNdis_CleanupContext(pContext);
    if (!NT_SUCCESS(cleanupStatus))
    {
        /* MiniportHaltEx returns VOID. The provider owner has still been
         * closed and local records are terminal tombstones. */
        DPrintf(0, "ERROR: adapter halt cleanup used terminal file-owner recovery (%X)", cleanupStatus);
    }""",
    ),
    Rewrite(
        "R41_worker_use_untyped_initial_reference",
        "viogpu/viogpudo/viogpudo.cpp",
        """    status = ObReferenceObjectByHandle(threadHandle,
                                       SYNCHRONIZE,
                                       *PsThreadType,""",
        """    status = ObReferenceObjectByHandle(threadHandle,
                                       SYNCHRONIZE,
                                       NULL,""",
    ),
    Rewrite(
        "R42_worker_skip_handle_fallback_reference",
        "viogpu/viogpudo/viogpudo.cpp",
        """        status = ObReferenceObjectByHandle(m_WorkThreadHandle,
                                           SYNCHRONIZE,
                                           *PsThreadType,""",
        """        status = STATUS_UNSUCCESSFUL;
        if (FALSE) status = ObReferenceObjectByHandle(m_WorkThreadHandle,
                                           SYNCHRONIZE,
                                           *PsThreadType,""",
    ),
    Rewrite(
        "R43_adapter_destructor_hide_failure_in_assert",
        "viogpu/viogpudo/viogpudo.cpp",
        """        if (!NT_SUCCESS(status))
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "viogpu adapter destructor: StopNativeContextTransport failed, status=0x%08X\\n",
                       status);
        }
        NT_ASSERT(NT_SUCCESS(status));""",
        """        NT_ASSERT(NT_SUCCESS(status));""",
    ),
    Rewrite(
        "R44_collapse_host_context_outcomes",
        "viogpu/common/viogpu_queue.h",
        """    VioGpuHostContextRejected,
    VioGpuHostContextUnknown,""",
        """    VioGpuHostContextRejected,
    VioGpuHostContextUnknown = VioGpuHostContextRejected,""",
    ),
    Rewrite(
        "R45_publish_submitted_before_enqueue",
        "viogpu/common/viogpu_queue.cpp",
        """    if (QueueBuffer(buf) < 0)
    {
        buf->complete_cb = NULL;
        buf->complete_ctx = NULL;
        buf->synchronous_epoch_state = 0;
        return FALSE;
    }
    *submitted = TRUE;""",
        """    *submitted = TRUE;
    if (QueueBuffer(buf) < 0)
    {
        buf->complete_cb = NULL;
        buf->complete_ctx = NULL;
        buf->synchronous_epoch_state = 0;
        return FALSE;
    }""",
    ),
    Rewrite(
        "R46_submit_create_before_owner_ledger",
        "viogpu/viogpudo/viogpudo.cpp",
        """    InsertTailList(&m_NativeContextRegistry, &owner->AdapterLink);

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->BindingLock, &oldIrql);
    context->Adapter = this;
    context->Owner = owner;
    context->Generation = generation;
    context->ResetGeneration = resetGeneration;
    context->ContextId = contextId;
    InterlockedExchange(&context->State, VioGpuNativeContextCreating);
    KeReleaseSpinLock(&context->BindingLock, oldIrql);

    VIOGPU_HOST_CONTEXT_RESULT createResult = m_CtrlQueue.CreateNativeContext(contextId);""",
        """    VIOGPU_HOST_CONTEXT_RESULT createResult = m_CtrlQueue.CreateNativeContext(contextId);
    InsertTailList(&m_NativeContextRegistry, &owner->AdapterLink);

    KIRQL oldIrql;
    KeAcquireSpinLock(&context->BindingLock, &oldIrql);
    context->Adapter = this;
    context->Owner = owner;
    context->Generation = generation;
    context->ResetGeneration = resetGeneration;
    context->ContextId = contextId;
    InterlockedExchange(&context->State, VioGpuNativeContextCreating);
    KeReleaseSpinLock(&context->BindingLock, oldIrql);""",
    ),
    Rewrite(
        "R47_destroy_accept_every_control_error",
        "viogpu/common/viogpu_queue.cpp",
        """        else if (IsPlainControlErrorResponse(response) && response->type == VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID)""",
        """        else if (IsPlainControlErrorResponse(response))""",
    ),
    Rewrite(
        "R48_destroy_retire_unknown_owner",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (destroyResult == VioGpuHostContextConfirmed || destroyResult == VioGpuHostContextRejected)
    {
        RetireNativeContextOwnerLocked(owner);
    }""",
        """    if (destroyResult == VioGpuHostContextConfirmed || destroyResult == VioGpuHostContextRejected ||
        destroyResult == VioGpuHostContextUnknown)
    {
        RetireNativeContextOwnerLocked(owner);
    }""",
    ),
    Rewrite(
        "R49_destroy_retire_not_submitted_owner",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (destroyResult == VioGpuHostContextConfirmed || destroyResult == VioGpuHostContextRejected)
    {
        RetireNativeContextOwnerLocked(owner);
    }""",
        """    if (destroyResult == VioGpuHostContextConfirmed || destroyResult == VioGpuHostContextRejected ||
        destroyResult == VioGpuHostContextNotSubmitted)
    {
        RetireNativeContextOwnerLocked(owner);
    }""",
    ),
    Rewrite(
        "R50_create_retire_unknown_owner",
        "viogpu/viogpudo/viogpudo.cpp",
        """        if (!hostContextCreated &&
            (createResult == VioGpuHostContextNotSubmitted || createResult == VioGpuHostContextRejected))""",
        """        if (!hostContextCreated &&
            (createResult == VioGpuHostContextNotSubmitted || createResult == VioGpuHostContextRejected ||
             createResult == VioGpuHostContextUnknown))""",
    ),
    Rewrite(
        "R51_drop_no_reset_owner_guard",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (!IsListEmpty(&m_NativeContextRegistry) && !m_bVirtioInitialized)
    {
        InterlockedCompareExchange(&m_NativeContextState, VioGpuNativeContextFailed, VioGpuNativeContextOffline);
        FailNativeContextAtAnyIrql();
        return STATUS_DEVICE_NOT_READY;
    }
""",
        "",
    ),
    Rewrite(
        "R52_invalidate_before_quiescing_publication",
        "viogpu/viogpudo/viogpudo.cpp",
        """    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s\\n", __FUNCTION__));

    LONG state = InterlockedCompareExchange(&m_NativeContextState,
                                            VioGpuNativeContextOffline,
                                            VioGpuNativeContextOffline);""",
        """    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s\\n", __FUNCTION__));

    InvalidateNativeContextRegistrationsLocked();
    LONG state = InterlockedCompareExchange(&m_NativeContextState,
                                            VioGpuNativeContextOffline,
                                            VioGpuNativeContextOffline);""",
    ),
    Rewrite(
        "R53_reset_before_isr_barriers",
        "viogpu/viogpudo/viogpudo.cpp",
        """    InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
    if (m_bVirtioInitialized)
    {
        status = SynchronizeInterruptMessages();""",
        """    status = virtio_device_reset_checked(&m_VioDev);
    InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
    if (m_bVirtioInitialized)
    {
        status = SynchronizeInterruptMessages();""",
    ),
    Rewrite(
        "R53b_partial_init_skips_retained_barrier",
        "viogpu/viogpudo/viogpudo.cpp",
        "ULONG messageCount = m_PciResources.GetInterruptMessageCount();",
        "ULONG messageCount = m_bQueuesInitialized ? m_PciResources.GetInterruptMessageCount() : 0U;",
    ),
    Rewrite(
        "R54_retire_owners_before_reset_status_proof",
        "viogpu/viogpudo/viogpudo.cpp",
        """        status = virtio_device_reset_checked(&m_VioDev);
        if (!NT_SUCCESS(status) || virtio_get_status(&m_VioDev) != 0)
        {
            FailNativeContextAtAnyIrql();
            return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;
        }
        status = SynchronizeInterruptMessages();
        if (!NT_SUCCESS(status))
        {
            FailNativeContextAtAnyIrql();
            return status;
        }
        RetireAllNativeContextOwnersLocked();""",
        """        status = virtio_device_reset_checked(&m_VioDev);
        if (virtio_get_status(&m_VioDev) != 0)
        {
            FailNativeContextAtAnyIrql();
            return STATUS_DEVICE_NOT_READY;
        }
        status = SynchronizeInterruptMessages();
        if (!NT_SUCCESS(status))
        {
            FailNativeContextAtAnyIrql();
            return status;
        }
        RetireAllNativeContextOwnersLocked();""",
    ),
    Rewrite(
        "R55_ignore_reset_status_failure",
        "viogpu/viogpudo/viogpudo.cpp",
        """        if (!NT_SUCCESS(status) || virtio_get_status(&m_VioDev) != 0)
        {
            FailNativeContextAtAnyIrql();
            return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;
        }""",
        """        if (!NT_SUCCESS(status))
        {
            FailNativeContextAtAnyIrql();
            return status;
        }""",
    ),
    Rewrite(
        "R56_bypass_context_device_reservation",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (!ReferenceDevice(device))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_WDDM_CONTEXT *context = new (NonPagedPoolNx) VIOGPU_WDDM_CONTEXT;""",
        """    if (FALSE && !ReferenceDevice(device))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    VIOGPU_WDDM_CONTEXT *context = new (NonPagedPoolNx) VIOGPU_WDDM_CONTEXT;""",
    ),
    Rewrite(
        "R57_destroy_device_without_closing_bit",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            LONG closingState = state | VIOGPU_WDDM_DEVICE_CLOSING;""",
        """            LONG closingState = state;""",
    ),
    Rewrite(
        "R58_leak_render_snapshot_on_short_buffer",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """        status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;""",
        """        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;""",
    ),
    Rewrite(
        "R59_reopen_hardware_rundown_after_failed_stop",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (NT_SUCCESS(status))
    {
        ExReInitializeRundownProtection(&m_HardwareOperations);
        m_HardwareRundownCompleted = FALSE;
    }
    return status;""",
        """    ExReInitializeRundownProtection(&m_HardwareOperations);
    m_HardwareRundownCompleted = FALSE;
    return status;""",
    ),
    Rewrite(
        "R60_enable_compile_only_driver_entry",
        "viogpu/viogpuwddm/driver_entry.cpp",
        """    return STATUS_NOT_SUPPORTED;
}""",
        """    return VioGpuWddmInitializeMiniportCompileOnly(driverObject, registryPath);
}""",
    ),
    Rewrite(
        "R61_enable_submit_without_retirement",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(submitCommand);
    return STATUS_NOT_SUPPORTED;
}""",
        """    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(submitCommand);
    return STATUS_SUCCESS;
}""",
    ),
    Rewrite(
        "R62_skip_hardware_rundown_state_init",
        "viogpu/viogpudo/viogpudo.cpp",
        """      m_pHWDevice(NULL), m_HardwareRundownCompleted(FALSE)""",
        """      m_pHWDevice(NULL)""",
    ),
    Rewrite(
        "R63_repeat_hardware_rundown_wait_on_stop_retry",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (!m_HardwareRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_HardwareOperations);
        ExRundownCompleted(&m_HardwareOperations);
        m_HardwareRundownCompleted = TRUE;
    }
    NTSTATUS status = STATUS_SUCCESS;""",
        """    ExWaitForRundownProtectionRelease(&m_HardwareOperations);
    ExRundownCompleted(&m_HardwareOperations);
    m_HardwareRundownCompleted = TRUE;
    NTSTATUS status = STATUS_SUCCESS;""",
    ),
    Rewrite(
        "R64_skip_hardware_rundown_completed_on_stop",
        "viogpu/viogpudo/viogpudo.cpp",
        """        ExWaitForRundownProtectionRelease(&m_HardwareOperations);
        ExRundownCompleted(&m_HardwareOperations);
        m_HardwareRundownCompleted = TRUE;
    }
    NTSTATUS status = STATUS_SUCCESS;""",
        """        ExWaitForRundownProtectionRelease(&m_HardwareOperations);
        m_HardwareRundownCompleted = TRUE;
    }
    NTSTATUS status = STATUS_SUCCESS;""",
    ),
    Rewrite(
        "R65_skip_hardware_rundown_completed_in_destructor",
        "viogpu/viogpudo/viogpudo.cpp",
        """        ExWaitForRundownProtectionRelease(&m_HardwareOperations);
        ExRundownCompleted(&m_HardwareOperations);
        m_HardwareRundownCompleted = TRUE;
    }
    DbgPrintEx""",
        """        ExWaitForRundownProtectionRelease(&m_HardwareOperations);
        m_HardwareRundownCompleted = TRUE;
    }
    DbgPrintEx""",
    ),
    Rewrite(
        "R66_publish_hardware_open_before_reinitialize",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (NT_SUCCESS(status))
    {
        ExReInitializeRundownProtection(&m_HardwareOperations);
        m_HardwareRundownCompleted = FALSE;
    }
    return status;""",
        """    if (NT_SUCCESS(status))
    {
        m_HardwareRundownCompleted = FALSE;
        ExReInitializeRundownProtection(&m_HardwareOperations);
    }
    return status;""",
    ),
    Rewrite(
        "R67_skip_context_rundown_state_init",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    ExInitializeRundownProtection(&context->Operations);
    context->OperationsRundownCompleted = FALSE;
    context->Device = device;""",
        """    ExInitializeRundownProtection(&context->Operations);
    context->Device = device;""",
    ),
    Rewrite(
        "R68_repeat_context_rundown_wait_on_destroy_retry",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (!context->OperationsRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&context->Operations);
        ExRundownCompleted(&context->Operations);
        context->OperationsRundownCompleted = TRUE;
    }

    BOOLEAN released = FALSE;""",
        """    ExWaitForRundownProtectionRelease(&context->Operations);
    ExRundownCompleted(&context->Operations);
    context->OperationsRundownCompleted = TRUE;

    BOOLEAN released = FALSE;""",
    ),
    Rewrite(
        "R69_skip_context_rundown_completed",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """        ExWaitForRundownProtectionRelease(&context->Operations);
        ExRundownCompleted(&context->Operations);
        context->OperationsRundownCompleted = TRUE;""",
        """        ExWaitForRundownProtectionRelease(&context->Operations);
        context->OperationsRundownCompleted = TRUE;""",
    ),
    Rewrite(
        "R70_reopen_context_rundown_on_destroy_failure",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (!released)
    {
        return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;
    }""",
        """    if (!released)
    {
        context->OperationsRundownCompleted = FALSE;
        return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;
    }""",
    ),
    Rewrite(
        "R71_reinitialize_terminal_context_rundown",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    VIOGPU_WDDM_DEVICE *device = context->Device;
    context->Signature = 0;""",
        """    VIOGPU_WDDM_DEVICE *device = context->Device;
    ExReInitializeRundownProtection(&context->Operations);
    context->Signature = 0;""",
    ),
    Rewrite(
        "R72_bypass_render_context_rundown",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (!ExAcquireRundownProtection(&context->Operations))
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE)""",
        """    if (FALSE && !ExAcquireRundownProtection(&context->Operations))
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE)""",
    ),
    Rewrite(
        "R73_release_context_before_native_snapshot",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
    if (!VioGpuAdapter::AcquireNativeContextSnapshot(&context->NativeContext, &snapshot))""",
        """    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};
    ExReleaseRundownProtection(&context->Operations);
    if (!VioGpuAdapter::AcquireNativeContextSnapshot(&context->NativeContext, &snapshot))""",
    ),
    Rewrite(
        "R74_bypass_vidmm_hardware_rundown",
        "viogpu/viogpudo/viogpudo.cpp",
        """BOOLEAN VioGpuDod::QueryVidMmSegment(PPHYSICAL_ADDRESS physicalAddress, SIZE_T *size) const
{
    if (!ExAcquireRundownProtection(&m_HardwareOperations))""",
        """BOOLEAN VioGpuDod::QueryVidMmSegment(PPHYSICAL_ADDRESS physicalAddress, SIZE_T *size) const
{
    if (FALSE && !ExAcquireRundownProtection(&m_HardwareOperations))""",
    ),
    Rewrite(
        "R75_bypass_readiness_hardware_rundown",
        "viogpu/viogpudo/viogpudo.cpp",
        """BOOLEAN VioGpuDod::QueryNativeContextReadiness(_Out_ PGPU_CAPSET_DRM capset,
                                               _Out_opt_ UINT *capsetVersion,
                                               _Out_opt_ UINT *capsetSize,
                                               _Out_opt_ ULONGLONG *resetGeneration)
{
    if (!ExAcquireRundownProtection(&m_HardwareOperations))""",
        """BOOLEAN VioGpuDod::QueryNativeContextReadiness(_Out_ PGPU_CAPSET_DRM capset,
                                               _Out_opt_ UINT *capsetVersion,
                                               _Out_opt_ UINT *capsetSize,
                                               _Out_opt_ ULONGLONG *resetGeneration)
{
    if (FALSE && !ExAcquireRundownProtection(&m_HardwareOperations))""",
    ),
    Rewrite(
        "R76_bypass_create_context_hardware_rundown",
        "viogpu/viogpudo/viogpudo.cpp",
        """NTSTATUS VioGpuDod::CreateNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                        _In_ ULONGLONG expectedResetGeneration)
{
    if (!ExAcquireRundownProtection(&m_HardwareOperations))""",
        """NTSTATUS VioGpuDod::CreateNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                        _In_ ULONGLONG expectedResetGeneration)
{
    if (FALSE && !ExAcquireRundownProtection(&m_HardwareOperations))""",
    ),
    Rewrite(
        "R77_bypass_destroy_context_hardware_rundown",
        "viogpu/viogpudo/viogpudo.cpp",
        """    *released = FALSE;
    if (!ExAcquireRundownProtection(&m_HardwareOperations))""",
        """    *released = FALSE;
    if (FALSE && !ExAcquireRundownProtection(&m_HardwareOperations))""",
    ),
    Rewrite(
        "R78_release_hardware_rundown_before_adapter_use",
        "viogpu/viogpudo/viogpudo.cpp",
        """    VioGpuAdapter *adapter = m_pHWDevice;
    NTSTATUS status = !IsHardwareResetRequested() && adapter != NULL ? adapter->CreateNativeContext(context,
                                                                                                    expectedResetGeneration)
                                                                     : STATUS_DEVICE_NOT_READY;
    ExReleaseRundownProtection(&m_HardwareOperations);""",
        """    VioGpuAdapter *adapter = m_pHWDevice;
    ExReleaseRundownProtection(&m_HardwareOperations);
    NTSTATUS status = !IsHardwareResetRequested() && adapter != NULL ? adapter->CreateNativeContext(context,
                                                                                                    expectedResetGeneration)
                                                                     : STATUS_DEVICE_NOT_READY;""",
    ),
    Rewrite(
        "R79_generation_ignores_hardware_reset_gate",
        "viogpu/viogpudo/viogpudo.cpp",
        """    return !m_pVioGpuDod->IsHardwareResetRequested() &&
#if defined(VIOGPU_WDDM_CI_ONLY)
           m_DrmHostPool.IsActive() && m_GpuGuestPool.IsActive() &&
#endif
           generation == InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0) &&""",
        """    return
#if defined(VIOGPU_WDDM_CI_ONLY)
           m_DrmHostPool.IsActive() && m_GpuGuestPool.IsActive() &&
#endif
           generation == InterlockedCompareExchange(&m_NativeContextGeneration, 0, 0) &&""",
    ),
    Rewrite(
        "R80_system_display_gate_after_early_return",
        "viogpu/viogpudo/viogpudo.cpp",
        """    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\\n", __FUNCTION__));
    InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);""",
        """    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\\n", __FUNCTION__));
    if (!IsVgaDevice())
    {
        return STATUS_UNSUCCESSFUL;
    }
    InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);""",
    ),
    Rewrite(
        "R81_start_overwrites_pending_reset",
        "viogpu/viogpudo/viogpudo.cpp",
        """    LONG startResetState = InterlockedCompareExchange(&m_HardwareResetState,
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
        return STATUS_DEVICE_NOT_READY;
    }""",
        """    InterlockedExchange(&m_HardwareResetState, VioGpuHardwareRecovering);""",
    ),
    Rewrite(
        "R82_get_device_info_skips_recovery_rollback",
        "viogpu/viogpudo/viogpudo.cpp",
        """        VIOGPU_LOG_ASSERTION1("DxgkCbGetDeviceInformation failed with status 0x%X\\n", Status);
        InterlockedCompareExchange(&m_HardwareResetState, startResetState, VioGpuHardwareRecovering);
        return Status;""",
        """        VIOGPU_LOG_ASSERTION1("DxgkCbGetDeviceInformation failed with status 0x%X\\n", Status);
        return Status;""",
    ),
    Rewrite(
        "R83_check_hardware_skips_recovery_rollback",
        "viogpu/viogpudo/viogpudo.cpp",
        """        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "viogpu StartDevice: CheckHardware failed\\n");
        InterlockedCompareExchange(&m_HardwareResetState, startResetState, VioGpuHardwareRecovering);
        return STATUS_GRAPHICS_DRIVER_MISMATCH;""",
        """        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "viogpu StartDevice: CheckHardware failed\\n");
        return STATUS_GRAPHICS_DRIVER_MISMATCH;""",
    ),
    Rewrite(
        "R84_allocation_skips_recovery_rollback",
        "viogpu/viogpudo/viogpudo.cpp",
        """        DbgPrint(TRACE_LEVEL_ERROR, ("StartDevice failed to allocate memory\\n"));
        InterlockedCompareExchange(&m_HardwareResetState, startResetState, VioGpuHardwareRecovering);
        return Status;""",
        """        DbgPrint(TRACE_LEVEL_ERROR, ("StartDevice failed to allocate memory\\n"));
        return Status;""",
    ),
    Rewrite(
        "R85_destructor_deletes_retained_adapter",
        "viogpu/viogpudo/viogpudo.cpp",
        "    NT_ASSERT(m_pHWDevice == NULL);",
        """    delete m_pHWDevice;
    m_pHWDevice = NULL;""",
    ),
    Rewrite(
        "R86_assume_single_descriptor_msi_count",
        "viogpu/common/viogpu_pci.cpp",
        "(IsMSIEnabled() && m_InterruptMessageCount >= 2U));",
        "(IsMSIEnabled() && m_InterruptMessageCount >= 1U));",
    ),
    Rewrite(
        "R87_allow_two_message_start_layout",
        "viogpu/common/viogpu_pci.cpp",
        """                                     (!IsMSIEnabled() ||
                                      (m_InterruptMessageCount >= 3U && m_InterruptMessageCount <= 4U));""",
        """                                     (!IsMSIEnabled() ||
                                      (m_InterruptMessageCount >= 2U && m_InterruptMessageCount <= 4U));""",
    ),
    Rewrite(
        "R88_barrier_ignores_unknown_message_count",
        "viogpu/viogpudo/viogpudo.cpp",
        "    if (!m_PciResources.HasKnownInterruptMessageCount())",
        "    if (FALSE && !m_PciResources.HasKnownInterruptMessageCount())",
    ),
    Rewrite(
        "R89_hwclose_skips_transport_stop",
        "viogpu/viogpudo/viogpudo.cpp",
        """    NTSTATUS status = StopNativeContextTransport();
    if (NT_SUCCESS(status))
    {
        InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);""",
        """    NTSTATUS status = STATUS_SUCCESS;
    if (NT_SUCCESS(status))
    {
        InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);""",
    ),
    Rewrite(
        "R90_hwclose_skips_isr_gate",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (NT_SUCCESS(status))
    {
        InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
        status = SynchronizeInterruptMessages();
    }
    if (NT_SUCCESS(status))
    {
        KeFlushQueuedDpcs();""",
        """    if (NT_SUCCESS(status))
    {
        status = SynchronizeInterruptMessages();
    }
    if (NT_SUCCESS(status))
    {
        KeFlushQueuedDpcs();""",
    ),
    Rewrite(
        "R91_hwclose_skips_final_barrier",
        "viogpu/viogpudo/viogpudo.cpp",
        """        InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
        status = SynchronizeInterruptMessages();
    }
    if (NT_SUCCESS(status))""",
        """        InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);
    }
    if (NT_SUCCESS(status))""",
    ),
    Rewrite(
        "R92_hwclose_skips_dpc_drain",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (NT_SUCCESS(status))
    {
        KeFlushQueuedDpcs();
        status = m_PciResources.Close();
    }
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s status=0x%08X\\n", __FUNCTION__, status));""",
        """    if (NT_SUCCESS(status))
    {
        status = m_PciResources.Close();
    }
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s status=0x%08X\\n", __FUNCTION__, status));""",
    ),
    Rewrite(
        "R93_hwclose_skips_pci_close",
        "viogpu/viogpudo/viogpudo.cpp",
        """        KeFlushQueuedDpcs();
        status = m_PciResources.Close();
    }
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s status=0x%08X\\n", __FUNCTION__, status));""",
        """        KeFlushQueuedDpcs();
    }
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s status=0x%08X\\n", __FUNCTION__, status));""",
    ),
    Rewrite(
        "R94_clear_interrupt_count_before_bar_unmap",
        "viogpu/common/viogpu_pci.cpp",
        """    NTSTATUS firstFailure = STATUS_SUCCESS;
    for (UINT bar = 0; bar < PCI_TYPE0_ADDRESSES; ++bar)""",
        """    NTSTATUS firstFailure = STATUS_SUCCESS;
    m_InterruptMessageCountKnown = FALSE;
    for (UINT bar = 0; bar < PCI_TYPE0_ADDRESSES; ++bar)""",
    ),
    Rewrite(
        "R95_d0_failure_discards_concurrent_reset",
        "viogpu/viogpudo/viogpudo.cpp",
        "            InterlockedCompareExchange(&m_HardwareResetState, VioGpuHardwareResetRequested, VioGpuHardwareRecovering);",
        "            InterlockedExchange(&m_HardwareResetState, VioGpuHardwareActive);",
    ),
    Rewrite(
        "R96_d0_publish_overwrites_concurrent_reset",
        "viogpu/viogpudo/viogpudo.cpp",
        """                if (InterlockedCompareExchange(&m_HardwareResetState, VioGpuHardwareActive, VioGpuHardwareRecovering) !=
                    VioGpuHardwareRecovering)
                {
                    return STATUS_DEVICE_NOT_READY;
                }""",
        """                InterlockedExchange(&m_HardwareResetState, VioGpuHardwareActive);""",
    ),
    Rewrite(
        "R97_failed_start_skips_outer_rundown",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (!m_HardwareRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&m_HardwareOperations);
        ExRundownCompleted(&m_HardwareOperations);
        m_HardwareRundownCompleted = TRUE;
    }

    NTSTATUS closeStatus = m_pHWDevice->HWClose();""",
        """    NTSTATUS closeStatus = m_pHWDevice->HWClose();""",
    ),
    Rewrite(
        "R98_failed_start_reopens_before_close",
        "viogpu/viogpudo/viogpudo.cpp",
        """        delete m_pHWDevice;
        m_pHWDevice = NULL;
        ExReInitializeRundownProtection(&m_HardwareOperations);""",
        """        ExReInitializeRundownProtection(&m_HardwareOperations);
        delete m_pHWDevice;
        m_pHWDevice = NULL;""",
    ),
    Rewrite(
        "R99_failed_start_reopens_after_close_failure",
        "viogpu/viogpudo/viogpudo.cpp",
        """    }
    return NT_SUCCESS(closeStatus) ? failureStatus : closeStatus;
}""",
        """    }
    else
    {
        ExReInitializeRundownProtection(&m_HardwareOperations);
        m_HardwareRundownCompleted = FALSE;
    }
    return NT_SUCCESS(closeStatus) ? failureStatus : closeStatus;
}""",
    ),
    Rewrite(
        "R100_failed_start_bypasses_shared_unwind",
        "viogpu/viogpudo/viogpudo.cpp",
        """        DbgPrint(TRACE_LEVEL_ERROR, ("HWInit failed with status 0x%X\\n", Status));
        return UnwindFailedStart(Status);""",
        """        DbgPrint(TRACE_LEVEL_ERROR, ("HWInit failed with status 0x%X\\n", Status));
        delete m_pHWDevice;
        m_pHWDevice = NULL;
        return Status;""",
    ),
    Rewrite(
        "R101_start_rejects_stopped_restart",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (startResetState == VioGpuHardwareResetRequested)
    {
        startResetState = InterlockedCompareExchange(&m_HardwareResetState,
                                                     VioGpuHardwareRecovering,
                                                     VioGpuHardwareResetRequested);
    }""",
        """    if (startResetState == VioGpuHardwareResetRequested)
    {
        return STATUS_DEVICE_NOT_READY;
    }""",
    ),
    Rewrite(
        "R102_reset_callback_skips_final_publication",
        "viogpu/viogpudo/viogpudo.cpp",
        """            adapter->ResetDevice();
            InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);""",
        """            adapter->ResetDevice();""",
    ),
    Rewrite(
        "R103_system_display_skips_final_publication",
        "viogpu/viogpudo/viogpudo.cpp",
        """    BOOLEAN reset = adapter != NULL && adapter->ResetToVgaMode();
    InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
    ExReleaseRundownProtection(&m_HardwareOperations);""",
        """    BOOLEAN reset = adapter != NULL && adapter->ResetToVgaMode();
    ExReleaseRundownProtection(&m_HardwareOperations);""",
    ),
    Rewrite(
        "R104_line_interrupt_programs_msix_config",
        "viogpu/common/viogpu_pci.cpp",
        "    if (pdev->IsMSIEnabled())",
        "    if (TRUE)",
    ),
    Rewrite(
        "R105_line_interrupt_drops_config_bit",
        "viogpu/viogpudo/viogpudo.cpp",
        "    if ((isrstat & VIRTIO_PCI_ISR_CONFIG) != 0)",
        "    if (FALSE)",
    ),
    Rewrite(
        "R106_line_interrupt_config_requires_combined_status",
        "viogpu/viogpudo/viogpudo.cpp",
        "    if ((isrstat & VIRTIO_PCI_ISR_CONFIG) != 0)",
        "    if (isrstat == 3)",
    ),
    Rewrite(
        "R107_line_interrupt_config_overwrites_queue_reasons",
        "viogpu/viogpudo/viogpudo.cpp",
        "        intReason |= ISR_REASON_CHANGE;",
        "        intReason = ISR_REASON_CHANGE;",
    ),
    Rewrite(
        "R108_line_interrupt_drops_cursor_queue_reason",
        "viogpu/viogpudo/viogpudo.cpp",
        "        intReason |= ISR_REASON_DISPLAY | ISR_REASON_CURSOR;",
        "        intReason |= ISR_REASON_DISPLAY;",
    ),
    Rewrite(
        "R109_line_interrupt_claims_zero_status",
        "viogpu/viogpudo/viogpudo.cpp",
        "    BOOLEAN serviced = intReason != 0;",
        "    BOOLEAN serviced = TRUE;",
    ),
    Rewrite(
        "R110_line_interrupt_reacknowledges_status",
        "viogpu/viogpudo/viogpudo.cpp",
        """        UCHAR isrstat = virtio_read_isr_status(&m_VioDev);

        if ((isrstat & 1U) != 0)""",
        """        UCHAR isrstat = virtio_read_isr_status(&m_VioDev);
        isrstat |= virtio_read_isr_status(&m_VioDev);

        if ((isrstat & 1U) != 0)""",
    ),
    Rewrite(
        "R111_line_interrupt_queue_requires_queue_only_status",
        "viogpu/viogpudo/viogpudo.cpp",
        "    if ((isrstat & 1U) != 0)",
        "    if (isrstat == 1)",
    ),
    Rewrite(
        "R112_line_interrupt_moves_config_bit",
        "VirtIO/virtio_pci.h",
        "#define VIRTIO_PCI_ISR_CONFIG     0x2",
        "#define VIRTIO_PCI_ISR_CONFIG     0x4",
    ),
    Rewrite(
        "R113_wddm_abi_adds_forward_version_macro",
        "viogpu/shared/viogpu_wddm_abi.h",
        "#define VIOGPU_WDDM_ABI_VERSION            0U",
        "#define VIOGPU_WDDM_ABI_VERSION            0U\n#define VIOGPU_WDDM_MIN_VERSION            0U",
    ),
    Rewrite(
        "R114_wddm_abi_advertises_unknown_capability",
        "viogpu/shared/viogpu_wddm_abi.h",
        "#define VIOGPU_WDDM_CAPABILITIES_NONE      0ULL",
        "#define VIOGPU_WDDM_CAPABILITIES_NONE      1ULL",
    ),
    Rewrite(
        "R115_reset_generation_stops_advancing",
        "viogpu/viogpudo/viogpudo.cpp",
        """    ClearNativeContextReadiness();
    InterlockedIncrement(&m_NativeContextGeneration);
    InterlockedIncrement64(&m_NativeContextResetGeneration);
    InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);""",
        """    ClearNativeContextReadiness();
    InterlockedIncrement(&m_NativeContextGeneration);
    InterlockedExchange(&m_InterruptDispatchEnabled, FALSE);""",
    ),
    Rewrite(
        "R116_umd_private_accepts_input",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (adapter == NULL || queryAdapterInfo->pInputData != NULL || queryAdapterInfo->InputDataSize != 0 ||
        queryAdapterInfo->pOutputData == NULL || queryAdapterInfo->OutputDataSize != sizeof(VIOGPU_WDDM_ADAPTER_INFO))""",
        """    if (adapter == NULL ||
        queryAdapterInfo->pOutputData == NULL || queryAdapterInfo->OutputDataSize != sizeof(VIOGPU_WDDM_ADAPTER_INFO))""",
    ),
    Rewrite(
        "R117_context_accepts_zero_reset_generation",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (!IsCurrentAbiHeader(&privateData.Header, sizeof(privateData)) || privateData.ExpectedResetGeneration == 0 ||
        privateData.Flags != VIOGPU_WDDM_CONTEXT_FLAGS_NONE || privateData.Reserved != 0)""",
        """    if (!IsCurrentAbiHeader(&privateData.Header, sizeof(privateData)) ||
        privateData.Flags != VIOGPU_WDDM_CONTEXT_FLAGS_NONE || privateData.Reserved != 0)""",
    ),
    Rewrite(
        "R118_render_allows_short_patch_stream",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "header->CommandStreamOffset != referencesEnd || header->CommandStreamSize < sizeof(ULONGLONG) ||",
        "header->CommandStreamOffset != referencesEnd ||",
    ),
    Rewrite(
        "R119_render_skips_allocation_extent",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            reference->AllocationOffset > deviceAllocation->Allocation->PrivateData.Size ||
            reference->Length > deviceAllocation->Allocation->PrivateData.Size - reference->AllocationOffset ||""",
        """            reference->AllocationOffset > deviceAllocation->Allocation->PrivateData.Size ||""",
    ),
    Rewrite(
        "R120_render_decouples_patch_identity",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "reference->PatchOffset != patch->PatchOffset - header->CommandStreamOffset ||",
        "reference->PatchOffset == patch->PatchOffset - header->CommandStreamOffset ||",
    ),
    Rewrite(
        "R121_render_accepts_reserved_patch_bits",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "reference->PatchOffset > header->CommandStreamSize - sizeof(ULONGLONG) || patch->Reserved != 0 ||",
        "reference->PatchOffset > header->CommandStreamSize - sizeof(ULONGLONG) ||",
    ),
    Rewrite(
        "R122_context_reads_output_handle",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "context->RuntimeContext = NULL;",
        "context->RuntimeContext = createContext->hContext;",
    ),
    Rewrite(
        "R123_render_drops_command_bound",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "        render->CommandLength > VIOGPU_WDDM_DMA_BUFFER_SIZE || render->pDmaBuffer == NULL ||",
        "        render->pDmaBuffer == NULL ||",
    ),
    Rewrite(
        "R124_render_validates_mutable_command",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "status = ValidateCommandHeader(command,",
        "status = ValidateCommandHeader(reinterpret_cast<const VIOGPU_WDDM_RENDER_COMMAND *>(render->pCommand),",
    ),
    Rewrite(
        "R125_render_publishes_before_generation_check",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (NT_SUCCESS(status) &&
        !snapshot.Adapter->IsNativeContextGenerationCurrent(snapshot.Generation, snapshot.ResetGeneration))""",
        """    render->MultipassOffset = render->CommandLength;
    if (NT_SUCCESS(status) &&
        !snapshot.Adapter->IsNativeContextGenerationCurrent(snapshot.Generation, snapshot.ResetGeneration))""",
    ),
    Rewrite(
        "R126_render_skips_generation_check",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (NT_SUCCESS(status) &&
        !snapshot.Adapter->IsNativeContextGenerationCurrent(snapshot.Generation, snapshot.ResetGeneration))""",
        """    if (FALSE &&
        !snapshot.Adapter->IsNativeContextGenerationCurrent(snapshot.Generation, snapshot.ResetGeneration))""",
    ),
    Rewrite(
        "R127_allocation_drops_private_snapshot",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "        allocation->PrivateData = privateData;",
        "        RtlZeroMemory(&allocation->PrivateData, sizeof(allocation->PrivateData));",
    ),
    Rewrite(
        "R128_open_allocation_skips_private_identity",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "        if (RtlCompareMemory(&privateData, &allocation->PrivateData, sizeof(privateData)) != sizeof(privateData))",
        "        if (FALSE)",
    ),
    Rewrite(
        "R129_context_treats_affinity_as_ordinal",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "        createContext->NodeOrdinal != 0 || createContext->EngineAffinity != 1 || createContext->Flags.Value != 0 ||",
        "        createContext->NodeOrdinal != 0 || createContext->EngineAffinity != 0 || createContext->Flags.Value != 0 ||",
    ),
    Rewrite(
        "R130_render_leaks_patch_snapshot",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "    delete[] patchSnapshot;",
        "    patchSnapshot = NULL;",
    ),
    Rewrite(
        "R131_render_leaks_command_snapshot",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "    delete[] commandSnapshot;",
        "    commandSnapshot = NULL;",
    ),
    Rewrite(
        "R132_patch_fakes_vidmm_iova",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    UNREFERENCED_PARAMETER(patchArguments);
    return STATUS_NOT_SUPPORTED;""",
        """    if (patchArguments != NULL && patchArguments->pAllocationList != NULL)
    {
        ULONGLONG gpuAddress = patchArguments->pAllocationList[0].PhysicalAddress.QuadPart;
        UNREFERENCED_PARAMETER(gpuAddress);
    }
    return STATUS_SUCCESS;""",
    ),
    Rewrite(
        "R133_render_copies_mutable_command_to_output",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "            RtlCopyMemory(commandSnapshot, render->pCommand, render->CommandLength);",
        "            RtlCopyMemory(render->pDmaBuffer, render->pCommand, render->CommandLength);",
    ),
    Rewrite(
        "R134_render_copies_mutable_patch_to_output",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "            RtlCopyMemory(patchSnapshot, render->pPatchLocationListIn, patchBytes);",
        "            RtlCopyMemory(render->pPatchLocationListOut, render->pPatchLocationListIn, patchBytes);",
    ),
    Rewrite(
        "R135_render_accepts_overlapping_patch_slots",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            if (patch->PatchOffset < previousPatch->PatchOffset + sizeof(ULONGLONG) &&
                previousPatch->PatchOffset < patch->PatchOffset + sizeof(ULONGLONG))""",
        """            if (FALSE && patch->PatchOffset < previousPatch->PatchOffset + sizeof(ULONGLONG) &&
                previousPatch->PatchOffset < patch->PatchOffset + sizeof(ULONGLONG))""",
    ),
    Rewrite(
        "R136_open_allocation_publishes_inapplicable_outputs",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmCloseAllocation""",
        """    DXGKARG_OPENALLOCATION *mutableOpenAllocation = const_cast<DXGKARG_OPENALLOCATION *>(openAllocation);
    mutableOpenAllocation->SubresourceOffset = 0;
    mutableOpenAllocation->Pitch = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmCloseAllocation""",
    ),
    Rewrite(
        "R137_umd_private_skips_full_output_initialization",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "    InitializeAbiHeader(&adapterInfo->Header, sizeof(*adapterInfo));",
        "    InitializeAbiHeader(&adapterInfo->Header, sizeof(adapterInfo->Header));",
    ),
    Rewrite(
        "R138_context_probes_dxgkrnl_private_data_as_user_address",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "        RtlCopyMemory(&privateData, createContext->pPrivateDriverData, sizeof(privateData));",
        """        ProbeForRead(createContext->pPrivateDriverData, sizeof(privateData), __alignof(VIOGPU_WDDM_CONTEXT_CREATE));
        RtlCopyMemory(&privateData, createContext->pPrivateDriverData, sizeof(privateData));""",
    ),
    Rewrite(
        "R139_render_accepts_cross_device_open",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            deviceAllocation->Device != device || deviceAllocation->Allocation == NULL ||""",
        """            deviceAllocation->Allocation == NULL ||""",
    ),
    Rewrite(
        "R140_render_writes_read_only_open",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            (deviceAllocation->ReadOnly && (reference->Flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0))""",
        """            FALSE)""",
    ),
    Rewrite(
        "R141_render_uses_page_aligned_backing_extent",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            reference->AllocationOffset > deviceAllocation->Allocation->PrivateData.Size ||
            reference->Length > deviceAllocation->Allocation->PrivateData.Size - reference->AllocationOffset ||""",
        """            reference->AllocationOffset > deviceAllocation->Allocation->BackingSize ||
            reference->Length > deviceAllocation->Allocation->BackingSize - (SIZE_T)reference->AllocationOffset ||""",
    ),
    Rewrite(
        "R142_close_accepts_duplicate_handles",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            if (closeAllocation->pOpenHandleList[previousIndex] == closeAllocation->pOpenHandleList[index])""",
        """            if (FALSE)""",
    ),
    Rewrite(
        "R143_destroy_accepts_duplicate_handles",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            if (destroyAllocation->pAllocationList[previousIndex] == destroyAllocation->pAllocationList[index])""",
        """            if (FALSE)""",
    ),
    Rewrite(
        "R144_wddm_abi_imports_user_crt_header",
        "viogpu/shared/viogpu_wddm_abi.h",
        "#define VIOGPU_WDDM_ABI_H\n",
        "#define VIOGPU_WDDM_ABI_H\n\n#include <stdint.h>\n",
    ),
    Rewrite(
        "R145_wddm_abi_uint32_loses_width",
        "viogpu/shared/viogpu_wddm_abi.h",
        "typedef unsigned int VIOGPU_WDDM_UINT32;",
        "typedef unsigned long long VIOGPU_WDDM_UINT32;",
    ),
    Rewrite(
        "R146_render_uses_nonexistent_patch_type",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "    D3DDDI_PATCHLOCATIONLIST *patchSnapshot = NULL;",
        "    D3DDI_PATCHLOCATIONLIST *patchSnapshot = NULL;",
    ),
    Rewrite(
        "R147_context_info_bypasses_context_rundown",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    VIOGPU_WDDM_CONTEXT *context = reinterpret_cast<VIOGPU_WDDM_CONTEXT *>(escape->hContext);
    if (!ExAcquireRundownProtection(&context->Operations))""",
        """    VIOGPU_WDDM_CONTEXT *context = reinterpret_cast<VIOGPU_WDDM_CONTEXT *>(escape->hContext);
    if (FALSE && !ExAcquireRundownProtection(&context->Operations))""",
    ),
    Rewrite(
        "R148_context_info_accepts_cross_device_context",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE || context->Device != device ||
        device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE || device->Adapter != adapter)""",
        """    if (context->Signature != VIOGPU_WDDM_CONTEXT_SIGNATURE ||
        device->Signature != VIOGPU_WDDM_DEVICE_SIGNATURE || device->Adapter != adapter)""",
    ),
    Rewrite(
        "R149_context_info_uses_capset_va",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """        snapshotAcquired = TRUE;
        ULONGLONG vaEnd = snapshot.VaStart + snapshot.VaSize;""",
        """        snapshotAcquired = TRUE;
        GPU_CAPSET_DRM capset = {};
        if (NT_SUCCESS(adapter->QueryNativeContextReadiness(&capset, NULL, NULL, NULL)))
        {
            snapshot.VaStart = capset.msm.va_start;
            snapshot.VaSize = capset.msm.va_size;
        }
        ULONGLONG vaEnd = snapshot.VaStart + snapshot.VaSize;""",
    ),
    Rewrite(
        "R150_context_info_accepts_nonzero_output_fields",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (request.Flags != VIOGPU_WDDM_ESCAPE_FLAGS_NONE || request.ExpectedResetGeneration == 0 ||
        request.VaStart != 0 || request.VaSize != 0 || request.ResetGeneration != 0)""",
        """    if (request.Flags != VIOGPU_WDDM_ESCAPE_FLAGS_NONE || request.ExpectedResetGeneration == 0)""",
    ),
    Rewrite(
        "R151_context_info_accepts_zero_va",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """        if (snapshot.ResetGeneration != request.ExpectedResetGeneration || snapshot.VaStart == 0 ||
            snapshot.VaSize == 0 || (snapshot.VaStart & (PAGE_SIZE - 1)) != 0 ||""",
        """        if (snapshot.ResetGeneration != request.ExpectedResetGeneration ||
            (snapshot.VaStart & (PAGE_SIZE - 1)) != 0 ||""",
    ),
    Rewrite(
        "R152_destroy_context_leaves_va",
        "viogpu/viogpudo/viogpudo.cpp",
        """    context->ContextId = 0;
    context->VaStart = 0;
    context->VaSize = 0;
    InterlockedExchange(&context->State, VioGpuNativeContextDead);
    KeReleaseSpinLock(&context->BindingLock, oldIrql);
    *released = TRUE;""",
        """    context->ContextId = 0;
    InterlockedExchange(&context->State, VioGpuNativeContextDead);
    KeReleaseSpinLock(&context->BindingLock, oldIrql);
    *released = TRUE;""",
    ),
    Rewrite(
        "R153_reset_context_leaves_va",
        "viogpu/viogpudo/viogpudo.cpp",
        """            context->Registered = FALSE;
            context->Adapter = NULL;
            context->Owner = NULL;
            context->Generation = 0;
            context->ResetGeneration = 0;
            context->ContextId = 0;
            context->VaStart = 0;
            context->VaSize = 0;
            InterlockedExchange(&context->State, VioGpuNativeContextDead);""",
        """            context->Registered = FALSE;
            context->Adapter = NULL;
            context->Owner = NULL;
            context->Generation = 0;
            context->ResetGeneration = 0;
            context->ContextId = 0;
            InterlockedExchange(&context->State, VioGpuNativeContextDead);""",
    ),
    Rewrite(
        "R154_escape_callback_bypasses_wrapper",
        "viogpu/viogpuwddm/driver_entry.cpp",
        "    initialData->DxgkDdiEscape = VioGpuWddmEscape;",
        "    initialData->DxgkDdiEscape = VioGpuDodEscape;",
    ),
    Rewrite(
        "R155_control_seed_allows_special_apc",
        "viogpu/viogpudo/viogpudo.cpp",
        """    VioGpuDrmHostPoolMapping mapping;
    KeEnterGuardedRegion();
    BOOLEAN acquired = adapter->AcquireDrmHostPoolMapping(&mapping);
    PMSM_SHMEM shmem = NULL;
    PMSM_CCMD_IOCTL_SIMPLE_GET_PARAM_RSP response = NULL;""",
        """    VioGpuDrmHostPoolMapping mapping;
    KeEnterCriticalRegion();
    BOOLEAN acquired = adapter->AcquireDrmHostPoolMapping(&mapping);
    PMSM_SHMEM shmem = NULL;
    PMSM_CCMD_IOCTL_SIMPLE_GET_PARAM_RSP response = NULL;""",
    ),
    Rewrite(
        "R156_control_consume_allows_special_apc",
        "viogpu/viogpudo/viogpudo.cpp",
        """    VioGpuDrmHostPoolMapping mapping;
    KeEnterGuardedRegion();
    BOOLEAN acquired = adapter->AcquireDrmHostPoolMapping(&mapping);
    PMSM_SHMEM shmem = NULL;
    PMSM_CCMD_IOCTL_SIMPLE_GET_PARAM_RSP sharedResponse = NULL;""",
        """    VioGpuDrmHostPoolMapping mapping;
    KeEnterCriticalRegion();
    BOOLEAN acquired = adapter->AcquireDrmHostPoolMapping(&mapping);
    PMSM_SHMEM shmem = NULL;
        PMSM_CCMD_IOCTL_SIMPLE_GET_PARAM_RSP sharedResponse = NULL;""",
    ),
    Rewrite(
        "R157_legacy_resource_ids_enter_native_range",
        "viogpu/viogpudo/viogpudo.cpp",
        "m_Idr.Init(1, VIOGPU_NATIVE_RESOURCE_ID_START)",
        "m_Idr.Init(1, MAXULONG)",
    ),
    Rewrite(
        "R158_legacy_resource_id_drops_upper_bound",
        "viogpu/common/viogpu_idr.cpp",
        "else if (m_nextId != 0 && m_nextId < m_endId)",
        "else if (m_nextId != 0)",
    ),
    Rewrite(
        "R159_legacy_resource_id_increment_is_unlocked",
        "viogpu/common/viogpu_idr.cpp",
        """    FreeId *freeId = NULL;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    if (m_endId != 0 && !IsListEmpty(&m_freeList))""",
        """    FreeId *freeId = NULL;

    KIRQL oldIrql;
    if (m_endId != 0 && !IsListEmpty(&m_freeList))""",
    ),
    Rewrite(
        "R160_guest_pool_uses_host_identity",
        "viogpu/common/viogpu_named_pool.cpp",
        'static const CHAR VIOGPU_GUEST_POOL_NAME[] = "gpu_guest";',
        'static const CHAR VIOGPU_GUEST_POOL_NAME[] = "drm2kgsl_host";',
    ),
    Rewrite(
        "R161_full_wddm_segment_uses_restricted_dma",
        "viogpu/viogpudo/viogpudo.cpp",
        "    return m_GpuGuestPool.QueryPhysicalRange(physicalAddress, size);",
        "    PVOID baseAddress = NULL;\n    return m_RdmaPool.QueryVidMmSegment(&baseAddress, physicalAddress, size);",
    ),
    Rewrite(
        "R162_transport_skips_guest_pool_connect",
        "viogpu/viogpudo/viogpudo.cpp",
        """    status = ConnectGpuGuestPool();
    if (!NT_SUCCESS(status))
    {
        return status;
    }""",
        """    status = STATUS_SUCCESS;
    if (!NT_SUCCESS(status))
    {
        return status;
    }""",
    ),
    Rewrite(
        "R163_ready_ignores_guest_pool",
        "viogpu/viogpudo/viogpudo.cpp",
        """#if defined(VIOGPU_WDDM_CI_ONLY)
                    m_DrmHostPool.IsActive() && m_GpuGuestPool.IsActive() &&
#endif""",
        """#if defined(VIOGPU_WDDM_CI_ONLY)
                    m_DrmHostPool.IsActive() &&
#endif""",
    ),
    Rewrite(
        "R164_physical_range_skips_generation_recheck",
        "viogpu/common/viogpu_named_pool.cpp",
        """                    (ULONG64)rangeBase.QuadPart <= MAXULONGLONG - (rangeSize - 1) &&
                    generation == InterlockedCompareExchange64(&m_Generation, 0, 0) && IsActive();""",
        """                    (ULONG64)rangeBase.QuadPart <= MAXULONGLONG - (rangeSize - 1) && IsActive();""",
    ),
    Rewrite(
        "R165_disconnects_host_before_guest_pool",
        "viogpu/viogpudo/viogpudo.cpp",
        """    status = m_GpuGuestPool.Disconnect();
    if (!NT_SUCCESS(status))
    {
        FailNativeContextAtAnyIrql();
        return status;
    }
    status = m_DrmHostPool.Disconnect();""",
        """    status = m_DrmHostPool.Disconnect();
    if (!NT_SUCCESS(status))
    {
        FailNativeContextAtAnyIrql();
        return status;
    }
    status = m_GpuGuestPool.Disconnect();""",
    ),
    Rewrite(
        "R166_guest_connect_targets_host_pool",
        "viogpu/viogpudo/viogpudo.cpp",
        "    return ConnectNamedPoolWithRetry(&m_GpuGuestPool);",
        "    return ConnectNamedPoolWithRetry(&m_DrmHostPool);",
    ),
    Rewrite(
        "R167_callback_waits_for_cleanup",
        "viogpu/common/viogpu_named_pool.cpp",
        """    if (queueCleanup)
    {
        QueuePnpCleanup();
    }""",
        """    if (queueCleanup)
    {
        KeWaitForSingleObject(&m_PnpCleanupComplete, Executive, KernelMode, FALSE, NULL);
        QueuePnpCleanup();
    }""",
    ),
    Rewrite(
        "R168_query_remove_stops_vetoing",
        "viogpu/common/viogpu_named_pool.cpp",
        """    return targetOwned && pnpState == VioGpuNamedPoolConnected ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;""",
        """    UNREFERENCED_PARAMETER(notificationFileObject);
    UNREFERENCED_PARAMETER(currentFileObject);
    return STATUS_SUCCESS;""",
    ),
    Rewrite(
        "R169_surprise_remove_skips_adapter_failure",
        "viogpu/common/viogpu_named_pool.cpp",
        "        m_FailureCallback(m_FailureContext);",
        "        UNREFERENCED_PARAMETER(m_FailureContext);",
    ),
    Rewrite(
        "R170_full_stable_segment_branches_swapped",
        "viogpu/viogpudo/viogpudo.cpp",
        """#if defined(VIOGPU_WDDM_CI_ONLY)
    return m_GpuGuestPool.QueryPhysicalRange(physicalAddress, size);
#else
    PVOID baseAddress = NULL;
    return m_RdmaPool.QueryVidMmSegment(&baseAddress, physicalAddress, size);
#endif""",
        """#if defined(VIOGPU_WDDM_CI_ONLY)
    PVOID baseAddress = NULL;
    return m_RdmaPool.QueryVidMmSegment(&baseAddress, physicalAddress, size);
#else
    return m_GpuGuestPool.QueryPhysicalRange(physicalAddress, size);
#endif""",
    ),
    Rewrite(
        "R171_cleanup_generation_precedes_event_clear",
        "viogpu/common/viogpu_named_pool.cpp",
        """    InterlockedExchange(&m_PnpCleanupStatus, STATUS_PENDING);
    KeClearEvent(&m_PnpCleanupComplete);
    InterlockedIncrement(&m_PnpCleanupGeneration);""",
        """    InterlockedExchange(&m_PnpCleanupStatus, STATUS_PENDING);
    InterlockedIncrement(&m_PnpCleanupGeneration);
    KeClearEvent(&m_PnpCleanupComplete);""",
    ),
    Rewrite(
        "R172_pool_loss_leaves_outer_ddi_active",
        "viogpu/viogpudo/viogpudo.cpp",
        "            dod->RequestHardwareResetAtAnyIrql();",
        "            UNREFERENCED_PARAMETER(dod);",
    ),
    Rewrite(
        "R173_connect_skips_connecting_state",
        "viogpu/common/viogpu_named_pool.cpp",
        "    InterlockedExchange(&m_PnpState, VioGpuNamedPoolConnecting);",
        "    InterlockedExchange(&m_PnpState, VioGpuNamedPoolConnected);",
    ),
    Rewrite(
        "R174_connect_seeds_generation_after_register",
        "viogpu/common/viogpu_named_pool.cpp",
        "    LONG64 registrationGeneration = InterlockedIncrement64(&m_Generation);",
        "    LONG64 registrationGeneration = InterlockedCompareExchange64(&m_Generation, 0, 0);",
    ),
    Rewrite(
        "R175_connect_drops_postready_recheck",
        "viogpu/common/viogpu_named_pool.cpp",
        """        commitValid = InterlockedCompareExchange(&m_PnpState, 0, 0) == VioGpuNamedPoolConnected &&
                      InterlockedCompareExchange(&m_RemovalLatched, FALSE, FALSE) == FALSE &&
                      InterlockedCompareExchange64(&m_Generation, 0, 0) == registrationGeneration;""",
        """        commitValid = InterlockedCompareExchange(&m_PnpState, 0, 0) == VioGpuNamedPoolConnected;""",
    ),
    Rewrite(
        "R176_connecting_remove_uses_connected_cas",
        "viogpu/common/viogpu_named_pool.cpp",
        """    LONG previousState = InterlockedCompareExchange(&m_PnpState, VioGpuNamedPoolFailed, VioGpuNamedPoolConnecting);
    BOOLEAN queueCleanup = FALSE;""",
        """    LONG previousState = InterlockedCompareExchange(&m_PnpState, VioGpuNamedPoolFailed, VioGpuNamedPoolConnected);
    BOOLEAN queueCleanup = FALSE;""",
    ),
    Rewrite(
        "R177_connecting_remove_queues_worker",
        "viogpu/common/viogpu_named_pool.cpp",
        """    if (queueCleanup)
    {
        QueuePnpCleanup();
    }""",
        """    QueuePnpCleanup();""",
    ),
    Rewrite(
        "R178_connect_drops_pre_registration_file_publish",
        "viogpu/common/viogpu_named_pool.cpp",
        "    InterlockedExchangePointer((PVOID volatile *)&m_FileObject, selectedConnection.FileObject);",
        "    m_FileObject = selectedConnection.FileObject;",
    ),
    Rewrite(
        "R179_query_remove_allows_connected_commit_window",
        "viogpu/common/viogpu_named_pool.cpp",
        "    return targetOwned && pnpState == VioGpuNamedPoolConnected ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;",
        "    return targetOwned && IsActive() ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;",
    ),
    Rewrite(
        "R180_register_failure_drops_callback_join",
        "viogpu/common/viogpu_named_pool.cpp",
        """        ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);
        ExRundownCompleted(&m_NotificationCallbacks);
        ReleaseNamedPoolConnection(&selectedConnection);""",
        """        ExReleaseRundownProtection(&m_NotificationCallbacks);
        ExRundownCompleted(&m_NotificationCallbacks);
        ReleaseNamedPoolConnection(&selectedConnection);""",
    ),
    Rewrite(
        "R181_query_remove_ignores_shutdown",
        "viogpu/common/viogpu_named_pool.cpp",
        """    if (InterlockedCompareExchange(&m_ShuttingDown, FALSE, FALSE) != FALSE)
    {
        return STATUS_SUCCESS;
    }

    LONG pnpState = InterlockedCompareExchange(&m_PnpState, 0, 0);""",
        """    LONG pnpState = InterlockedCompareExchange(&m_PnpState, 0, 0);""",
    ),
    Rewrite(
        "R182_connect_publishes_state_before_latch_clear",
        "viogpu/common/viogpu_named_pool.cpp",
        """    InterlockedExchange(&m_RemovalLatched, FALSE);
    InterlockedExchange(&m_Ready, FALSE);
    InterlockedExchange(&m_PnpState, VioGpuNamedPoolConnecting);""",
        """    InterlockedExchange(&m_PnpState, VioGpuNamedPoolConnecting);
    InterlockedExchange(&m_RemovalLatched, FALSE);
    InterlockedExchange(&m_Ready, FALSE);""",
    ),
    Rewrite(
        "R183_register_failure_releases_before_join",
        "viogpu/common/viogpu_named_pool.cpp",
        """        ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);
        ExRundownCompleted(&m_NotificationCallbacks);
        ReleaseNamedPoolConnection(&selectedConnection);""",
        """        ReleaseNamedPoolConnection(&selectedConnection);
        ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);
        ExRundownCompleted(&m_NotificationCallbacks);""",
    ),
    Rewrite(
        "R184_connect_commits_before_owner_transfer",
        "viogpu/common/viogpu_named_pool.cpp",
        """    m_DirectInterface = selectedConnection.DirectInterface;
    m_BasePA = selectedConnection.Query.BasePhysicalAddress;
    m_Size = (SIZE_T)selectedConnection.Query.TotalSize;
    RtlZeroMemory(&selectedName, sizeof(selectedName));
    RtlZeroMemory(&selectedConnection, sizeof(selectedConnection));

    LONG previousState = InterlockedCompareExchange(&m_PnpState, VioGpuNamedPoolConnected, VioGpuNamedPoolConnecting);""",
        """    LONG previousState = InterlockedCompareExchange(&m_PnpState, VioGpuNamedPoolConnected, VioGpuNamedPoolConnecting);
    m_DirectInterface = selectedConnection.DirectInterface;
    m_BasePA = selectedConnection.Query.BasePhysicalAddress;
    m_Size = (SIZE_T)selectedConnection.Query.TotalSize;
    RtlZeroMemory(&selectedName, sizeof(selectedName));
        RtlZeroMemory(&selectedConnection, sizeof(selectedConnection));""",
    ),
    Rewrite(
        "R185_register_failure_skips_rundown_completion",
        "viogpu/common/viogpu_named_pool.cpp",
        """        ExRundownCompleted(&m_NotificationCallbacks);
        ReleaseNamedPoolConnection(&selectedConnection);""",
        """        ReleaseNamedPoolConnection(&selectedConnection);""",
    ),
    Rewrite(
        "R186_disconnect_skips_operations_completion",
        "viogpu/common/viogpu_named_pool.cpp",
        """        ExWaitForRundownProtectionRelease(&m_Operations);
        ExRundownCompleted(&m_Operations);""",
        """        ExWaitForRundownProtectionRelease(&m_Operations);""",
    ),
    Rewrite(
        "R187_disconnect_skips_callback_completion",
        "viogpu/common/viogpu_named_pool.cpp",
        """        ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);
        ExRundownCompleted(&m_NotificationCallbacks);
        m_NotificationRundownCompleted = TRUE;""",
        """        ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);
        m_NotificationRundownCompleted = TRUE;""",
    ),
    Rewrite(
        "R188_query_remove_ignores_shutdown_gate",
        "viogpu/common/viogpu_named_pool.cpp",
        """    if (InterlockedCompareExchange(&m_ShuttingDown, FALSE, FALSE) != FALSE)
    {
        return STATUS_SUCCESS;
    }

    LONG pnpState = InterlockedCompareExchange(&m_PnpState, 0, 0);""",
        """    LONG pnpState = InterlockedCompareExchange(&m_PnpState, 0, 0);""",
    ),
    Rewrite(
        "R189_has_owner_reads_pnp_state_non_atomically",
        "viogpu/common/viogpu_named_pool.cpp",
        """    LockState();
    LONG pnpState = InterlockedCompareExchange(&m_PnpState, 0, 0);
    PFILE_OBJECT fileObject = reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOID volatile *)&m_FileObject,
                                                                                               NULL,
                                                                                               NULL));""",
        """    LockState();
    LONG pnpState = m_PnpState;
    PFILE_OBJECT fileObject = reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOID volatile *)&m_FileObject,
                                                                                               NULL,
                                                                                               NULL));""",
    ),
    Rewrite(
        "R190_connect_reads_pnp_state_non_atomically",
        "viogpu/common/viogpu_named_pool.cpp",
        """    if (IsActive())
    {
        UnlockState();
        return STATUS_SUCCESS;
    }
    LONG pnpState = InterlockedCompareExchange(&m_PnpState, 0, 0);""",
        """    if (IsActive())
    {
        UnlockState();
        return STATUS_SUCCESS;
    }
    LONG pnpState = m_PnpState;""",
    ),
    Rewrite(
        "R191_acquire_reads_file_object_non_atomically",
        "viogpu/common/viogpu_named_pool.cpp",
        """    BOOLEAN acquired = FALSE;
    LONG64 generation = InterlockedCompareExchange64(&m_Generation, 0, 0);
    PFILE_OBJECT fileObject = reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOID volatile *)&m_FileObject,
                                                                                               NULL,
                                                                                               NULL));""",
        """    BOOLEAN acquired = FALSE;
    LONG64 generation = InterlockedCompareExchange64(&m_Generation, 0, 0);
    PFILE_OBJECT fileObject = m_FileObject;""",
    ),
    Rewrite(
        "R192_rdma_disconnect_skips_operations_completion",
        "viogpu/common/viogpu_rdma.cpp",
        """        ExWaitForRundownProtectionRelease(&m_Operations);
        ExRundownCompleted(&m_Operations);
        m_RundownCompleted = TRUE;""",
        """        ExWaitForRundownProtectionRelease(&m_Operations);
        m_RundownCompleted = TRUE;""",
    ),
    Rewrite(
        "R193_adapter_destructor_skips_native_context_completion",
        "viogpu/viogpudo/viogpudo.cpp",
        """    ExWaitForRundownProtectionRelease(&m_NativeContextReferences);
    ExRundownCompleted(&m_NativeContextReferences);
    CloseResolutionEvent();""",
        """    ExWaitForRundownProtectionRelease(&m_NativeContextReferences);
    CloseResolutionEvent();""",
    ),
    Rewrite(
        "R194_provider_release_skips_mapping_completion",
        "droidvmpool/droidvmpool.c",
        """        ExWaitForRundownProtectionRelease(&deviceContext->MappingReferences);
        ExRundownCompleted(&deviceContext->MappingReferences);
        deviceContext->MappingRundownCompleted = TRUE;""",
        """        ExWaitForRundownProtectionRelease(&deviceContext->MappingReferences);
        deviceContext->MappingRundownCompleted = TRUE;""",
    ),
    Rewrite(
        "R195_disconnect_bypasses_cleanup_claim",
        "viogpu/common/viogpu_named_pool.cpp",
        """    NTSTATUS waitStatus = BeginPnpCleanupTeardown();""",
        """    NTSTATUS waitStatus = KeWaitForSingleObject(&m_PnpCleanupComplete, Executive, KernelMode, FALSE, NULL);""",
    ),
    Rewrite(
        "R196_queue_cleanup_skips_spin_lock",
        "viogpu/common/viogpu_named_pool.cpp",
        """void VioGpuNamedPool::QueuePnpCleanup(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PnpCleanupLock, &oldIrql);""",
        """void VioGpuNamedPool::QueuePnpCleanup(void)
{
    KIRQL oldIrql;
    oldIrql = PASSIVE_LEVEL;""",
    ),
    Rewrite(
        "R197_cleanup_publishes_after_event_clear",
        "viogpu/common/viogpu_named_pool.cpp",
        """    InterlockedExchange(&m_PnpCleanupQueued, VioGpuNamedPoolCleanupPublishing);
    InterlockedExchange(&m_PnpCleanupWorkerState, VioGpuNamedPoolWorkerRunning);
    InterlockedExchange(&m_PnpCleanupStatus, STATUS_PENDING);
    KeClearEvent(&m_PnpCleanupComplete);""",
        """    InterlockedExchange(&m_PnpCleanupWorkerState, VioGpuNamedPoolWorkerRunning);
    InterlockedExchange(&m_PnpCleanupStatus, STATUS_PENDING);
    KeClearEvent(&m_PnpCleanupComplete);
    InterlockedExchange(&m_PnpCleanupQueued, VioGpuNamedPoolCleanupPublishing);""",
    ),
    Rewrite(
        "R198_worker_skips_cleanup_claim",
        "viogpu/common/viogpu_named_pool.cpp",
        """    if (!pool->ClaimQueuedPnpCleanup())
    {
        ExReleaseRundownProtection(&pool->m_PnpCleanupWorkerReferences);
        return;
    }""",
        """    if (FALSE)
    {
        ExReleaseRundownProtection(&pool->m_PnpCleanupWorkerReferences);
        return;
    }""",
    ),
    Rewrite(
        "R199_register_failure_unlocks_before_callback_join",
        "viogpu/common/viogpu_named_pool.cpp",
        """        InterlockedExchange(&m_ShuttingDown, TRUE);

        /*
         * A failed registration may still have a callback in flight.""",
        """        InterlockedExchange(&m_ShuttingDown, TRUE);
        UnlockState();

        /*
         * A failed registration may still have a callback in flight.""",
    ),
    Rewrite(
        "R200_worker_releases_lifetime_before_completion",
        "viogpu/common/viogpu_named_pool.cpp",
        """    pool->CompletePnpCleanupTeardown(status, TRUE);
    /* The work item was dequeued before this routine ran; no pool access follows this release. */
    ExReleaseRundownProtection(&pool->m_PnpCleanupWorkerReferences);""",
        """    ExReleaseRundownProtection(&pool->m_PnpCleanupWorkerReferences);
    pool->CompletePnpCleanupTeardown(status, TRUE);""",
    ),
    Rewrite(
        "R201_waiter_skips_worker_rundown_completion",
        "viogpu/common/viogpu_named_pool.cpp",
        """            ExWaitForRundownProtectionRelease(&m_PnpCleanupWorkerReferences);
            ExRundownCompleted(&m_PnpCleanupWorkerReferences);""",
        """            ExWaitForRundownProtectionRelease(&m_PnpCleanupWorkerReferences);""",
    ),
    Rewrite(
        "R202_worker_completion_publishes_reusable_idle",
        "viogpu/common/viogpu_named_pool.cpp",
        """    if (!worker)
    {
        NT_ASSERT(InterlockedCompareExchange(&m_PnpCleanupWorkerState, 0, 0) == VioGpuNamedPoolWorkerIdle);
        InterlockedExchange(&m_PnpCleanupQueued, VioGpuNamedPoolCleanupIdle);
    }""",
        """    UNREFERENCED_PARAMETER(worker);
    InterlockedExchange(&m_PnpCleanupQueued, VioGpuNamedPoolCleanupIdle);""",
    ),
    Rewrite(
        "R203_queue_reinitializes_embedded_work_item",
        "viogpu/common/viogpu_named_pool.cpp",
        """    KeClearEvent(&m_PnpCleanupComplete);
    InterlockedIncrement(&m_PnpCleanupGeneration);""",
        """    KeClearEvent(&m_PnpCleanupComplete);
    ExInitializeWorkItem(&m_PnpCleanupWorkItem, PnpCleanupWorker, this);
    InterlockedIncrement(&m_PnpCleanupGeneration);""",
    ),
    Rewrite(
        "R204_waiter_skips_worker_rundown_reinitialize",
        "viogpu/common/viogpu_named_pool.cpp",
        "            ExReInitializeRundownProtection(&m_PnpCleanupWorkerReferences);",
        "",
    ),
    Rewrite(
        "R205_create_skips_context_pin",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            if (!VioGpuAdapter::ReferenceNativeContextAllocation(&snapshot, &nativeContext))
            {
                VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);""",
        """            if (FALSE)
            {
                VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);""",
    ),
    Rewrite(
        "R206_resident_allocation_relooks_up_va",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (!VioGpuAdapter::AcquireNativeContextSnapshot(allocation->NativeContext, snapshot))
    {
        return FALSE;
    }""",
        """    if (!allocation->Adapter->AcquireNativeContextSnapshotForAllocation(
            allocation->PrivateData.RequestedIova,
            allocation->BackingSize,
            allocation->PrivateData.ExpectedResetGeneration,
            allocation->PrivateData.ContextId,
            snapshot))
    {
        return FALSE;
    }""",
    ),
    Rewrite(
        "R207_destroy_context_ignores_allocation_refs",
        "viogpu/viogpudo/viogpudo.cpp",
        """    if (ReadNativeAllocationCount(owner) != 0 || context->AllocationReferences != 0 ||
        !IsListEmpty(&context->AllocationRanges))""",
        """    if (ReadNativeAllocationCount(owner) != 0 || !IsListEmpty(&context->AllocationRanges))""",
    ),
    Rewrite(
        "R208_validate_allocation_drops_ulong_backing_gate",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            (privateData->RequestedIova & (PAGE_SIZE - 1)) != 0 || privateData->ExpectedResetGeneration == 0 ||
            privateData->ContextId == 0 || localAlignedSize > MAXULONG ||
            privateData->RequestedIova > MAXULONGLONG - ((ULONGLONG)localAlignedSize - 1))""",
        """            (privateData->RequestedIova & (PAGE_SIZE - 1)) != 0 || privateData->ExpectedResetGeneration == 0 ||
            privateData->ContextId == 0 ||
            privateData->RequestedIova > MAXULONGLONG - ((ULONGLONG)localAlignedSize - 1))""",
    ),
    Rewrite(
        "R209_create_guest_drops_ulong_backing_gate",
        "viogpu/viogpudo/viogpudo.cpp",
        """        resourceId < VIOGPU_NATIVE_RESOURCE_ID_START || blobId == 0 || resourceId != blobId || logicalSize == 0 ||
        backingSize == 0 ||
        logicalSize > backingSize || backingSize < PAGE_SIZE || backingSize > MAXULONG ||
        (backingSize & (PAGE_SIZE - 1)) != 0 || logicalSize <= (ULONGLONG)backingSize - PAGE_SIZE ||""",
        """        resourceId < VIOGPU_NATIVE_RESOURCE_ID_START || blobId == 0 || resourceId != blobId || logicalSize == 0 ||
        backingSize == 0 ||
        logicalSize > backingSize || backingSize < PAGE_SIZE ||
        (backingSize & (PAGE_SIZE - 1)) != 0 || logicalSize <= (ULONGLONG)backingSize - PAGE_SIZE ||""",
    ),
    Rewrite(
        "R210_gem_new_drops_guest_alloc",
        "viogpu/viogpudo/viogpudo.cpp",
        "    request.flags = msmFlags | MSM_BO_GUEST_ALLOC;",
        "    request.flags = msmFlags;",
    ),
    Rewrite(
        "R232_create_guest_accepts_subpage_backing",
        "viogpu/viogpudo/viogpudo.cpp",
        "        logicalSize > backingSize || backingSize < PAGE_SIZE || backingSize > MAXULONG ||",
        "        logicalSize > backingSize || backingSize > MAXULONG ||",
    ),
    Rewrite(
        "R233_create_guest_accepts_unaligned_logical_size",
        "viogpu/viogpudo/viogpudo.cpp",
        "        (backingSize & (PAGE_SIZE - 1)) != 0 || logicalSize <= (ULONGLONG)backingSize - PAGE_SIZE ||",
        "        (backingSize & (PAGE_SIZE - 1)) != 0 ||",
    ),
    Rewrite(
        "R234_create_guest_accepts_id_mismatch",
        "viogpu/viogpudo/viogpudo.cpp",
        "resourceId != blobId ||",
        "FALSE ||",
    ),
    Rewrite(
        "R235_create_guest_accepts_unaligned_pool_base",
        "viogpu/viogpudo/viogpudo.cpp",
        "(baseAddress.QuadPart & (PAGE_SIZE - 1)) == 0 &&",
        "TRUE &&",
    ),
    Rewrite(
        "R236_paging_reenters_dod_rundown",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    BOOLEAN acquired = AcquireNativeGuestPoolMapping(snapshot, &mapping);
    BOOLEAN valid = acquired && mapping.GetBaseAddress() != NULL && mapping.GetGeneration() != 0 &&
                    (expectedPoolGeneration == 0 || mapping.GetGeneration() == expectedPoolGeneration) &&
                    segmentOffset <= (ULONGLONG)mapping.GetSize() &&
                    allocation->BackingSize <= mapping.GetSize() - (SIZE_T)segmentOffset;""",
        """    BOOLEAN acquired = allocation->Adapter->AcquireGpuGuestPoolMapping(&mapping);
    BOOLEAN valid = acquired && mapping.GetBaseAddress() != NULL && mapping.GetGeneration() != 0 &&
                    (expectedPoolGeneration == 0 || mapping.GetGeneration() == expectedPoolGeneration) &&
                    segmentOffset <= (ULONGLONG)mapping.GetSize() &&
                    allocation->BackingSize <= mapping.GetSize() - (SIZE_T)segmentOffset;""",
    ),
    Rewrite(
        "R237_paging_defers_mdl_lifetime",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """            status = CopyNativePlacement(allocation,
                                         placementOffset,
                                         transferOffset,
                                         transferSize,
                                         poolGeneration,
                                         systemAddress,
                                         (packetFlags & VioGpuWddmPagingFlagPageIn) != 0,
                                         &snapshot,
                                         &observedPoolGeneration);""",
        """            UNREFERENCED_PARAMETER(systemAddress);
            UNREFERENCED_PARAMETER(observedPoolGeneration);
            status = STATUS_SUCCESS;""",
    ),
    Rewrite(
        "R238_passive_cancel_leaves_queued_work",
        "viogpu/viogpudo/viogpudo.cpp",
        "ownership = VioGpuNativePassiveWorkRemoved;",
        "ownership = VioGpuNativePassiveWorkNotQueued;",
    ),
    Rewrite(
        "R239_passive_queue_ignores_retirement",
        "viogpu/viogpudo/viogpudo.cpp",
        "InterlockedCompareExchange(&work->Retired, 0, 0) == 0)",
        "TRUE)",
    ),
    Rewrite(
        "R240_worker_ownership_not_published",
        "viogpu/viogpudo/viogpudo.cpp",
        "InterlockedExchange(&work->State, VioGpuNativePassiveWorkWorkerOwned);",
        "InterlockedExchange(&work->State, VioGpuNativePassiveWorkIdle);",
    ),
    Rewrite(
        "R211_blob_drops_guest_handle",
        "viogpu/common/viogpu_queue.cpp",
        """    command->blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST;
    command->blob_flags = blob_flags;
    command->nr_entries = 1;""",
        """    command->blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST;
    command->blob_flags = blob_flags & ~VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE;
    command->nr_entries = 1;""",
    ),
    Rewrite(
        "R212_blob_borrows_caller_sg_entry",
        "viogpu/common/viogpu_queue.cpp",
        """    PGPU_MEM_ENTRY ownedEntry = static_cast<PGPU_MEM_ENTRY>(m_pBuf->AllocateMemory(sizeof(*ownedEntry)));
    if (ownedEntry == NULL)
    {
        ReleaseBuffer(vbuf);
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }
    *ownedEntry = *entry;""",
        """    PGPU_MEM_ENTRY ownedEntry = const_cast<PGPU_MEM_ENTRY>(entry);""",
    ),
    Rewrite(
        "R213_paging_worker_skips_executing_claim",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """        if (InterlockedCompareExchange(&pagingPrivate->Transaction.State,
                                       VioGpuWddmPagingTransactionExecuting,
                                       VioGpuWddmPagingTransactionQueued) != VioGpuWddmPagingTransactionQueued)""",
        """        if (InterlockedCompareExchange(&pagingPrivate->Transaction.State,
                                       VioGpuWddmPagingTransactionFinished,
                                       VioGpuWddmPagingTransactionQueued) != VioGpuWddmPagingTransactionQueued)""",
    ),
    Rewrite(
        "R214_paging_releases_before_batch_rollback",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    }
    return TRUE;
}

NTSTATUS ExecutePagingTransaction""",
        """    }
    ReleasePagingTransactionReference(transaction);
    return TRUE;
}

NTSTATUS ExecutePagingTransaction""",
    ),
    Rewrite(
        "R215_paging_cancel_rejects_queued",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "state != VioGpuWddmPagingTransactionBuilt && state != VioGpuWddmPagingTransactionQueued",
        "state != VioGpuWddmPagingTransactionBuilt",
    ),
    Rewrite(
        "R216_paging_cancel_ignores_executing",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """        if (state == VioGpuWddmPagingTransactionExecuting)
        {
            InterlockedExchange(&transaction->CancelRequested, 1);
            return TRUE;
        }""",
        """        if (state == VioGpuWddmPagingTransactionExecuting)
        {
            return FALSE;
        }""",
    ),
    Rewrite(
        "R217_paging_worker_drops_execution_marker",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "InterlockedExchange(&pagingPrivate->Transaction.ExecutionStarted, 1);",
        "InterlockedExchange(&pagingPrivate->Transaction.ExecutionStarted, 0);",
    ),
    Rewrite(
        "R218_context_lookup_ignores_identity",
        "viogpu/viogpudo/viogpudo.cpp",
        """owner->ResetGeneration != resetGeneration || owner->ContextId == 0 ||
                owner->ContextId != expectedContextId)""",
        """owner->ResetGeneration != resetGeneration || owner->ContextId == 0)""",
    ),
    Rewrite(
        "R219_context_range_allows_overlap",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "if (range->Iova <= existingEnd && existing->Iova <= rangeEnd)",
        "if (FALSE)",
    ),
    Rewrite(
        "R220_paging_worker_skips_batch_rollback",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (!NT_SUCCESS(status))
    {
        RollbackPagingBatch(privateBuffer, privateBufferSize, privateStart, privateEnd, count);
    }""",
        """    if (!NT_SUCCESS(status))
    {
        UNREFERENCED_PARAMETER(privateBuffer);
    }""",
    ),
    Rewrite(
        "R221_paging_rejects_repeated_transfer_start",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "if (continuingPageIn)",
        "if (FALSE && continuingPageIn)",
    ),
    Rewrite(
        "R222_paging_batch_offset_uses_uint",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    ULONGLONG candidate = static_cast<ULONGLONG>(base) + static_cast<ULONGLONG>(index) * static_cast<ULONGLONG>(stride);""",
        """    UINT candidate = base + index * static_cast<UINT>(stride);""",
    ),
    Rewrite(
        "R223_destroy_leaves_context_range",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "            NTSTATUS rangeStatus = UnregisterNativeAllocationRange(allocation);\n            if (!NT_SUCCESS(rangeStatus))\n            {\n                return rangeStatus;\n            }",
        "            NTSTATUS rangeStatus = STATUS_SUCCESS;\n            if (!NT_SUCCESS(rangeStatus))\n            {\n                return rangeStatus;\n            }",
    ),
    Rewrite(
        "R224_blob_failure_skips_gem_new_rollback",
        "viogpu/viogpudo/viogpudo.cpp",
        "if (result != VioGpuHostContextUnknown)",
        "if (FALSE)",
    ),
    Rewrite(
        "R225_blob_rollback_accepts_invalid_resource",
        "viogpu/viogpudo/viogpudo.cpp",
        "if (rollback == VioGpuHostContextConfirmed && ReleaseNativeAllocationCount(snapshot->Owner))",
        "if (rollback == VioGpuHostContextConfirmed || rollback == VioGpuHostContextRejected)",
    ),
    Rewrite(
        "R226_paging_skips_capacity_gate",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "if (dmaSize < sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET) || dmaPrivateSize < sizeof(VIOGPU_WDDM_PAGING_PRIVATE))",
        "if (FALSE)",
    ),
    Rewrite(
        "R227_paging_leaves_dma_size_unchanged",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "pagingBuffer->DmaSize = dmaSize - sizeof(*packet);",
        "pagingBuffer->DmaSize = dmaSize;",
    ),
    Rewrite(
        "R228_paging_uses_render_kind",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "privateData->Kind = VioGpuWddmDmaKindPaging;",
        "privateData->Kind = VioGpuWddmDmaKindRender;",
    ),
    Rewrite(
        "R229_paging_rejects_multipass",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "pagingBuffer->pDmaBufferPrivateData == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)",
        "pagingBuffer->pDmaBufferPrivateData == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL ||\n        pagingBuffer->MultipassOffset != 0)",
    ),
    Rewrite(
        "R230_paging_drops_reserved_zero",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "packet->Reserved = 0;",
        "packet->Reserved = 1;",
    ),
    Rewrite(
        "R231_destroy_accepts_invalid_resource",
        "viogpu/viogpudo/viogpudo.cpp",
        """if (result == VioGpuHostContextConfirmed)
    {
        BOOLEAN countReleased = ReleaseNativeAllocationCount(snapshot->Owner);
        if (!countReleased)
        {
            FailNativeContextAtAnyIrql();
            return VioGpuHostContextUnknown;
        }
        *released = TRUE;""",
        """if (result == VioGpuHostContextConfirmed || result == VioGpuHostContextRejected)
    {
        BOOLEAN countReleased = ReleaseNativeAllocationCount(snapshot->Owner);
        if (!countReleased)
        {
            FailNativeContextAtAnyIrql();
            return VioGpuHostContextUnknown;
        }
        *released = TRUE;""",
    ),
    Rewrite(
        "R241_fault_resets_submitted_endpoint",
        "viogpu/viogpudo/viogpudo.cpp",
        """    InvalidateNativeFenceTracker();
#endif
    if (ExAcquireRundownProtection(&m_HardwareOperations))""",
        """    ResetNativeFenceTracker();
#endif
    if (ExAcquireRundownProtection(&m_HardwareOperations))""",
    ),
    Rewrite(
        "R242_reset_publishes_before_stop",
        "viogpu/viogpudo/viogpudo.cpp",
        """    NTSTATUS status = adapter->SetPowerState(&m_DeviceInfo, PowerDeviceD3, &m_CurrentMode);
#if defined(VIOGPU_WDDM_CI_ONLY)
    /* StopNativeContextTransport has now drained every submitter that could
     * append to the tracker.  Discard pending entries only after that barrier. */
    InvalidateNativeFenceTracker();
#endif
    if (NT_SUCCESS(status))
    {
        m_AdapterPowerState = PowerDeviceD3;
#if defined(VIOGPU_WDDM_CI_ONLY)
        /* Only a completed hardware reset may advance the scheduler fence. */
        CompleteNativeFenceReset();
#endif
    }""",
        """#if defined(VIOGPU_WDDM_CI_ONLY)
    InvalidateNativeFenceTracker();
    CompleteNativeFenceReset();
#endif
    NTSTATUS status = adapter->SetPowerState(&m_DeviceInfo, PowerDeviceD3, &m_CurrentMode);
    if (NT_SUCCESS(status))
    {
        m_AdapterPowerState = PowerDeviceD3;
    }""",
    ),
    Rewrite(
        "R243_preempt_allows_inflight_queue",
        "viogpu/viogpuwddm/wddmddi.cpp",
        "!adapter->IsNativeFenceQueueEmpty()",
        "adapter->IsNativeFenceQueueEmpty()",
    ),
    Rewrite(
        "R244_preempt_notify_failure_continues",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """    if (!adapter->NotifyNativeSchedulerInterrupt(&notify, TRUE))
    {
        adapter->ResetDevice();
    }""",
        """    if (!adapter->NotifyNativeSchedulerInterrupt(&notify, TRUE))
    {
        UNREFERENCED_PARAMETER(notify);
    }""",
    ),
    Rewrite(
        "R245_restart_skips_d0_recovery",
        "viogpu/viogpudo/viogpudo.cpp",
        "NTSTATUS status = SetPowerState(DISPLAY_ADAPTER_HW_ID, PowerDeviceD0, PowerActionNone);",
        "NTSTATUS status = STATUS_SUCCESS;",
    ),
    Rewrite(
        "R246_invalidate_clears_completed_endpoint",
        "viogpu/viogpudo/viogpudo.cpp",
        """    RtlZeroMemory(m_NativeFences, sizeof(m_NativeFences));
    /* Keep the submitted/completed endpoints.  An adapter-wide reset must""",
        """    RtlZeroMemory(m_NativeFences, sizeof(m_NativeFences));
    InterlockedExchange(&m_NativeCompletedFence, 0);
    /* Keep the submitted/completed endpoints.  An adapter-wide reset must""",
    ),
    Rewrite(
        "R247_d0_recovery_skips_fence_publish",
        "viogpu/viogpudo/viogpudo.cpp",
        """#if defined(VIOGPU_WDDM_CI_ONLY)
                CompleteNativeFenceReset();
#endif
            }
            else if (InterlockedCompareExchange""",
        """#if defined(VIOGPU_WDDM_CI_ONLY)
                /* reset fence publication removed */
#endif
            }
            else if (InterlockedCompareExchange""",
    ),
    Rewrite(
        "S01_pending_comparison_reversed",
        "viogpu/common/viogpu_rdma.cpp",
        "if (status == STATUS_PENDING)",
        "if (STATUS_PENDING == status)",
        accepted=True,
    ),
    Rewrite(
        "S02_failure_comparison_explicit",
        "viogpu/common/viogpu_rdma.cpp",
        "if (!NT_SUCCESS(m_DisconnectStatus))",
        "if (NT_SUCCESS(m_DisconnectStatus) == FALSE)",
        accepted=True,
    ),
    Rewrite(
        "S03_reference_device_overload_ignored",
        "viogpu/viogpuwddm/wddmddi.cpp",
        """void DereferenceDevice(VIOGPU_WDDM_DEVICE *device);

BOOLEAN ReferenceDevice(VIOGPU_WDDM_DEVICE *device)
{""",
        """void DereferenceDevice(VIOGPU_WDDM_DEVICE *device);

BOOLEAN ReferenceDevice(VIOGPU_WDDM_DEVICE *device, BOOLEAN allowClosing)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(allowClosing);
    return FALSE;
}

BOOLEAN ReferenceDevice(VIOGPU_WDDM_DEVICE *device)
{""",
        accepted=True,
    ),
)


def tree_hash(root: Path) -> str:
    digest = hashlib.sha256()
    for subtree in CHECKED_SUBTREES:
        for path in sorted((root / subtree).rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                digest.update(path.relative_to(root).as_posix().encode())
                digest.update(path.read_bytes())
    return digest.hexdigest()


def run_checker(root: Path) -> tuple[int, str]:
    outputs = []
    for checker in CHECKERS:
        result = subprocess.run(
            [sys.executable, checker],
            cwd=root,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=30,
        )
        output = result.stdout.strip().replace("\n", " / ")
        outputs.append(f"{Path(checker).name}: {output}")
        if result.returncode != 0:
            return result.returncode, " / ".join(outputs)
    return 0, " / ".join(outputs)


def main() -> int:
    parser = argparse.ArgumentParser(description="Attack the viogpuwddm source contract")
    parser.add_argument(
        "--case",
        action="append",
        default=[],
        metavar="NAME",
        help="run only the named rewrite; repeat to select multiple rewrites",
    )
    arguments = parser.parse_args()
    requested = set(arguments.case)
    available = {rewrite.name for rewrite in REWRITES}
    unknown = requested - available
    if unknown:
        print(f"UNKNOWN_CASES {','.join(sorted(unknown))}")
        return 2
    rewrites = tuple(rewrite for rewrite in REWRITES if not requested or rewrite.name in requested)

    source_hash = tree_hash(ROOT)
    failures = 0
    with tempfile.TemporaryDirectory(prefix="viogpu-rdma-contract.") as temporary:
        frozen = Path(temporary) / "baseline"
        for subtree in CHECKED_SUBTREES:
            shutil.copytree(
                ROOT / subtree,
                frozen / subtree,
                ignore=shutil.ignore_patterns("__pycache__"),
            )
        code, output = run_checker(frozen)
        print(f"BASELINE exit={code} {output}")
        if code != 0:
            return 2

        for rewrite in rewrites:
            case = Path(temporary) / rewrite.name
            shutil.copytree(frozen, case)
            try:
                target = case / rewrite.path
                source = target.read_text(encoding="utf-8")
                count = source.count(rewrite.old)
                if count != 1:
                    print(f"{rewrite.name} SETUP expected=1 found={count}")
                    failures += 1
                    continue
                target.write_text(source.replace(rewrite.old, rewrite.new, 1), encoding="utf-8")
                code, output = run_checker(case)
                passed = code == 0
                expected = rewrite.accepted
                outcome = "PASS" if passed == expected else "FAIL"
                print(f"{rewrite.name} {outcome} exit={code} {output}")
                failures += passed != expected
            finally:
                shutil.rmtree(case, ignore_errors=True)

    if tree_hash(ROOT) != source_hash:
        print("SOURCE_CHANGED")
        return 2
    print(f"SUMMARY failures={failures} cases={len(rewrites)}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
