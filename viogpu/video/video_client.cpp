/* SPDX-License-Identifier: BSD-3-Clause
 * User-mode VioGPU video bridge. No codec or GPU capabilities are fabricated here.
 * API calls on one handle must be serialized; Close must not race another call.
 */
#include "video_api.h"
#include <setupapi.h>
#include <new>

namespace {
const GUID VideoInterface=VGPU_VIDEO_INTERFACE_GUID_INIT;
struct Client { HANDLE Device; uint64_t Generation; };
/* Create a canonical private-ABI request header. */
void Header(VGPU_VIDEO_HEADER &h, uint32_t bytes, uint64_t generation)
{
    h.Version=VGPU_VIDEO_VERSION; h.Size=bytes; h.Generation=generation;
}
/* Perform a bounded synchronous driver operation, preserving Win32 transport errors. */
HRESULT Call(Client *c, DWORD code, const void *in, DWORD inBytes, void *out, DWORD outBytes, DWORD &actual)
{
    actual=0;
    if (!DeviceIoControl(c->Device,code,const_cast<void *>(in),inBytes,out,outBytes,&actual,nullptr))
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}
/* Validate the common reply before returning it to callers. */
bool ValidReply(const VGPU_VIDEO_HEADER &h, uint32_t size, uint64_t generation)
{
    return h.Version==VGPU_VIDEO_VERSION && h.Size==size && h.Generation==generation;
}
/* Wrap a fixed-size control with a version/generation header. */
template<class T> HRESULT Fixed(HANDLE session, DWORD code, T *data)
{
    if (!session || !data) return E_INVALIDARG;
    Client *c=reinterpret_cast<Client *>(session);
    Header(data->Header,sizeof(T),c->Generation);
    DWORD actual=0;
    HRESULT hr=Call(c,code,data,sizeof(T),data,sizeof(T),actual);
    if (SUCCEEDED(hr) && (actual!=sizeof(T) || !ValidReply(data->Header,sizeof(T),c->Generation)))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    return hr;
}
}

