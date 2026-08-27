#include <windows.h>

#include "d3dumddi_compat.h"

namespace
{

struct ACTIVATION_ADAPTER
{
    D3D10DDI_HRTADAPTER RuntimeAdapter;
};

#if defined(VIOGPU_WDDM_TEST_IMPLEMENTATIONS)
struct ACTIVATION_DEVICE
{
    ULONG Signature;
    volatile LONG CallCount;
    volatile LONG LastCall;
};

struct ACTIVATION_RESOURCE
{
    ULONG Signature;
    D3D10DDI_HRTRESOURCE RuntimeResource;
};

struct ACTIVATION_QUERY
{
    ULONG Signature;
    D3D10DDI_HRTQUERY RuntimeQuery;
};

struct ACTIVATION_DEFERRED_CONTEXT
{
    ULONG Signature;
    D3D10DDI_HRTCORELAYER RuntimeCoreLayer;
    UINT Flags;
    volatile LONG CallCount;
    volatile LONG LastCall;
};

/* D3D creates state, view, and shader objects through a large family of
 * callbacks.  The opt-in probe gives each family an opaque owner so the
 * runtime can exercise allocation and teardown without entering a renderer.
 * The product UMD keeps these callbacks absent until a real command ABI is
 * available. */
struct ACTIVATION_OBJECT
{
    ULONG Signature;
    PVOID RuntimeHandle;
};

constexpr ULONG ACTIVATION_DEVICE_SIGNATURE = 0x56494F54UL;
constexpr ULONG ACTIVATION_RESOURCE_SIGNATURE = 0x56494F52UL;
constexpr ULONG ACTIVATION_QUERY_SIGNATURE = 0x56494F51UL;
constexpr ULONG ACTIVATION_DEFERRED_CONTEXT_SIGNATURE = 0x56494443UL;
constexpr ULONG ACTIVATION_ELEMENT_LAYOUT_SIGNATURE = 0x56494F45UL;
constexpr ULONG ACTIVATION_SAMPLER_SIGNATURE = 0x56494F4DUL;
constexpr ULONG ACTIVATION_SHADER_SIGNATURE = 0x56494F48UL;
constexpr ULONG ACTIVATION_SHADER_VIEW_SIGNATURE = 0x56494F56UL;
constexpr ULONG ACTIVATION_RENDER_TARGET_VIEW_SIGNATURE = 0x564F5254UL;
constexpr ULONG ACTIVATION_DEPTH_STENCIL_VIEW_SIGNATURE = 0x56445356UL;
constexpr ULONG ACTIVATION_BLEND_STATE_SIGNATURE = 0x56424C44UL;
constexpr ULONG ACTIVATION_DEPTH_STENCIL_STATE_SIGNATURE = 0x56445353UL;
constexpr ULONG ACTIVATION_RASTERIZER_STATE_SIGNATURE = 0x56525354UL;
constexpr ULONG ACTIVATION_UNORDERED_ACCESS_VIEW_SIGNATURE = 0x56554156UL;
constexpr ULONG ACTIVATION_COMMAND_LIST_SIGNATURE = 0x56434C53UL;

enum ACTIVATION_CALL : LONG
{
    ActivationCallFlush = 1,
    ActivationCallDraw,
    ActivationCallDrawIndexed,
    ActivationCallDrawInstanced,
    ActivationCallDrawIndexedInstanced,
    ActivationCallDrawAuto,
    ActivationCallIaSetInputLayout,
    ActivationCallIaSetVertexBuffers,
    ActivationCallIaSetIndexBuffer,
    ActivationCallIaSetTopology,
    ActivationCallVsSetShader,
    ActivationCallPsSetShader,
    ActivationCallGsSetShader,
    ActivationCallCsSetShader,
    ActivationCallVsSetShaderWithIfaces,
    ActivationCallPsSetShaderWithIfaces,
    ActivationCallGsSetShaderWithIfaces,
    ActivationCallHsSetShaderWithIfaces,
    ActivationCallDsSetShaderWithIfaces,
    ActivationCallCsSetShaderWithIfaces,
    ActivationCallCsSetShaderResources,
    ActivationCallCsSetSamplers,
    ActivationCallCsSetConstantBuffers,
    ActivationCallVsSetConstantBuffers,
    ActivationCallPsSetConstantBuffers,
    ActivationCallGsSetConstantBuffers,
    ActivationCallVsSetShaderResources,
    ActivationCallPsSetShaderResources,
    ActivationCallGsSetShaderResources,
    ActivationCallVsSetSamplers,
    ActivationCallPsSetSamplers,
    ActivationCallGsSetSamplers,
    ActivationCallSetRenderTargets,
    ActivationCallSetBlendState,
    ActivationCallSetDepthStencilState,
    ActivationCallSetRasterizerState,
    ActivationCallSetViewports,
    ActivationCallSetScissorRects,
    ActivationCallSetPredication,
    ActivationCallClearRenderTarget,
    ActivationCallClearDepthStencil,
    ActivationCallResourceCopyRegion,
    ActivationCallResourceCopy,
    ActivationCallResourceUpdate,
    ActivationCallConstantBufferUpdate,
    ActivationCallGenerateMips,
    ActivationCallResourceMap,
    ActivationCallResourceUnmap,
    ActivationCallSetStreamOutputTargets,
    ActivationCallResourceResolve,
    ActivationCallSetTextFilterSize,
    ActivationCallDispatch,
    ActivationCallDispatchIndirect,
    ActivationCallDrawIndexedInstancedIndirect,
    ActivationCallDrawInstancedIndirect,
    ActivationCallSetResourceMinLOD,
    ActivationCallHsSetShaderResources,
    ActivationCallHsSetShader,
    ActivationCallHsSetSamplers,
    ActivationCallHsSetConstantBuffers,
    ActivationCallDsSetShaderResources,
    ActivationCallDsSetShader,
    ActivationCallDsSetSamplers,
    ActivationCallDsSetConstantBuffers,
    ActivationCallClearUnorderedAccessViewUint,
    ActivationCallClearUnorderedAccessViewFloat,
    ActivationCallCsSetUnorderedAccessViews,
    ActivationCallCopyStructureCount,
    ActivationCallCommandListExecute,
    ActivationCallCheckDeferredContextHandleSizes,
    ActivationCallCalcDeferredContextHandleSize,
    ActivationCallCreateCommandList,
    ActivationCallDestroyCommandList,
    ActivationCallCreateDeferredContext,
    ActivationCallAbandonCommandList,
    ActivationCallRecycleCreateDeferredContext,
    ActivationCallRecycleCommandList,
    ActivationCallRecycleCreateCommandList,
    ActivationCallRecycleDestroyCommandList,
};

VOID ActivationRecordDeviceCall(D3D10DDI_HDEVICE device, ACTIVATION_CALL call)
{
    ACTIVATION_DEVICE *state = static_cast<ACTIVATION_DEVICE *>(device.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_DEVICE_SIGNATURE)
    {
        return;
    }

    InterlockedExchange(&state->LastCall, static_cast<LONG>(call));
    InterlockedIncrement(&state->CallCount);
}

VOID ActivationRecordDeferredContextCall(D3D10DDI_HDEVICE device, ACTIVATION_CALL call)
{
    ACTIVATION_DEFERRED_CONTEXT *state = static_cast<ACTIVATION_DEFERRED_CONTEXT *>(device.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_DEFERRED_CONTEXT_SIGNATURE)
    {
        return;
    }

    InterlockedExchange(&state->LastCall, static_cast<LONG>(call));
    InterlockedIncrement(&state->CallCount);
}

BOOLEAN ActivationIsObject(PVOID privateData, ULONG signature)
{
    const ACTIVATION_OBJECT *state = static_cast<const ACTIVATION_OBJECT *>(privateData);
    return state != nullptr && state->Signature == signature && state->RuntimeHandle != nullptr;
}

VOID ActivationInitializeObject(PVOID privateData, PVOID runtimeHandle, ULONG signature)
{
    if (privateData == nullptr || runtimeHandle == nullptr)
    {
        return;
    }

    ACTIVATION_OBJECT *state = static_cast<ACTIVATION_OBJECT *>(privateData);
    state->Signature = signature;
    state->RuntimeHandle = runtimeHandle;
}

VOID ActivationDestroyObject(PVOID privateData, ULONG signature)
{
    ACTIVATION_OBJECT *state = static_cast<ACTIVATION_OBJECT *>(privateData);
    if (state == nullptr || state->Signature != signature)
    {
        return;
    }

    state->RuntimeHandle = nullptr;
    state->Signature = 0;
}

/* Recycling keeps the DDI handle's private allocation owned by the runtime;
 * only the runtime identity is invalidated until a recycle-create callback
 * installs the next identity. */
VOID ActivationRecycleObject(PVOID privateData, ULONG signature)
{
    ACTIVATION_OBJECT *state = static_cast<ACTIVATION_OBJECT *>(privateData);
    if (state == nullptr || state->Signature != signature)
    {
        return;
    }

    state->RuntimeHandle = nullptr;
}

SIZE_T ActivationObjectSize()
{
    return sizeof(ACTIVATION_OBJECT);
}

#define VIOGPU_ACTIVATION_OBJECT_CALC(name, argument_type)                                                             \
    SIZE_T APIENTRY name(D3D10DDI_HDEVICE device, const argument_type *arguments)                                      \
    {                                                                                                                  \
        UNREFERENCED_PARAMETER(device);                                                                                \
        UNREFERENCED_PARAMETER(arguments);                                                                             \
        return ActivationObjectSize();                                                                                 \
    }

VIOGPU_ACTIVATION_OBJECT_CALC(ActivationCalcPrivateElementLayoutSize, D3D10DDIARG_CREATEELEMENTLAYOUT)
VIOGPU_ACTIVATION_OBJECT_CALC(ActivationCalcPrivateSamplerSize, D3D10_DDI_SAMPLER_DESC)
VIOGPU_ACTIVATION_OBJECT_CALC(ActivationCalcPrivateShaderResourceViewSize, D3D10DDIARG_CREATESHADERRESOURCEVIEW)
VIOGPU_ACTIVATION_OBJECT_CALC(ActivationCalcPrivateRenderTargetViewSize, D3D10DDIARG_CREATERENDERTARGETVIEW)
VIOGPU_ACTIVATION_OBJECT_CALC(ActivationCalcPrivateDepthStencilViewSize, D3D10DDIARG_CREATEDEPTHSTENCILVIEW)
VIOGPU_ACTIVATION_OBJECT_CALC(ActivationCalcPrivateBlendStateSize, D3D10_DDI_BLEND_DESC)
VIOGPU_ACTIVATION_OBJECT_CALC(ActivationCalcPrivateDepthStencilStateSize, D3D10_DDI_DEPTH_STENCIL_DESC)
VIOGPU_ACTIVATION_OBJECT_CALC(ActivationCalcPrivateRasterizerStateSize, D3D10_DDI_RASTERIZER_DESC)

SIZE_T APIENTRY ActivationCalcPrivateShaderSize(D3D10DDI_HDEVICE device,
                                                const UINT *shaderCode,
                                                const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(shaderCode);
    UNREFERENCED_PARAMETER(signatures);
    return ActivationObjectSize();
}

SIZE_T APIENTRY
ActivationCalcPrivateGeometryShaderWithStreamOutput(D3D10DDI_HDEVICE device,
                                                    const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *arguments,
                                                    const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    UNREFERENCED_PARAMETER(signatures);
    return ActivationObjectSize();
}

VOID APIENTRY ActivationCreateElementLayout(D3D10DDI_HDEVICE device,
                                            const D3D10DDIARG_CREATEELEMENTLAYOUT *arguments,
                                            D3D10DDI_HELEMENTLAYOUT elementLayout,
                                            D3D10DDI_HRTELEMENTLAYOUT runtimeElementLayout)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(elementLayout.pDrvPrivate,
                                   runtimeElementLayout.handle,
                                   ACTIVATION_ELEMENT_LAYOUT_SIGNATURE);
    }
}

VOID APIENTRY ActivationDestroyElementLayout(D3D10DDI_HDEVICE device, D3D10DDI_HELEMENTLAYOUT elementLayout)
{
    UNREFERENCED_PARAMETER(device);
    ActivationDestroyObject(elementLayout.pDrvPrivate, ACTIVATION_ELEMENT_LAYOUT_SIGNATURE);
}

VOID APIENTRY ActivationCreateSampler(D3D10DDI_HDEVICE device,
                                      const D3D10_DDI_SAMPLER_DESC *arguments,
                                      D3D10DDI_HSAMPLER sampler,
                                      D3D10DDI_HRTSAMPLER runtimeSampler)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(sampler.pDrvPrivate, runtimeSampler.handle, ACTIVATION_SAMPLER_SIGNATURE);
    }
}

VOID APIENTRY ActivationDestroySampler(D3D10DDI_HDEVICE device, D3D10DDI_HSAMPLER sampler)
{
    UNREFERENCED_PARAMETER(device);
    ActivationDestroyObject(sampler.pDrvPrivate, ACTIVATION_SAMPLER_SIGNATURE);
}

VOID ActivationCreateShader(D3D10DDI_HSHADER shader, D3D10DDI_HRTSHADER runtimeShader)
{
    ActivationInitializeObject(shader.pDrvPrivate, runtimeShader.handle, ACTIVATION_SHADER_SIGNATURE);
}

VOID APIENTRY ActivationCreateVertexShader(D3D10DDI_HDEVICE device,
                                           const UINT *shaderCode,
                                           D3D10DDI_HSHADER shader,
                                           D3D10DDI_HRTSHADER runtimeShader,
                                           const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(signatures);
    if (shaderCode != nullptr)
    {
        ActivationCreateShader(shader, runtimeShader);
    }
}

