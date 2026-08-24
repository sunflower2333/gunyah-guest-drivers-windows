#include <windows.h>

#include "d3dumddi_compat.h"

namespace
{

struct ACTIVATION_ADAPTER
{
    D3D10DDI_HRTADAPTER RuntimeAdapter;
};

/* A successful D3D adapter open is enough for dxgkrnl to complete legacy
 * adapter activation.  D3D device creation remains fail-closed because all
 * real command submission belongs to the Mesa Turnip Vulkan ICD. */
SIZE_T APIENTRY ActivationCalcPrivateDeviceSize(D3D10DDI_HADAPTER adapter,
                                                const D3D10DDIARG_CALCPRIVATEDEVICESIZE *arguments)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(arguments);
    return 0;
}

HRESULT APIENTRY ActivationCreateDevice(D3D10DDI_HADAPTER adapter, D3D10DDIARG_CREATEDEVICE *arguments)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(arguments);
    return E_NOTIMPL;
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
    if (arguments->pData != nullptr && arguments->DataSize != 0)
    {
        ZeroMemory(arguments->pData, arguments->DataSize);
    }
    return S_OK;
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