/* Resolve a media interface by index and open an exclusive session on that function. */
extern "C" HRESULT WINAPI VioGpuVideoOpen(UINT index, HANDLE *session, VGPU_VIDEO_OPEN *information)
{
    if (!session || !information) return E_INVALIDARG;
    *session=nullptr; ZeroMemory(information,sizeof(*information));
    HDEVINFO set=SetupDiGetClassDevsW(&VideoInterface,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if (set==INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    SP_DEVICE_INTERFACE_DATA iface={}; iface.cbSize=sizeof(iface);
    if (!SetupDiEnumDeviceInterfaces(set,nullptr,&VideoInterface,index,&iface)) {
        DWORD error=GetLastError(); SetupDiDestroyDeviceInfoList(set); return HRESULT_FROM_WIN32(error);
    }
    DWORD bytes=0;
    SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&bytes,nullptr);
    if (!bytes || bytes>(1u<<20)) { SetupDiDestroyDeviceInfoList(set); return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
    auto *detail=static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,bytes));
    if (!detail) { SetupDiDestroyDeviceInfoList(set); return E_OUTOFMEMORY; }
    detail->cbSize=sizeof(*detail);
    HANDLE device=INVALID_HANDLE_VALUE; DWORD error=ERROR_SUCCESS;
    if (SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,bytes,nullptr,nullptr)) {
        device=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if (device==INVALID_HANDLE_VALUE) error=GetLastError();
    } else error=GetLastError();
    HeapFree(GetProcessHeap(),0,detail); SetupDiDestroyDeviceInfoList(set);
    if (device==INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(error);
    Client *c=new(std::nothrow) Client{device,0};
    if (!c) { CloseHandle(device); return E_OUTOFMEMORY; }
    VGPU_VIDEO_HEADER request={}; Header(request,sizeof(request),0); DWORD actual=0;
    HRESULT hr=Call(c,IOCTL_VGPU_VIDEO_OPEN,&request,sizeof(request),information,sizeof(*information),actual);
    if (SUCCEEDED(hr) && (actual!=sizeof(*information) ||
        information->Header.Version!=VGPU_VIDEO_VERSION || information->Header.Size!=sizeof(*information) ||
        !information->Header.Generation)) hr=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (FAILED(hr)) { CloseHandle(device); delete c; return hr; }
    c->Generation=information->Header.Generation;
    *session=reinterpret_cast<HANDLE>(c); return S_OK;
}
/* Pass only pointer-free controls accepted by the kernel allowlist. */
extern "C" HRESULT WINAPI VioGpuVideoControl(HANDLE session, VGPU_VIDEO_CONTROL *control)
{
    if (!control || !VmediaControlSize(control->Code) || control->PayloadBytes!=VmediaControlSize(control->Code))
        return E_INVALIDARG;
    return Fixed(session,IOCTL_VGPU_VIDEO_CONTROL,control);
}
/* Allocate a complete queue transactionally; actual Count can differ from requested Count. */
extern "C" HRESULT WINAPI VioGpuVideoAllocate(HANDLE session, VGPU_VIDEO_BUFFERS *buffers)
{
    return Fixed(session,IOCTL_VGPU_VIDEO_BUFFERS,buffers);
}
/* Copy user data into the buffered IOCTL; its lifetime ends at the return from this call. */
extern "C" HRESULT WINAPI VioGpuVideoQueue(HANDLE session, const VGPU_VIDEO_BUFFER *buffer, const void *data)
{
    if (!session || !buffer || buffer->BytesUsed>VGPU_VIDEO_MAX_BUFFER_BYTES ||
        VmediaQueueIndex(buffer->Type)<0 || (buffer->Type==VMEDIA_CAPTURE && buffer->BytesUsed) ||
        (buffer->BytesUsed && !data)) return E_INVALIDARG;
    Client *c=reinterpret_cast<Client *>(session);
    DWORD size=static_cast<DWORD>(sizeof(*buffer))+buffer->BytesUsed;
    auto *request=static_cast<VGPU_VIDEO_BUFFER *>(HeapAlloc(GetProcessHeap(),0,size));
    if (!request) return E_OUTOFMEMORY;
    *request=*buffer; Header(request->Header,sizeof(*request),c->Generation);
    if (buffer->BytesUsed) CopyMemory(request+1,data,buffer->BytesUsed);
    DWORD actual=0; HRESULT hr=Call(c,IOCTL_VGPU_VIDEO_QUEUE,request,size,nullptr,0,actual);
    HeapFree(GetProcessHeap(),0,request); return hr;
}
/* A polling timeout is S_FALSE, and must never be interpreted as LAST/EOS. */
extern "C" HRESULT WINAPI VioGpuVideoDequeue(HANDLE session, VGPU_VIDEO_EVENT *event)
{
    HRESULT hr=Fixed(session,IOCTL_VGPU_VIDEO_EVENT,event);
    if (hr==HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)) return S_FALSE;
    return hr;
}
/* Copy only the bytes the kernel declared valid in the returned capture buffer. */
extern "C" HRESULT WINAPI VioGpuVideoCopy(HANDLE session, VGPU_VIDEO_BUFFER *buffer, void *data, UINT capacity)
{
    if (!session || !buffer || capacity>VGPU_VIDEO_MAX_BUFFER_BYTES || (capacity && !data)) return E_INVALIDARG;
    Client *c=reinterpret_cast<Client *>(session);
    VGPU_VIDEO_BUFFER request=*buffer; Header(request.Header,sizeof(request),c->Generation);
    DWORD size=static_cast<DWORD>(sizeof(request))+capacity;
    auto *response=static_cast<VGPU_VIDEO_BUFFER *>(HeapAlloc(GetProcessHeap(),0,size));
    if (!response) return E_OUTOFMEMORY;
    DWORD actual=0; HRESULT hr=Call(c,IOCTL_VGPU_VIDEO_COPY,&request,sizeof(request),response,size,actual);
    if (SUCCEEDED(hr)) {
        if (actual<sizeof(*response) || !ValidReply(response->Header,sizeof(*response),c->Generation) ||
            response->BytesUsed>capacity || response->BytesUsed!=actual-sizeof(*response))
            hr=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        else { *buffer=*response; if (buffer->BytesUsed) CopyMemory(data,response+1,buffer->BytesUsed); }
    }
    HeapFree(GetProcessHeap(),0,response); return hr;
}
/* Start/stop streaming on exactly one codec queue. */
extern "C" HRESULT WINAPI VioGpuVideoStream(HANDLE session, VGPU_VIDEO_STREAM *stream)
{
    return Fixed(session,IOCTL_VGPU_VIDEO_STREAM,stream);
}
/* Close the host session before releasing the OS handle; cleanup repeats safely on failure. */
extern "C" HRESULT WINAPI VioGpuVideoClose(HANDLE session)
{
    if (!session) return E_INVALIDARG;
    Client *c=reinterpret_cast<Client *>(session); VGPU_VIDEO_HEADER request={}; DWORD actual=0;
    Header(request,sizeof(request),c->Generation);
    HRESULT hr=Call(c,IOCTL_VGPU_VIDEO_CLOSE,&request,sizeof(request),nullptr,0,actual);
    CloseHandle(c->Device); delete c; return hr;
}
