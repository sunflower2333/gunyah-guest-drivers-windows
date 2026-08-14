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

#pragma once

#include "viogpu.h"
#include "viogpu_queue.h"
#include "viogpu_rdma.h"
#if defined(VIOGPU_WDDM_CI_ONLY)
#include "viogpu_named_pool.h"
#endif

#pragma pack(push)
#pragma pack(1)

typedef struct
{
    UINT DriverStarted : 1;
    UINT HardwareInit : 1;
    UINT PointerEnabled : 1;
    UINT VgaDevice : 1;
    UINT FlexResolution : 1;
    UINT UsePhysicalMemory : 1;
    UINT UsePresentProgress : 1;
    UINT RequireRestrictedDma : 1;
    UINT Unused : 24;
} DRIVER_STATUS_FLAG;

#pragma pack(pop)

typedef struct _CURRENT_MODE
{
    DXGK_DISPLAY_INFORMATION DispInfo;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;
    D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling;
    UINT SrcModeWidth;
    UINT SrcModeHeight;
    struct _CURRENT_MODE_FLAGS
    {
        UINT SourceNotVisible : 1;
        UINT FullscreenPresent : 1;
        UINT FrameBufferIsActive : 1;
        UINT DoNotMapOrUnmap : 1;
        UINT IsInternal : 1;
        UINT Unused : 27;
    } Flags;

    PVOID FrameBuffer;
    PVOID RamFrameBuffer;
} CURRENT_MODE;

class VioGpuDod;
class VioGpuAdapter;
struct VIOGPU_NATIVE_CONTEXT_REGISTRATION;

enum VIOGPU_NATIVE_CONTEXT_OWNER_STATE : LONG
{
    VioGpuNativeContextOwnerCreating = 0,
    VioGpuNativeContextOwnerLive,
    VioGpuNativeContextOwnerDestroying,
};

struct VIOGPU_NATIVE_CONTEXT_OWNER
{
    LIST_ENTRY AdapterLink;
    VIOGPU_NATIVE_CONTEXT_REGISTRATION *Registration;
    VIOGPU_NATIVE_CONTEXT_OWNER_STATE State;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT ContextId;
#if defined(VIOGPU_WDDM_CI_ONLY)
    UINT ControlResourceId;
    ULONG ControlPoolOffset;
    ULONG ControlBlobSize;
    ULONG LastControlSeqno;
    ULONGLONG ControlPoolGeneration;
    BOOLEAN ControlResourceCreated;
    BOOLEAN ControlMapped;
#endif
};

enum VIOGPU_NATIVE_CONTEXT_OBJECT_STATE : LONG
{
    VioGpuNativeContextAllocated = 0,
    VioGpuNativeContextCreating,
    VioGpuNativeContextLive,
    VioGpuNativeContextDestroying,
    VioGpuNativeContextDead,
};

struct VIOGPU_NATIVE_CONTEXT_REGISTRATION
{
    KSPIN_LOCK BindingLock;
    VioGpuAdapter *Adapter;
    VIOGPU_NATIVE_CONTEXT_OWNER *Owner;
    volatile LONG State;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT ContextId;
    ULONGLONG VaStart;
    ULONGLONG VaSize;
    BOOLEAN Registered;
};

struct VIOGPU_NATIVE_CONTEXT_SNAPSHOT
{
    VioGpuAdapter *Adapter;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT ContextId;
    ULONGLONG VaStart;
    ULONGLONG VaSize;
};

enum VIOGPU_NATIVE_CONTEXT_STATE : LONG
{
    VioGpuNativeContextOffline = 0,
    VioGpuNativeContextStarting,
    VioGpuNativeContextReady,
    VioGpuNativeContextQuiescing,
    VioGpuNativeContextFailed,
};

enum VIOGPU_HARDWARE_RESET_STATE : LONG
{
    VioGpuHardwareActive = 0,
    VioGpuHardwareResetRequested,
    VioGpuHardwareRecovering,
};

