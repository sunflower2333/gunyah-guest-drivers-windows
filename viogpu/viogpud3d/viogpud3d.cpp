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

    /* The D3D11 runtime selects the union's p11DeviceFuncs member. Keep this
     * table deliberately sparse: the capability response advertises no 3D
     * pipeline, while these exact-signature callbacks let an ABI probe verify
     * compute/indirect dispatch wiring without exposing a second renderer. */
    if (arguments->Interface == D3D11_0_DDI_INTERFACE_VERSION && arguments->p11DeviceFuncs != nullptr)
    {
        D3D11DDI_DEVICEFUNCS *functions11 = arguments->p11DeviceFuncs;
        ZeroMemory(functions11, sizeof(*functions11));
        functions11->pfnSetRenderTargets = ActivationSetRenderTargets11;
        functions11->pfnRelocateDeviceFuncs = ActivationRelocateDeviceFuncs11;
        functions11->pfnCreateComputeShader = ActivationCreateComputeShader;
        functions11->pfnCsSetShader = ActivationPsSetShader;
        functions11->pfnCsSetShaderResources = ActivationPsSetShaderResources;
        functions11->pfnCsSetSamplers = ActivationPsSetSamplers;
        functions11->pfnCsSetConstantBuffers = ActivationPsSetConstantBuffers;
        functions11->pfnHsSetShaderResources = ActivationHsSetShaderResources;
        functions11->pfnHsSetShader = ActivationHsSetShader;
        functions11->pfnHsSetSamplers = ActivationHsSetSamplers;
        functions11->pfnHsSetConstantBuffers = ActivationHsSetConstantBuffers;
        functions11->pfnDsSetShaderResources = ActivationDsSetShaderResources;
        functions11->pfnDsSetShader = ActivationDsSetShader;
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
