#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKER = "viogpu/viogpuwddm/check-contract.py"
CHECKED_SUBTREES = ("viogpu", "rdmapool", "VirtIO", "NetKVM", "viostor", "vioscsi")


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
        """m_GpuBuf.HasAllocationOwner() || m_RdmaPool.HasArenaOwner() || m_FrameSegment.GetSize() != 0 ||
        m_CursorSegment.GetSize() != 0 ||""",
        """m_GpuBuf.HasAllocationOwner() || m_RdmaPool.IsActive() || m_FrameSegment.GetSize() != 0 ||
        m_CursorSegment.GetSize() != 0 ||""",
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
    result = subprocess.run(
        [sys.executable, CHECKER],
        cwd=root,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=30,
    )
    return result.returncode, result.stdout.strip().replace("\n", " / ")


def main() -> int:
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

        for rewrite in REWRITES:
            case = Path(temporary) / rewrite.name
            shutil.copytree(frozen, case)
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

    if tree_hash(ROOT) != source_hash:
        print("SOURCE_CHANGED")
        return 2
    print(f"SUMMARY failures={failures} cases={len(REWRITES)}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