struct VIOGPU_NATIVE_CONTEXT_READINESS
{
    BOOLEAN Ready;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT CapsetVersion;
    UINT CapsetSize;
    GPU_CAPSET_DRM Capset;
};

class VioGpuAdapter : IVioGpuPCI
{
  public:
    VioGpuAdapter(_In_ VioGpuDod *pVioGpuDod);
    ~VioGpuAdapter(void);
    NTSTATUS SetCurrentMode(ULONG Mode, CURRENT_MODE *pCurrentMode);
    ULONG GetModeCount(void)
    {
        return m_ModeCount;
    }
    NTSTATUS SetPowerState(DXGK_DEVICE_INFO *pDeviceInfo,
                           DEVICE_POWER_STATE DevicePowerState,
                           CURRENT_MODE *pCurrentMode);
    NTSTATUS HWInit(PCM_RESOURCE_LIST pResList, DXGK_DISPLAY_INFORMATION *pDispInfo);
    NTSTATUS HWClose(void);
    NTSTATUS ExecutePresentDisplayOnly(_In_ BYTE *DstAddr,
                                       _In_ UINT DstBitPerPixel,
                                       _In_ BYTE *SrcAddr,
                                       _In_ UINT SrcBytesPerPixel,
                                       _In_ LONG SrcPitch,
                                       _In_ ULONG NumMoves,
                                       _In_ D3DKMT_MOVE_RECT *pMoves,
                                       _In_ ULONG NumDirtyRects,
                                       _In_ RECT *pDirtyRect,
                                       _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                                       _In_ const CURRENT_MODE *pModeCur);
    VOID BlackOutScreen(CURRENT_MODE *pCurrentMod);
    BOOLEAN InterruptRoutine(_In_ PDXGKRNL_INTERFACE pDxgkInterface, _In_ ULONG MessageNumber);
    VOID DpcRoutine(_In_ PDXGKRNL_INTERFACE pDxgkInterface);
    VOID ResetDevice(VOID);
    NTSTATUS SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape, _In_ CONST CURRENT_MODE *pModeCur);
    NTSTATUS SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition,
                                _In_ CONST CURRENT_MODE *pModeCur);
    NTSTATUS Escape(_In_ CONST DXGKARG_ESCAPE *pEscap);
    CPciResources *GetPciResources(void)
    {
        return &m_PciResources;
    }
    BOOLEAN ResetToVgaMode(void);
    BOOLEAN IsMSIEnabled()
    {
        return m_PciResources.IsMSIEnabled();
    }
    PHYSICAL_ADDRESS GetFrameBufferPA(void)
    {
        return m_PciResources.GetPciBar(0)->GetPA();
    }
    SIZE_T GetFrameSegmentSize(void)
    {
        return m_FrameSegment.GetSize();
    }
    PDXGKRNL_INTERFACE GetDxgkInterface(void);
    PVOID AllocateDmaMemory(SIZE_T size, SIZE_T alignment);
    void FreeDmaMemory(PVOID address);
    PHYSICAL_ADDRESS GetDmaPhysicalAddress(PVOID address);
    BOOLEAN IsRestrictedDmaActive(void);
    BOOLEAN QueryVidMmSegment(PPHYSICAL_ADDRESS physicalAddress, SIZE_T *size) const;
#if defined(VIOGPU_WDDM_CI_ONLY)
    BOOLEAN AcquireDrmHostPoolMapping(_Out_ VioGpuDrmHostPoolMapping *mapping) const;