VOID APIENTRY ActivationCreateGeometryShader(D3D10DDI_HDEVICE device,
                                             const UINT *shaderCode,
                                             D3D10DDI_HSHADER shader,
                                             D3D10DDI_HRTSHADER runtimeShader,
                                             const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(signatures);
    if (shaderCode != nullptr)
    {
        ActivationCreateShader(shader, runtimeShader);
    }
}

VOID APIENTRY ActivationCreatePixelShader(D3D10DDI_HDEVICE device,
                                          const UINT *shaderCode,
                                          D3D10DDI_HSHADER shader,
                                          D3D10DDI_HRTSHADER runtimeShader,
                                          const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(signatures);
    if (shaderCode != nullptr)
    {
        ActivationCreateShader(shader, runtimeShader);
    }
}

VOID APIENTRY
ActivationCreateGeometryShaderWithStreamOutput(D3D10DDI_HDEVICE device,
                                               const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *arguments,
                                               D3D10DDI_HSHADER shader,
                                               D3D10DDI_HRTSHADER runtimeShader,
                                               const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(signatures);
    if (arguments != nullptr)
    {
        ActivationCreateShader(shader, runtimeShader);
    }
}

VOID APIENTRY ActivationDestroyShader(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader)
{
    UNREFERENCED_PARAMETER(device);
    ActivationDestroyObject(shader.pDrvPrivate, ACTIVATION_SHADER_SIGNATURE);
}

VOID APIENTRY ActivationCreateShaderResourceView(D3D10DDI_HDEVICE device,
                                                 const D3D10DDIARG_CREATESHADERRESOURCEVIEW *arguments,
                                                 D3D10DDI_HSHADERRESOURCEVIEW view,
                                                 D3D10DDI_HRTSHADERRESOURCEVIEW runtimeView)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(view.pDrvPrivate, runtimeView.handle, ACTIVATION_SHADER_VIEW_SIGNATURE);
    }
}

VOID APIENTRY ActivationDestroyShaderResourceView(D3D10DDI_HDEVICE device, D3D10DDI_HSHADERRESOURCEVIEW view)
{
    UNREFERENCED_PARAMETER(device);
    ActivationDestroyObject(view.pDrvPrivate, ACTIVATION_SHADER_VIEW_SIGNATURE);
}

VOID APIENTRY ActivationCreateRenderTargetView(D3D10DDI_HDEVICE device,
                                               const D3D10DDIARG_CREATERENDERTARGETVIEW *arguments,
                                               D3D10DDI_HRENDERTARGETVIEW view,
                                               D3D10DDI_HRTRENDERTARGETVIEW runtimeView)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(view.pDrvPrivate, runtimeView.handle, ACTIVATION_RENDER_TARGET_VIEW_SIGNATURE);
    }
}

VOID APIENTRY ActivationDestroyRenderTargetView(D3D10DDI_HDEVICE device, D3D10DDI_HRENDERTARGETVIEW view)
{
    UNREFERENCED_PARAMETER(device);
    ActivationDestroyObject(view.pDrvPrivate, ACTIVATION_RENDER_TARGET_VIEW_SIGNATURE);
}

VOID APIENTRY ActivationCreateDepthStencilView(D3D10DDI_HDEVICE device,
                                               const D3D10DDIARG_CREATEDEPTHSTENCILVIEW *arguments,
                                               D3D10DDI_HDEPTHSTENCILVIEW view,
                                               D3D10DDI_HRTDEPTHSTENCILVIEW runtimeView)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(view.pDrvPrivate, runtimeView.handle, ACTIVATION_DEPTH_STENCIL_VIEW_SIGNATURE);
    }
}

VOID APIENTRY ActivationDestroyDepthStencilView(D3D10DDI_HDEVICE device, D3D10DDI_HDEPTHSTENCILVIEW view)
{
    UNREFERENCED_PARAMETER(device);
    ActivationDestroyObject(view.pDrvPrivate, ACTIVATION_DEPTH_STENCIL_VIEW_SIGNATURE);
}

VOID APIENTRY ActivationCreateBlendState(D3D10DDI_HDEVICE device,
                                         const D3D10_DDI_BLEND_DESC *arguments,
                                         D3D10DDI_HBLENDSTATE state,
                                         D3D10DDI_HRTBLENDSTATE runtimeState)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(state.pDrvPrivate, runtimeState.handle, ACTIVATION_BLEND_STATE_SIGNATURE);
    }
}

VOID APIENTRY ActivationDestroyBlendState(D3D10DDI_HDEVICE device, D3D10DDI_HBLENDSTATE state)
{
    UNREFERENCED_PARAMETER(device);
    ActivationDestroyObject(state.pDrvPrivate, ACTIVATION_BLEND_STATE_SIGNATURE);
}

VOID APIENTRY ActivationCreateDepthStencilState(D3D10DDI_HDEVICE device,
                                                const D3D10_DDI_DEPTH_STENCIL_DESC *arguments,
                                                D3D10DDI_HDEPTHSTENCILSTATE state,
                                                D3D10DDI_HRTDEPTHSTENCILSTATE runtimeState)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(state.pDrvPrivate, runtimeState.handle, ACTIVATION_DEPTH_STENCIL_STATE_SIGNATURE);
    }
}

VOID APIENTRY ActivationDestroyDepthStencilState(D3D10DDI_HDEVICE device, D3D10DDI_HDEPTHSTENCILSTATE state)
{
    UNREFERENCED_PARAMETER(device);
    ActivationDestroyObject(state.pDrvPrivate, ACTIVATION_DEPTH_STENCIL_STATE_SIGNATURE);
}

VOID APIENTRY ActivationCreateRasterizerState(D3D10DDI_HDEVICE device,
                                              const D3D10_DDI_RASTERIZER_DESC *arguments,
                                              D3D10DDI_HRASTERIZERSTATE state,
                                              D3D10DDI_HRTRASTERIZERSTATE runtimeState)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(state.pDrvPrivate, runtimeState.handle, ACTIVATION_RASTERIZER_STATE_SIGNATURE);
    }
}

VOID APIENTRY ActivationDestroyRasterizerState(D3D10DDI_HDEVICE device, D3D10DDI_HRASTERIZERSTATE state)
{
    UNREFERENCED_PARAMETER(device);
    ActivationDestroyObject(state.pDrvPrivate, ACTIVATION_RASTERIZER_STATE_SIGNATURE);
}

#undef VIOGPU_ACTIVATION_OBJECT_CALC

/* The test device has no rendering entry points.  It only proves that the
 * D3D runtime can allocate and tear down the private device record without
 * handing an uninitialised function table to dxgkrnl. */
VOID APIENTRY ActivationDestroyDevice(D3D10DDI_HDEVICE device)
{
    ACTIVATION_DEVICE *state = static_cast<ACTIVATION_DEVICE *>(device.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_DEVICE_SIGNATURE)
    {
        return;
    }

    state->Signature = 0;
    state->CallCount = 0;
    state->LastCall = 0;
}

/* The opt-in device has no queued GPU work.  Publishing an explicit flush
 * callback keeps the DDI table complete for runtimes that issue a flush while
 * probing the device, without pretending that a command stream was retired. */
VOID APIENTRY ActivationFlush(D3D10DDI_HDEVICE device)
{
    ActivationRecordDeviceCall(device, ActivationCallFlush);
}

/* These callbacks intentionally stop at DDI call-shape validation.  The
 * activation shim must never turn them into a second command transport; the
 * real renderer is Mesa Turnip's Vulkan UMD. */
VOID APIENTRY ActivationDraw(D3D10DDI_HDEVICE device, UINT vertexCount, UINT startVertexLocation)
{
    UNREFERENCED_PARAMETER(vertexCount);
    UNREFERENCED_PARAMETER(startVertexLocation);
    ActivationRecordDeviceCall(device, ActivationCallDraw);
}

VOID APIENTRY ActivationDrawIndexed(D3D10DDI_HDEVICE device,
                                    UINT indexCount,
                                    UINT startIndexLocation,
                                    INT baseVertexLocation)
{
    UNREFERENCED_PARAMETER(indexCount);
    UNREFERENCED_PARAMETER(startIndexLocation);
    UNREFERENCED_PARAMETER(baseVertexLocation);
    ActivationRecordDeviceCall(device, ActivationCallDrawIndexed);
}

VOID APIENTRY ActivationDrawInstanced(D3D10DDI_HDEVICE device,
                                      UINT vertexCountPerInstance,
                                      UINT instanceCount,
                                      UINT startVertexLocation,
                                      UINT startInstanceLocation)
{
    UNREFERENCED_PARAMETER(vertexCountPerInstance);
    UNREFERENCED_PARAMETER(instanceCount);
    UNREFERENCED_PARAMETER(startVertexLocation);
    UNREFERENCED_PARAMETER(startInstanceLocation);
    ActivationRecordDeviceCall(device, ActivationCallDrawInstanced);
}

VOID APIENTRY ActivationDrawIndexedInstanced(D3D10DDI_HDEVICE device,
                                             UINT indexCountPerInstance,
                                             UINT instanceCount,
                                             UINT startIndexLocation,
                                             INT baseVertexLocation,
                                             UINT startInstanceLocation)
{
    UNREFERENCED_PARAMETER(indexCountPerInstance);
    UNREFERENCED_PARAMETER(instanceCount);
    UNREFERENCED_PARAMETER(startIndexLocation);
    UNREFERENCED_PARAMETER(baseVertexLocation);
    UNREFERENCED_PARAMETER(startInstanceLocation);
    ActivationRecordDeviceCall(device, ActivationCallDrawIndexedInstanced);
}

VOID APIENTRY ActivationDrawAuto(D3D10DDI_HDEVICE device)
{
    ActivationRecordDeviceCall(device, ActivationCallDrawAuto);
}

VOID APIENTRY ActivationIaSetInputLayout(D3D10DDI_HDEVICE device, D3D10DDI_HELEMENTLAYOUT layout)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(layout.pDrvPrivate, ACTIVATION_ELEMENT_LAYOUT_SIGNATURE));
    ActivationRecordDeviceCall(device, ActivationCallIaSetInputLayout);
}

VOID APIENTRY ActivationIaSetVertexBuffers(D3D10DDI_HDEVICE device,
                                           UINT startSlot,
                                           UINT numberOfBuffers,
                                           const D3D10DDI_HRESOURCE *resources,
                                           const UINT *strides,
                                           const UINT *offsets)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfBuffers);
    UNREFERENCED_PARAMETER(resources);
    UNREFERENCED_PARAMETER(strides);
    UNREFERENCED_PARAMETER(offsets);
    ActivationRecordDeviceCall(device, ActivationCallIaSetVertexBuffers);
}

VOID APIENTRY ActivationIaSetIndexBuffer(D3D10DDI_HDEVICE device,
                                         D3D10DDI_HRESOURCE resource,
                                         DXGI_FORMAT format,
                                         UINT offset)
{
    UNREFERENCED_PARAMETER(resource);
    UNREFERENCED_PARAMETER(format);
    UNREFERENCED_PARAMETER(offset);
    ActivationRecordDeviceCall(device, ActivationCallIaSetIndexBuffer);
}

VOID APIENTRY ActivationIaSetTopology(D3D10DDI_HDEVICE device, D3D10_DDI_PRIMITIVE_TOPOLOGY topology)
{
    UNREFERENCED_PARAMETER(topology);
    ActivationRecordDeviceCall(device, ActivationCallIaSetTopology);
}

VOID APIENTRY ActivationVsSetShader(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(shader.pDrvPrivate, ACTIVATION_SHADER_SIGNATURE));
    ActivationRecordDeviceCall(device, ActivationCallVsSetShader);
}

VOID APIENTRY ActivationPsSetShader(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(shader.pDrvPrivate, ACTIVATION_SHADER_SIGNATURE));
    ActivationRecordDeviceCall(device, ActivationCallPsSetShader);
}

VOID APIENTRY ActivationGsSetShader(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(shader.pDrvPrivate, ACTIVATION_SHADER_SIGNATURE));
    ActivationRecordDeviceCall(device, ActivationCallGsSetShader);
}

VOID APIENTRY ActivationCsSetShader(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(shader.pDrvPrivate, ACTIVATION_SHADER_SIGNATURE));
    ActivationRecordDeviceCall(device, ActivationCallCsSetShader);
}

VOID APIENTRY ActivationCsSetShaderResources(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numberOfViews,
                                             const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfViews);
    UNREFERENCED_PARAMETER(views);
    ActivationRecordDeviceCall(device, ActivationCallCsSetShaderResources);
}

VOID APIENTRY ActivationCsSetSamplers(D3D10DDI_HDEVICE device,
                                      UINT startSlot,
                                      UINT numberOfSamplers,
                                      const D3D10DDI_HSAMPLER *samplers)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfSamplers);
    UNREFERENCED_PARAMETER(samplers);
    ActivationRecordDeviceCall(device, ActivationCallCsSetSamplers);
}

VOID APIENTRY ActivationCsSetConstantBuffers(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numberOfBuffers,
                                             const D3D10DDI_HRESOURCE *resources)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfBuffers);
    UNREFERENCED_PARAMETER(resources);
    ActivationRecordDeviceCall(device, ActivationCallCsSetConstantBuffers);
}

