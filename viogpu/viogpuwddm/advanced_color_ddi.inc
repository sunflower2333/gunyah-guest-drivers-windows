/* Included after the allocation/lifecycle helpers. Opt-in WDDM 2.3 candidate. */
_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmSetTargetAdjustedColorimetry(CONST HANDLE hAdapter,
                                                                                D3DDDI_VIDEO_PRESENT_TARGET_ID target,
                                                                                DXGK_COLORIMETRY adjusted)
{
    auto adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || target != 0 || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    VioGpuDod::ColorStateOperation operation(adapter);
    if (!operation.Acquired())
    {
        return STATUS_DEVICE_NOT_READY;
    }
    VIOGPU_DISPLAY_COLOR_RESPONSE caps = {};
    if (!adapter->QueryDisplayColor(&caps))
    {
        return STATUS_NOT_SUPPORTED;
    }
    // An OS-adjusted monitor description cannot create new physical capabilities.
    if (adjusted.StandardColorimetryFlags.ST2084 && !(caps.usable_hdr_types & VIOGPU_DISPLAY_COLOR_PQ))
    {
        return STATUS_NOT_SUPPORTED;
    }
    KIRQL oldIrql;
    KeAcquireSpinLock(&adapter->m_ColorStateLock, &oldIrql);
    adapter->m_ColorCapabilities = caps;
    KeReleaseSpinLock(&adapter->m_ColorStateLock, oldIrql);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY
VioGpuWddmSetVidPnSourceAddressMpo3(CONST HANDLE hAdapter, DXGKARG_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3 *args)
{
    if (hAdapter == NULL || args == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    args->OutputFlags.Value = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        // The documented recall contract permits a PASSIVE_LEVEL retry. No
        // allocation, passive lock or virtqueue wait happens at interrupt level.
        args->OutputFlags.PrePresentNeeded = 1;
        return STATUS_RETRY;
    }
    auto adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (args->VidPnSourceId != 0 || (args->InputFlags.Value & ~8U) != 0 || args->PlaneCount != 1 ||
        args->ppPlanes == NULL || args->ppPlanes[0] == NULL || args->pPostComposition != NULL || args->Duration != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    auto plane = args->ppPlanes[0];
    plane->OutputFlags.Value = 0;
    if (plane->LayerIndex != 0 || plane->PresentId == 0 || plane->PresentId > MAXLONGLONG || plane->ContextCount != 1 ||
        plane->ppContextData == NULL || plane->ppContextData[0] == NULL ||
        (plane->InputFlags.Value != 3U && plane->InputFlags.Value != 5U) || plane->DriverPrivateDataSize != 0 ||
        plane->pDriverPrivateData != NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    const auto primary = plane->ppContextData[0];
    auto allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(primary->hAllocation);
    const auto &attr = plane->PlaneAttributes;
    if (!IsOwnedAllocation(allocation, adapter) || !IsStandardPrimaryAllocation(allocation) ||
        allocation->Width > MAXLONG || allocation->Height > MAXLONG || allocation->Format != D3DDDIFMT_A2B10G10R10 ||
        primary->SegmentId != VIOGPU_WDDM_SEGMENT_ID || primary->SegmentAddress.QuadPart < 0 || attr.Flags.Value != 0 ||
        attr.Blend.Value != 0 || attr.Rotation != D3DDDI_ROTATION_IDENTITY ||
        attr.ColorSpaceType != D3DDDI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || attr.SrcRect.left != 0 ||
        attr.SrcRect.top != 0 || attr.SrcRect.right != static_cast<LONG>(allocation->Width) ||
        attr.SrcRect.bottom != static_cast<LONG>(allocation->Height) ||
        RtlCompareMemory(&attr.SrcRect, &attr.DstRect, sizeof(RECT)) != sizeof(RECT) ||
        RtlCompareMemory(&attr.SrcRect, &attr.ClipRect, sizeof(RECT)) != sizeof(RECT))
    {
        return STATUS_INVALID_PARAMETER;
    }
    VIOGPU_DISPLAY_COLOR_RESPONSE caps;
    KIRQL oldIrql;
    KeAcquireSpinLock(&adapter->m_ColorStateLock, &oldIrql);
    caps = adapter->m_ColorCapabilities;
    KeReleaseSpinLock(&adapter->m_ColorStateLock, oldIrql);
    if (caps.generation == 0 || !(caps.usable_hdr_types & VIOGPU_DISPLAY_COLOR_PQ))
    {
        return STATUS_NOT_SUPPORTED;
    }
    if (!adapter->BeginColorStateOperation(FALSE))
    {
        args->OutputFlags.PrePresentNeeded = 1;
        return STATUS_RETRY;
    }
    const ULONG fenceEpoch = adapter->QueryNativeFenceEpoch();
    if (fenceEpoch == adapter->m_ColorPresentQueuedEpoch && plane->PresentId <= adapter->m_ColorPresentLastQueuedId)
    {
        adapter->EndColorStateOperation();
        return STATUS_INVALID_PARAMETER;
    }
    auto work = new (NonPagedPoolNx) VIOGPU_COLOR_PRESENT_WORK;
    if (work == NULL)
    {
        adapter->EndColorStateOperation();
        return STATUS_NO_MEMORY;
    }
    RtlZeroMemory(work, sizeof(*work));
    work->Adapter = adapter;
    work->Allocation = allocation;
    work->PrimaryAddress = static_cast<ULONGLONG>(primary->SegmentAddress.QuadPart);
    work->PresentId = plane->PresentId;
    work->FenceEpoch = fenceEpoch;
    work->Color.generation = caps.generation;
    work->Color.format = VIOGPU_DISPLAY_FORMAT_AB30;
    work->Color.encoding = VIOGPU_DISPLAY_COLOR_PQ;
    NTSTATUS status = STATUS_SUCCESS;
    if (args->pHDRMetaData != NULL)
    {
        const auto hdr = args->pHDRMetaData;
        if (hdr->Type == D3DDDI_HDR_METADATA_TYPE_NONE)
        {
            if (hdr->Size != 0 || hdr->pMetaData != NULL)
            {
                status = STATUS_INVALID_PARAMETER;
            }
        }
        else if (hdr->Type != D3DDDI_HDR_METADATA_TYPE_HDR10 || hdr->Size != sizeof(D3DDDI_HDR_METADATA_HDR10) ||
                 hdr->pMetaData == NULL)
        {
            status = STATUS_INVALID_PARAMETER;
        }
        else
        {
            const auto metadata = static_cast<const D3DDDI_HDR_METADATA_HDR10 *>(hdr->pMetaData);
            work->Color.has_static_metadata = 1;
            for (UINT i = 0; i < 2; ++i)
            {
                work->Color.chromaticities[i] = metadata->RedPrimary[i];
                work->Color.chromaticities[i + 2] = metadata->GreenPrimary[i];
                work->Color.chromaticities[i + 4] = metadata->BluePrimary[i];
                work->Color.chromaticities[i + 6] = metadata->WhitePoint[i];
            }
            work->Color.max_mastering_luminance = metadata->MaxMasteringLuminance;
            work->Color.min_mastering_luminance = metadata->MinMasteringLuminance;
            work->Color.max_content_light_level = metadata->MaxContentLightLevel;
            work->Color.max_frame_average_light_level = metadata->MaxFrameAverageLightLevel;
        }
    }
    if (status == STATUS_SUCCESS && !VioGpuValidDisplayMetadata(&work->Color))
    {
        status = STATUS_INVALID_PARAMETER;
    }
    if (status == STATUS_SUCCESS && !adapter->AcquireNativeSubmissionOperation())
    {
        status = STATUS_DEVICE_NOT_READY;
    }
    else if (status == STATUS_SUCCESS)
    {
        status = AcquireAllocationSubmissionReference(allocation, adapter);
        if (status != STATUS_SUCCESS)
        {
            adapter->ReleaseNativeSubmissionOperation();
        }
    }
    if (status != STATUS_SUCCESS)
    {
        delete work;
        adapter->EndColorStateOperation();
        return status;
    }
    adapter->m_ColorPresentLastQueuedId = plane->PresentId;
    adapter->m_ColorPresentQueuedEpoch = fenceEpoch;
    InterlockedExchange(&adapter->m_ColorPresentActive, 1);
    // For immediate flips OS must wait for the reported PresentId; returning
    // from this callback only means queued, not that the primary is reusable.
    if (plane->InputFlags.FlipImmediate)
    {
        plane->OutputFlags.HsyncInterruptCompletion = 1;
    }
    ExInitializeWorkItem(&work->Work, VioGpuColorPresentWorker, work);
    ExQueueWorkItem(&work->Work, DelayedWorkQueue);
    return STATUS_SUCCESS;
}