#endif
    BOOLEAN QueryNativeContextReadiness(_Out_ PGPU_CAPSET_DRM capset,
                                        _Out_opt_ UINT *capsetVersion,
                                        _Out_opt_ UINT *capsetSize,
                                        _Out_opt_ ULONGLONG *resetGeneration);
    NTSTATUS CreateNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                 _In_ ULONGLONG expectedResetGeneration);
    NTSTATUS DestroyNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context, _Out_ BOOLEAN *released);
    static BOOLEAN AcquireNativeContextSnapshot(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                                _Out_ VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot);
    static void ReleaseNativeContextSnapshot(_Inout_ VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot);
    static VioGpuAdapter *ReferenceNativeContextAdapter(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context);
    static void DereferenceNativeContextAdapter(_In_ VioGpuAdapter *adapter);
    static BOOLEAN IsNativeContextReleased(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context);
    BOOLEAN IsNativeContextGenerationCurrent(_In_ LONG generation, _In_ ULONGLONG resetGeneration);

    PVIDEO_MODE_INFORMATION GetModeInfo(UINT idx)
    {
        return &m_ModeInfo[idx];
    }
    USHORT GetModeNumber(USHORT idx)
    {
        return (USHORT)m_ModeInfo[idx].ModeIndex;
    }
    USHORT GetCurrentModeIndex(void)
    {
        return m_CurrentModeIndex;
    }
    VOID SetCurrentModeIndex(USHORT idx)
    {
        m_CurrentModeIndex = idx;
    }
    VioGpuDod *GetVioGpu(void)
    {
        return m_pVioGpuDod;
    }
    ULONG GetInstanceId(void)
    {
        return m_Id;
    }
    PBYTE GetEdidData(void);
    PBYTE GetCTA861Data(void);

  protected:
  private:
    NTSTATUS VioGpuAdapterInit(DXGK_DISPLAY_INFORMATION *pDispInfo);
    void SetVideoModeInfo(UINT Idx, PVIOGPU_DISP_MODE pModeInfo);
    NTSTATUS StopNativeContextTransport(void);
    NTSTATUS StopNativeContextTransportLocked(void);
    NTSTATUS SynchronizeInterruptMessages(void);
    void InvalidateNativeContextRegistrationsLocked(void);
    void RetireAllNativeContextOwnersLocked(void);
    void RetireNativeContextOwnerLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner);
    UINT AllocateNativeContextIdLocked(void);
#if defined(VIOGPU_WDDM_CI_ONLY)
    UINT AllocateNativeResourceIdLocked(void);
    VIOGPU_HOST_CONTEXT_RESULT QueryNativeContextParameterLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner,
                                                                 _In_ ULONG parameter,
                                                                 _Out_ PULONGLONG value);
    VIOGPU_HOST_CONTEXT_RESULT DestroyNativeContextHostObjectsLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner);
#endif
    BOOLEAN BeginNativeContextInitialization(void);
    BOOLEAN CompleteNativeContextInitialization(void);
    void FailNativeContextAtAnyIrql(void);
    NTSTATUS NegotiateNativeContextFeatures(void);
    NTSTATUS ProbeNativeContextReadiness(void);
    NTSTATUS ConnectRestrictedDma(void);
#if defined(VIOGPU_WDDM_CI_ONLY)
    NTSTATUS ConnectDrmHostPool(void);
    NTSTATUS ConnectGpuGuestPool(void);
    static VOID NamedPoolFailureCallback(_In_opt_ PVOID context);