VOID ActivationSetShaderWithIfaces(D3D10DDI_HDEVICE device,
                                   D3D10DDI_HSHADER shader,
                                   UINT classInstanceCount,
                                   const UINT *classInstances,
                                   const D3D11DDIARG_POINTERDATA *interfacePointerData,
                                   ACTIVATION_CALL call)
{
    if (!ActivationIsObject(shader.pDrvPrivate, ACTIVATION_SHADER_SIGNATURE) ||
        (classInstanceCount != 0 && (classInstances == nullptr || interfacePointerData == nullptr)))
    {
        return;
    }

    UNREFERENCED_PARAMETER(classInstances);
    UNREFERENCED_PARAMETER(interfacePointerData);
    ActivationRecordDeviceCall(device, call);
}

VOID APIENTRY ActivationVsSetShaderWithIfaces(D3D10DDI_HDEVICE device,
                                              D3D10DDI_HSHADER shader,
                                              UINT classInstanceCount,
                                              const UINT *classInstances,
                                              const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
    ActivationSetShaderWithIfaces(device,
                                  shader,
                                  classInstanceCount,
                                  classInstances,
                                  interfacePointerData,
                                  ActivationCallVsSetShaderWithIfaces);
}

VOID APIENTRY ActivationPsSetShaderWithIfaces(D3D10DDI_HDEVICE device,
                                              D3D10DDI_HSHADER shader,
                                              UINT classInstanceCount,
                                              const UINT *classInstances,
                                              const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
    ActivationSetShaderWithIfaces(device,
                                  shader,
                                  classInstanceCount,
                                  classInstances,
                                  interfacePointerData,
                                  ActivationCallPsSetShaderWithIfaces);
}

VOID APIENTRY ActivationGsSetShaderWithIfaces(D3D10DDI_HDEVICE device,
                                              D3D10DDI_HSHADER shader,
                                              UINT classInstanceCount,
                                              const UINT *classInstances,
                                              const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
    ActivationSetShaderWithIfaces(device,
                                  shader,
                                  classInstanceCount,
                                  classInstances,
                                  interfacePointerData,
                                  ActivationCallGsSetShaderWithIfaces);
}

VOID APIENTRY ActivationHsSetShaderWithIfaces(D3D10DDI_HDEVICE device,
                                              D3D10DDI_HSHADER shader,
                                              UINT classInstanceCount,
                                              const UINT *classInstances,
                                              const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
    ActivationSetShaderWithIfaces(device,
                                  shader,
                                  classInstanceCount,
                                  classInstances,
                                  interfacePointerData,
                                  ActivationCallHsSetShaderWithIfaces);
}

VOID APIENTRY ActivationDsSetShaderWithIfaces(D3D10DDI_HDEVICE device,
                                              D3D10DDI_HSHADER shader,
                                              UINT classInstanceCount,
                                              const UINT *classInstances,
                                              const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
    ActivationSetShaderWithIfaces(device,
                                  shader,
                                  classInstanceCount,
                                  classInstances,
                                  interfacePointerData,
                                  ActivationCallDsSetShaderWithIfaces);
}

VOID APIENTRY ActivationCsSetShaderWithIfaces(D3D10DDI_HDEVICE device,
                                              D3D10DDI_HSHADER shader,
                                              UINT classInstanceCount,
                                              const UINT *classInstances,
                                              const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
    ActivationSetShaderWithIfaces(device,
                                  shader,
                                  classInstanceCount,
                                  classInstances,
                                  interfacePointerData,
                                  ActivationCallCsSetShaderWithIfaces);
}

VOID APIENTRY ActivationVsSetConstantBuffers(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numberOfBuffers,
                                             const D3D10DDI_HRESOURCE *resources)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfBuffers);
    UNREFERENCED_PARAMETER(resources);
    ActivationRecordDeviceCall(device, ActivationCallVsSetConstantBuffers);
}

VOID APIENTRY ActivationPsSetConstantBuffers(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numberOfBuffers,
                                             const D3D10DDI_HRESOURCE *resources)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfBuffers);
    UNREFERENCED_PARAMETER(resources);
    ActivationRecordDeviceCall(device, ActivationCallPsSetConstantBuffers);
}

VOID APIENTRY ActivationGsSetConstantBuffers(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numberOfBuffers,
                                             const D3D10DDI_HRESOURCE *resources)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfBuffers);
    UNREFERENCED_PARAMETER(resources);
    ActivationRecordDeviceCall(device, ActivationCallGsSetConstantBuffers);
}

VOID APIENTRY ActivationVsSetShaderResources(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numberOfViews,
                                             const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfViews);
    UNREFERENCED_PARAMETER(views);
    ActivationRecordDeviceCall(device, ActivationCallVsSetShaderResources);
}

VOID APIENTRY ActivationPsSetShaderResources(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numberOfViews,
                                             const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfViews);
    UNREFERENCED_PARAMETER(views);
    ActivationRecordDeviceCall(device, ActivationCallPsSetShaderResources);
}

VOID APIENTRY ActivationGsSetShaderResources(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numberOfViews,
                                             const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfViews);
    UNREFERENCED_PARAMETER(views);
    ActivationRecordDeviceCall(device, ActivationCallGsSetShaderResources);
}

VOID APIENTRY ActivationVsSetSamplers(D3D10DDI_HDEVICE device,
                                      UINT startSlot,
                                      UINT numberOfSamplers,
                                      const D3D10DDI_HSAMPLER *samplers)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfSamplers);
    UNREFERENCED_PARAMETER(samplers);
    ActivationRecordDeviceCall(device, ActivationCallVsSetSamplers);
}

VOID APIENTRY ActivationPsSetSamplers(D3D10DDI_HDEVICE device,
                                      UINT startSlot,
                                      UINT numberOfSamplers,
                                      const D3D10DDI_HSAMPLER *samplers)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfSamplers);
    UNREFERENCED_PARAMETER(samplers);
    ActivationRecordDeviceCall(device, ActivationCallPsSetSamplers);
}

VOID APIENTRY ActivationGsSetSamplers(D3D10DDI_HDEVICE device,
                                      UINT startSlot,
                                      UINT numberOfSamplers,
                                      const D3D10DDI_HSAMPLER *samplers)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numberOfSamplers);
    UNREFERENCED_PARAMETER(samplers);
    ActivationRecordDeviceCall(device, ActivationCallGsSetSamplers);
}

VOID APIENTRY ActivationSetRenderTargets(D3D10DDI_HDEVICE device,
                                         const D3D10DDI_HRENDERTARGETVIEW *renderTargetViews,
                                         UINT numberOfViews,
                                         UINT clearViews,
                                         D3D10DDI_HDEPTHSTENCILVIEW depthStencilView)
{
    UNREFERENCED_PARAMETER(renderTargetViews);
    UNREFERENCED_PARAMETER(numberOfViews);
    UNREFERENCED_PARAMETER(clearViews);
    UNREFERENCED_PARAMETER(depthStencilView);
    ActivationRecordDeviceCall(device, ActivationCallSetRenderTargets);
}

VOID APIENTRY ActivationSetBlendState(D3D10DDI_HDEVICE device,
                                      D3D10DDI_HBLENDSTATE blendState,
                                      const FLOAT blendFactor[4],
                                      UINT sampleMask)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(blendState.pDrvPrivate, ACTIVATION_BLEND_STATE_SIGNATURE));
    UNREFERENCED_PARAMETER(blendFactor);
    UNREFERENCED_PARAMETER(sampleMask);
    ActivationRecordDeviceCall(device, ActivationCallSetBlendState);
}

VOID APIENTRY ActivationSetDepthStencilState(D3D10DDI_HDEVICE device,
                                             D3D10DDI_HDEPTHSTENCILSTATE depthStencilState,
                                             UINT stencilRef)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(depthStencilState.pDrvPrivate, ACTIVATION_DEPTH_STENCIL_STATE_SIGNATURE));
    UNREFERENCED_PARAMETER(stencilRef);
    ActivationRecordDeviceCall(device, ActivationCallSetDepthStencilState);
}

VOID APIENTRY ActivationSetRasterizerState(D3D10DDI_HDEVICE device, D3D10DDI_HRASTERIZERSTATE rasterizerState)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(rasterizerState.pDrvPrivate, ACTIVATION_RASTERIZER_STATE_SIGNATURE));
    ActivationRecordDeviceCall(device, ActivationCallSetRasterizerState);
}

VOID APIENTRY ActivationSetViewports(D3D10DDI_HDEVICE device,
                                     UINT numberOfViewports,
                                     UINT clearViewports,
                                     const D3D10_DDI_VIEWPORT *viewports)
{
    UNREFERENCED_PARAMETER(numberOfViewports);
    UNREFERENCED_PARAMETER(clearViewports);
    UNREFERENCED_PARAMETER(viewports);
    ActivationRecordDeviceCall(device, ActivationCallSetViewports);
}

VOID APIENTRY ActivationSetScissorRects(D3D10DDI_HDEVICE device,
                                        UINT numberOfRects,
                                        UINT clearRects,
                                        const D3D10_DDI_RECT *rects)
{
    UNREFERENCED_PARAMETER(numberOfRects);
    UNREFERENCED_PARAMETER(clearRects);
    UNREFERENCED_PARAMETER(rects);
    ActivationRecordDeviceCall(device, ActivationCallSetScissorRects);
}

VOID APIENTRY ActivationSetPredication(D3D10DDI_HDEVICE device, D3D10DDI_HQUERY query, BOOL predicateValue)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(query.pDrvPrivate, ACTIVATION_QUERY_SIGNATURE));
    UNREFERENCED_PARAMETER(predicateValue);
    ActivationRecordDeviceCall(device, ActivationCallSetPredication);
}

VOID APIENTRY ActivationClearRenderTargetView(D3D10DDI_HDEVICE device,
                                              D3D10DDI_HRENDERTARGETVIEW view,
                                              FLOAT colorRGBA[4])
{
    UNREFERENCED_PARAMETER(ActivationIsObject(view.pDrvPrivate, ACTIVATION_RENDER_TARGET_VIEW_SIGNATURE));
    UNREFERENCED_PARAMETER(colorRGBA);
    ActivationRecordDeviceCall(device, ActivationCallClearRenderTarget);
}

VOID APIENTRY ActivationClearDepthStencilView(D3D10DDI_HDEVICE device,
                                              D3D10DDI_HDEPTHSTENCILVIEW view,
                                              UINT flags,
                                              FLOAT depth,
                                              UINT8 stencil)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(view.pDrvPrivate, ACTIVATION_DEPTH_STENCIL_VIEW_SIGNATURE));
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(depth);
    UNREFERENCED_PARAMETER(stencil);
    ActivationRecordDeviceCall(device, ActivationCallClearDepthStencil);
}

VOID APIENTRY ActivationResourceCopyRegion(D3D10DDI_HDEVICE device,
                                           D3D10DDI_HRESOURCE destination,
                                           UINT destinationSubresource,
                                           UINT destinationX,
                                           UINT destinationY,
                                           UINT destinationZ,
                                           D3D10DDI_HRESOURCE source,
                                           UINT sourceSubresource,
                                           const D3D10_DDI_BOX *sourceBox)
{
    UNREFERENCED_PARAMETER(destination);
    UNREFERENCED_PARAMETER(destinationSubresource);
    UNREFERENCED_PARAMETER(destinationX);
    UNREFERENCED_PARAMETER(destinationY);
    UNREFERENCED_PARAMETER(destinationZ);
    UNREFERENCED_PARAMETER(source);
    UNREFERENCED_PARAMETER(sourceSubresource);
    UNREFERENCED_PARAMETER(sourceBox);
    ActivationRecordDeviceCall(device, ActivationCallResourceCopyRegion);
}

VOID APIENTRY ActivationResourceCopy(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE destination, D3D10DDI_HRESOURCE source)
{
    UNREFERENCED_PARAMETER(destination);
    UNREFERENCED_PARAMETER(source);
    ActivationRecordDeviceCall(device, ActivationCallResourceCopy);
}

VOID APIENTRY ActivationResourceUpdateSubresourceUP(D3D10DDI_HDEVICE device,
                                                    D3D10DDI_HRESOURCE destination,
                                                    UINT destinationSubresource,
                                                    const D3D10_DDI_BOX *destinationBox,
                                                    const VOID *sourceData,
                                                    UINT sourceRowPitch,
                                                    UINT sourceDepthPitch)
{
    UNREFERENCED_PARAMETER(destination);
    UNREFERENCED_PARAMETER(destinationSubresource);
    UNREFERENCED_PARAMETER(destinationBox);
    UNREFERENCED_PARAMETER(sourceData);
    UNREFERENCED_PARAMETER(sourceRowPitch);
    UNREFERENCED_PARAMETER(sourceDepthPitch);
    ActivationRecordDeviceCall(device, ActivationCallResourceUpdate);
}

VOID APIENTRY ActivationDefaultConstantBufferUpdateSubresourceUP(D3D10DDI_HDEVICE device,
                                                                 D3D10DDI_HRESOURCE destination,
                                                                 UINT destinationSubresource,
                                                                 const D3D10_DDI_BOX *destinationBox,
                                                                 const VOID *sourceData,
                                                                 UINT sourceRowPitch,
                                                                 UINT sourceDepthPitch)
{
    UNREFERENCED_PARAMETER(destination);
    UNREFERENCED_PARAMETER(destinationSubresource);
    UNREFERENCED_PARAMETER(destinationBox);
    UNREFERENCED_PARAMETER(sourceData);
    UNREFERENCED_PARAMETER(sourceRowPitch);
    UNREFERENCED_PARAMETER(sourceDepthPitch);
    ActivationRecordDeviceCall(device, ActivationCallConstantBufferUpdate);
}

VOID APIENTRY ActivationGenerateMips(D3D10DDI_HDEVICE device, D3D10DDI_HSHADERRESOURCEVIEW view)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(view.pDrvPrivate, ACTIVATION_SHADER_VIEW_SIGNATURE));
    ActivationRecordDeviceCall(device, ActivationCallGenerateMips);
}

