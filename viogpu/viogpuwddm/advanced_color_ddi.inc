/* Included after the allocation/lifecycle helpers. Opt-in WDDM 2.3 candidate. */
static BOOLEAN VioGpuSupportedHdrPlane(VioGpuDod *adapter,
                                       VIOGPU_WDDM_ALLOCATION *allocation,
                                       const DXGK_MULTIPLANE_OVERLAY_ATTRIBUTES3 &attr)
{
    return IsOwnedAllocation(allocation, adapter) && IsStandardPrimaryAllocation(allocation) &&
           allocation->Width != 0 && allocation->Height != 0 && allocation->Width <= MAXLONG &&
           allocation->Height <= MAXLONG && allocation->Format == D3DDDIFMT_A2B10G10R10 && attr.Flags.Value == 0 &&
           attr.Blend.Value == 0 && attr.Rotation == D3DDDI_ROTATION_IDENTITY &&
           attr.ColorSpaceType == D3DDDI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 && attr.SrcRect.left == 0 &&
           attr.SrcRect.top == 0 && attr.SrcRect.right == static_cast<LONG>(allocation->Width) &&
           attr.SrcRect.bottom == static_cast<LONG>(allocation->Height) &&
           RtlCompareMemory(&attr.SrcRect, &attr.DstRect, sizeof(RECT)) == sizeof(RECT) &&
           RtlCompareMemory(&attr.SrcRect, &attr.ClipRect, sizeof(RECT)) == sizeof(RECT);
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmSetTimingsFromVidPn(CONST HANDLE hAdapter,
                                                                       DXGKARG_SETTIMINGSFROMVIDPN *args)
{
    auto adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || args == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL || args->SetFlags.Value != 0 ||
        args->pResultsFlags == NULL || args->PathCount != 1 || args->pSetTimingPathInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    args->pResultsFlags->Value = 0;
    auto &path = args->pSetTimingPathInfo[0];
    path.OutputFlags = 0;
    path.DiagnosticInfo = 0;
    const bool connected = InterlockedCompareExchange(&adapter->m_ColorMonitorConnected, 0, 0) != 0;
    if (path.Input.Active && !connected)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (path.VidPnTargetId != 0 || path.Input.Reserved != 0 || path.Input.IgnoreConnectivity ||
        (path.Input.Active && path.Input.VidPnPathUpdates == DXGK_PATH_UPDATE_REMOVED))
    {
        return STATUS_INVALID_PARAMETER;
    }
    const auto wire = path.SelectedWireFormat;
    // This backend cannot preserve a firmware-owned scanout across the virtio
    // device reset. The DDI contract reserves failure for bad parameters, so a
    // PreserveInherited request is applied as a normal timing set and the
    // replacement is reported below as a black-content timing-change glitch.
    const bool hdr = path.OutputWireColorSpace == D3DDDI_OUTPUT_WIRE_COLOR_SPACE_G2084_P2020;
    if (path.Input.Active &&
        (wire.YCbCr444 != 0 || wire.YCbCr422 != 0 || wire.YCbCr420 != 0 || wire.Intensity != 0 ||
         wire.Rgb != static_cast<UINT>(hdr ? D3DKMDT_BITS_PER_COMPONENT_10 : D3DKMDT_BITS_PER_COMPONENT_08) ||
         (!hdr && path.OutputWireColorSpace != D3DDDI_OUTPUT_WIRE_COLOR_SPACE_G22_P709)))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (path.Input.Active && hdr)
    {
        VIOGPU_DISPLAY_COLOR_RESPONSE caps = {};
        if (!adapter->QueryDisplayColor(&caps) || !(caps.usable_hdr_types & VIOGPU_DISPLAY_COLOR_PQ))
        {
            return STATUS_NOT_SUPPORTED;
        }
    }
    DXGKARG_COMMITVIDPN commit = {};
    commit.hFunctionalVidPn = args->hFunctionalVidPn;
    commit.AffectedVidPnSourceId = 0;
    commit.Flags.PathPoweredOff = !path.Input.Active;
    // Commit owns the color/configuration slot across pinned-mode validation,
    // real framebuffer recreation, metadata, scanout and rational CRTC timing.
    const auto status = adapter->CommitVidPn(&commit, hdr ? D3DDDIFMT_A2B10G10R10 : D3DDDIFMT_A8R8G8B8);
    if (NT_SUCCESS(status))
    {
        // Legacy HPD notification is also reflected by QueryChildStatus.
        // There is no numbered QueryConnectionChange packet on this route.
        RtlZeroMemory(&path.TargetState, sizeof(path.TargetState));
        path.TargetState.TargetId = 0;
        path.TargetState.ConnectionStatus = connected ? MonitorStatusConnected : MonitorStatusDisconnected;
        if (connected)
        {
            path.TargetState.MonitorConnect.LinkTargetType = D3DKMDT_VOT_INTERNAL;
        }
        path.Output.RecheckMPO = 1;
        path.GlitchCause = DXGK_GLITCH_CAUSE_TIMING_CHANGE;
        path.GlitchEffect = DXGK_GLITCH_EFFECT_BLACK_CONTENT;
        path.GlitchDuration = DXGK_GLITCH_DURATION_INDEFINITE;
    }
    return status;
}

_Use_decl_annotations_ NTSTATUS APIENTRY
VioGpuWddmCheckMultiPlaneOverlaySupport3(CONST HANDLE hAdapter, DXGKARG_CHECKMULTIPLANEOVERLAYSUPPORT3 *args)
{
    auto adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || args == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    args->Supported = FALSE;
    args->ReturnInfo.Value = 0;
    if (args->PlaneCount != 1 || args->ppPlanes == NULL || args->ppPlanes[0] == NULL || args->PostCompositionCount != 0)
    {
        return STATUS_SUCCESS;
    }
    const auto plane = args->ppPlanes[0];
    auto allocation = reinterpret_cast<VIOGPU_WDDM_ALLOCATION *>(plane->hAllocation);
    if (plane->VidPnSourceId != 0 || plane->LayerIndex != 0 ||
        !VioGpuSupportedHdrPlane(adapter, allocation, plane->PlaneAttributes))
    {
        return STATUS_SUCCESS;
    }
    VioGpuDod::ColorStateOperation operation(adapter);
    if (!operation.Acquired())
    {
        return STATUS_DEVICE_NOT_READY;
    }
    VIOGPU_DISPLAY_COLOR_RESPONSE caps = {};
    args->Supported = adapter->QueryDisplayColor(&caps) && (caps.usable_hdr_types & VIOGPU_DISPLAY_COLOR_PQ) != 0 &&
                      adapter->MatchesNativeHdrMode(allocation->Width, allocation->Height);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmSetTargetGamma(CONST HANDLE hAdapter,
                                                                  CONST DXGKARG_SETTARGETGAMMA *args)
{
    auto adapter = reinterpret_cast<VioGpuDod *>(hAdapter);
    if (adapter == NULL || args == NULL || args->TargetId != 0 || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    VioGpuDod::ColorStateOperation operation(adapter);
    if (!operation.Acquired())
    {
        return STATUS_DEVICE_NOT_READY;
    }
    auto transform = new (NonPagedPoolNx) VIOGPU_DISPLAY_TRANSFORM;
    if (transform == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    RtlZeroMemory(transform, sizeof(*transform));
    transform->version = 1;
    transform->size = sizeof(*transform);
    // 0x3f800000 is float32 1.0. The kernel does no floating point work.
    transform->scalar = 0x3f800000U;
    transform->scale[0] = transform->scale[1] = transform->scale[2] = 0x3f800000U;
    const auto &gamma = args->GammaRamp;
    NTSTATUS status = STATUS_SUCCESS;
    switch (gamma.Type)
    {
        case D3DDDI_GAMMARAMP_DEFAULT:
            if (gamma.DataSize != 0 || gamma.Data.pRaw != NULL)
            {
                status = STATUS_INVALID_PARAMETER;
            }
            break;
        case D3DDDI_GAMMARAMP_DXGI_1:
            if (gamma.DataSize != sizeof(D3DDDI_GAMMA_RAMP_DXGI_1) || gamma.Data.pDxgi1 == NULL)
            {
                status = STATUS_INVALID_PARAMETER;
            }
            else
            {
                transform->kind = 1;
                transform->lut_count = 1025;
                RtlCopyMemory(transform->scale, &gamma.Data.pDxgi1->Scale, sizeof(transform->scale));
                RtlCopyMemory(transform->offset, &gamma.Data.pDxgi1->Offset, sizeof(transform->offset));
                RtlCopyMemory(transform->lut, gamma.Data.pDxgi1->GammaCurve, sizeof(gamma.Data.pDxgi1->GammaCurve));
            }
            break;
        case D3DDDI_GAMMARAMP_MATRIX_3x4:
            if (gamma.DataSize != sizeof(D3DKMDT_3x4_COLORSPACE_TRANSFORM) || gamma.Data.p3x4 == NULL)
            {
                status = STATUS_INVALID_PARAMETER;
            }
            else
            {
                transform->kind = 2;
                transform->lut_count = 4096;
                RtlCopyMemory(transform->matrix, gamma.Data.p3x4->ColorMatrix3x4, sizeof(transform->matrix));
                RtlCopyMemory(&transform->scalar, &gamma.Data.p3x4->ScalarMultiplier, sizeof(transform->scalar));
                RtlCopyMemory(transform->lut, gamma.Data.p3x4->LookupTable1D, sizeof(transform->lut));
            }
            break;
        default:
            status = STATUS_NOT_SUPPORTED;
    }
    if (status == STATUS_SUCCESS && !VioGpuValidDisplayTransform(transform))
    {
        status = STATUS_INVALID_PARAMETER;
    }
    VIOGPU_DISPLAY_COLOR_RESPONSE caps = {};
    if (status == STATUS_SUCCESS && !adapter->QueryDisplayColor(&caps))
    {
        status = STATUS_NOT_SUPPORTED;
    }
    if (status == STATUS_SUCCESS)
    {
        const auto result = adapter->SetTargetTransform(caps.generation, transform);
        // Host applies this to the active resource immediately, including an
        // idle desktop, and fences all readers before acknowledging the DDI.
        status = result == VioGpuHostContextConfirmed ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
        if (result == VioGpuHostContextUnknown)
        {
            adapter->RequestHardwareResetAtAnyIrql();
        }
    }
    delete transform;
    return status;
}

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
    // The OS has already validated this description. Capability fields are
    // always zero in this notification, so they must not gate HDR admission.
    // Our wire remains canonical PQ/2020 or SDR/709; OS color correction is
    // delivered separately by SetTargetGamma, which applies within a frame.
    KIRQL oldIrql;
    KeAcquireSpinLock(&adapter->m_ColorStateLock, &oldIrql);
    adapter->m_AdjustedColorimetry = adjusted;
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
    if (!VioGpuSupportedHdrPlane(adapter, allocation, plane->PlaneAttributes) ||
        primary->SegmentId != VIOGPU_WDDM_SEGMENT_ID || primary->SegmentAddress.QuadPart < 0)
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
    if (!adapter->MatchesNativeHdrMode(allocation->Width, allocation->Height))
    {
        adapter->EndColorStateOperation();
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
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