#endif
    NTSTATUS StartNativeContextTransport(DXGK_DISPLAY_INFORMATION *pDispInfo);
    NTSTATUS FailNativeContextInitialization(NTSTATUS status);
    NTSTATUS StartWorkThread(void);
    NTSTATUS StopWorkThread(void);
    void ClearNativeContextReadiness(void);
    NTSTATUS BuildModeList(DXGK_DISPLAY_INFORMATION *pDispInfo);
    BOOLEAN AckFeature(UINT64 Feature);
    BOOLEAN GetDisplayInfo(void);
    int ProcessEdid(void);
    void FixEdid(void);
    BOOLEAN GetEdids(void);
    int AddEdidModes(void);
    BOOLEAN UpdateModes(USHORT xres, USHORT yres, int &cnt);
    NTSTATUS UpdateChildStatus(BOOLEAN connect);
    void SetCustomDisplay(_In_ USHORT xres, _In_ USHORT yres);
    BOOLEAN CreateFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, CURRENT_MODE *pCurrentMode);
    void DestroyFrameBufferObj(BOOLEAN bReset, BOOLEAN bKeepBuffer);
    BOOLEAN CreateCursor(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape, _In_ CONST CURRENT_MODE *pCurrentMode);
    BOOLEAN UpdateCursor(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape, _In_ CONST CURRENT_MODE *pCurrentMode);
    void DestroyCursor(void);
    BOOLEAN GpuObjectAttach(UINT res_id, VioGpuObj *obj);
    void static ThreadWork(_In_ PVOID Context);
    void ThreadWorkRoutine(void);
    void ConfigChanged(void);
    NTSTATUS VirtIoDeviceInit(void);
    VOID CreateResolutionEvent(VOID);
    VOID NotifyResolutionEvent(VOID);
    VOID CloseResolutionEvent(VOID);

  private:
    VioGpuDod *m_pVioGpuDod;
    PVIDEO_MODE_INFORMATION m_ModeInfo;
    ULONG m_ModeCount;
    USHORT m_CurrentModeIndex;
    USHORT m_CustomModeIndex;
    ULONG m_Id;
    BYTE m_EDIDs[EDID_RAW_BLOCK_SIZE];
    BOOLEAN m_bEDID;

    VirtIODevice m_VioDev;
    CPciResources m_PciResources;
    VioGpuRdmaPool m_RdmaPool;
#if defined(VIOGPU_WDDM_CI_ONLY)
    VioGpuDrmHostPool m_DrmHostPool;
    VioGpuGuestPool m_GpuGuestPool;
#endif
    UINT64 m_u64HostFeatures;
    UINT64 m_u64GuestFeatures;
    UINT32 m_u32NumCapsets;
    UINT32 m_u32NumScanouts;
    KSPIN_LOCK m_NativeContextReadinessLock;
    VIOGPU_NATIVE_CONTEXT_READINESS m_NativeContextReadiness;
    KMUTEX m_NativeContextLifecycleMutex;
    LIST_ENTRY m_NativeContextRegistry;
    EX_RUNDOWN_REF m_NativeContextReferences;
    UINT m_NextNativeContextId;
#if defined(VIOGPU_WDDM_CI_ONLY)
    UINT m_NextNativeResourceId;
#endif
    volatile LONG m_NativeContextState;
    volatile LONG m_NativeContextGeneration;
    DECLSPEC_ALIGN(8) volatile LONG64 m_NativeContextResetGeneration;
    volatile LONG m_InterruptDispatchEnabled;
    BOOLEAN m_bVirtioInitialized;
    BOOLEAN m_bQueuesInitialized;
    HANDLE m_WorkThreadHandle;
    CtrlQueue m_CtrlQueue;
    CrsrQueue m_CursorQueue;
    VioGpuBuf m_GpuBuf;
    VioGpuIdr m_Idr;
    VioGpuObj *m_pFrameBuf;
    VioGpuObj *m_pCursorBuf;
    VioGpuMemSegment m_CursorSegment;
    VioGpuMemSegment m_FrameSegment;
    volatile ULONG m_PendingWorks;
    KEVENT m_ConfigUpdateEvent;
    PETHREAD m_pWorkThread;
    BOOLEAN m_bStopWorkThread;
    PKEVENT m_ResolutionEvent;
    HANDLE m_ResolutionEventHandle;
};

class VioGpuDod
{
  private:
    DEVICE_OBJECT *m_pPhysicalDevice;
    DXGKRNL_INTERFACE m_DxgkInterface;
    DXGK_DEVICE_INFO m_DeviceInfo;

    DEVICE_POWER_STATE m_MonitorPowerState;
    DEVICE_POWER_STATE m_AdapterPowerState;
    DRIVER_STATUS_FLAG m_Flags;

    CURRENT_MODE m_CurrentMode;

    DXGK_DISPLAY_INFORMATION m_SystemDisplayInfo;