/* Mapping is deliberately a shape-only operation in the activation probe.
 * There is no backing allocation to expose, but a zeroed output prevents the
 * runtime from consuming uninitialised pointers or pitches during a probe. */
VOID APIENTRY ActivationResourceMap(D3D10DDI_HDEVICE device,
                                    D3D10DDI_HRESOURCE resource,
                                    UINT subresource,
                                    D3D10_DDI_MAP mapType,
                                    UINT mapFlags,
                                    D3D10DDI_MAPPED_SUBRESOURCE *mappedResource)
{
    UNREFERENCED_PARAMETER(subresource);
    UNREFERENCED_PARAMETER(mapType);
    UNREFERENCED_PARAMETER(mapFlags);
    ACTIVATION_RESOURCE *state = static_cast<ACTIVATION_RESOURCE *>(resource.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_RESOURCE_SIGNATURE)
    {
        return;
    }
    if (mappedResource != nullptr)
    {
        ZeroMemory(mappedResource, sizeof(*mappedResource));
    }
    ActivationRecordDeviceCall(device, ActivationCallResourceMap);
}

VOID APIENTRY ActivationResourceUnmap(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource, UINT subresource)
{
    UNREFERENCED_PARAMETER(subresource);
    ACTIVATION_RESOURCE *state = static_cast<ACTIVATION_RESOURCE *>(resource.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_RESOURCE_SIGNATURE)
    {
        return;
    }
    ActivationRecordDeviceCall(device, ActivationCallResourceUnmap);
}

VOID APIENTRY ActivationSetStreamOutputTargets(D3D10DDI_HDEVICE device,
                                               UINT numberOfBuffers,
                                               UINT clearTargets,
                                               const D3D10DDI_HRESOURCE *resources,
                                               const UINT *offsets)
{
    UNREFERENCED_PARAMETER(numberOfBuffers);
    UNREFERENCED_PARAMETER(clearTargets);
    UNREFERENCED_PARAMETER(resources);
    UNREFERENCED_PARAMETER(offsets);
    ActivationRecordDeviceCall(device, ActivationCallSetStreamOutputTargets);
}

VOID APIENTRY ActivationResolveSubresource(D3D10DDI_HDEVICE device,
                                           D3D10DDI_HRESOURCE destination,
                                           UINT destinationSubresource,
                                           D3D10DDI_HRESOURCE source,
                                           UINT sourceSubresource,
                                           DXGI_FORMAT format)
{
    UNREFERENCED_PARAMETER(destination);
    UNREFERENCED_PARAMETER(destinationSubresource);
    UNREFERENCED_PARAMETER(source);
    UNREFERENCED_PARAMETER(sourceSubresource);
    UNREFERENCED_PARAMETER(format);
    ActivationRecordDeviceCall(device, ActivationCallResourceResolve);
}

VOID APIENTRY ActivationSetTextFilterSize(D3D10DDI_HDEVICE device, UINT width, UINT height)
{
    UNREFERENCED_PARAMETER(width);
    UNREFERENCED_PARAMETER(height);
    ActivationRecordDeviceCall(device, ActivationCallSetTextFilterSize);
}

/* D3D11's compute and indirect callbacks are present only in the opt-in
 * activation probe. They record the call shape and never emit a command or
 * touch a resource, so the probe cannot accidentally become a renderer. */
VOID APIENTRY ActivationDispatch(D3D10DDI_HDEVICE device, UINT x, UINT y, UINT z)
{
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);
    UNREFERENCED_PARAMETER(z);
    ActivationRecordDeviceCall(device, ActivationCallDispatch);
}

VOID APIENTRY ActivationDispatchIndirect(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource, UINT offset)
{
    UNREFERENCED_PARAMETER(resource);
    UNREFERENCED_PARAMETER(offset);
    ActivationRecordDeviceCall(device, ActivationCallDispatchIndirect);
}

VOID APIENTRY ActivationDrawIndexedInstancedIndirect(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource, UINT offset)
{
    UNREFERENCED_PARAMETER(resource);
    UNREFERENCED_PARAMETER(offset);
    ActivationRecordDeviceCall(device, ActivationCallDrawIndexedInstancedIndirect);
}

VOID APIENTRY ActivationDrawInstancedIndirect(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource, UINT offset)
{
    UNREFERENCED_PARAMETER(resource);
    UNREFERENCED_PARAMETER(offset);
    ActivationRecordDeviceCall(device, ActivationCallDrawInstancedIndirect);
}

VOID APIENTRY ActivationSetResourceMinLOD(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource, FLOAT minLod)
{
    UNREFERENCED_PARAMETER(resource);
    UNREFERENCED_PARAMETER(minLod);
    ActivationRecordDeviceCall(device, ActivationCallSetResourceMinLOD);
}

SIZE_T APIENTRY ActivationCalcPrivateTessellationShaderSize(D3D10DDI_HDEVICE device,
                                                            const UINT *shaderCode,
                                                            const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(shaderCode);
    UNREFERENCED_PARAMETER(signatures);
    return ActivationObjectSize();
}

VOID APIENTRY ActivationCreateHullShader(D3D10DDI_HDEVICE device,
                                         const UINT *shaderCode,
                                         D3D10DDI_HSHADER shader,
                                         D3D10DDI_HRTSHADER runtimeShader,
                                         const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(signatures);
    if (shaderCode != nullptr)
    {
        ActivationCreateShader(shader, runtimeShader);
    }
}

VOID APIENTRY ActivationCreateDomainShader(D3D10DDI_HDEVICE device,
                                           const UINT *shaderCode,
                                           D3D10DDI_HSHADER shader,
                                           D3D10DDI_HRTSHADER runtimeShader,
                                           const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(signatures);
    if (shaderCode != nullptr)
    {
        ActivationCreateShader(shader, runtimeShader);
    }
}

VOID APIENTRY ActivationHsSetShaderResources(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numViews,
                                             const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numViews);
    UNREFERENCED_PARAMETER(views);
    ActivationRecordDeviceCall(device, ActivationCallHsSetShaderResources);
}

VOID APIENTRY ActivationHsSetShader(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(shader.pDrvPrivate, ACTIVATION_SHADER_SIGNATURE));
    ActivationRecordDeviceCall(device, ActivationCallHsSetShader);
}

VOID APIENTRY ActivationHsSetSamplers(D3D10DDI_HDEVICE device,
                                      UINT startSlot,
                                      UINT numSamplers,
                                      const D3D10DDI_HSAMPLER *samplers)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numSamplers);
    UNREFERENCED_PARAMETER(samplers);
    ActivationRecordDeviceCall(device, ActivationCallHsSetSamplers);
}

VOID APIENTRY ActivationHsSetConstantBuffers(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numBuffers,
                                             const D3D10DDI_HRESOURCE *buffers)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numBuffers);
    UNREFERENCED_PARAMETER(buffers);
    ActivationRecordDeviceCall(device, ActivationCallHsSetConstantBuffers);
}

VOID APIENTRY ActivationDsSetShaderResources(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numViews,
                                             const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numViews);
    UNREFERENCED_PARAMETER(views);
    ActivationRecordDeviceCall(device, ActivationCallDsSetShaderResources);
}

VOID APIENTRY ActivationDsSetShader(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader)
{
    UNREFERENCED_PARAMETER(ActivationIsObject(shader.pDrvPrivate, ACTIVATION_SHADER_SIGNATURE));
    ActivationRecordDeviceCall(device, ActivationCallDsSetShader);
}

VOID APIENTRY ActivationDsSetSamplers(D3D10DDI_HDEVICE device,
                                      UINT startSlot,
                                      UINT numSamplers,
                                      const D3D10DDI_HSAMPLER *samplers)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numSamplers);
    UNREFERENCED_PARAMETER(samplers);
    ActivationRecordDeviceCall(device, ActivationCallDsSetSamplers);
}

VOID APIENTRY ActivationDsSetConstantBuffers(D3D10DDI_HDEVICE device,
                                             UINT startSlot,
                                             UINT numBuffers,
                                             const D3D10DDI_HRESOURCE *buffers)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numBuffers);
    UNREFERENCED_PARAMETER(buffers);
    ActivationRecordDeviceCall(device, ActivationCallDsSetConstantBuffers);
}

SIZE_T APIENTRY ActivationCalcPrivateUnorderedAccessViewSize(D3D10DDI_HDEVICE device,
                                                             const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *arguments)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    return ActivationObjectSize();
}

VOID APIENTRY ActivationCreateUnorderedAccessView(D3D10DDI_HDEVICE device,
                                                  const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *arguments,
                                                  D3D11DDI_HUNORDEREDACCESSVIEW view,
                                                  D3D11DDI_HRTUNORDEREDACCESSVIEW runtimeView)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    ActivationInitializeObject(view.pDrvPrivate, runtimeView.handle, ACTIVATION_UNORDERED_ACCESS_VIEW_SIGNATURE);
}

VOID APIENTRY ActivationDestroyUnorderedAccessView(D3D10DDI_HDEVICE device, D3D11DDI_HUNORDEREDACCESSVIEW view)
{
    UNREFERENCED_PARAMETER(device);
    ActivationDestroyObject(view.pDrvPrivate, ACTIVATION_UNORDERED_ACCESS_VIEW_SIGNATURE);
}

VOID APIENTRY ActivationClearUnorderedAccessViewUint(D3D10DDI_HDEVICE device,
                                                     D3D11DDI_HUNORDEREDACCESSVIEW view,
                                                     const UINT values[4])
{
    UNREFERENCED_PARAMETER(ActivationIsObject(view.pDrvPrivate, ACTIVATION_UNORDERED_ACCESS_VIEW_SIGNATURE));
    UNREFERENCED_PARAMETER(values);
    ActivationRecordDeviceCall(device, ActivationCallClearUnorderedAccessViewUint);
}

VOID APIENTRY ActivationClearUnorderedAccessViewFloat(D3D10DDI_HDEVICE device,
                                                      D3D11DDI_HUNORDEREDACCESSVIEW view,
                                                      const FLOAT values[4])
{
    UNREFERENCED_PARAMETER(ActivationIsObject(view.pDrvPrivate, ACTIVATION_UNORDERED_ACCESS_VIEW_SIGNATURE));
    UNREFERENCED_PARAMETER(values);
    ActivationRecordDeviceCall(device, ActivationCallClearUnorderedAccessViewFloat);
}

VOID APIENTRY ActivationCsSetUnorderedAccessViews(D3D10DDI_HDEVICE device,
                                                  UINT startSlot,
                                                  UINT numViews,
                                                  const D3D11DDI_HUNORDEREDACCESSVIEW *views,
                                                  const UINT *initialCounts)
{
    UNREFERENCED_PARAMETER(startSlot);
    UNREFERENCED_PARAMETER(numViews);
    UNREFERENCED_PARAMETER(views);
    UNREFERENCED_PARAMETER(initialCounts);
    ActivationRecordDeviceCall(device, ActivationCallCsSetUnorderedAccessViews);
}

VOID APIENTRY ActivationCopyStructureCount(D3D10DDI_HDEVICE device,
                                           D3D10DDI_HRESOURCE destination,
                                           UINT destinationOffset,
                                           D3D11DDI_HUNORDEREDACCESSVIEW source)
{
    UNREFERENCED_PARAMETER(destination);
    UNREFERENCED_PARAMETER(destinationOffset);
    UNREFERENCED_PARAMETER(ActivationIsObject(source.pDrvPrivate, ACTIVATION_UNORDERED_ACCESS_VIEW_SIGNATURE));
    ActivationRecordDeviceCall(device, ActivationCallCopyStructureCount);
}

VOID APIENTRY ActivationCommandListExecute(D3D10DDI_HDEVICE device, D3D11DDI_HCOMMANDLIST commandList)
{
    if (!ActivationIsObject(commandList.pDrvPrivate, ACTIVATION_COMMAND_LIST_SIGNATURE))
    {
        return;
    }
    ActivationRecordDeviceCall(device, ActivationCallCommandListExecute);
}

VOID APIENTRY ActivationCheckDeferredContextHandleSizes(D3D10DDI_HDEVICE device,
                                                        UINT *handleSizeArray,
                                                        D3D11DDI_HANDLESIZE *handleSizes);
SIZE_T APIENTRY ActivationCalcDeferredContextHandleSize(D3D10DDI_HDEVICE device,
                                                        D3D11DDI_HANDLETYPE handleType,
                                                        VOID *handleData);
SIZE_T APIENTRY ActivationCalcPrivateDeferredContextSize(D3D10DDI_HDEVICE device,
                                                         const D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE *arguments);
VOID APIENTRY ActivationCreateDeferredContext(D3D10DDI_HDEVICE device,
                                              const D3D11DDIARG_CREATEDEFERREDCONTEXT *arguments);
VOID APIENTRY ActivationAbandonCommandList(D3D10DDI_HDEVICE device);
HRESULT APIENTRY ActivationRecycleCreateDeferredContext(D3D10DDI_HDEVICE device,
                                                        const D3D11DDIARG_CREATEDEFERREDCONTEXT *arguments);
VOID APIENTRY ActivationRecycleCommandList(D3D10DDI_HDEVICE device, D3D11DDI_HCOMMANDLIST commandList);
HRESULT APIENTRY ActivationRecycleCreateCommandList(D3D10DDI_HDEVICE device,
                                                    const D3D11DDIARG_CREATECOMMANDLIST *arguments,
                                                    D3D11DDI_HCOMMANDLIST commandList,
                                                    D3D11DDI_HRTCOMMANDLIST runtimeCommandList);
