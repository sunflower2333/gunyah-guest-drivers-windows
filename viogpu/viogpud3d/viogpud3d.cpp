#include <windows.h>

#include <d3d10umddi.h>
#include <d3dumddi.h>

extern "C" HRESULT APIENTRY OpenAdapter(_Inout_ D3DDDIARG_OPENADAPTER *openData)
{
    UNREFERENCED_PARAMETER(openData);
    return E_NOTIMPL;
}

extern "C" HRESULT APIENTRY OpenAdapter10(_Inout_ D3D10DDIARG_OPENADAPTER *openData)
{
    UNREFERENCED_PARAMETER(openData);
    return E_NOTIMPL;
}

extern "C" HRESULT APIENTRY OpenAdapter10_2(_Inout_ D3D10DDIARG_OPENADAPTER *openData)
{
    UNREFERENCED_PARAMETER(openData);
    return E_NOTIMPL;
}

BOOL WINAPI DllMain(_In_ HINSTANCE instance, _In_ DWORD reason, _In_opt_ LPVOID reserved)
{
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(reason);
    UNREFERENCED_PARAMETER(reserved);
    return TRUE;
}