    DXGKARG_SETPOINTERSHAPE m_PointerShape;
    VioGpuAdapter *m_pHWDevice;
    mutable EX_RUNDOWN_REF m_HardwareOperations;
    BOOLEAN m_HardwareRundownCompleted;
    mutable volatile LONG m_HardwareResetState;

    USHORT m_PersistentDispMode0Width;
    USHORT m_PersistentDispMode0Height;

  public:
    VioGpuDod(_In_ DEVICE_OBJECT *pPhysicalDeviceObject);
    ~VioGpuDod(void);
#pragma code_seg(push)
#pragma code_seg()
    BOOLEAN IsDriverActive() const
    {
        return m_Flags.DriverStarted;
    }
    BOOLEAN IsHardwareInit() const
    {
        return m_Flags.HardwareInit;
    }
    void SetHardwareInit(BOOLEAN init)
    {
        m_Flags.HardwareInit = init;
    }
    BOOLEAN IsPointerEnabled() const
    {
        return m_Flags.PointerEnabled;
    }
    void SetPointerEnabled(BOOLEAN Enabled)
    {
        m_Flags.PointerEnabled = Enabled;
    }
    BOOLEAN IsVgaDevice(void) const
    {
        return m_Flags.VgaDevice;
    }
    void SetVgaDevice(BOOLEAN Vga)
    {
        m_Flags.VgaDevice = Vga;
    }
    BOOLEAN IsFlexResolution(void) const
    {
        return m_Flags.FlexResolution;
    }
    void SetFlexResolution(BOOLEAN FlexRes)
    {
        m_Flags.FlexResolution = FlexRes;
    }
    BOOLEAN IsUsePhysicalMemory() const
    {
        return m_Flags.UsePhysicalMemory;
    }
    void SetUsePhysicalMemory(BOOLEAN enable)
    {
        m_Flags.UsePhysicalMemory = enable;
    }
    BOOLEAN IsUsePresentProgress() const
    {
        return m_Flags.UsePresentProgress;
    }
    void SetUsePresentProgress(BOOLEAN enable)
    {
        m_Flags.UsePresentProgress = enable;
    }
    BOOLEAN IsRestrictedDmaRequired() const
    {
        return m_Flags.RequireRestrictedDma;
    }
    void SetRestrictedDmaRequired(BOOLEAN required)
    {
        m_Flags.RequireRestrictedDma = required;
    }
    void SetPersistentDispMode0Width(USHORT res)
    {
        m_PersistentDispMode0Width = res;
    }
    USHORT GetPersistentDispMode0Width()
    {
        return m_PersistentDispMode0Width;
    }
    void SetPersistentDispMode0Height(USHORT res)
    {
        m_PersistentDispMode0Height = res;
    }
    USHORT GetPersistentDispMode0Height()
    {
        return m_PersistentDispMode0Height;
    }
    BOOLEAN IsPersistentDispMode0Set()
    {
        return (m_PersistentDispMode0Width > 0) && (m_PersistentDispMode0Height > 0);
    }
#pragma code_seg(pop)