VOID APIENTRY ActivationRecycleDestroyCommandList(D3D10DDI_HDEVICE device, D3D11DDI_HCOMMANDLIST commandList);

SIZE_T APIENTRY ActivationCalcPrivateCommandListSize(D3D10DDI_HDEVICE device,
                                                     const D3D11DDIARG_CREATECOMMANDLIST *arguments)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    return ActivationObjectSize();
}

VOID APIENTRY ActivationCreateCommandList(D3D10DDI_HDEVICE device,
                                          const D3D11DDIARG_CREATECOMMANDLIST *arguments,
                                          D3D11DDI_HCOMMANDLIST commandList,
                                          D3D11DDI_HRTCOMMANDLIST runtimeCommandList)
{
    if (arguments == nullptr || commandList.pDrvPrivate == nullptr || runtimeCommandList.handle == nullptr)
    {
        return;
    }
    ActivationInitializeObject(commandList.pDrvPrivate, runtimeCommandList.handle, ACTIVATION_COMMAND_LIST_SIGNATURE);
    ActivationRecordDeviceCall(device, ActivationCallCreateCommandList);
}

VOID APIENTRY ActivationDestroyCommandList(D3D10DDI_HDEVICE device, D3D11DDI_HCOMMANDLIST commandList)
{
    if (!ActivationIsObject(commandList.pDrvPrivate, ACTIVATION_COMMAND_LIST_SIGNATURE))
    {
        return;
    }
    ActivationDestroyObject(commandList.pDrvPrivate, ACTIVATION_COMMAND_LIST_SIGNATURE);
    ActivationRecordDeviceCall(device, ActivationCallDestroyCommandList);
}

SIZE_T APIENTRY ActivationCalcPrivateDeferredContextSize(D3D10DDI_HDEVICE device,
                                                         const D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE *arguments)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments == nullptr || arguments->Flags != 0)
    {
        return 0;
    }
    return sizeof(ACTIVATION_DEFERRED_CONTEXT);
}

VOID APIENTRY ActivationCreateDeferredContext(D3D10DDI_HDEVICE device,
                                              const D3D11DDIARG_CREATEDEFERREDCONTEXT *arguments)
{
    if (arguments == nullptr || arguments->Flags != 0 || arguments->hDrvContext.pDrvPrivate == nullptr)
    {
        return;
    }

    ACTIVATION_DEFERRED_CONTEXT *state = static_cast<ACTIVATION_DEFERRED_CONTEXT *>(arguments->hDrvContext.pDrvPrivate);
    state->Signature = ACTIVATION_DEFERRED_CONTEXT_SIGNATURE;
    state->RuntimeCoreLayer = arguments->hRTCoreLayer;
    state->Flags = arguments->Flags;
    state->CallCount = 0;
    state->LastCall = 0;

    /* A deferred context receives its own D3D11 table.  Populate only the
     * command-list/lifetime callbacks that the guarded probe can exercise;
     * product builds leave this entire family absent until a real command ABI
     * exists. */
    if (arguments->p11ContextFuncs != nullptr)
    {
        D3D11DDI_DEVICEFUNCS *functions = arguments->p11ContextFuncs;
        functions->pfnCheckDeferredContextHandleSizes = ActivationCheckDeferredContextHandleSizes;
        functions->pfnCalcDeferredContextHandleSize = ActivationCalcDeferredContextHandleSize;
        functions->pfnCalcPrivateDeferredContextSize = ActivationCalcPrivateDeferredContextSize;
        functions->pfnCreateDeferredContext = ActivationCreateDeferredContext;
        functions->pfnAbandonCommandList = ActivationAbandonCommandList;
        functions->pfnCalcPrivateCommandListSize = ActivationCalcPrivateCommandListSize;
        functions->pfnCreateCommandList = ActivationCreateCommandList;
        functions->pfnCommandListExecute = ActivationCommandListExecute;
        functions->pfnDestroyCommandList = ActivationDestroyCommandList;
        functions->pfnRecycleCommandList = ActivationRecycleCommandList;
        functions->pfnRecycleCreateCommandList = ActivationRecycleCreateCommandList;
        functions->pfnRecycleCreateDeferredContext = ActivationRecycleCreateDeferredContext;
        functions->pfnRecycleDestroyCommandList = ActivationRecycleDestroyCommandList;
    }
    ActivationRecordDeviceCall(device, ActivationCallCreateDeferredContext);
}

VOID APIENTRY ActivationAbandonCommandList(D3D10DDI_HDEVICE device)
{
    ACTIVATION_DEFERRED_CONTEXT *state = static_cast<ACTIVATION_DEFERRED_CONTEXT *>(device.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_DEFERRED_CONTEXT_SIGNATURE)
    {
        return;
    }

    /* The activation probe has no recorded pipeline state. Clearing the
     * context flags still models the runtime's abandon-to-clear transition. */
    state->Flags = 0;
    ActivationRecordDeferredContextCall(device, ActivationCallAbandonCommandList);
}

HRESULT APIENTRY ActivationRecycleCreateDeferredContext(D3D10DDI_HDEVICE device,
                                                        const D3D11DDIARG_CREATEDEFERREDCONTEXT *arguments)
{
    if (arguments == nullptr || arguments->Flags != 0 || arguments->hDrvContext.pDrvPrivate == nullptr)
    {
        return E_INVALIDARG;
    }

    ACTIVATION_DEFERRED_CONTEXT *state = static_cast<ACTIVATION_DEFERRED_CONTEXT *>(arguments->hDrvContext.pDrvPrivate);
    state->Signature = ACTIVATION_DEFERRED_CONTEXT_SIGNATURE;
    state->RuntimeCoreLayer = arguments->hRTCoreLayer;
    state->Flags = arguments->Flags;
    state->CallCount = 0;
    state->LastCall = 0;
    ActivationRecordDeviceCall(device, ActivationCallRecycleCreateDeferredContext);
    return S_OK;
}

VOID APIENTRY ActivationRecycleCommandList(D3D10DDI_HDEVICE device, D3D11DDI_HCOMMANDLIST commandList)
{
    ActivationRecycleObject(commandList.pDrvPrivate, ACTIVATION_COMMAND_LIST_SIGNATURE);
    ActivationRecordDeviceCall(device, ActivationCallRecycleCommandList);
}

HRESULT APIENTRY ActivationRecycleCreateCommandList(D3D10DDI_HDEVICE device,
                                                    const D3D11DDIARG_CREATECOMMANDLIST *arguments,
                                                    D3D11DDI_HCOMMANDLIST commandList,
                                                    D3D11DDI_HRTCOMMANDLIST runtimeCommandList)
{
    if (arguments == nullptr || commandList.pDrvPrivate == nullptr || runtimeCommandList.handle == nullptr)
    {
        return E_INVALIDARG;
    }
    ActivationInitializeObject(commandList.pDrvPrivate, runtimeCommandList.handle, ACTIVATION_COMMAND_LIST_SIGNATURE);
    ActivationRecordDeviceCall(device, ActivationCallRecycleCreateCommandList);
    return S_OK;
}

VOID APIENTRY ActivationRecycleDestroyCommandList(D3D10DDI_HDEVICE device, D3D11DDI_HCOMMANDLIST commandList)
{
    ActivationRecycleObject(commandList.pDrvPrivate, ACTIVATION_COMMAND_LIST_SIGNATURE);
    ActivationRecordDeviceCall(device, ActivationCallRecycleDestroyCommandList);
}

VOID APIENTRY ActivationCheckDeferredContextHandleSizes(D3D10DDI_HDEVICE device,
                                                        UINT *handleSizeArray,
                                                        D3D11DDI_HANDLESIZE *handleSizes)
{
    /* No deferred-context objects are advertised by this probe.  The D3D11
     * runtime double-polls this callback, so always publish an empty list and
     * never write the optional array. */
    UNREFERENCED_PARAMETER(handleSizes);
    if (handleSizeArray != nullptr)
    {
        *handleSizeArray = 0;
    }
    ActivationRecordDeviceCall(device, ActivationCallCheckDeferredContextHandleSizes);
}

SIZE_T APIENTRY ActivationCalcDeferredContextHandleSize(D3D10DDI_HDEVICE device,
                                                        D3D11DDI_HANDLETYPE handleType,
                                                        VOID *handleData)
{
    UNREFERENCED_PARAMETER(handleType);
    UNREFERENCED_PARAMETER(handleData);
    ActivationRecordDeviceCall(device, ActivationCallCalcDeferredContextHandleSize);
    return 0;
}

VOID APIENTRY ActivationCreateComputeShader(D3D10DDI_HDEVICE device,
                                            const UINT *shaderCode,
                                            D3D10DDI_HSHADER shader,
                                            D3D10DDI_HRTSHADER runtimeShader)
{
    UNREFERENCED_PARAMETER(device);
    if (shaderCode != nullptr)
    {
        ActivationCreateShader(shader, runtimeShader);
    }
}

VOID APIENTRY ActivationSetRenderTargets11(D3D10DDI_HDEVICE device,
                                           const D3D10DDI_HRENDERTARGETVIEW *renderTargetViews,
                                           UINT numberOfViews,
                                           UINT clearViews,
                                           D3D10DDI_HDEPTHSTENCILVIEW depthStencilView,
                                           const D3D11DDI_HUNORDEREDACCESSVIEW *unorderedAccessViews,
                                           const UINT *uavInitialCounts,
                                           UINT uavStartSlot,
                                           UINT numberOfUavs,
                                           UINT uavRangeStart,
                                           UINT uavRangeSize)
{
    UNREFERENCED_PARAMETER(renderTargetViews);
    UNREFERENCED_PARAMETER(numberOfViews);
    UNREFERENCED_PARAMETER(clearViews);
    UNREFERENCED_PARAMETER(depthStencilView);
    UNREFERENCED_PARAMETER(unorderedAccessViews);
    UNREFERENCED_PARAMETER(uavInitialCounts);
    UNREFERENCED_PARAMETER(uavStartSlot);
    UNREFERENCED_PARAMETER(numberOfUavs);
    UNREFERENCED_PARAMETER(uavRangeStart);
    UNREFERENCED_PARAMETER(uavRangeSize);
    ActivationRecordDeviceCall(device, ActivationCallSetRenderTargets);
}

VOID APIENTRY ActivationRelocateDeviceFuncs11(D3D10DDI_HDEVICE device, D3D11DDI_DEVICEFUNCS *functions)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(functions);
}

/* No resource hazard exists in the activation-only device: resource creation
 * is a private lifetime probe and does not expose a renderable allocation. */
VOID APIENTRY ActivationResourceReadAfterWriteHazard(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(resource);
}

BOOL APIENTRY ActivationResourceIsStagingBusy(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(resource);
    /* The probe device never queues work, so a valid staging owner is idle. */
    return FALSE;
}

VOID APIENTRY ActivationShaderResourceViewReadAfterWriteHazard(D3D10DDI_HDEVICE device,
                                                               D3D10DDI_HSHADERRESOURCEVIEW view,
                                                               D3D10DDI_HRESOURCE resource)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(view);
    UNREFERENCED_PARAMETER(resource);
}

/* The test table is immutable after CreateDevice, so relocation is an
 * intentional no-op rather than an uninitialised function pointer. */
VOID APIENTRY ActivationRelocateDeviceFuncs(D3D10DDI_HDEVICE device, D3D10DDI_DEVICEFUNCS *functions)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(functions);
}

/* This resource owner is deliberately limited to the DDI lifetime contract.
 * It does not allocate video memory or expose a command entry point; the
 * production rendering path remains Mesa's Native Context Vulkan UMD. */
SIZE_T APIENTRY ActivationCalcPrivateResourceSize(D3D10DDI_HDEVICE device, const D3D10DDIARG_CREATERESOURCE *arguments)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    return sizeof(ACTIVATION_RESOURCE);
}

VOID APIENTRY ActivationCreateResource(D3D10DDI_HDEVICE device,
                                       const D3D10DDIARG_CREATERESOURCE *arguments,
                                       D3D10DDI_HRESOURCE resource,
                                       D3D10DDI_HRTRESOURCE runtimeResource)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments == nullptr || arguments->pMipInfoList == nullptr || arguments->MipLevels == 0 ||
        arguments->ArraySize == 0 || resource.pDrvPrivate == nullptr || runtimeResource.handle == nullptr)
    {
        return;
    }

    ACTIVATION_RESOURCE *state = static_cast<ACTIVATION_RESOURCE *>(resource.pDrvPrivate);
    state->Signature = ACTIVATION_RESOURCE_SIGNATURE;
    state->RuntimeResource = runtimeResource;
}

/* D3D11 keeps the resource owner contract but widens the create descriptor.
 * These wrappers intentionally validate only the common lifetime fields so
 * the probe can exercise the D3D11 ABI without allocating a renderable BO. */
SIZE_T APIENTRY ActivationCalcPrivateResourceSize11(D3D10DDI_HDEVICE device,
                                                    const D3D11DDIARG_CREATERESOURCE *arguments)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    return sizeof(ACTIVATION_RESOURCE);
}

VOID APIENTRY ActivationCreateResource11(D3D10DDI_HDEVICE device,
                                         const D3D11DDIARG_CREATERESOURCE *arguments,
                                         D3D10DDI_HRESOURCE resource,
                                         D3D10DDI_HRTRESOURCE runtimeResource)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments == nullptr || arguments->pMipInfoList == nullptr || arguments->MipLevels == 0 ||
        arguments->ArraySize == 0 || resource.pDrvPrivate == nullptr || runtimeResource.handle == nullptr)
    {
        return;
    }

    ACTIVATION_RESOURCE *state = static_cast<ACTIVATION_RESOURCE *>(resource.pDrvPrivate);
    state->Signature = ACTIVATION_RESOURCE_SIGNATURE;
    state->RuntimeResource = runtimeResource;
}

