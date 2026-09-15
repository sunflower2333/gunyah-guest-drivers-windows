// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "../include/video_ioctl.h"
#pragma comment(lib,"setupapi.lib")

// Enumerate media functions independently of display adapter enumeration.
inline std::vector<std::wstring> VideoPaths()
{
    const GUID guid=VV_INTERFACE_GUID_INIT;
    HDEVINFO set=SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if (set==INVALID_HANDLE_VALUE) throw std::runtime_error("SetupDiGetClassDevs failed");
    std::vector<std::wstring> paths;
    try {
        for (DWORD i=0;;++i) {
            SP_DEVICE_INTERFACE_DATA item={}; item.cbSize=sizeof(item);
            if (!SetupDiEnumDeviceInterfaces(set,nullptr,&guid,i,&item)) {
                if (GetLastError()==ERROR_NO_MORE_ITEMS) break;
                throw std::runtime_error("SetupDiEnumDeviceInterfaces failed");
            }
            DWORD size=0;
            SetupDiGetDeviceInterfaceDetailW(set,&item,nullptr,0,&size,nullptr);
            if (GetLastError()!=ERROR_INSUFFICIENT_BUFFER || !size) throw std::runtime_error("Bad interface detail size");
            std::vector<unsigned char> bytes(size);
            auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(bytes.data());
            detail->cbSize=sizeof(*detail);
            if (!SetupDiGetDeviceInterfaceDetailW(set,&item,detail,size,nullptr,nullptr)) throw std::runtime_error("Interface detail failed");
            paths.emplace_back(detail->DevicePath);
        }
    } catch (...) { SetupDiDestroyDeviceInfoList(set); throw; }
    SetupDiDestroyDeviceInfoList(set);
    return paths;
}

class VideoClient {
    HANDLE handle_=INVALID_HANDLE_VALUE;
public:
    // Open the private admin-only bring-up endpoint, not a D3D/DXVA adapter.
    explicit VideoClient(const std::wstring &path)
    {
        handle_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if (handle_==INVALID_HANDLE_VALUE) throw std::runtime_error("CreateFile failed; run elevated and check the media driver");
    }
    // File cleanup sends CLOSE; a failed close never makes the kernel free in-flight RAM.
    ~VideoClient() { if (handle_!=INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    VideoClient(const VideoClient &)=delete;
    VideoClient &operator=(const VideoClient &)=delete;

    // Execute the versioned private ABI; Linux errno and transport errors stay distinct.
    VV_RESULT Exec(VV_REQUEST request, bool allow_error=false)
    {
        VV_RESULT result={}; DWORD used=0; request.version=VV_API_VERSION;
        if (!DeviceIoControl(handle_,IOCTL_VV_EXEC,&request,sizeof(request),&result,sizeof(result),&used,nullptr))
            throw std::runtime_error("Video transport Win32 error "+std::to_string(GetLastError()));
        if (used!=sizeof(result) || result.version!=VV_API_VERSION) throw std::runtime_error("Video ABI reply mismatch");
        if (result.error && !allow_error) throw std::runtime_error("Host Linux errno "+std::to_string(result.error));
        return result;
    }
    // Issue a payload-free operation such as OPEN, CLOSE, or STREAMON.
    VV_RESULT Op(uint32_t operation, uint32_t queue=0)
    {
        VV_REQUEST request={}; request.operation=operation; request.queue=queue;
        return Exec(request);
    }
    // Pass a fixed-size codec control; pointers and buffer allocation ioctls are excluded.
    template<class T> T Control(uint32_t code, T payload)
    {
        static_assert(sizeof(T)<=208,"control too large");
        VV_REQUEST request={}; request.operation=VV_OP_IOCTL; request.code=code;
        request.payload_size=sizeof(T); std::memcpy(request.payload,&payload,sizeof(T));
        auto result=Exec(request);
        if (result.payload_size) {
            if (result.payload_size!=sizeof(T)) throw std::runtime_error("Control reply length mismatch");
            std::memcpy(&payload,result.payload,sizeof(T));
        }
        return payload;
    }
    // Allocate the actual count returned by the host, never assume the requested count.
    VV_RESULT Allocate(uint32_t queue, uint32_t count)
    {
        VV_REQUEST request={}; request.operation=VV_OP_ALLOC; request.queue=queue; request.count=count;
        return Exec(request);
    }
    // Transfer buffer ownership to the host; wait for its event before reuse.
    void Queue(uint32_t queue, uint32_t index, uint32_t bytes=0, int64_t pts=0)
    {
        VV_REQUEST request={}; request.operation=VV_OP_QBUF; request.queue=queue;
        request.index=index; request.bytesused=bytes; request.timestamp_us=pts;
        Exec(request);
    }
    // Poll only the local completion FIFO; this never sends VIDIOC_DQBUF to the host.
    bool Event(VV_EVENT &event)
    {
        VV_REQUEST request={}; request.operation=VV_OP_EVENT;
        auto result=Exec(request,true);
        if (result.error==11) return false;
        if (result.error) throw std::runtime_error("Event errno "+std::to_string(result.error));
        event=result.event;
        return true;
    }
    // Copy a bounded sample in chunks through driver-owned ordinary guest RAM.
    void Write(uint32_t queue, uint32_t index, const std::vector<unsigned char> &data)
    {
        if (data.size()>VV_MAX_BUFFER_BYTES) throw std::runtime_error("Sample exceeds v1 limit");
        for (size_t at=0;at<data.size();) {
            DWORD used=0;
            const auto n=static_cast<uint32_t>(std::min<size_t>(VV_COPY_LIMIT,data.size()-at));
            VV_COPY header={queue,index,static_cast<uint32_t>(at),n};
            std::vector<unsigned char> input(sizeof(header)+n);
            std::memcpy(input.data(),&header,sizeof(header));
            std::memcpy(input.data()+sizeof(header),data.data()+at,n);
            if (!DeviceIoControl(handle_,IOCTL_VV_WRITE,input.data(),static_cast<DWORD>(input.size()),nullptr,0,&used,nullptr))
                throw std::runtime_error("Buffer write Win32 error "+std::to_string(GetLastError()));
            at+=n;
        }
    }
    // Copy only the validated payload of an already returned CAPTURE buffer.
    std::vector<unsigned char> Read(const VV_EVENT &event)
    {
        if (event.offset>event.bytesused || event.bytesused>VV_MAX_BUFFER_BYTES) throw std::runtime_error("Bad output extent");
        std::vector<unsigned char> output(event.bytesused-event.offset);
        for (size_t at=0;at<output.size();) {
            DWORD used=0;
            const auto n=static_cast<uint32_t>(std::min<size_t>(VV_COPY_LIMIT,output.size()-at));
            VV_COPY copy={event.queue,event.index,event.offset+static_cast<uint32_t>(at),n};
            if (!DeviceIoControl(handle_,IOCTL_VV_READ,&copy,sizeof(copy),output.data()+at,n,&used,nullptr) || used!=n)
                throw std::runtime_error("Buffer read failed");
            at+=n;
        }
        return output;
    }
};