    NTSTATUS StartDevice(_In_ DXGK_START_INFO *pDxgkStartInfo,
                         _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                         _Out_ ULONG *pNumberOfViews,
                         _Out_ ULONG *pNumberOfChildren);
    NTSTATUS StopDevice(VOID);
    VOID ResetDevice(VOID);
    NTSTATUS DispatchIoRequest(_In_ ULONG VidPnSourceId, _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket);
    NTSTATUS SetPowerState(_In_ ULONG HardwareUid,
                           _In_ DEVICE_POWER_STATE DevicePowerState,
                           _In_ POWER_ACTION ActionType);
    NTSTATUS QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                 _In_ ULONG ChildRelationsSize);
    NTSTATUS QueryChildStatus(_Inout_ DXGK_CHILD_STATUS *pChildStatus, _In_ BOOLEAN NonDestructiveOnly);
    NTSTATUS QueryDeviceDescriptor(_In_ ULONG ChildUid, _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor);
    BOOLEAN InterruptRoutine(_In_ ULONG MessageNumber);
    VOID DpcRoutine(VOID);
    NTSTATUS QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo);
    NTSTATUS SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition);
    NTSTATUS SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape);
    NTSTATUS Escape(_In_ CONST DXGKARG_ESCAPE *pEscape);
    NTSTATUS PresentDisplayOnly(_In_ CONST DXGKARG_PRESENT_DISPLAYONLY *pPresentDisplayOnly);
    NTSTATUS QueryInterface(_In_ CONST PQUERY_INTERFACE QueryInterface);
    NTSTATUS IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn);
    NTSTATUS RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn);
    NTSTATUS RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology);
    NTSTATUS RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes);
    NTSTATUS EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality);
    NTSTATUS SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility);
    NTSTATUS CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn);
    NTSTATUS
    UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath);
    NTSTATUS QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps);
    NTSTATUS StopDeviceAndReleasePostDisplayOwnership(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                      _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo);
    NTSTATUS SystemDisplayEnable(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                 _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                 _Out_ UINT *pWidth,
                                 _Out_ UINT *pHeight,
                                 _Out_ D3DDDIFORMAT *pColorFormat);
    VOID SystemDisplayWrite(_In_reads_bytes_(SourceHeight *SourceStride) VOID *pSource,
                            _In_ UINT SourceWidth,
                            _In_ UINT SourceHeight,
                            _In_ UINT SourceStride,
                            _In_ INT PositionX,
                            _In_ INT PositionY);
    NTSTATUS SetRegisterConfigInfo(void);

    PDXGKRNL_INTERFACE GetDxgkInterface(void)
    {
        return &m_DxgkInterface;
    }
    BOOLEAN QueryVidMmSegment(PPHYSICAL_ADDRESS physicalAddress, SIZE_T *size) const;
    BOOLEAN QueryNativeContextReadiness(_Out_ PGPU_CAPSET_DRM capset,
                                        _Out_opt_ UINT *capsetVersion,
                                        _Out_opt_ UINT *capsetSize,
                                        _Out_opt_ ULONGLONG *resetGeneration);
    NTSTATUS CreateNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                 _In_ ULONGLONG expectedResetGeneration);
    NTSTATUS DestroyNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context, _Out_ BOOLEAN *released);
    BOOLEAN IsHardwareResetRequested(void) const
    {
        return InterlockedCompareExchange(&m_HardwareResetState, VioGpuHardwareActive, VioGpuHardwareActive) !=
               VioGpuHardwareActive;
    }
    VOID RequestHardwareResetAtAnyIrql(void)
    {
        InterlockedExchange(&m_HardwareResetState, VioGpuHardwareResetRequested);
    }
    BOOLEAN IsHardwareInterruptDispatchAllowed(void) const
    {
        LONG state = InterlockedCompareExchange(&m_HardwareResetState, VioGpuHardwareActive, VioGpuHardwareActive);
        return state == VioGpuHardwareActive || state == VioGpuHardwareRecovering;
    }

  private:
    BOOLEAN CheckHardware();
    NTSTATUS UnwindFailedStart(_In_ NTSTATUS failureStatus);
    NTSTATUS WriteRegistryString(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PCSTR pszValue);
    NTSTATUS WriteRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PDWORD pdwValue);
    NTSTATUS ReadRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _Inout_ PDWORD pdwValue);
    NTSTATUS SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode,
                                  CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath);
    NTSTATUS AddSingleMonitorMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes);
    NTSTATUS AddSingleSourceMode(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface,
                                 D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    NTSTATUS AddSingleTargetMode(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface,
                                 D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                 _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    NTSTATUS IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode) const;
    NTSTATUS IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath) const;
    NTSTATUS SetRegisterInfo(_In_ ULONG Id, _In_ DWORD MemSize);
    NTSTATUS GetRegisterInfo(void);
    VOID BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *pVideoSignalInfo, PVIDEO_MODE_INFORMATION pModeInfo);
};