SIZE_T APIENTRY ActivationCalcPrivateShaderResourceViewSize11(D3D10DDI_HDEVICE device,
                                                              const D3D11DDIARG_CREATESHADERRESOURCEVIEW *arguments)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    return sizeof(ACTIVATION_OBJECT);
}

VOID APIENTRY ActivationCreateShaderResourceView11(D3D10DDI_HDEVICE device,
                                                   const D3D11DDIARG_CREATESHADERRESOURCEVIEW *arguments,
                                                   D3D10DDI_HSHADERRESOURCEVIEW view,
                                                   D3D10DDI_HRTSHADERRESOURCEVIEW runtimeView)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(view.pDrvPrivate, runtimeView.handle, ACTIVATION_SHADER_VIEW_SIGNATURE);
    }
}

SIZE_T APIENTRY ActivationCalcPrivateDepthStencilViewSize11(D3D10DDI_HDEVICE device,
                                                            const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *arguments)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    return sizeof(ACTIVATION_OBJECT);
}

VOID APIENTRY ActivationCreateDepthStencilView11(D3D10DDI_HDEVICE device,
                                                 const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *arguments,
                                                 D3D10DDI_HDEPTHSTENCILVIEW view,
                                                 D3D10DDI_HRTDEPTHSTENCILVIEW runtimeView)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(view.pDrvPrivate, runtimeView.handle, ACTIVATION_DEPTH_STENCIL_VIEW_SIGNATURE);
    }
}

SIZE_T APIENTRY ActivationCalcPrivateBlendStateSize11(D3D10DDI_HDEVICE device, const D3D10_1_DDI_BLEND_DESC *arguments)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    return sizeof(ACTIVATION_OBJECT);
}

VOID APIENTRY ActivationCreateBlendState11(D3D10DDI_HDEVICE device,
                                           const D3D10_1_DDI_BLEND_DESC *arguments,
                                           D3D10DDI_HBLENDSTATE state,
                                           D3D10DDI_HRTBLENDSTATE runtimeState)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments != nullptr)
    {
        ActivationInitializeObject(state.pDrvPrivate, runtimeState.handle, ACTIVATION_BLEND_STATE_SIGNATURE);
    }
}

SIZE_T APIENTRY
ActivationCalcPrivateGeometryShaderWithStreamOutput11(D3D10DDI_HDEVICE device,
                                                      const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *arguments,
                                                      const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    UNREFERENCED_PARAMETER(signatures);
    return sizeof(ACTIVATION_OBJECT);
}

VOID APIENTRY
ActivationCreateGeometryShaderWithStreamOutput11(D3D10DDI_HDEVICE device,
                                                 const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *arguments,
                                                 D3D10DDI_HSHADER shader,
                                                 D3D10DDI_HRTSHADER runtimeShader,
                                                 const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(signatures);
    if (arguments != nullptr)
    {
        ActivationCreateShader(shader, runtimeShader);
    }
}

VOID APIENTRY ActivationDestroyResource(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource)
{
    UNREFERENCED_PARAMETER(device);
    ACTIVATION_RESOURCE *state = static_cast<ACTIVATION_RESOURCE *>(resource.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_RESOURCE_SIGNATURE)
    {
        return;
    }
    state->Signature = 0;
    state->RuntimeResource.handle = nullptr;
}

/* Queries are part of the D3D10 device table even when no timestamp or
 * occlusion capability is advertised.  Keep their lifetime explicit so a
 * probe cannot call through a null callback; this owner never queues work or
 * fabricates a result. */
SIZE_T APIENTRY ActivationCalcPrivateQuerySize(D3D10DDI_HDEVICE device, const D3D10DDIARG_CREATEQUERY *arguments)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    return sizeof(ACTIVATION_QUERY);
}

VOID APIENTRY ActivationCreateQuery(D3D10DDI_HDEVICE device,
                                    const D3D10DDIARG_CREATEQUERY *arguments,
                                    D3D10DDI_HQUERY query,
                                    D3D10DDI_HRTQUERY runtimeQuery)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments == nullptr || query.pDrvPrivate == nullptr || runtimeQuery.handle == nullptr)
    {
        return;
    }

    ACTIVATION_QUERY *state = static_cast<ACTIVATION_QUERY *>(query.pDrvPrivate);
    state->Signature = ACTIVATION_QUERY_SIGNATURE;
    state->RuntimeQuery = runtimeQuery;
}

VOID APIENTRY ActivationDestroyQuery(D3D10DDI_HDEVICE device, D3D10DDI_HQUERY query)
{
    UNREFERENCED_PARAMETER(device);
    ACTIVATION_QUERY *state = static_cast<ACTIVATION_QUERY *>(query.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_QUERY_SIGNATURE)
    {
        return;
    }
    state->Signature = 0;
    state->RuntimeQuery.handle = nullptr;
}

VOID APIENTRY ActivationQueryBegin(D3D10DDI_HDEVICE device, D3D10DDI_HQUERY query)
{
    UNREFERENCED_PARAMETER(device);
    ACTIVATION_QUERY *state = static_cast<ACTIVATION_QUERY *>(query.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_QUERY_SIGNATURE)
    {
        return;
    }
}

VOID APIENTRY ActivationQueryEnd(D3D10DDI_HDEVICE device, D3D10DDI_HQUERY query)
{
    UNREFERENCED_PARAMETER(device);
    ACTIVATION_QUERY *state = static_cast<ACTIVATION_QUERY *>(query.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_QUERY_SIGNATURE)
    {
        return;
    }
}

VOID APIENTRY
ActivationQueryGetData(D3D10DDI_HDEVICE device, D3D10DDI_HQUERY query, VOID *data, UINT dataSize, UINT flags)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(flags);
    ACTIVATION_QUERY *state = static_cast<ACTIVATION_QUERY *>(query.pDrvPrivate);
    if (state == nullptr || state->Signature != ACTIVATION_QUERY_SIGNATURE)
    {
        return;
    }
    if (data != nullptr && dataSize != 0)
    {
        ZeroMemory(data, dataSize);
    }
}

VOID APIENTRY ActivationCheckCounterInfo(D3D10DDI_HDEVICE device, D3D10DDI_COUNTER_INFO *counterInfo)
{
    UNREFERENCED_PARAMETER(device);
    if (counterInfo != nullptr)
    {
        ZeroMemory(counterInfo, sizeof(*counterInfo));
    }
}

VOID APIENTRY ActivationCheckCounter(D3D10DDI_HDEVICE device,
                                     D3D10DDI_QUERY query,
                                     D3D10DDI_COUNTER_TYPE *counterType,
                                     UINT *activeCounters,
                                     LPSTR name,
                                     UINT *nameLength,
                                     LPSTR units,
                                     UINT *unitsLength,
                                     LPSTR description,
                                     UINT *descriptionLength)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(query);
    UNREFERENCED_PARAMETER(name);
    UNREFERENCED_PARAMETER(units);
    UNREFERENCED_PARAMETER(description);
    if (counterType != nullptr)
    {
        *counterType = static_cast<D3D10DDI_COUNTER_TYPE>(0);
    }
    if (activeCounters != nullptr)
    {
        *activeCounters = 0;
    }
    if (nameLength != nullptr)
    {
        *nameLength = 0;
    }
    if (unitsLength != nullptr)
    {
        *unitsLength = 0;
    }
    if (descriptionLength != nullptr)
    {
        *descriptionLength = 0;
    }
}

/* Shared-resource opens use the same bounded owner as local creates.  The
 * experiment records only the runtime identity; it does not import a shared
 * allocation or make a cross-process handle usable by the product path. */
SIZE_T APIENTRY ActivationCalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE device,
                                                        const D3D10DDIARG_OPENRESOURCE *arguments)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(arguments);
    return sizeof(ACTIVATION_RESOURCE);
}

VOID APIENTRY ActivationOpenResource(D3D10DDI_HDEVICE device,
                                     const D3D10DDIARG_OPENRESOURCE *arguments,
                                     D3D10DDI_HRESOURCE resource,
                                     D3D10DDI_HRTRESOURCE runtimeResource)
{
    UNREFERENCED_PARAMETER(device);
    if (arguments == nullptr || resource.pDrvPrivate == nullptr || runtimeResource.handle == nullptr)
    {
        return;
    }

    ACTIVATION_RESOURCE *state = static_cast<ACTIVATION_RESOURCE *>(resource.pDrvPrivate);
    state->Signature = ACTIVATION_RESOURCE_SIGNATURE;
    state->RuntimeResource = runtimeResource;
}

/* The activation shim advertises no renderable formats.  Returning an empty
 * capability mask is a useful DDI query result and avoids handing the runtime
 * an uninitialised output while the real Mesa Vulkan path remains separate. */
VOID APIENTRY ActivationCheckFormatSupport(D3D10DDI_HDEVICE device, DXGI_FORMAT format, UINT *formatSupport)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(format);
    if (formatSupport != nullptr)
    {
        *formatSupport = 0;
    }
}

VOID APIENTRY ActivationCheckMultisampleQualityLevels(D3D10DDI_HDEVICE device,
                                                      DXGI_FORMAT format,
                                                      UINT sampleCount,
                                                      UINT *qualityLevels)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(format);
    UNREFERENCED_PARAMETER(sampleCount);
    if (qualityLevels != nullptr)
    {
        *qualityLevels = 0;
    }
}
#endif

/* A successful D3D adapter open is enough for dxgkrnl to complete legacy
 * adapter activation.  Product D3D device creation remains fail-closed because
 * all real command submission belongs to the Mesa Turnip Vulkan ICD. */
SIZE_T APIENTRY ActivationCalcPrivateDeviceSize(D3D10DDI_HADAPTER adapter,
                                                const D3D10DDIARG_CALCPRIVATEDEVICESIZE *arguments)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(arguments);
#if defined(VIOGPU_WDDM_TEST_IMPLEMENTATIONS)
    return sizeof(ACTIVATION_DEVICE);
#else
    return 0;
#endif
}

