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
};

struct ACTIVATION_RESOURCE
{
    ULONG Signature;
    D3D10DDI_HRTRESOURCE RuntimeResource;
};

constexpr ULONG ACTIVATION_DEVICE_SIGNATURE = 0x56494F54UL;
constexpr ULONG ACTIVATION_RESOURCE_SIGNATURE = 0x56494F52UL;

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
    UNREFERENCED_PARAMETER(device);
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

    D3D10DDI_DEVICEFUNCS functions = {};
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
    functions.pfnCheckFormatSupport = ActivationCheckFormatSupport;
    functions.pfnCheckMultisampleQualityLevels = ActivationCheckMultisampleQualityLevels;
    *arguments->pDeviceFuncs = functions;
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