HRESULT APIENTRY ActivationCreateDevice(D3D10DDI_HADAPTER adapter, D3D10DDIARG_CREATEDEVICE *arguments)
{
    UNREFERENCED_PARAMETER(adapter);
#if defined(VIOGPU_WDDM_TEST_IMPLEMENTATIONS)
    if (arguments == nullptr || arguments->pDeviceFuncs == nullptr || arguments->hDrvDevice.pDrvPrivate == nullptr)
    {
        return E_INVALIDARG;
    }

    ACTIVATION_DEVICE *state = static_cast<ACTIVATION_DEVICE *>(arguments->hDrvDevice.pDrvPrivate);
    state->Signature = ACTIVATION_DEVICE_SIGNATURE;
    state->CallCount = 0;
    state->LastCall = 0;

    D3D10DDI_DEVICEFUNCS functions = {};
    functions.pfnDefaultConstantBufferUpdateSubresourceUP = ActivationDefaultConstantBufferUpdateSubresourceUP;
    functions.pfnVsSetConstantBuffers = ActivationVsSetConstantBuffers;
    functions.pfnPsSetShaderResources = ActivationPsSetShaderResources;
    functions.pfnPsSetShader = ActivationPsSetShader;
    functions.pfnPsSetSamplers = ActivationPsSetSamplers;
    functions.pfnVsSetShader = ActivationVsSetShader;
    functions.pfnDrawIndexed = ActivationDrawIndexed;
    functions.pfnDraw = ActivationDraw;
    functions.pfnPsSetConstantBuffers = ActivationPsSetConstantBuffers;
    functions.pfnIaSetInputLayout = ActivationIaSetInputLayout;
    functions.pfnIaSetVertexBuffers = ActivationIaSetVertexBuffers;
    functions.pfnIaSetIndexBuffer = ActivationIaSetIndexBuffer;
    functions.pfnDrawIndexedInstanced = ActivationDrawIndexedInstanced;
    functions.pfnDrawInstanced = ActivationDrawInstanced;
    functions.pfnDynamicIABufferMapNoOverwrite = ActivationResourceMap;
    functions.pfnDynamicIABufferUnmap = ActivationResourceUnmap;
    functions.pfnDynamicConstantBufferMapDiscard = ActivationResourceMap;
    functions.pfnDynamicIABufferMapDiscard = ActivationResourceMap;
    functions.pfnDynamicConstantBufferUnmap = ActivationResourceUnmap;
    functions.pfnDynamicResourceMapDiscard = ActivationResourceMap;
    functions.pfnDynamicResourceUnmap = ActivationResourceUnmap;
    functions.pfnGsSetConstantBuffers = ActivationGsSetConstantBuffers;
    functions.pfnGsSetShader = ActivationGsSetShader;
    functions.pfnIaSetTopology = ActivationIaSetTopology;
    functions.pfnVsSetShaderResources = ActivationVsSetShaderResources;
    functions.pfnVsSetSamplers = ActivationVsSetSamplers;
    functions.pfnGsSetShaderResources = ActivationGsSetShaderResources;
    functions.pfnGsSetSamplers = ActivationGsSetSamplers;
    functions.pfnSetRenderTargets = ActivationSetRenderTargets;
    functions.pfnSetBlendState = ActivationSetBlendState;
    functions.pfnSetDepthStencilState = ActivationSetDepthStencilState;
    functions.pfnSetRasterizerState = ActivationSetRasterizerState;
    functions.pfnResourceCopyRegion = ActivationResourceCopyRegion;
    functions.pfnResourceUpdateSubresourceUP = ActivationResourceUpdateSubresourceUP;
    functions.pfnDrawAuto = ActivationDrawAuto;
    functions.pfnSetViewports = ActivationSetViewports;
    functions.pfnSetScissorRects = ActivationSetScissorRects;
    functions.pfnClearRenderTargetView = ActivationClearRenderTargetView;
    functions.pfnClearDepthStencilView = ActivationClearDepthStencilView;
    functions.pfnSetPredication = ActivationSetPredication;
    functions.pfnGenMips = ActivationGenerateMips;
    functions.pfnResourceCopy = ActivationResourceCopy;
    functions.pfnSoSetTargets = ActivationSetStreamOutputTargets;
    functions.pfnResourceResolveSubresource = ActivationResolveSubresource;
    functions.pfnResourceMap = ActivationResourceMap;
    functions.pfnResourceUnmap = ActivationResourceUnmap;
    functions.pfnStagingResourceMap = ActivationResourceMap;
    functions.pfnStagingResourceUnmap = ActivationResourceUnmap;
    functions.pfnSetTextFilterSize = ActivationSetTextFilterSize;
    functions.pfnDestroyDevice = ActivationDestroyDevice;
    functions.pfnCalcPrivateResourceSize = ActivationCalcPrivateResourceSize;
    functions.pfnCalcPrivateOpenedResourceSize = ActivationCalcPrivateOpenedResourceSize;
    functions.pfnCreateResource = ActivationCreateResource;
    functions.pfnOpenResource = ActivationOpenResource;
    functions.pfnDestroyResource = ActivationDestroyResource;
    functions.pfnFlush = ActivationFlush;
    functions.pfnResourceReadAfterWriteHazard = ActivationResourceReadAfterWriteHazard;
    functions.pfnResourceIsStagingBusy = ActivationResourceIsStagingBusy;
    functions.pfnShaderResourceViewReadAfterWriteHazard = ActivationShaderResourceViewReadAfterWriteHazard;
    functions.pfnRelocateDeviceFuncs = ActivationRelocateDeviceFuncs;
    functions.pfnCalcPrivateQuerySize = ActivationCalcPrivateQuerySize;
    functions.pfnCreateQuery = ActivationCreateQuery;
    functions.pfnDestroyQuery = ActivationDestroyQuery;
    functions.pfnQueryBegin = ActivationQueryBegin;
    functions.pfnQueryEnd = ActivationQueryEnd;
    functions.pfnQueryGetData = ActivationQueryGetData;
    functions.pfnCheckCounterInfo = ActivationCheckCounterInfo;
    functions.pfnCheckCounter = ActivationCheckCounter;
    functions.pfnCheckFormatSupport = ActivationCheckFormatSupport;
    functions.pfnCheckMultisampleQualityLevels = ActivationCheckMultisampleQualityLevels;
    functions.pfnCalcPrivateElementLayoutSize = ActivationCalcPrivateElementLayoutSize;
    functions.pfnCreateElementLayout = ActivationCreateElementLayout;
    functions.pfnDestroyElementLayout = ActivationDestroyElementLayout;
    functions.pfnCalcPrivateSamplerSize = ActivationCalcPrivateSamplerSize;
    functions.pfnCreateSampler = ActivationCreateSampler;
    functions.pfnDestroySampler = ActivationDestroySampler;
    functions.pfnCalcPrivateShaderSize = ActivationCalcPrivateShaderSize;
    functions.pfnCreateVertexShader = ActivationCreateVertexShader;
    functions.pfnCreateGeometryShader = ActivationCreateGeometryShader;
    functions.pfnCreatePixelShader = ActivationCreatePixelShader;
    functions.pfnCalcPrivateGeometryShaderWithStreamOutput = ActivationCalcPrivateGeometryShaderWithStreamOutput;
    functions.pfnCreateGeometryShaderWithStreamOutput = ActivationCreateGeometryShaderWithStreamOutput;
    functions.pfnDestroyShader = ActivationDestroyShader;
    functions.pfnCalcPrivateShaderResourceViewSize = ActivationCalcPrivateShaderResourceViewSize;
    functions.pfnCreateShaderResourceView = ActivationCreateShaderResourceView;
    functions.pfnDestroyShaderResourceView = ActivationDestroyShaderResourceView;
    functions.pfnCalcPrivateRenderTargetViewSize = ActivationCalcPrivateRenderTargetViewSize;
    functions.pfnCreateRenderTargetView = ActivationCreateRenderTargetView;
    functions.pfnDestroyRenderTargetView = ActivationDestroyRenderTargetView;
    functions.pfnCalcPrivateDepthStencilViewSize = ActivationCalcPrivateDepthStencilViewSize;
    functions.pfnCreateDepthStencilView = ActivationCreateDepthStencilView;
    functions.pfnDestroyDepthStencilView = ActivationDestroyDepthStencilView;
    functions.pfnCalcPrivateBlendStateSize = ActivationCalcPrivateBlendStateSize;
    functions.pfnCreateBlendState = ActivationCreateBlendState;
    functions.pfnDestroyBlendState = ActivationDestroyBlendState;
    functions.pfnCalcPrivateDepthStencilStateSize = ActivationCalcPrivateDepthStencilStateSize;
    functions.pfnCreateDepthStencilState = ActivationCreateDepthStencilState;
    functions.pfnDestroyDepthStencilState = ActivationDestroyDepthStencilState;
    functions.pfnCalcPrivateRasterizerStateSize = ActivationCalcPrivateRasterizerStateSize;
    functions.pfnCreateRasterizerState = ActivationCreateRasterizerState;
    functions.pfnDestroyRasterizerState = ActivationDestroyRasterizerState;
    *arguments->pDeviceFuncs = functions;

    /* The D3D11 runtime selects the union's p11DeviceFuncs member. Publish the
     * complete guarded shape table so an ABI probe can exercise the common and
     * D3D11-specific callback contracts without exposing a second renderer. */
    if (arguments->Interface == D3D11_0_DDI_INTERFACE_VERSION && arguments->p11DeviceFuncs != nullptr)
    {
        D3D11DDI_DEVICEFUNCS *functions11 = arguments->p11DeviceFuncs;
        ZeroMemory(functions11, sizeof(*functions11));

        /* D3D11 repeats the common D3D10 callback surface in this table. Keep
         * both tables shape-compatible so an ABI probe can exercise either
         * interface without falling through a null function pointer. */
        functions11->pfnDefaultConstantBufferUpdateSubresourceUP = ActivationDefaultConstantBufferUpdateSubresourceUP;
        functions11->pfnVsSetConstantBuffers = ActivationVsSetConstantBuffers;
        functions11->pfnPsSetShaderResources = ActivationPsSetShaderResources;
        functions11->pfnPsSetShader = ActivationPsSetShader;
        functions11->pfnPsSetSamplers = ActivationPsSetSamplers;
        functions11->pfnVsSetShader = ActivationVsSetShader;
        functions11->pfnDrawIndexed = ActivationDrawIndexed;
        functions11->pfnDraw = ActivationDraw;
        functions11->pfnDynamicIABufferMapNoOverwrite = ActivationResourceMap;
        functions11->pfnDynamicIABufferUnmap = ActivationResourceUnmap;
        functions11->pfnDynamicConstantBufferMapDiscard = ActivationResourceMap;
        functions11->pfnDynamicIABufferMapDiscard = ActivationResourceMap;
        functions11->pfnDynamicConstantBufferUnmap = ActivationResourceUnmap;
        functions11->pfnPsSetConstantBuffers = ActivationPsSetConstantBuffers;
        functions11->pfnIaSetInputLayout = ActivationIaSetInputLayout;
        functions11->pfnIaSetVertexBuffers = ActivationIaSetVertexBuffers;
        functions11->pfnIaSetIndexBuffer = ActivationIaSetIndexBuffer;
        functions11->pfnDrawIndexedInstanced = ActivationDrawIndexedInstanced;
        functions11->pfnDrawInstanced = ActivationDrawInstanced;
        functions11->pfnDynamicResourceMapDiscard = ActivationResourceMap;
        functions11->pfnDynamicResourceUnmap = ActivationResourceUnmap;
        functions11->pfnGsSetConstantBuffers = ActivationGsSetConstantBuffers;
        functions11->pfnGsSetShader = ActivationGsSetShader;
        functions11->pfnIaSetTopology = ActivationIaSetTopology;
        functions11->pfnStagingResourceMap = ActivationResourceMap;
        functions11->pfnStagingResourceUnmap = ActivationResourceUnmap;
        functions11->pfnVsSetShaderResources = ActivationVsSetShaderResources;
        functions11->pfnVsSetSamplers = ActivationVsSetSamplers;
        functions11->pfnGsSetShaderResources = ActivationGsSetShaderResources;
        functions11->pfnGsSetSamplers = ActivationGsSetSamplers;
        functions11->pfnSetRenderTargets = ActivationSetRenderTargets11;
        functions11->pfnShaderResourceViewReadAfterWriteHazard = ActivationShaderResourceViewReadAfterWriteHazard;
        functions11->pfnResourceReadAfterWriteHazard = ActivationResourceReadAfterWriteHazard;
        functions11->pfnSetBlendState = ActivationSetBlendState;
        functions11->pfnSetDepthStencilState = ActivationSetDepthStencilState;
        functions11->pfnSetRasterizerState = ActivationSetRasterizerState;
        functions11->pfnQueryEnd = ActivationQueryEnd;
        functions11->pfnQueryBegin = ActivationQueryBegin;
        functions11->pfnResourceCopyRegion = ActivationResourceCopyRegion;
        functions11->pfnResourceUpdateSubresourceUP = ActivationResourceUpdateSubresourceUP;
        functions11->pfnSoSetTargets = ActivationSetStreamOutputTargets;
        functions11->pfnDrawAuto = ActivationDrawAuto;
        functions11->pfnSetViewports = ActivationSetViewports;
        functions11->pfnSetScissorRects = ActivationSetScissorRects;
        functions11->pfnClearRenderTargetView = ActivationClearRenderTargetView;
        functions11->pfnClearDepthStencilView = ActivationClearDepthStencilView;
        functions11->pfnSetPredication = ActivationSetPredication;
        functions11->pfnQueryGetData = ActivationQueryGetData;
        functions11->pfnFlush = ActivationFlush;
        functions11->pfnGenMips = ActivationGenerateMips;
        functions11->pfnResourceCopy = ActivationResourceCopy;
        functions11->pfnResourceResolveSubresource = ActivationResolveSubresource;
        functions11->pfnResourceMap = ActivationResourceMap;
        functions11->pfnResourceUnmap = ActivationResourceUnmap;
        functions11->pfnResourceIsStagingBusy = ActivationResourceIsStagingBusy;
        functions11->pfnRelocateDeviceFuncs = ActivationRelocateDeviceFuncs11;
        functions11->pfnCalcPrivateResourceSize = ActivationCalcPrivateResourceSize11;
        functions11->pfnCalcPrivateOpenedResourceSize = ActivationCalcPrivateOpenedResourceSize;
        functions11->pfnCreateResource = ActivationCreateResource11;
        functions11->pfnOpenResource = ActivationOpenResource;
        functions11->pfnDestroyResource = ActivationDestroyResource;
        functions11->pfnCalcPrivateShaderResourceViewSize = ActivationCalcPrivateShaderResourceViewSize11;
        functions11->pfnCreateShaderResourceView = ActivationCreateShaderResourceView11;
        functions11->pfnDestroyShaderResourceView = ActivationDestroyShaderResourceView;
        functions11->pfnCalcPrivateRenderTargetViewSize = ActivationCalcPrivateRenderTargetViewSize;
        functions11->pfnCreateRenderTargetView = ActivationCreateRenderTargetView;
        functions11->pfnDestroyRenderTargetView = ActivationDestroyRenderTargetView;
        functions11->pfnCalcPrivateDepthStencilViewSize = ActivationCalcPrivateDepthStencilViewSize11;
        functions11->pfnCreateDepthStencilView = ActivationCreateDepthStencilView11;
        functions11->pfnDestroyDepthStencilView = ActivationDestroyDepthStencilView;
        functions11->pfnCalcPrivateElementLayoutSize = ActivationCalcPrivateElementLayoutSize;
        functions11->pfnCreateElementLayout = ActivationCreateElementLayout;
        functions11->pfnDestroyElementLayout = ActivationDestroyElementLayout;
        functions11->pfnCalcPrivateBlendStateSize = ActivationCalcPrivateBlendStateSize11;
        functions11->pfnCreateBlendState = ActivationCreateBlendState11;
        functions11->pfnDestroyBlendState = ActivationDestroyBlendState;
        functions11->pfnCalcPrivateDepthStencilStateSize = ActivationCalcPrivateDepthStencilStateSize;
        functions11->pfnCreateDepthStencilState = ActivationCreateDepthStencilState;
        functions11->pfnDestroyDepthStencilState = ActivationDestroyDepthStencilState;
        functions11->pfnCalcPrivateRasterizerStateSize = ActivationCalcPrivateRasterizerStateSize;
        functions11->pfnCreateRasterizerState = ActivationCreateRasterizerState;
        functions11->pfnDestroyRasterizerState = ActivationDestroyRasterizerState;
        functions11->pfnCalcPrivateShaderSize = ActivationCalcPrivateShaderSize;
        functions11->pfnCreateVertexShader = ActivationCreateVertexShader;
        functions11->pfnCreateGeometryShader = ActivationCreateGeometryShader;
        functions11->pfnCreatePixelShader = ActivationCreatePixelShader;
        functions11->pfnCalcPrivateGeometryShaderWithStreamOutput = ActivationCalcPrivateGeometryShaderWithStreamOutput11;
        functions11->pfnCreateGeometryShaderWithStreamOutput = ActivationCreateGeometryShaderWithStreamOutput11;
        functions11->pfnDestroyShader = ActivationDestroyShader;
        functions11->pfnCalcPrivateSamplerSize = ActivationCalcPrivateSamplerSize;
        functions11->pfnCreateSampler = ActivationCreateSampler;
        functions11->pfnDestroySampler = ActivationDestroySampler;
        functions11->pfnCalcPrivateQuerySize = ActivationCalcPrivateQuerySize;
        functions11->pfnCreateQuery = ActivationCreateQuery;
        functions11->pfnDestroyQuery = ActivationDestroyQuery;
        functions11->pfnCheckFormatSupport = ActivationCheckFormatSupport;
        functions11->pfnCheckMultisampleQualityLevels = ActivationCheckMultisampleQualityLevels;
        functions11->pfnCheckCounterInfo = ActivationCheckCounterInfo;
        functions11->pfnCheckCounter = ActivationCheckCounter;
        functions11->pfnDestroyDevice = ActivationDestroyDevice;
        functions11->pfnSetTextFilterSize = ActivationSetTextFilterSize;
        functions11->pfnResourceConvert = ActivationResourceCopy;
        functions11->pfnResourceConvertRegion = ActivationResourceCopyRegion;
        functions11->pfnVsSetShaderWithIfaces = ActivationVsSetShaderWithIfaces;
        functions11->pfnPsSetShaderWithIfaces = ActivationPsSetShaderWithIfaces;
        functions11->pfnGsSetShaderWithIfaces = ActivationGsSetShaderWithIfaces;
        functions11->pfnCreateComputeShader = ActivationCreateComputeShader;
        functions11->pfnCsSetShader = ActivationCsSetShader;
        functions11->pfnCsSetShaderWithIfaces = ActivationCsSetShaderWithIfaces;
        functions11->pfnCsSetShaderResources = ActivationCsSetShaderResources;
        functions11->pfnCsSetSamplers = ActivationCsSetSamplers;
        functions11->pfnCsSetConstantBuffers = ActivationCsSetConstantBuffers;
        functions11->pfnHsSetShaderResources = ActivationHsSetShaderResources;
        functions11->pfnHsSetShader = ActivationHsSetShader;
        functions11->pfnHsSetShaderWithIfaces = ActivationHsSetShaderWithIfaces;
        functions11->pfnHsSetSamplers = ActivationHsSetSamplers;
        functions11->pfnHsSetConstantBuffers = ActivationHsSetConstantBuffers;
        functions11->pfnDsSetShaderResources = ActivationDsSetShaderResources;
        functions11->pfnDsSetShader = ActivationDsSetShader;
        functions11->pfnDsSetShaderWithIfaces = ActivationDsSetShaderWithIfaces;
        functions11->pfnDsSetSamplers = ActivationDsSetSamplers;
        functions11->pfnDsSetConstantBuffers = ActivationDsSetConstantBuffers;
        functions11->pfnCreateHullShader = ActivationCreateHullShader;
        functions11->pfnCreateDomainShader = ActivationCreateDomainShader;
        functions11->pfnCalcPrivateTessellationShaderSize = ActivationCalcPrivateTessellationShaderSize;
        functions11->pfnCalcPrivateUnorderedAccessViewSize = ActivationCalcPrivateUnorderedAccessViewSize;
        functions11->pfnCreateUnorderedAccessView = ActivationCreateUnorderedAccessView;
        functions11->pfnDestroyUnorderedAccessView = ActivationDestroyUnorderedAccessView;
        functions11->pfnClearUnorderedAccessViewUint = ActivationClearUnorderedAccessViewUint;
        functions11->pfnClearUnorderedAccessViewFloat = ActivationClearUnorderedAccessViewFloat;
        functions11->pfnCsSetUnorderedAccessViews = ActivationCsSetUnorderedAccessViews;
        functions11->pfnResourceConvert = ActivationResourceCopy;
        functions11->pfnResourceConvertRegion = ActivationResourceCopyRegion;
        functions11->pfnDispatch = ActivationDispatch;
        functions11->pfnDispatchIndirect = ActivationDispatchIndirect;
        functions11->pfnDrawIndexedInstancedIndirect = ActivationDrawIndexedInstancedIndirect;
        functions11->pfnDrawInstancedIndirect = ActivationDrawInstancedIndirect;
        functions11->pfnSetResourceMinLOD = ActivationSetResourceMinLOD;
        functions11->pfnCopyStructureCount = ActivationCopyStructureCount;
        functions11->pfnCommandListExecute = ActivationCommandListExecute;
        functions11->pfnCalcPrivateCommandListSize = ActivationCalcPrivateCommandListSize;
        functions11->pfnCreateCommandList = ActivationCreateCommandList;
        functions11->pfnDestroyCommandList = ActivationDestroyCommandList;
        functions11->pfnCheckDeferredContextHandleSizes = ActivationCheckDeferredContextHandleSizes;
        functions11->pfnCalcDeferredContextHandleSize = ActivationCalcDeferredContextHandleSize;
        functions11->pfnCalcPrivateDeferredContextSize = ActivationCalcPrivateDeferredContextSize;
        functions11->pfnCreateDeferredContext = ActivationCreateDeferredContext;
        functions11->pfnAbandonCommandList = ActivationAbandonCommandList;
        functions11->pfnRecycleCommandList = ActivationRecycleCommandList;
        functions11->pfnRecycleCreateCommandList = ActivationRecycleCreateCommandList;
        functions11->pfnRecycleCreateDeferredContext = ActivationRecycleCreateDeferredContext;
        functions11->pfnRecycleDestroyCommandList = ActivationRecycleDestroyCommandList;
    }
    return S_OK;
#else
    UNREFERENCED_PARAMETER(arguments);
    return E_NOTIMPL;
#endif
}

HRESULT APIENTRY ActivationCloseAdapter(D3D10DDI_HADAPTER adapter)
{
    ACTIVATION_ADAPTER *state = static_cast<ACTIVATION_ADAPTER *>(adapter.pDrvPrivate);
    if (state == nullptr)
    {
        return E_INVALIDARG;
    }
    HeapFree(GetProcessHeap(), 0, state);
    return S_OK;
}

HRESULT APIENTRY ActivationGetSupportedVersions(D3D10DDI_HADAPTER adapter, UINT32 *entryCount, UINT64 *versions)
{
    UNREFERENCED_PARAMETER(adapter);
    if (entryCount == nullptr)
    {
        return E_INVALIDARG;
    }

    constexpr UINT32 supportedCount = 1;
    if (versions != nullptr && *entryCount < supportedCount)
    {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    *entryCount = supportedCount;
    if (versions != nullptr)
    {
        versions[0] = D3D10_0_DDI_SUPPORTED;
    }
    return S_OK;
}

HRESULT APIENTRY ActivationGetCaps(D3D10DDI_HADAPTER adapter, const D3D10_2DDIARG_GETCAPS *arguments)
{
    UNREFERENCED_PARAMETER(adapter);
    if (arguments == nullptr || (arguments->DataSize != 0 && arguments->pData == nullptr))
    {
        return E_INVALIDARG;
    }
#if defined(VIOGPU_WDDM_TEST_IMPLEMENTATIONS)
    /* The opt-in build exercises the capability-query ABI without advertising
     * a rendering feature.  Each recognized payload is zeroed only when the
     * runtime supplied its exact size; unknown requests stay explicit. */
    /* D3D runtimes use a zero-sized call as a probe in a few interface
     * negotiation paths.  There is no payload to initialize in that form;
     * accept it, while still rejecting a missing buffer for non-zero data. */
    if (arguments->DataSize == 0)
    {
        return arguments->pData == nullptr ? S_OK : E_INVALIDARG;
    }
    if (arguments->pData == nullptr)
    {
        return E_INVALIDARG;
    }

    switch (arguments->Type)
    {
        case D3D11DDICAPS_THREADING:
            if (arguments->DataSize != sizeof(D3D11DDI_THREADING_CAPS))
            {
                return E_INVALIDARG;
            }
            ZeroMemory(arguments->pData, arguments->DataSize);
            static_cast<D3D11DDI_THREADING_CAPS *>(arguments->pData)->Caps = 0;
            return S_OK;

        case D3D11DDICAPS_3DPIPELINESUPPORT:
            if (arguments->DataSize != sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS))
            {
                return E_INVALIDARG;
            }
            ZeroMemory(arguments->pData, arguments->DataSize);
            static_cast<D3D11DDI_3DPIPELINESUPPORT_CAPS *>(arguments->pData)->Caps = 0;
            return S_OK;

        case D3D11DDICAPS_SHADER:
            if (arguments->DataSize != sizeof(D3D11DDI_SHADER_CAPS))
            {
                return E_INVALIDARG;
            }
            ZeroMemory(arguments->pData, arguments->DataSize);
            static_cast<D3D11DDI_SHADER_CAPS *>(arguments->pData)->Caps = 0;
            return S_OK;

        case D3D11_1DDICAPS_D3D11_OPTIONS:
            if (arguments->DataSize != sizeof(D3D11_1DDI_D3D11_OPTIONS_DATA))
            {
                return E_INVALIDARG;
            }
            ZeroMemory(arguments->pData, arguments->DataSize);
            static_cast<D3D11_1DDI_D3D11_OPTIONS_DATA *>(arguments->pData)->AssignDebugBinarySupport = FALSE;
            static_cast<D3D11_1DDI_D3D11_OPTIONS_DATA *>(arguments->pData)->OutputMergerLogicOp = FALSE;
            return S_OK;

        case D3D11_1DDICAPS_ARCHITECTURE_INFO:
            if (arguments->DataSize != sizeof(D3D11_1DDI_ARCHITECTURE_INFO_DATA))
            {
                return E_INVALIDARG;
            }
            ZeroMemory(arguments->pData, arguments->DataSize);
            static_cast<D3D11_1DDI_ARCHITECTURE_INFO_DATA *>(arguments->pData)->TileBasedDeferredRenderer = FALSE;
            return S_OK;

        case D3D11_1DDICAPS_SHADER_MIN_PRECISION_SUPPORT:
            if (arguments->DataSize != sizeof(D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA))
            {
                return E_INVALIDARG;
            }
            ZeroMemory(arguments->pData, arguments->DataSize);
            static_cast<D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA *>(arguments->pData)->PixelShaderMinPrecision = 0;
            static_cast<D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA *>(arguments->pData)->AllOtherStagesMinPrecision =
                                                                                                                0;
            return S_OK;

        default:
            return E_NOTIMPL;
    }
#else
    if (arguments->pData != nullptr && arguments->DataSize != 0)
    {
        ZeroMemory(arguments->pData, arguments->DataSize);
    }
    return S_OK;
#endif
}

HRESULT OpenAdapter10Common(D3D10DDIARG_OPENADAPTER *openData)
{
    if (openData == nullptr)
    {
        return E_INVALIDARG;
    }

    ACTIVATION_ADAPTER *state = static_cast<ACTIVATION_ADAPTER *>(HeapAlloc(GetProcessHeap(),
                                                                            HEAP_ZERO_MEMORY,
                                                                            sizeof(ACTIVATION_ADAPTER)));
    if (state == nullptr)
    {
        return E_OUTOFMEMORY;
    }
    state->RuntimeAdapter = openData->hRTAdapter;

    if (openData->pAdapterFuncs != nullptr)
    {
        D3D10DDI_ADAPTERFUNCS functions = {};
        functions.pfnCalcPrivateDeviceSize = ActivationCalcPrivateDeviceSize;
        functions.pfnCreateDevice = ActivationCreateDevice;
        functions.pfnCloseAdapter = ActivationCloseAdapter;
        *openData->pAdapterFuncs = functions;
    }
    openData->hAdapter.pDrvPrivate = state;
    return S_OK;
}

} // namespace

extern "C" HRESULT APIENTRY OpenAdapter(_Inout_ D3DDDIARG_OPENADAPTER *openData)
{
    UNREFERENCED_PARAMETER(openData);
    return E_NOTIMPL;
}

extern "C" HRESULT APIENTRY OpenAdapter10(_Inout_ D3D10DDIARG_OPENADAPTER *openData)
{
    if (openData == nullptr || openData->pAdapterFuncs == nullptr)
    {
        return E_INVALIDARG;
    }
    return OpenAdapter10Common(openData);
}

extern "C" HRESULT APIENTRY OpenAdapter10_2(_Inout_ D3D10DDIARG_OPENADAPTER *openData)
{
    if (openData == nullptr || openData->pAdapterFuncs_2 == nullptr)
    {
        return E_INVALIDARG;
    }

    HRESULT status = OpenAdapter10Common(openData);
    if (FAILED(status))
    {
        return status;
    }

    D3D10_2DDI_ADAPTERFUNCS functions = {};
    functions.pfnCalcPrivateDeviceSize = ActivationCalcPrivateDeviceSize;
    functions.pfnCreateDevice = ActivationCreateDevice;
    functions.pfnCloseAdapter = ActivationCloseAdapter;
    functions.pfnGetSupportedVersions = ActivationGetSupportedVersions;
    functions.pfnGetCaps = ActivationGetCaps;
    *openData->pAdapterFuncs_2 = functions;
    return S_OK;
}

BOOL WINAPI DllMain(_In_ HINSTANCE instance, _In_ DWORD reason, _In_opt_ LPVOID reserved)
{
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(reason);
    UNREFERENCED_PARAMETER(reserved);
    return TRUE;
}
